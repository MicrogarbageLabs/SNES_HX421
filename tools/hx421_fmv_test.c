/* hx421_fmv_test.c — headless smoke test for the FMV static-frame slice.
 * Runs the DLL in FMV mode (HX421_FMV=1 + the M2 boot ROM), steps the 4-band
 * pipeline, and checks each band's emitted DMA body + staged data:
 *   band 0 -> A-bus bank, BG setup, CGRAM slot, tilemap slot, CHR slot, RTL
 *   bands 1-3 -> A-bus bank, CHR slot, RTL
 * plus the PSRAM->window CHR band copy is non-empty and the band advances on
 * FRAME_DONE. Visual correctness is the bsnes step. Public domain (CC0). */
#include "hx421.h"
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

typedef int      (*fn_init)(const Hx421Config *);
typedef void     (*fn_void)(void);
typedef uint32_t (*fn_pull)(int16_t *, uint32_t);
typedef void     (*fn_step)(uint64_t);
typedef uint8_t  (*fn_read)(uint32_t);

#define BANK 0xC00000u
#define FRAME_READY 0x7800u
#define FRAME_DONE  0x79C1u
#define OFF_BODY    0x0208u
#define OFF_CHR     0x0D00u

static fn_read rd;
static uint8_t R(uint32_t off) { return rd(BANK | off); }

static int fails = 0;
#define CHECK(cond, msg) do { if (cond) printf("  PASS %s\n", msg); \
    else { printf("  FAIL %s\n", msg); fails++; } } while (0)

/* does the body of buffer `base` contain byte sequence seq[len] within `span`? */
static int body_has(uint32_t base, const uint8_t *seq, int len, int span) {
    for (int i = 0; i <= span - len; ++i) {
        int ok = 1;
        for (int j = 0; j < len; ++j)
            if (R(base + OFF_BODY + i + j) != seq[j]) { ok = 0; break; }
        if (ok) return 1;
    }
    return 0;
}

/* find the RTL ($6B) terminating the body within `span` */
static int body_rtl_at(uint32_t base, int span) {
    for (int i = 0; i < span; ++i) if (R(base + OFF_BODY + i) == 0x6B) return i;
    return -1;
}

static int chr_nonzero(uint32_t base) {
    for (int i = 0; i < 4800; ++i) if (R(base + OFF_CHR + i)) return 1;
    return 0;
}

int main(void) {
    _putenv("HX421_FMV=1");
    _putenv("HX421_FMV_PREROLL=0");   /* test the steady-state letterbox, not the black preroll */
    _putenv("HX421_ROM=../../snes/build/hx421boot.bin");

    HMODULE h = LoadLibraryA("hx421.dll");
    if (!h) { printf("LoadLibrary failed (%lu)\n", (unsigned long)GetLastError()); return 1; }
    fn_init init = (fn_init)(void *)GetProcAddress(h, "hx421_init");
    fn_void shut = (fn_void)(void *)GetProcAddress(h, "hx421_shutdown");
    fn_step step = (fn_step)(void *)GetProcAddress(h, "hx421_step");
    rd = (fn_read)(void *)GetProcAddress(h, "hx421_cart_read");
    if (!init || !shut || !step || !rd) { printf("resolve failed\n"); return 1; }

    Hx421Config cfg; memset(&cfg, 0, sizeof cfg);
    cfg.abi_version = HX421_ABI_VERSION;
    cfg.cart_window_size = HX421_CART_WINDOW_BYTES;
    cfg.audio_sample_rate = 44100;
    cfg.audio_frames_max = 4096;
    cfg.pad_count = 2;
    cfg.rom_select = HX421_ROM_SMOKE;
    if (init(&cfg)) { printf("init failed\n"); return 1; }

    /* A-bus bank baked once; CGRAM BBAD ($22); VMDATAL BBAD ($18); BGMODE write */
    const uint8_t A1B0[]  = { 0xA9, 0xC0, 0x8D, 0x04, 0x43 };
    const uint8_t CGBB[]  = { 0xA9, 0x22, 0x8D, 0x01, 0x43 };
    const uint8_t VMBB[]  = { 0xA9, 0x18, 0x8D, 0x01, 0x43 };
    const uint8_t BGMD[]  = { 0xA9, 0x01, 0x8D, 0x05, 0x21 };

    for (int band = 0; band < 4; ++band) {
        step(16666666ull);
        uint8_t ready = R(FRAME_READY);
        uint32_t base = (ready == 2) ? 0x3000u : 0x0000u;
        printf("\n[band %d] FRAME_READY=%d base=$%04X\n", band, ready, base);
        CHECK(ready == 1 || ready == 2, "frame published (ready = buf+1)");
        CHECK(R(base + 0x0200u) == 8 && R(base + 0x0201u) == 216, "FMV letterbox top=8 vis_end=216");
        CHECK(R(base + 0x0202u) == 0 && R(base + 0x0203u) == 0, "siphon off");
        CHECK(body_has(base, A1B0, 5, 8), "body: A-bus bank $C0 set once");
        CHECK(body_has(base, VMBB, 5, 300), "body: a VRAM (VMDATAL) DMA slot");
        CHECK(chr_nonzero(base), "CHR band copied into window (gradient non-zero)");
        int rtl = body_rtl_at(base, 400);
        CHECK(rtl > 0, "body: ends in RTL ($6B)");
        if (band == 0)
            CHECK(body_has(base, BGMD, 5, 40),  "band 0: BG setup (BGMODE=1)");
        if (band == 3)
            CHECK(body_has(base, CGBB, 5, 400), "band 3: CGRAM slot (frame palette + flip)");
        (void)R(FRAME_DONE);   /* advance the band */
    }

    /* cycle wrapped back to band 0? */
    step(16666666ull);
    printf("\n[wrap] next band body should be band 0 again (has BG setup)\n");
    uint32_t base = (R(FRAME_READY) == 2) ? 0x3000u : 0x0000u;
    CHECK(body_has(base, BGMD, 5, 40), "band cycle wrapped to band 0");

    shut();
    FreeLibrary(h);
    printf("\n==== %s (%d failures) ====\n", fails ? "FMV SLICE FAIL" : "FMV SLICE OK", fails);
    return fails ? 2 : 0;
}
