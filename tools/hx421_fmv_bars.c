/* ============================================================
 *  hx421_fmv_bars.c — measure a .fmv clip's own letterbox bars
 *
 *  Decodes sampled frames of an FMV2 file and reports, per frame, how many
 *  fully-black pixel rows sit at the top and bottom of the 240x208 image.
 *  This distinguishes the FRAME's letterbox (our 8-line kernel border) from
 *  the CLIP's own baked-in black bars — which is what determines how far a
 *  sprite cursor should actually be allowed to travel.
 *
 *  A pixel counts as black if its tile colour index is 0 (transparent ->
 *  backdrop) or its CGRAM entry is BGR555 zero.
 *
 *  Build: gcc -std=c11 -Wall -O2 tools/hx421_fmv_bars.c -o fmv_bars
 *  Usage: fmv_bars <movie.fmv> [sample_count]
 *
 *  Public domain (CC0). No warranty.
 * ============================================================ */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#define TILES_W     30
#define TILES_H     26
#define IMG_W       (TILES_W * 8)      /* 240 */
#define IMG_H       (TILES_H * 8)      /* 208 */
#define CGRAM_BYTES 256u
#define TM_BYTES    (TILES_H * TILES_W * 2u)   /* 1560 */
#define CHR_TILES   780u
#define CHR_BYTES   (CHR_TILES * 32u)          /* 24960 */
#define BLOCK       (CGRAM_BYTES + TM_BYTES + CHR_BYTES)

static uint32_t rd32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "usage: %s <movie.fmv> [samples]\n", argv[0]); return 2; }
    int samples = (argc > 2) ? atoi(argv[2]) : 12;
    if (samples < 1) samples = 1;

    FILE *f = fopen(argv[1], "rb");
    if (!f) { perror("open"); return 1; }

    uint8_t hdr[32];
    if (fread(hdr, 1, 32, f) != 32 || memcmp(hdr, "FMV2", 4) != 0) {
        fprintf(stderr, "not an FMV2 file\n"); fclose(f); return 1;
    }
    uint32_t fps = rd32(hdr + 8), nframes = rd32(hdr + 12);
    uint32_t rate = rd32(hdr + 16), abytes = rd32(hdr + 24);
    uint32_t unit = abytes + BLOCK;
    printf("FMV2: %ux%u  fps=%u  frames=%u  rate=%u  abytes=%u  unit=%u\n",
           IMG_W, IMG_H, fps, nframes, rate, abytes, unit);

    uint8_t *u = malloc(unit);
    if (!u) { fclose(f); return 1; }

    int min_top = IMG_H, min_bot = IMG_H;   /* worst case across sampled frames */
    long sum_top = 0, sum_bot = 0; int n = 0;

    for (int s = 0; s < samples; ++s) {
        uint32_t idx = (uint32_t)((uint64_t)nframes * s / samples);
        if (fseek(f, (long)(32u + (uint64_t)idx * unit), SEEK_SET) != 0) break;
        if (fread(u, 1, unit, f) != unit) break;

        const uint8_t *cg  = u + abytes;
        const uint8_t *tm  = cg + CGRAM_BYTES;
        const uint8_t *chr = tm + TM_BYTES;

        /* decode to an is-black bitmap */
        static uint8_t blk[IMG_H][IMG_W];
        for (int ty = 0; ty < TILES_H; ++ty) {
            for (int tx = 0; tx < TILES_W; ++tx) {
                const uint8_t *e = tm + (ty * TILES_W + tx) * 2;
                unsigned ent  = (unsigned)e[0] | ((unsigned)e[1] << 8);
                unsigned tile = ent & 0x3FFu;
                unsigned pal  = (ent >> 10) & 7u;
                unsigned vflip = (ent >> 15) & 1u, hflip = (ent >> 14) & 1u;
                const uint8_t *t = chr + (tile < CHR_TILES ? tile : 0) * 32u;
                for (int py = 0; py < 8; ++py) {
                    int sy = vflip ? (7 - py) : py;
                    uint8_t p0 = t[2 * sy], p1 = t[2 * sy + 1];
                    uint8_t p2 = t[16 + 2 * sy], p3 = t[16 + 2 * sy + 1];
                    for (int px = 0; px < 8; ++px) {
                        int sx  = hflip ? (7 - px) : px;
                        int bit = 7 - sx;
                        unsigned c = (unsigned)(((p0 >> bit) & 1)
                                   | (((p1 >> bit) & 1) << 1)
                                   | (((p2 >> bit) & 1) << 2)
                                   | (((p3 >> bit) & 1) << 3));
                        int black;
                        if (c == 0) black = 1;                    /* transparent -> backdrop */
                        else {
                            unsigned ci = pal * 16u + c;
                            unsigned v = (unsigned)cg[ci * 2] | ((unsigned)cg[ci * 2 + 1] << 8);
                            black = (v == 0);
                        }
                        blk[ty * 8 + py][tx * 8 + px] = (uint8_t)black;
                    }
                }
            }
        }

        int top = 0, bot = 0;
        for (int y = 0; y < IMG_H; ++y) {
            int all = 1;
            for (int x = 0; x < IMG_W; ++x) if (!blk[y][x]) { all = 0; break; }
            if (all) top++; else break;
        }
        for (int y = IMG_H - 1; y >= 0; --y) {
            int all = 1;
            for (int x = 0; x < IMG_W; ++x) if (!blk[y][x]) { all = 0; break; }
            if (all) bot++; else break;
        }
        printf("  frame %6u: black rows  top=%3d  bottom=%3d   "
               "(image lines %d..%d, screen %d..%d)\n",
               idx, top, bot, top, IMG_H - 1 - bot, 8 + top, 8 + IMG_H - 1 - bot);
        if (top < min_top) min_top = top;
        if (bot < min_bot) min_bot = bot;
        sum_top += top; sum_bot += bot; n++;
    }

    if (n) {
        printf("\nacross %d sampled frames:\n", n);
        printf("  minimum black rows : top=%d  bottom=%d  (always-black margin)\n", min_top, min_bot);
        printf("  average black rows : top=%.1f bottom=%.1f\n",
               (double)sum_top / n, (double)sum_bot / n);
        printf("\n  => always-visible picture occupies SCREEN lines %d..%d\n",
               8 + min_top, 8 + IMG_H - 1 - min_bot);
        printf("  => 8x8 cursor Y clamp should be %d..%d\n",
               8 + min_top, 8 + IMG_H - 1 - min_bot - 7);
    }
    free(u);
    fclose(f);
    return 0;
}
