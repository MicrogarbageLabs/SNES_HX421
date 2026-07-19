/* hx421_host_test.c — minimal host harness for hx421.dll.
 *
 * Proves the cartridge-seam ABI end-to-end WITHOUT ares: LoadLibrary the DLL,
 * resolve every hx421_* symbol by name (the same GetProcAddress pattern the
 * ares board / real host uses), init in SMOKE mode (which starts the diagnostic
 * tone), pull ~2 s of audio through hx421_audio_pull, write it to dll_out.wav,
 * and report RMS. Non-silent audio through the DLL boundary == the contract works.
 *
 * Public domain (CC0).
 */
#include "hx421.h"

#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

typedef int          (*fn_init)(const Hx421Config *);
typedef void         (*fn_void)(void);
typedef const char  *(*fn_str)(void);
typedef uint32_t     (*fn_u32)(void);
typedef uint32_t     (*fn_pull)(int16_t *, uint32_t);
typedef void         (*fn_step)(uint64_t);
typedef int32_t      (*fn_cmd)(const Hx421AudioCmd *);

static void write_wav(const char *path, const int16_t *s, uint32_t frames, uint32_t rate) {
    uint32_t data = frames * 2u * 2u, byte_rate = rate * 2u * 2u;
    FILE *f = fopen(path, "wb");
    if (!f) return;
    fwrite("RIFF", 1, 4, f); uint32_t riff = 36u + data; fwrite(&riff, 4, 1, f);
    fwrite("WAVEfmt ", 1, 8, f);
    uint32_t fmtlen = 16; uint16_t pcm = 1, ch = 2, bits = 16, blk = 4;
    fwrite(&fmtlen, 4, 1, f); fwrite(&pcm, 2, 1, f); fwrite(&ch, 2, 1, f);
    fwrite(&rate, 4, 1, f); fwrite(&byte_rate, 4, 1, f); fwrite(&blk, 2, 1, f); fwrite(&bits, 2, 1, f);
    fwrite("data", 1, 4, f); fwrite(&data, 4, 1, f);
    fwrite(s, 2, frames * 2u, f);
    fclose(f);
}

#define RESOLVE(T, name) T name = (T)(void *)GetProcAddress(h, #name); if (!name) { printf("resolve %s failed\n", #name); return 1; }

int main(void) {
    HMODULE h = LoadLibraryA("hx421.dll");
    if (!h) { printf("LoadLibrary(hx421.dll) failed (err %lu)\n", GetLastError()); return 1; }

    RESOLVE(fn_init, hx421_init);
    RESOLVE(fn_void, hx421_shutdown);
    RESOLVE(fn_str,  hx421_version);
    RESOLVE(fn_u32,  hx421_abi_version);
    RESOLVE(fn_pull, hx421_audio_pull);
    RESOLVE(fn_step, hx421_step);

    printf("loaded: version=\"%s\" abi=%08x (header %08x)\n",
           hx421_version(), hx421_abi_version(), (unsigned)HX421_ABI_VERSION);
    if (hx421_abi_version() != HX421_ABI_VERSION) { printf("ABI mismatch\n"); return 1; }

    Hx421Config cfg; memset(&cfg, 0, sizeof cfg);
    cfg.abi_version      = HX421_ABI_VERSION;
    cfg.cart_window_size = HX421_CART_WINDOW_BYTES;
    cfg.audio_sample_rate = 0;                 /* auto -> 44100 */
    cfg.audio_frames_max = 4096;
    cfg.pad_count        = 2;
    cfg.rom_select       = HX421_ROM_SMOKE;    /* -> diagnostic tone */
    int rc = hx421_init(&cfg);
    if (rc) { printf("hx421_init rc=%d\n", rc); return 1; }

    /* Command-channel exercise: when HX421_CMD is set, the runtime preloaded SFX
     * slots and skipped auto-start, so drive it via the command channel. */
    if (getenv("HX421_CMD")) {
        fn_cmd hx421_audio_command = (fn_cmd)(void *)GetProcAddress(h, "hx421_audio_command");
        if (!hx421_audio_command) { printf("resolve hx421_audio_command failed\n"); return 1; }
        Hx421AudioCmd c;
        memset(&c, 0, sizeof c); c.opcode = HX421_ACMD_PLAY_MUSIC; c.arg = 0;
        printf("PLAY_MUSIC       -> voice %d\n", hx421_audio_command(&c));
        memset(&c, 0, sizeof c); c.opcode = HX421_ACMD_TRIGGER; c.slot = 0;
        printf("TRIGGER slot 0   -> voice %d\n", hx421_audio_command(&c));
        memset(&c, 0, sizeof c); c.opcode = HX421_ACMD_TRIGGER; c.slot = 1; c.pan = -16000;
        printf("TRIGGER slot 1 L -> voice %d\n", hx421_audio_command(&c));
    }

    const uint32_t RATE = 44100u, TOTAL = RATE * 2u, BLK = 1024u;
    static int16_t out[44100u * 2u * 2u];      /* 2 s stereo */
    uint32_t got = 0;
    while (got < TOTAL) {
        uint32_t want = (TOTAL - got) < BLK ? (TOTAL - got) : BLK;
        uint32_t n = hx421_audio_pull(out + (size_t)got * 2u, want);
        hx421_step(0);
        if (n == 0) break;
        got += n;
    }

    double sum = 0; int16_t peak = 0;
    for (uint32_t i = 0; i < got * 2u; ++i) {
        double v = out[i]; sum += v * v;
        int16_t a = out[i] < 0 ? (int16_t)-out[i] : out[i];
        if (a > peak) peak = a;
    }
    double rms = got ? sqrt(sum / (double)(got * 2u)) : 0.0;
    printf("pulled %u frames, rms=%.1f, peak=%d\n", got, rms, peak);
    write_wav("dll_out.wav", out, got, RATE);

    hx421_shutdown();
    FreeLibrary(h);

    if (got == 0 || rms < 50.0) { printf("FAIL: silence through the ABI\n"); return 2; }
    printf("OK: audio flowed through the hx421.dll ABI\n");
    return 0;
}
