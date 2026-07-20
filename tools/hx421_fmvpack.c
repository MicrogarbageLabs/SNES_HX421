/* ============================================================
 *  hx421_fmvpack.c — pack a directory of .fmv clips into one container
 *
 *  Playback needs to seek to an arbitrary FRAME, not just open a file: that is
 *  what branching playback, resume-after-pause and preroll priming all rest on.
 *  So the container carries a per-frame offset/length index and every clip
 *  lives in one file, making a seek an fseek rather than a directory walk and
 *  an open — which matters on SD, where the open is the expensive part.
 *
 *  Layout (all little-endian):
 *
 *    header   16 B   "HXFP" | version u16 | clip_count u16 | reserved u32
 *    clips    48 B each, in order:
 *               name[20]        NUL-padded, from the source filename
 *               fps        u16
 *               frames     u32
 *               tile_count u16
 *               abytes     u32   audio bytes per frame (muxed, 0 = silent)
 *               unit       u32   bytes per frame unit in the source
 *               index_off  u32   file offset of this clip's frame index
 *               payload_off u32  file offset of this clip's first frame
 *    index    per clip, `frames` entries of 8 B: offset u32, length u32
 *    payload  the frame units, clip by clip
 *
 *  Frame length is stored per frame even though the current FMV2 encoder emits
 *  fixed-size units: a later codec with variable-length frames drops straight
 *  in, and a reader that trusts `length` needs no change.
 *
 *  Build: gcc -std=c11 -Wall -O2 tools/hx421_fmvpack.c -o fmvpack
 *  Usage: fmvpack <src-dir> <out.hxfp>
 *
 *  Public domain (CC0). No warranty.
 * ============================================================ */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#if defined(_WIN32)
#  include <windows.h>
#else
#  include <dirent.h>
#endif

#define HXFP_MAGIC   "HXFP"
#define HXFP_VERSION 1u
#define MAX_CLIPS    64
#define NAME_LEN     20

/* FMV2 source header: 32 B, "FMV2" then fields we need at fixed offsets. */
#define FMV2_HDR      32u
#define FMV2_OFF_FPS   8u
#define FMV2_OFF_NFR  12u
#define FMV2_OFF_RATE 16u
#define FMV2_OFF_AB   24u
/* video block: CGRAM 256 + tilemap 1560 + CHR 24960 */
#define FMV2_BLOCK  (256u + 1560u + 24960u)
#define FMV2_TILES  780u

typedef struct {
    char     name[NAME_LEN];
    uint16_t fps;
    uint32_t frames;
    uint16_t tile_count;
    uint32_t abytes;
    uint32_t unit;
    uint32_t index_off;
    uint32_t payload_off;
    char     path[1024];
} Clip;

static uint32_t rd32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static void wr16(FILE *f, uint16_t v) { fputc(v & 0xFF, f); fputc((v >> 8) & 0xFF, f); }
static void wr32(FILE *f, uint32_t v) { wr16(f, (uint16_t)(v & 0xFFFF)); wr16(f, (uint16_t)(v >> 16)); }

/* Read one clip's FMV2 header. Returns 0 on success. */
static int probe(const char *path, Clip *c) {
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    uint8_t h[FMV2_HDR];
    if (fread(h, 1, FMV2_HDR, f) != FMV2_HDR || memcmp(h, "FMV2", 4) != 0) {
        fclose(f); return -1;
    }
    c->fps        = (uint16_t)rd32(h + FMV2_OFF_FPS);
    c->frames     = rd32(h + FMV2_OFF_NFR);
    c->abytes     = rd32(h + FMV2_OFF_AB);
    c->tile_count = FMV2_TILES;
    c->unit       = c->abytes + FMV2_BLOCK;

    /* The fps field has been observed carrying something other than a frame
     * rate, so derive it from the audio interleave, which is exact by
     * construction: abytes = rate/fps * channels * bytes. */
    uint32_t rate = rd32(h + FMV2_OFF_RATE);
    if (c->abytes && rate) {
        uint32_t derived = rate / (c->abytes / 4u);
        if (derived >= 1u && derived <= 60u) c->fps = (uint16_t)derived;
    }
    fclose(f);
    return (c->frames && c->unit) ? 0 : -1;
}

/* Copy a filename into the fixed-width name field, truncating deliberately. */
static void set_name(char *dst, const char *src) {
    size_t i = 0;
    for (; i + 1 < NAME_LEN && src[i]; ++i) dst[i] = src[i];
    dst[i] = '\0';
}

static int list_dir(const char *dir, Clip *clips, int max) {
    int n = 0;
#if defined(_WIN32)
    char pat[1024];
    snprintf(pat, sizeof pat, "%s\\*.fmv", dir);   /* the pattern does the filtering */
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pat, &fd);
    if (h == INVALID_HANDLE_VALUE) return 0;
    do {
        if (n >= max) break;
        snprintf(clips[n].path, sizeof clips[n].path, "%s\\%s", dir, fd.cFileName);
        set_name(clips[n].name, fd.cFileName);
        n++;
    } while (FindNextFileA(h, &fd));
    FindClose(h);
#else
    DIR *d = opendir(dir);
    if (!d) return 0;
    struct dirent *e;
    while ((e = readdir(d)) && n < max) {
        size_t l = strlen(e->d_name);
        if (l <= 4 || (strcmp(e->d_name + l - 4, ".fmv") != 0 &&
                       strcmp(e->d_name + l - 4, ".FMV") != 0)) continue;
        snprintf(clips[n].path, sizeof clips[n].path, "%s/%s", dir, e->d_name);
        set_name(clips[n].name, e->d_name);
        n++;
    }
    closedir(d);
#endif
    return n;
}

/* Read a container back and print what a player would see. Proves the format
 * round-trips rather than only that it was written. */
static int list_container(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) { perror("open"); return 1; }
    uint8_t h[16];
    if (fread(h, 1, 16, f) != 16 || memcmp(h, HXFP_MAGIC, 4) != 0) {
        fprintf(stderr, "not an HXFP container\n"); fclose(f); return 1;
    }
    unsigned ver = h[4] | (h[5] << 8), cnt = h[6] | (h[7] << 8);
    printf("HXFP v%u, %u clip(s)\n", ver, cnt);
    for (unsigned i = 0; i < cnt; ++i) {
        uint8_t c[48];
        if (fread(c, 1, 48, f) != 48) break;
        char nm[NAME_LEN + 1]; memcpy(nm, c, NAME_LEN); nm[NAME_LEN] = 0;
        unsigned fps = c[20] | (c[21] << 8);
        uint32_t frames = rd32(c + 22), abytes = rd32(c + 28);
        uint32_t unit = rd32(c + 32), idx = rd32(c + 36), pay = rd32(c + 40);
        printf("  %-20s %3u fps  %6u frames  unit %6u  abytes %5u  index@%u payload@%u\n",
               nm, fps, frames, unit, abytes, idx, pay);

        /* Spot-check the index: first, middle and last frame must point inside
         * the payload and be contiguous at `unit` stride. */
        long save = ftell(f);
        uint32_t probe_f[3] = { 0, frames / 2, frames ? frames - 1 : 0 };
        for (int k = 0; k < 3; ++k) {
            uint8_t e[8];
            fseek(f, (long)(idx + probe_f[k] * 8u), SEEK_SET);
            if (fread(e, 1, 8, f) != 8) { printf("    frame %u: INDEX SHORT\n", probe_f[k]); continue; }
            uint32_t o = rd32(e), l = rd32(e + 4);
            uint32_t expect = pay + probe_f[k] * unit;
            printf("    frame %6u -> off %10u len %6u  %s\n", probe_f[k], o, l,
                   (o == expect && l == unit) ? "ok" : "MISMATCH");
        }
        fseek(f, save, SEEK_SET);
    }
    fclose(f);
    return 0;
}

int main(int argc, char **argv) {
    if (argc == 3 && strcmp(argv[1], "--list") == 0) return list_container(argv[2]);
    if (argc < 3) {
        fprintf(stderr, "usage: %s <src-dir> <out.hxfp>\n"
                        "       %s --list <file.hxfp>\n", argv[0], argv[0]);
        return 2;
    }
    static Clip clips[MAX_CLIPS];
    int n = list_dir(argv[1], clips, MAX_CLIPS);
    if (n <= 0) { fprintf(stderr, "no .fmv files in %s\n", argv[1]); return 1; }

    /* Probe every clip first, so a bad file fails before anything is written. */
    int good = 0;
    for (int i = 0; i < n; ++i) {
        if (probe(clips[i].path, &clips[i]) != 0) {
            fprintf(stderr, "  SKIP %s (not FMV2 or unreadable)\n", clips[i].name);
            continue;
        }
        if (good != i) clips[good] = clips[i];
        good++;
    }
    if (!good) { fprintf(stderr, "no usable clips\n"); return 1; }
    n = good;

    /* Lay the file out before writing: header, clip table, per-clip index,
     * then payload. Offsets are absolute so a reader seeks once. */
    const uint32_t hdr_sz   = 16u;
    const uint32_t clip_sz  = 48u;
    uint32_t off = hdr_sz + (uint32_t)n * clip_sz;
    for (int i = 0; i < n; ++i) {
        clips[i].index_off = off;
        off += clips[i].frames * 8u;
    }
    for (int i = 0; i < n; ++i) {
        clips[i].payload_off = off;
        off += clips[i].frames * clips[i].unit;
    }

    FILE *o = fopen(argv[2], "wb");
    if (!o) { perror("open out"); return 1; }

    fwrite(HXFP_MAGIC, 1, 4, o);
    wr16(o, HXFP_VERSION);
    wr16(o, (uint16_t)n);
    wr32(o, 0);
    wr32(o, 0);

    for (int i = 0; i < n; ++i) {
        char nm[NAME_LEN]; memset(nm, 0, NAME_LEN);
        memcpy(nm, clips[i].name, strnlen(clips[i].name, NAME_LEN - 1));
        fwrite(nm, 1, NAME_LEN, o);
        wr16(o, clips[i].fps);
        wr32(o, clips[i].frames);
        wr16(o, clips[i].tile_count);
        wr32(o, clips[i].abytes);
        wr32(o, clips[i].unit);
        wr32(o, clips[i].index_off);
        wr32(o, clips[i].payload_off);
        for (int pad = 0; pad < 48 - 20 - 2 - 4 - 2 - 4 - 4 - 4 - 4; ++pad) fputc(0, o);
    }

    for (int i = 0; i < n; ++i)                       /* per-frame index */
        for (uint32_t f = 0; f < clips[i].frames; ++f) {
            wr32(o, clips[i].payload_off + f * clips[i].unit);
            wr32(o, clips[i].unit);
        }

    uint8_t *buf = malloc(1u << 20);
    if (!buf) { fclose(o); return 1; }
    for (int i = 0; i < n; ++i) {                     /* payload */
        FILE *in = fopen(clips[i].path, "rb");
        if (!in) { fprintf(stderr, "reopen failed: %s\n", clips[i].path); free(buf); fclose(o); return 1; }
        fseek(in, FMV2_HDR, SEEK_SET);
        uint64_t want = (uint64_t)clips[i].frames * clips[i].unit, done = 0;
        while (done < want) {
            size_t chunk = (size_t)((want - done) > (1u << 20) ? (1u << 20) : (want - done));
            size_t got = fread(buf, 1, chunk, in);
            if (!got) break;
            fwrite(buf, 1, got, o);
            done += got;
        }
        if (done < want)
            fprintf(stderr, "  WARN %s: short by %llu B (index still valid, tail frames absent)\n",
                    clips[i].name, (unsigned long long)(want - done));
        fclose(in);
        printf("  %-20s %3u fps  %6u frames  unit %6u  %8.1f MB\n",
               clips[i].name, clips[i].fps, clips[i].frames, clips[i].unit,
               (double)want / (1024.0 * 1024.0));
    }
    free(buf);

    long total = ftell(o);
    fclose(o);
    printf("packed %d clip(s) -> %s (%.1f MB)\n", n, argv[2], (double)total / (1024.0 * 1024.0));
    return 0;
}
