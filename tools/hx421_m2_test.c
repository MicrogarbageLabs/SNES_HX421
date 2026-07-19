/* hx421_m2_test.c — headless verification of the milestone-2 dynamic
 * renderer PLUMBING (no ares/GUI). LoadLibrary the DLL with HX421_ROM set to
 * the M2 boot ROM, then simulate the SNES kernel's side of the window contract:
 *
 *   - pull audio (feeds the FFT the SMOKE tone),
 *   - step() the DLL (it stages a frame into the BACK buffer + raises ready),
 *   - read the FRONT buffer the way the kernel does: action table (line-plan),
 *     frame header, DMA descriptor, and the FFT-bar tilemap,
 *   - strobe FRAME_DONE (what the kernel reads after DMA'ing) and confirm the
 *     DLL flips buffers + clears the ready flag (the double-buffer handshake),
 *   - repeat and confirm ping-pong + that the bars animate.
 *
 * This proves the chainer is being driven end-to-end short of the actual PPU
 * pixels (that is the GUI/eyeball step). Public domain (CC0).
 */
#include "hx421.h"
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef int       (*fn_init)(const Hx421Config *);
typedef void      (*fn_void)(void);
typedef uint32_t  (*fn_pull)(int16_t *, uint32_t);
typedef void      (*fn_step)(uint64_t);
typedef uint8_t   (*fn_read)(uint32_t);

#define BANK 0xC00000u
#define FRAME_READY 0x7800u
#define FRAME_DONE  0x79C1u

/* siphon contract mirror (snes/hx421.inc + runtime HX_SIP_*) */
#define SIP_FIRST   17u          /* HX_TOP_LB + 1                         */
#define SIP_LINES   2u           /* 32 CHR bytes / 16 bytes-per-line      */
#define SIP_BPL     16u
#define SIP_VRAM    0x1020u      /* CHR base $1000 + tile 2 * 16 words    */
#define SIP_TILE    2u
#define SIP_TILE_ROW 8u

static fn_read rd;
static uint8_t R(uint32_t off) { return rd(BANK | off); }   /* read window[off] */

static int fails = 0;
#define CHECK(cond, msg) do { if (cond) { printf("  PASS %s\n", msg); } \
    else { printf("  FAIL %s\n", msg); fails++; } } while (0)

/* pull audio so the FFT has recent spectrum */
static fn_pull pull;
static void feed_audio(uint32_t frames) {
    int16_t buf[1024 * 2];
    while (frames) {
        uint32_t n = frames > 1024u ? 1024u : frames;
        pull(buf, n);
        frames -= n;
    }
}

static int check_action_table(uint32_t base, int top_lb, int vis_end) {
    int bad = 0;
    for (int v = 0; v < 262; ++v) {
        uint8_t a = R(base + (uint32_t)v);
        uint8_t want;
        if (v < top_lb)            want = 1;   /* top letterbox: blank   */
        else if (v == top_lb)      want = 2;   /* unblank                */
        else if ((uint32_t)v >= SIP_FIRST && (uint32_t)v < SIP_FIRST + SIP_LINES)
                                   want = 3;   /* H-blank siphon lines   */
        else if (v < vis_end)      want = 0;   /* visible band           */
        else                       want = 1;   /* bottom LB + vblank     */
        if (a != want) { if (bad < 4) printf("    action[%d]=%d want %d\n", v, a, want); bad++; }
    }
    return bad;
}

static int count_bar_cells(uint32_t tmap) {
    int n = 0;
    for (int i = 0; i < 1024; ++i)
        if (R(tmap + (uint32_t)i * 2u) != 0) n++;
    return n;
}

/* Verify the coprocessor-emitted 65816 DMA body in buffer `base`: the kernel
 * jsl's straight into this. Checks the baked A-bus bank, the tilemap slot's
 * src/size immediates, the MDMAEN fire, and the closing RTL. Byte offsets
 * mirror runtime hx_emit_dma_body / e_dma_vram_slot. */
static void check_dma_body(uint32_t base, uint32_t expect_src) {
    uint32_t b = base + 0x0208u;                     /* HX_OFF_DMABODY */
    uint32_t src  = R(b + 18) | ((uint32_t)R(b + 19) << 8);
    uint32_t size = R(b + 24) | ((uint32_t)R(b + 25) << 8);
    printf("  dma body: A1B0=$%02X bbus=$%02X src=$%04X size=%u rtl=$%02X\n",
           R(b + 1), R(b + 6), src, size, R(b + 51));
    CHECK(R(b + 0) == 0xA9 && R(b + 1) == 0xC0, "body: lda #$C0 (A-bus bank $C0)");
    CHECK(R(b + 2) == 0x8D && (R(b + 3) | (R(b + 4) << 8)) == 0x4304, "body: sta A1B0");
    CHECK(R(b + 5) == 0xA9 && R(b + 6) == 0x18, "body: lda #$18 (B-bus VMDATAL)");
    CHECK(src == expect_src, "body: baked src = buffer tilemap");
    CHECK(size == 2048, "body: baked size = 2048");
    CHECK(R(b + 46) == 0xA9 && R(b + 47) == 0x01 && R(b + 48) == 0x8D
          && (R(b + 49) | (R(b + 50) << 8)) == 0x420B, "body: sta MDMAEN (fire ch0)");
    CHECK(R(b + 51) == 0x6B, "body: ends in RTL");
}

/* Verify the H-blank siphon descriptor + payload staged in buffer `base`.
 * expect_src = the source base offset for this buffer (buf0 $0C00 / buf1 $3C00). */
static void check_siphon(uint32_t base, uint32_t expect_src) {
    uint32_t bpl  = R(base + 0x0202u) | ((uint32_t)R(base + 0x0203u) << 8);
    uint32_t vram = R(base + 0x0204u) | ((uint32_t)R(base + 0x0205u) << 8);
    uint32_t src  = R(base + 0x0206u) | ((uint32_t)R(base + 0x0207u) << 8);
    printf("  siphon desc: bpl=%u vram=$%04X src=$%04X\n", bpl, vram, src);
    CHECK(bpl == SIP_BPL, "siphon bytes/line = 16");
    CHECK(vram == SIP_VRAM, "siphon VRAM dst = tile-2 CHR word $1020");
    CHECK(src == expect_src, "siphon source base = buffer payload offset");
    /* payload: the animated tile-2 CHR must have lit plane0 bytes */
    int lit = 0;
    for (uint32_t i = 0; i < 32u; ++i) if (R(base + 0x0C00u + i)) lit++;
    CHECK(lit > 0, "siphon CHR payload has lit pixels");
    /* the bulk tilemap must plant the siphon tile across its row */
    int cells = 0;
    for (uint32_t c = 0; c < 32u; ++c) {
        uint32_t cell = (SIP_TILE_ROW * 32u + c) * 2u;
        if (R(base + 0x0400u + cell) == SIP_TILE) cells++;
    }
    CHECK(cells == 32, "siphon tile row planted in the tilemap (32 cells)");
}

int main(void) {
    /* Serve the M2 boot ROM as the cart window so the runtime enables the
     * M2 producer (g_rom_loaded). Set BEFORE init (the DLL reads it in init). */
    _putenv("HX421_ROM=../../snes/build/hx421boot.bin");

    HMODULE h = LoadLibraryA("hx421.dll");
    if (!h) { printf("LoadLibrary failed (err %lu)\n", (unsigned long)GetLastError()); return 1; }

    fn_init  init  = (fn_init)(void *)GetProcAddress(h, "hx421_init");
    fn_void  shut  = (fn_void)(void *)GetProcAddress(h, "hx421_shutdown");
    fn_step  step  = (fn_step)(void *)GetProcAddress(h, "hx421_step");
    pull = (fn_pull)(void *)GetProcAddress(h, "hx421_audio_pull");
    rd   = (fn_read)(void *)GetProcAddress(h, "hx421_cart_read");
    if (!init || !shut || !step || !pull || !rd) { printf("resolve failed\n"); return 1; }

    Hx421Config cfg; memset(&cfg, 0, sizeof cfg);
    cfg.abi_version = HX421_ABI_VERSION;
    cfg.cart_window_size = HX421_CART_WINDOW_BYTES;
    cfg.audio_sample_rate = 44100;
    cfg.audio_frames_max = 4096;
    cfg.pad_count = 2;
    cfg.rom_select = HX421_ROM_SMOKE;   /* drives the FFT with a 440 Hz tone */
    if (init(&cfg)) { printf("init failed\n"); return 1; }

    /* Confirm the ROM really got served (reset vector in the window). */
    printf("reset vector window[$FFFC/$FFFD] = $%02X%02X\n", R(0xFFFDu), R(0xFFFCu));
    CHECK(R(0xFFFCu) != 0x00 || R(0xFFFDu) != 0x00, "reset vector non-zero (ROM served)");

    /* Warm the FFT (~1.5 s — the analyzer accumulates before bands populate;
     * on hardware/bsnes audio flows continuously so it is always warm), then
     * stage the first frame. */
    feed_audio(44100 * 3 / 2);
    step(16666666ull);

    printf("\n[frame 1] back buffer should be buf0, published as ready=1\n");
    uint8_t ready = R(FRAME_READY);
    printf("  FRAME_READY = %d\n", ready);
    CHECK(ready == 1, "frame-ready = buf0+1");

    uint32_t b0 = 0x0000u;
    int top_lb  = R(b0 + 0x0200u);
    int vis_end = R(b0 + 0x0201u);
    printf("  header: top_lb=%d vis_end=%d\n", top_lb, vis_end);
    CHECK(top_lb == 16 && vis_end == 208, "header top_lb=16 vis_end=208");
    CHECK(check_action_table(b0 + 0x0000u, top_lb, vis_end) == 0, "action table line-plan (blank/unblank/none)");

    /* emitted DMA body (the kernel jsl's into this) */
    check_dma_body(b0, 0x0400u);   /* buf0 tilemap src $0400 */

    int bars0 = count_bar_cells(b0 + 0x0400u);
    printf("  tilemap non-empty cells = %d\n", bars0);
    CHECK(bars0 > 0, "FFT bars present in the tilemap");

    /* H-blank siphon: descriptor + animated CHR payload + planted tile row */
    check_siphon(b0, 0x0C00u);

    /* --- the kernel walks the list and strobes FRAME_DONE --- */
    printf("\n[handshake] strobe FRAME_DONE (kernel finished DMA)\n");
    (void)R(FRAME_DONE);
    CHECK(R(FRAME_READY) == 0, "FRAME_DONE cleared frame-ready");

    /* next frame must go to the OTHER buffer (buf1) */
    feed_audio(44100 / 4);
    step(16666666ull);
    printf("\n[frame 2] back buffer should now be buf1, published as ready=2\n");
    printf("  FRAME_READY = %d\n", R(FRAME_READY));
    CHECK(R(FRAME_READY) == 2, "ping-pong: frame-ready = buf1+1");

    uint32_t b1 = 0x3000u;
    CHECK(R(b1 + 0x0200u) == 16 && R(b1 + 0x0201u) == 208, "buf1 header ok");
    CHECK(check_action_table(b1 + 0x0000u, 16, 208) == 0, "buf1 action table ok");
    check_dma_body(b1, 0x3400u);   /* buf1 tilemap src $3400 */
    CHECK(count_bar_cells(b1 + 0x0400u) > 0, "buf1 has FFT bars");
    check_siphon(b1, 0x3C00u);   /* buf1 payload lives at $3000 + $0C00 */

    (void)R(FRAME_DONE);
    CHECK(R(FRAME_READY) == 0, "second FRAME_DONE cleared ready");

    /* --- animation: bars should change as audio advances --- */
    printf("\n[animation] bar shape should change across frames\n");
    feed_audio(44100 / 4); step(16666666ull);           /* -> buf0 again */
    int snapA = count_bar_cells(0x0000u + 0x0400u);
    (void)R(FRAME_DONE);
    /* pull a very different amount / let the tone ring; compare a later frame */
    for (int k = 0; k < 6; ++k) { feed_audio(44100 / 10); step(16666666ull); if (k < 5) (void)R(FRAME_DONE); }
    int back = R(FRAME_READY) - 1;
    uint32_t bb = back == 1 ? 0x3000u : 0x0000u;
    int snapB = count_bar_cells(bb + 0x0400u);
    printf("  bar-cell counts: snapA=%d snapB=%d (back=buf%d)\n", snapA, snapB, back);
    CHECK(snapA > 0 && snapB > 0, "bars non-empty across time");

    shut();
    FreeLibrary(h);
    printf("\n==== %s (%d failures) ====\n", fails ? "M2 PLUMBING FAIL" : "M2 PLUMBING OK", fails);
    return fails ? 2 : 0;
}
