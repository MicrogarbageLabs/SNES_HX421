/* hx421_runtime.c — the HX-421 coprocessor runtime: implements the
 * cartridge-seam ABI (include/hx421.h) over the portable audio engine
 * (engine/service.h, the hxa_* API).
 *
 * This is the same runtime on both hosts: built as hx421.dll for the ares
 * host (PC), or linked static into the M3 firmware (HX421_STATIC). The only
 * platform piece here is the file reader (stdio on PC); an MCU port swaps it
 * for an SD/flash reader.
 *
 * M0 scope: the audio path is real (pull -> hxa_render, drift PLL live in the
 * mixer). The cart window / mailbox is a minimal buffer + status byte — enough
 * to answer bus reads; the full DMA-descriptor / frame-staging surface and the
 * command channel that drive video + trigger audio arrive in later milestones.
 * For now audio is driven by autostart (a streamed WAV) or, in SMOKE mode, a
 * built-in diagnostic tone, so the DLL is self-demonstrating.
 *
 * Public domain (CC0). No warranty.
 */
#include "hx421.h"
#include "service.h"          /* hxa_* (also pulls audio_mixer.h -> Q15_ONE) */
#include "audio/sink.h"       /* AudioSinkBackend (the WASAPI live sink, DLL only) */

#include <stdint.h>
#include <string.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

/* ---- singleton runtime state (the ABI is a single instance, like mgapi) --- */
static HxaService *g_svc;
static uint8_t     g_window[HX421_CART_WINDOW_BYTES];
static uint16_t    g_pads[HX421_MAX_PADS];
static int         g_mouse_dx, g_mouse_dy;
static unsigned    g_mouse_btn;
static uint64_t    g_now_ns;
static uint64_t    g_reset_deadline_ns;
static uint64_t    g_reset_hold_ns;
static int         g_resetting;
static uint64_t    g_pull_frames;    /* cumulative master-clock reference */
static int         g_smoke_tone;     /* SMOKE: procedural continuous sine (mixer BYPASS) */
static int         g_smoke_mixer;    /* SMOKE: continuous sine fed as PCM THROUGH the mixer */
static AudioVoiceHandle g_smoke_voice;
static double      g_smoke_phase;
static uint32_t    g_smoke_rate;

#define HX421_SFX_SLOTS 8
static AudioObjHandle   g_sfx[HX421_SFX_SLOTS];  /* loaded one-shot SFX, by slot */
static AudioVoiceHandle g_music_voice;           /* current streamed-music voice */
static int              g_cmd_mode;              /* command-driven (no auto-start) */
static uint64_t         g_fft_next;              /* next g_pull_frames to print the spectrum */
static int              g_rom_loaded;            /* HX421_ROM served a real boot image */

/* ---- M2 dynamic renderer: double-buffered frame production ------------- *
 * The DLL plays the RISC-V: each frame it writes a line-plan action table +
 * a staged BG tilemap of FFT bars into the BACK buffer of the served window,
 * then raises the frame-ready flag. The SNES kernel (snes/kernel.s) reads the
 * FRONT buffer via a free-running H-IRQ + per-line action table, and strobes
 * HX_FRAME_DONE_ADDR when it has DMA'd the frame — on which we flip buffers.
 * Window contract mirrors snes/hx421.inc — keep both in sync. See
 * docs/window-contract.md. */
#define HX_BUF0_BASE        0x0000u
#define HX_BUF1_BASE        0x3000u
#define HX_OFF_ACTION       0x0000u   /* 512 B: action[V], V=0..261            */
#define HX_OFF_HDR_TOP_LB   0x0200u   /* top-letterbox line count (unblank V)  */
#define HX_OFF_HDR_VIS_END  0x0201u   /* first bottom-letterbox line           */
#define HX_OFF_SIP_BPL      0x0202u   /* word: siphon bytes/line (0 = off)     */
#define HX_OFF_SIP_VRAM     0x0204u   /* word: siphon VRAM word dst (set once) */
#define HX_OFF_SIP_SRC      0x0206u   /* word: siphon source base in bank $C0  */
#define HX_OFF_DMABODY      0x0208u   /* RISC-V-emitted 65816 DMA body (baked) */
#define HX_OFF_TILEMAP      0x0400u   /* 2048 B: 32x32 BG1 tilemap words       */
#define HX_OFF_SIPHON_DATA  0x0C00u   /* siphon payload (clear of the tilemap) */
#define HX_FRAME_READY_ADDR 0x7800u   /* 0 = none; else (back_index + 1)       */
#define HX_FRAME_DONE_ADDR  0x79C1u   /* SNES read-strobe: frame DMA'd -> flip */

#define HX_TOP_LB      16             /* top letterbox lines (>= tilemap-fits) */
#define HX_BOT_LB      16             /* bottom letterbox lines                */
#define HX_VIS_END     (224 - HX_BOT_LB)  /* first bottom-letterbox line (208) */
#define HX_TILE_COLS   32
#define HX_TILE_ROWS   32
#define HX_FFT_BARS    16
#define HX_BAR_MAXH    24             /* max bar height (tile rows) in the band */
#define HX_BAR_ANCHOR  25            /* BG tile row bars grow up from (bottom)  */

/* --- H-blank siphon demo: stream an animated 32-byte CHR for a "siphon
 * tile" into VRAM during ACTIVE display, purely through the per-line
 * H-blank siphon path (never the vblank bulk DMA). The bulk tilemap plants
 * that tile across a row in the UPPER-MIDDLE of the visible area -- fetched
 * (screen line ~64) long after the siphon writes its CHR at V=17/18, so no
 * roll-ahead hazard. If the siphon lands you see a scrolling green band; if
 * it does not, the row stays backdrop. This is the framework proof that a
 * plain GP-DMA to VMDATA lands in VRAM inside H-blank while the screen is
 * live -- the primitive the rolling 3D / FMV paths will build on. */
#define HX_SIP_BPL         16u                    /* bytes/line (2 lines = 32 B)  */
#define HX_SIP_TILE        2u                     /* the siphon tile index        */
#define HX_SIP_VRAM_WORD   (0x1000u + HX_SIP_TILE * 16u) /* CHR base $1000 + N*16 */
#define HX_SIP_CHR_BYTES   32u                    /* one 4bpp tile                */
#define HX_SIP_FIRST_LINE  (HX_TOP_LB + 1)        /* first visible siphon line    */
#define HX_SIP_LINES       (HX_SIP_CHR_BYTES / HX_SIP_BPL) /* = 2                 */
#define HX_SIP_TILE_ROW    8u                      /* tile row (line ~64) >> V=18 */

/* ---- sprite-overlay window layout (used by the emitter below) ------------
 * SHARED regions above both frame buffers (buffers end at 0x5FFF; the kernel
 * image lives at $8000+; strobes sit at 0x7800/0x79C1). Not double-buffered:
 * OAM/CHR/CGRAM are pushed to the PPU every frame regardless.
 *
 * GOTCHA (from mgapi): OBJ CHR starts at VRAM word 28880 (tile 269), NOT 28864.
 * At BG1 CHR base 0x4000 the FMV's BLANK_TILE (780) aliases to word 28864, so
 * sprite CHR there paints the crosshair into the FMV letterbox on alternate
 * frames. BG1 never references past tile 780, so 28880+ is genuinely free. */
#define HX_OFF_SPR_CHR     0x6000u   /* 384 B: 12 OBJ tiles                  */
#define HX_OFF_SPR_CGRAM   0x6180u   /* 128 B: OBJ pal 0-3 (cursor + holes)  */
#define HX_OFF_SPR_PAL47   0x6200u   /* 128 B: OBJ pal 4-7 (FFT gradient)    */
#define HX_OFF_SPR_OAM     0x6280u   /* 544 B: full OAM (low 512 + high 32)  */
#define HX_SPR_CHR_BYTES    384u
#define HX_SPR_CGRAM_BYTES  128u
#define HX_SPR_OAM_BYTES    544u
#define HX_SPR_OAM_ACTIVE   256u     /* sprites 0-63 low table (60 Hz push)  */
#define HX_SPR_VRAM_WORD  28880u
#define HX_SPR_TILE_CURSOR  269u
#define HX_SPR_TILE_HOLE    270u
#define HX_SPR_FFT_TILE0    271u     /* 271..279 = fill level 0..8           */
#define HX_SPR_TILE_CAP     280u
#define HX_SPR_OBSEL       0x03u     /* size pair 8/16, namesel 0, base 0x6000 */
#define HX_SPR_CGADD        128u     /* OBJ palette 0 = CGRAM index 128      */
#define HX_SPR_FFT_CGADD    192u     /* OBJ palette 4                        */

/* ---- 65816 DMA-body emitter (the max-bandwidth NMI-emitter technique) ----
 * The coprocessor (here the DLL) bakes the per-frame VRAM push as a straight
 * run of immediate-loaded DMA slots -- no descriptor list for the SNES to
 * read, no runtime dest dispatch -- ending in RTL. The kernel jsl's straight
 * into it in the served window (execute-from-window). Mirrors microgarbage
 * mg_nmi_emit_dma_direct / copro_r3d e_dma_vram. */
#define HW_A1B0   0x4304u
#define HW_BBAD0  0x4301u
#define HW_DMAP0  0x4300u
#define HW_A1T0L  0x4302u
#define HW_DAS0L  0x4305u
#define HW_VMAIN  0x2115u
#define HW_VMADDL 0x2116u
#define HW_MDMAEN 0x420Bu

static void e8(uint8_t *c, size_t *p, uint8_t b) { c[(*p)++] = b; }
static void e16(uint8_t *c, size_t *p, uint16_t w) { c[(*p)++] = (uint8_t)w; c[(*p)++] = (uint8_t)(w >> 8); }
/* lda #imm8 ; sta abs16   (A 8-bit) */
static void e_lda_sta8(uint8_t *c, size_t *p, uint8_t imm, uint16_t addr) {
    e8(c, p, 0xA9); e8(c, p, imm); e8(c, p, 0x8D); e16(c, p, addr);
}
/* lda #imm16 ; sta abs16  (A 16-bit) */
static void e_lda_sta16(uint8_t *c, size_t *p, uint16_t imm, uint16_t addr) {
    e8(c, p, 0xA9); e16(c, p, imm); e8(c, p, 0x8D); e16(c, p, addr);
}
/* One baked VRAM DMA slot: src (offset in bank $C0) -> VRAM word `vword`,
 * `size` bytes, VMADD set + word-increment. A16 for the 16-bit fields. */
static void e_dma_vram_slot(uint8_t *c, size_t *p, uint16_t src, uint16_t size, uint16_t vword) {
    e_lda_sta8(c, p, 0x18, HW_BBAD0);           /* B-bus = VMDATAL           */
    e_lda_sta8(c, p, 0x01, HW_DMAP0);           /* 2-reg lo/hi, A increments */
    e8(c, p, 0xC2); e8(c, p, 0x20);             /* rep #$20 (A16)            */
    e_lda_sta16(c, p, src, HW_A1T0L);           /* A-bus source              */
    e_lda_sta16(c, p, size, HW_DAS0L);          /* byte count                */
    e8(c, p, 0xE2); e8(c, p, 0x20);             /* sep #$20 (A8)             */
    e_lda_sta8(c, p, 0x80, HW_VMAIN);           /* word incr after high byte */
    e8(c, p, 0xC2); e8(c, p, 0x20);             /* rep #$20                  */
    e_lda_sta16(c, p, vword, HW_VMADDL);        /* VRAM word dst             */
    e8(c, p, 0xE2); e8(c, p, 0x20);             /* sep #$20                  */
    e_lda_sta8(c, p, 0x01, HW_MDMAEN);          /* fire channel 0            */
}
/* Emit the whole DMA body into buffer `base`: A-bus bank once, the tilemap
 * slot, then RTL. Returns the byte length. */
static size_t hx_emit_dma_body(uint32_t base, uint32_t tmap_src) {
    uint8_t *c = &g_window[base + HX_OFF_DMABODY];
    size_t p = 0;
    e_lda_sta8(c, &p, 0xC0u, HW_A1B0);                /* A-bus bank = $C0 (once) */
    e_dma_vram_slot(c, &p, (uint16_t)tmap_src, 2048u, 0x0000u);  /* tilemap -> VRAM word 0 */
    e8(c, &p, 0x6B);                                  /* rtl */
    return p;
}

/* ============================ FMV static-frame slice ================== */
/* B3 first slice: a synthetic 240x208 4bpp frame lives in host RAM (the PSRAM
 * analog); each NMI the DLL copies ONE subframe band into the window staging
 * region and emits a DMA body for it, so BRAM only ever holds one band. 4
 * bands = one video frame (15 fps cadence). Static: the same frame is
 * re-delivered forever, so the picture holds while the steady-state
 * PSRAM->window->VRAM pipeline runs continuously. */

/* window staging offsets within a buffer (the emitted body bakes these) */
#define FMV_OFF_CGRAM      0x0400u   /* 16 colors x2 = 32 B (band 0 only)   */
#define FMV_OFF_TILEMAP    0x0500u   /* 32x32 map words = 2048 B (band 0)   */
#define FMV_OFF_CHR        0x0D00u   /* the band's CHR chunk (<= 7040 B)    */

/* FMV VRAM homes (words) + BG regs. Real .fmv changes CGRAM + tilemap (palette
 * bits) every frame, so BOTH are double-buffered alongside CHR; band 3 flips
 * BG1SC + BG12NBA atomically (BG1SC tilemap base is in 1024-WORD units). */
#define FMV_CHR_A_VWORD    0x0000u   /* CHR base A (word)                   */
#define FMV_CHR_B_VWORD    0x4000u   /* CHR base B (word)                   */
#define FMV_TM_A_VWORD     0x7C00u   /* tilemap base A (word)               */
#define FMV_TM_B_VWORD     0x7800u   /* tilemap base B (word)               */
#define FMV_BG1SC_A        0x7Cu     /* ($7C00>>10)<<2 = 31<<2, 32x32 map   */
#define FMV_BG1SC_B        0x78u     /* ($7800>>10)<<2 = 30<<2, 32x32 map   */
#define FMV_BG12NBA_A      0x00u     /* BG1 CHR base $0000 ($0000/$1000=0)  */
#define FMV_BG12NBA_B      0x04u     /* BG1 CHR base $4000 ($4000/$1000=4)  */

/* real .fmv (FMV2) layout: unit = [audio abytes][CGRAM 256][tilemap 1560][CHR 24960] */
#define FMV_CGRAM_BYTES    256u      /* 8 palettes x 16 colours             */
#define FMV_TM_SRC_BYTES   (30u * 26u * 2u)   /* 1560: raw 30x26 map words  */
#define FMV_CHR_SRC_BYTES  (FMV_TILES * 32u)  /* 24960: 780 tiles           */
#define FMV_BLOCK          (FMV_CGRAM_BYTES + FMV_TM_SRC_BYTES + FMV_CHR_SRC_BYTES) /* 26776 */
#define FMV_UNIT_MAX       (12000u + FMV_BLOCK)   /* audio(<=11760) + video */

/* letterbox: 208 visible lines centered in 224 (8 top + 8 bottom) */
#define FMV_TOP_LB   8
#define FMV_VIS_END  (224 - 8)       /* 216 */

/* geometry: 240x208 = 30x26 tiles = 780 tiles, split into 4 bands like
 * microgarbage (each chunk under the ~7.4 KB vblank burst budget). */
#define FMV_TILES       780                  /* animated content tiles (30x26) */
#define FMV_BLANK_TILE  FMV_TILES            /* tile 780 = permanent black blank */
/* band split: content 0..779 plus the blank tile 780 in the last band, each
 * chunk under the ~7.4 KB vblank burst budget */
/* Split balanced against the ~7452 B vblank burst budget (8/8 letterbox) with
 * the sprite overlay AND the tilemap spread 4 ways (512 B/band instead of the
 * whole 2 KB choking band 0 — the back tilemap isn't displayed until the
 * band-3 flip, so it can be filled incrementally):
 *   b0 205t 6560 + TM 512 + OAM 256                           = 7328
 *   b1 184t 5888 + TM 512 + OBJ CHR 384 + pal 256 + OAM 256   = 7296
 *   b2 205t 6560 + TM 512 + OAM 256                           = 7328
 *   b3 187t 5984 + TM 512 + CGRAM 256 + OAM 544               = 7296
 * Sum 781 tiles (780 content + blank). Spare is now ~124 B on EVERY band
 * rather than pooled on one — that even spread is what leaves room for the
 * FFT overlay's tilemap updates.
 * NOTE: our kernel has no cycle-budgeted chainer (the emitter replaced it), so
 * an overrun is NOT deferred — it writes during active display and is visible.
 * Budget discipline is entirely here. */
static const int fmv_band_tiles[4] = { 205, 184, 205, 187 };   /* sum = 781 */

/* the PSRAM-analog frame store (host RAM): the CURRENT decoded frame. Only a
 * subframe band of this is copied into the window (BRAM) per NMI. */
static uint8_t  fmv_cgram[FMV_CGRAM_BYTES];         /* 128 colours, BGR555 */
static uint8_t  fmv_tilemap[2048];                  /* 32x32 map words     */
static uint8_t  fmv_chr[(FMV_TILES + 1) * 32];      /* 780 content + blank */
static int      fmv_generated;                      /* one-shot gen guard  */

/* real .fmv streaming (the file is the SD analog; we read one unit per frame
 * into fmv_unit = the PSRAM analog, never resident whole). NULL file => the
 * synthetic scrolling source. */
static FILE            *fmv_file;
static long             fmv_data_off;               /* 32: first unit offset */
static uint32_t         fmv_nframes;
static uint32_t         fmv_abytes;                 /* audio bytes / frame   */
static uint32_t         fmv_unit_bytes;             /* abytes + FMV_BLOCK    */
static uint32_t         fmv_cur_frame;
static AudioVoiceHandle fmv_audio_voice;            /* FMV PCM music voice   */

/* one baked CGRAM DMA slot: src -> CGRAM starting at cgadd, `size` bytes */
static void e_dma_cgram_slot(uint8_t *c, size_t *p, uint16_t src, uint16_t size, uint8_t cgadd) {
    e_lda_sta8(c, p, cgadd, 0x2121u);           /* CGADD start index    */
    e_lda_sta8(c, p, 0x22, HW_BBAD0);           /* B-bus = CGDATA       */
    e_lda_sta8(c, p, 0x00, HW_DMAP0);           /* 1 reg, A increments  */
    e8(c, p, 0xC2); e8(c, p, 0x20);             /* rep #$20             */
    e_lda_sta16(c, p, src, HW_A1T0L);
    e_lda_sta16(c, p, size, HW_DAS0L);
    e8(c, p, 0xE2); e8(c, p, 0x20);             /* sep #$20             */
    e_lda_sta8(c, p, 0x01, HW_MDMAEN);          /* fire                 */
}

/* One baked OAM DMA slot: src -> OAM from word `oamaddr` (0 = sprite 0),
 * `size` bytes. OAMADDL/H is a WORD address, so the low table is words
 * 0..255 and the high table starts at word 256. */
static void e_dma_oam_slot(uint8_t *c, size_t *p, uint16_t src, uint16_t size,
                           uint16_t oamaddr) {
    e8(c, p, 0xC2); e8(c, p, 0x20);             /* rep #$20             */
    e_lda_sta16(c, p, oamaddr, 0x2102u);        /* OAMADDL/H            */
    e8(c, p, 0xE2); e8(c, p, 0x20);             /* sep #$20             */
    e_lda_sta8(c, p, 0x04, HW_BBAD0);           /* B-bus = OAMDATA      */
    e_lda_sta8(c, p, 0x00, HW_DMAP0);           /* 1 reg, A increments  */
    e8(c, p, 0xC2); e8(c, p, 0x20);             /* rep #$20             */
    e_lda_sta16(c, p, src, HW_A1T0L);
    e_lda_sta16(c, p, size, HW_DAS0L);
    e8(c, p, 0xE2); e8(c, p, 0x20);             /* sep #$20             */
    e_lda_sta8(c, p, 0x01, HW_MDMAEN);          /* fire                 */
}

/* BG1 scroll registers. Both are WRITE-TWICE (low byte then high byte).
 *
 * SNES quirk: the first displayed scanline shows BG line BGnVOFS+1, so VOFS
 * must be -1 (0x3FF in the 10-bit field) for screen line N to show BG line N.
 * Left at 0 the entire image sits one line high: our content (BG lines 8..215)
 * renders on screen 7..214, so the top content row hides under the letterbox
 * and the last screen line (215) shows blank BG past the content — the picture
 * appears to end a line early. These were never initialised at all before. */
static void e_bg_scroll(uint8_t *c, size_t *p) {
    e_lda_sta8(c, p, 0x00, 0x210Du);            /* BG1HOFS low  = 0     */
    e8(c, p, 0x8D); e16(c, p, 0x210Du);         /* BG1HOFS high = 0 (A still 0) */
    e_lda_sta8(c, p, 0xFF, 0x210Eu);            /* BG1VOFS low  = 0xFF  */
    e_lda_sta8(c, p, 0x03, 0x210Eu);            /* BG1VOFS high = 0x03 -> -1 */
}

/* emit BG setup (mode 1, BG1 + OBJ on main screen). BG1SC (tilemap base) and
 * BG12NBA (CHR base) are set per band — they select/flip the A/B double
 * buffers. OBSEL is static (OBJ CHR base 0x6000, 8/16 size pair). */
static void e_bg_setup(uint8_t *c, size_t *p) {
    e_lda_sta8(c, p, 0x01, 0x2105u);            /* BGMODE = mode 1      */
    e_lda_sta8(c, p, HX_SPR_OBSEL, 0x2101u);    /* OBSEL: OBJ CHR base  */
    e_lda_sta8(c, p, 0x11, 0x212Cu);            /* TM: BG1 + OBJ on main*/
    e_bg_scroll(c, p);                          /* HOFS 0, VOFS -1      */
}

/* ============ sprite overlay ("film critic": cursor + holes + FFT) ========
 * SHARED window regions above both frame buffers (buffers end at 0x5FFF; the
 * kernel image lives at $8000+; strobes are at 0x7800/0x79C1) — one live copy,
 * not double-buffered: OAM/CHR/CGRAM are pushed to the PPU every frame anyway.
 *
 * VRAM: all 12 OBJ tiles sit CONTIGUOUSLY at word 28880 (OBSEL base 0x6000 +
 * tile 269*16) and upload as ONE slot on an EARLY band — a separate late slot
 * risks being starved when the burst runs long.
 *
 * GOTCHA (from mgapi): word 28864 (tile 268) is AVOIDED. At BG1 CHR base
 * 0x4000 the FMV's BLANK_TILE (780) aliases to that same word, so sprite CHR
 * there paints the crosshair into the FMV letterbox on alternate frames. BG1
 * never references past tile 780, so 28880+ is genuinely free. */
static int g_spr_cx = 124, g_spr_cy = 100;   /* cursor position (screen px)  */

/* Cursor travel limits (inclusive, sprite top-left in screen px). Defaults put
 * the 8x8 cursor exactly inside the FMV picture: content is BG rows 1..26 =
 * scanlines 8..215, and a sprite at Y=n renders on n..n+7, so 8..208 reaches
 * both borders. Verified against the clip itself with tools/hx421_fmv_bars.c —
 * movie.fmv has NO baked-in letterbox (minimum black rows 0 top and bottom),
 * so the frame's own border is the only limit. Overridable at runtime because
 * display overscan can crop a line or two of what's nominally visible. */
static int g_spr_clamp_l = 8, g_spr_clamp_r = 240;
static int g_spr_clamp_t = 8, g_spr_clamp_b = 208;

/* OAM allocation. Sprite-vs-sprite layering is by OAM INDEX (lowest = front),
 * NOT the priority bits — so the order here IS the draw order:
 *   0        cursor        (in front of everything)
 *   1..32    FFT bars      (8 bands x [cap, 3 fill rows]) — over the holes
 *   33..48   bullet holes  (16, FIFO)
 * 49 of the 64 sprites the 256-byte low-table push covers. */
#define HX_FFT_SPR0        1u
#define HX_SPR_POOL_FIRST 33u
#define HX_SPR_POOL_LAST  48u
static struct { uint8_t x, y, pal, active; } g_spr_hole[64];   /* [33..48] used */

/* --- FFT spectrum bars: 8 bands, 8 px wide, up to 24 px tall, green->yellow->
 * red by row, with falling peak caps. Placed with a 4 px border off the FMV
 * picture's left and bottom edges (picture is x 8..247, y 8..215):
 *   bars x 12..75, bottom row y 204..211, top row y 188..195. */
#define HX_FFT_BANDS   8
#define HX_FFT_ROWS    3
#define HX_FFT_MAXH   (HX_FFT_ROWS * 8)     /* 24 px */
#define HX_FFT_BASE_X 12u                   /* leftmost bar X (4 px border)  */
#define HX_FFT_BOT_Y  204u                  /* Y of the BOTTOM row's sprite  */
#define HX_FFT_CAP_Y  212u                  /* one past the bars' last line  */
#define HX_FFT_FLOOR  16                    /* noise floor before scaling    */
static uint8_t g_fft_lvl[HX_FFT_BANDS];     /* smoothed band level 0..255    */
static uint8_t g_fft_h[HX_FFT_BANDS];       /* bar height 0..24 px           */
static uint8_t g_fft_peak[HX_FFT_BANDS];    /* peak-hold height, falls 1px/f */
/* Per-band gain (x256) compensating the natural bass->treble rolloff, so the
 * high bands aren't pinned at 1 px. Same curve as the reference. */
static const uint16_t g_fft_gain[HX_FFT_BANDS] = { 40, 39, 38, 45, 51, 59, 96, 256 };
static unsigned        g_spr_next_hole = HX_SPR_POOL_FIRST;
static unsigned        g_spr_prev_left;        /* left-button edge detect  */
static int             g_spr_cursor_hidden;    /* hidden while right held  */
static uint32_t        g_spr_rng = 0x1234567u; /* LCG for palette variety  */
static AudioObjHandle  g_spr_bullet;           /* gunshot SFX (0 = none)   */

/* 8-row 1-bit mask -> one 8x8 4bpp tile: set bit = colour index 1, clear = 0
 * (transparent). Plane 0 carries the mask; planes 1-3 stay zero. */
static void spr_build_tile_idx1(uint8_t tile[32], const uint8_t mask[8]) {
    memset(tile, 0, 32);
    for (int y = 0; y < 8; ++y) tile[2 * y] = mask[y];
}
/* Two-tone tile: pixels inside `inner` get colour index 2, pixels in `outer`
 * but not `inner` get index 1, everything else transparent. Index bits come
 * from the bitplanes, so plane 0 carries the rim and plane 1 the centre. */
static void spr_build_tile_idx12(uint8_t tile[32], const uint8_t outer[8],
                                 const uint8_t inner[8]) {
    memset(tile, 0, 32);
    for (int y = 0; y < 8; ++y) {
        tile[2 * y]     = (uint8_t)(outer[y] & (uint8_t)~inner[y]);  /* -> index 1 */
        tile[2 * y + 1] = (uint8_t)(outer[y] & inner[y]);            /* -> index 2 */
    }
}

/* write one BGR555 palette entry as explicit LE bytes (portable, no aliasing) */
static void spr_pal_entry(uint8_t *base, int pal, int idx, int r, int g, int b) {
    uint16_t v = (uint16_t)(((b >> 3) << 10) | ((g >> 3) << 5) | (r >> 3));
    base[(pal * 16 + idx) * 2 + 0] = (uint8_t)v;
    base[(pal * 16 + idx) * 2 + 1] = (uint8_t)(v >> 8);
}

/* Build the static OBJ assets into the window ONCE (FMV start). */
static void hx_spr_build_assets(void) {
    static const uint8_t cursor[8]     = { 0x18,0x18,0x18,0xFF,0xFF,0x18,0x18,0x18 };
    /* Bullet hole: bright scorched RIM around a dark centre. The rim is what
     * keeps it visible over black areas of the video; the dark centre is what
     * makes it read as a hole rather than a dot. */
    static const uint8_t hole_out[8]   = { 0x3C,0x7E,0xFF,0xFF,0xFF,0xFF,0x7E,0x3C };
    static const uint8_t hole_in[8]    = { 0x00,0x3C,0x7E,0x7E,0x7E,0x7E,0x3C,0x00 };
    uint8_t *chr = &g_window[HX_OFF_SPR_CHR];
    memset(chr, 0, HX_SPR_CHR_BYTES);
    spr_build_tile_idx1(chr + 0 * 32, cursor);                    /* tile 269 */
    spr_build_tile_idx12(chr + 1 * 32, hole_out, hole_in);        /* tile 270 */
    for (int n = 0; n <= 8; ++n) {                        /* 271..279 fill 0..8 */
        uint8_t mask[8];
        for (int row = 0; row < 8; ++row)
            mask[row] = (uint8_t)((row >= 8 - n) ? 0xFF : 0x00);
        spr_build_tile_idx1(chr + (2 + n) * 32, mask);
    }
    { uint8_t cap[8] = { 0xFF, 0xFF, 0, 0, 0, 0, 0, 0 };  /* tile 280: 2px cap */
      spr_build_tile_idx1(chr + 11 * 32, cap); }

    uint8_t *cg = &g_window[HX_OFF_SPR_CGRAM];            /* OBJ pal 0-3 */
    memset(cg, 0, HX_SPR_CGRAM_BYTES);
    spr_pal_entry(cg, 0, 1,  64, 255,  96);               /* cursor: green   */
    /* Hole tints, picked at random per shot: entry 1 = rim, entry 2 = centre.
     * Rims carry the luminance so a hole never vanishes over black frames
     * (the earlier flat near-black tints did); centres stay dark so it reads
     * as a puncture rather than a sticker. */
    spr_pal_entry(cg, 1, 1, 196,  96,  56);  spr_pal_entry(cg, 1, 2,  60, 24, 16); /* rust  */
    spr_pal_entry(cg, 2, 1, 170, 126,  76);  spr_pal_entry(cg, 2, 2,  52, 34, 20); /* brown */
    spr_pal_entry(cg, 3, 1, 176,  60,  46);  spr_pal_entry(cg, 3, 2,  55, 16, 14); /* red   */

    uint8_t *cf = &g_window[HX_OFF_SPR_PAL47];            /* OBJ pal 4-7 */
    memset(cf, 0, HX_SPR_CGRAM_BYTES);
    spr_pal_entry(cf, 0, 1, 255, 255, 255);               /* pal4: peak cap  */
    spr_pal_entry(cf, 1, 1,  40, 230,  60);               /* pal5: green     */
    spr_pal_entry(cf, 2, 1, 240, 220,  40);               /* pal6: yellow    */
    spr_pal_entry(cf, 3, 1, 240,  60,  40);               /* pal7: red       */
}

/* Rebuild the live OAM. High table stays ALL ZERO (every sprite 8x8, X<256),
 * which is what makes the 256-byte low-only pushes self-consistent between
 * the once-per-frame full 544-byte push. */
static void hx_spr_write_oam(void) {
    uint8_t *oam = &g_window[HX_OFF_SPR_OAM];
    memset(oam, 0, HX_SPR_OAM_BYTES);
    for (unsigned i = 0; i < 128; ++i) oam[i * 4 + 1] = 240u;   /* park offscreen */

    if (!g_spr_cursor_hidden) {                            /* sprite 0 = cursor */
        oam[0] = (uint8_t)g_spr_cx;
        oam[1] = (uint8_t)g_spr_cy;
        oam[2] = (uint8_t)(HX_SPR_TILE_CURSOR & 0xFFu);
        oam[3] = (uint8_t)((3u << 4) | (0u << 1) | ((HX_SPR_TILE_CURSOR >> 8) & 1u));
    }
    /* FFT bars, sprites 1..32. Within a band the CAP takes the lowest index so
     * it draws in front of the fill rows it overlaps — again, index is what
     * decides sprite-vs-sprite layering, not the priority bits. */
    for (int b = 0; b < HX_FFT_BANDS; ++b) {
        uint8_t  bx   = (uint8_t)(HX_FFT_BASE_X + (unsigned)b * 8u);
        unsigned base = HX_FFT_SPR0 + (unsigned)b * 4u;

        unsigned cs = base;                                /* peak cap */
        oam[cs * 4 + 0] = bx;
        oam[cs * 4 + 1] = (uint8_t)(HX_FFT_CAP_Y - g_fft_peak[b] - 2u);
        oam[cs * 4 + 2] = (uint8_t)(HX_SPR_TILE_CAP & 0xFFu);
        oam[cs * 4 + 3] = (uint8_t)((3u << 4) | (4u << 1)   /* prio 3, pal 4 white */
                                  | ((HX_SPR_TILE_CAP >> 8) & 1u));

        for (int r = 0; r < HX_FFT_ROWS; ++r) {            /* fill rows, bottom-up */
            int fill = (int)g_fft_h[b] - r * 8;
            if (fill < 0) fill = 0;
            if (fill > 8) fill = 8;
            unsigned s    = base + 1u + (unsigned)r;
            uint16_t tile = (uint16_t)(HX_SPR_FFT_TILE0 + fill);
            oam[s * 4 + 0] = bx;
            oam[s * 4 + 1] = (fill > 0) ? (uint8_t)(HX_FFT_BOT_Y - (unsigned)r * 8u)
                                        : 240u;            /* empty row: hide */
            oam[s * 4 + 2] = (uint8_t)(tile & 0xFFu);
            oam[s * 4 + 3] = (uint8_t)((2u << 4)            /* pal 5/6/7 = G/Y/R */
                                     | ((5u + (unsigned)r) << 1)
                                     | ((tile >> 8) & 1u));
        }
    }

    for (unsigned i = HX_SPR_POOL_FIRST; i <= HX_SPR_POOL_LAST; ++i) {
        if (!g_spr_hole[i].active) continue;               /* 33..48 = holes */
        oam[i * 4 + 0] = g_spr_hole[i].x;
        oam[i * 4 + 1] = g_spr_hole[i].y;
        oam[i * 4 + 2] = (uint8_t)(HX_SPR_TILE_HOLE & 0xFFu);
        oam[i * 4 + 3] = (uint8_t)((2u << 4)                      /* prio 2 */
                                 | ((g_spr_hole[i].pal & 7u) << 1)
                                 | ((HX_SPR_TILE_HOLE >> 8) & 1u));
    }
}

/* Sample the live band meter (it runs over the final mixed output, so it tracks
 * the movie's own audio), fold 16 host bands to 8 by max-of-pairs, and smooth:
 * instant attack, 1/4 decay for a natural meter fall. Then derive bar heights
 * and advance the peak holds (1 px per frame). */
static void hx_fft_tick(void) {
    uint32_t raw[16];
    uint32_t n = hxa_fft_bands(g_svc, raw, 16);
    for (int i = 0; i < HX_FFT_BANDS; ++i) {
        uint32_t a = ((uint32_t)(2 * i)     < n) ? raw[2 * i]     : 0u;
        uint32_t c = ((uint32_t)(2 * i + 1) < n) ? raw[2 * i + 1] : 0u;
        uint32_t v = (a > c) ? a : c;
        if (v > 255u) v = 255u;
        if (v >= g_fft_lvl[i]) g_fft_lvl[i] = (uint8_t)v;                        /* attack */
        else g_fft_lvl[i] = (uint8_t)(g_fft_lvl[i] - ((g_fft_lvl[i] - v) >> 2)); /* decay  */

        int hv = (int)g_fft_lvl[i] - HX_FFT_FLOOR;
        if (hv < 0) hv = 0;
        int h = (hv * (int)g_fft_gain[i]) >> 8;
        if (h > HX_FFT_MAXH) h = HX_FFT_MAXH;
        g_fft_h[i] = (uint8_t)h;

        if (h >= (int)g_fft_peak[i]) g_fft_peak[i] = (uint8_t)h;   /* jump to peak */
        else if (g_fft_peak[i] > 0)  g_fft_peak[i]--;              /* fall 1 px/f  */
    }
}

/* Once per SNES frame: drain the port-2 mouse mailbox into the cursor, clamp
 * to screen, rebuild OAM. The embedder (hx421_chip.cpp) clocks the Mouse's
 * 32-bit report and calls hx421_post_mouse, which ACCUMULATES deltas; this is
 * the ONLY consumer — a second one would steal motion, since draining resets. */
static void hx_spr_tick(void) {
    hx_fft_tick();
    int dx = g_mouse_dx, dy = g_mouse_dy;
    g_mouse_dx = 0; g_mouse_dy = 0;                        /* drain */

    g_spr_cx += dx;
    g_spr_cy += dy;
    if (g_spr_cx < g_spr_clamp_l) g_spr_cx = g_spr_clamp_l;
    if (g_spr_cx > g_spr_clamp_r) g_spr_cx = g_spr_clamp_r;
    if (g_spr_cy < g_spr_clamp_t) g_spr_cy = g_spr_clamp_t;
    if (g_spr_cy > g_spr_clamp_b) g_spr_cy = g_spr_clamp_b;

    unsigned left  = g_mouse_btn & 1u;
    unsigned right = (g_mouse_btn >> 1) & 1u;

    if (right) {                                  /* clear the pool while held */
        for (unsigned i = HX_SPR_POOL_FIRST; i <= HX_SPR_POOL_LAST; ++i)
            g_spr_hole[i].active = 0u;
        g_spr_next_hole = HX_SPR_POOL_FIRST;
    }
    g_spr_cursor_hidden = (int)right;             /* and hide the crosshair    */

    if (left && !g_spr_prev_left) {               /* PRESS EDGE -> shoot       */
        g_spr_rng = g_spr_rng * 1664525u + 1013904223u;
        unsigned h = g_spr_next_hole;
        g_spr_hole[h].x      = (uint8_t)g_spr_cx;
        g_spr_hole[h].y      = (uint8_t)g_spr_cy;
        g_spr_hole[h].pal    = (uint8_t)(1u + ((g_spr_rng >> 28) % 3u));  /* pal 1..3 */
        g_spr_hole[h].active = 1u;
        g_spr_next_hole = (h >= HX_SPR_POOL_LAST) ? HX_SPR_POOL_FIRST : (h + 1u);

        /* Gunshot on its own voice, panned by where on screen the shot landed,
         * so rapid clicks layer over each other and over the FMV audio. */
        if (g_spr_bullet) {
            int32_t pan = ((int32_t)g_spr_cx - 124) * 280;   /* 8..240 -> ~+-32k */
            if (pan >  32767) pan =  32767;
            if (pan < -32767) pan = -32767;
            (void)hxa_trigger_sfx(g_svc, g_spr_bullet, 0x6000, pan);
        }
    }
    g_spr_prev_left = left;

    hx_spr_write_oam();
}

/* encode one 8x8 4bpp tile at content origin (x0,y0); `phase` scrolls the
 * diagonal colour bands for animation (proves per-frame playback). */
static void fmv_encode_tile(uint8_t *chr, int x0, int y0, int phase) {
    for (int py = 0; py < 8; ++py) {
        uint8_t pl0 = 0, pl1 = 0, pl2 = 0, pl3 = 0;
        for (int px = 0; px < 8; ++px) {
            int x = x0 + px, y = y0 + py;
            int color = ((x + y + phase) >> 3) & 15;     /* scrolling bands */
            int bit = 7 - px;                            /* px0 = MSB       */
            if (color & 1) pl0 |= (uint8_t)(1 << bit);
            if (color & 2) pl1 |= (uint8_t)(1 << bit);
            if (color & 4) pl2 |= (uint8_t)(1 << bit);
            if (color & 8) pl3 |= (uint8_t)(1 << bit);
        }
        chr[2 * py]      = pl0;   chr[2 * py + 1]      = pl1;   /* planes 0/1 */
        chr[16 + 2 * py] = pl2;   chr[16 + 2 * py + 1] = pl3;   /* planes 2/3 */
    }
}

/* static parts (CGRAM palette + tilemap layout) — built once. The tilemap is
 * position-linear (tile idx = row*30+col) and shared by both CHR buffers, so
 * only the CHR changes per frame. */
static void fmv_gen_static(void) {
    if (fmv_generated) return;
    fmv_generated = 1;
    /* CGRAM: colour 0 = BLACK backdrop (the letterbox + the centring margins),
     * colours 1..15 = a blue -> green -> red ramp. (Synthetic uses palette 0
     * only; the rest of the 128-colour bank stays 0.) */
    memset(fmv_cgram, 0, sizeof fmv_cgram);
    for (int i = 0; i < 16; ++i) {
        int r = 0, g = 0, b = 0;
        if (i > 0) {
            if (i < 8) { b = 31 - i * 3; g = i * 4;            r = 0; }
            else       { b = 0;          g = 31 - (i - 8) * 4; r = (i - 8) * 4; }
            if (r < 0) r = 0;
            if (g < 0) g = 0;
            if (b < 0) b = 0;
        }
        uint16_t col = (uint16_t)((b << 10) | (g << 5) | r);
        fmv_cgram[2 * i]     = (uint8_t)col;
        fmv_cgram[2 * i + 1] = (uint8_t)(col >> 8);
    }
    /* tilemap: fill every cell with the BLANK tile (permanent black), then lay
     * the 30x26 (240x208) content over it, CENTRED -- shifted +1 tile both axes
     * (8px) so it sits centred in 256px and under the 8px top letterbox. The
     * blank tile never animates, so the borders stay solid black. */
    for (int i = 0; i < 32 * 32; ++i) {
        fmv_tilemap[i * 2]     = (uint8_t)(FMV_BLANK_TILE & 0xFF);
        fmv_tilemap[i * 2 + 1] = (uint8_t)((FMV_BLANK_TILE >> 8) & 0x03);
    }
    for (int row = 0; row < 26; ++row) {
        for (int col = 0; col < 30; ++col) {
            int t = row * 30 + col;
            int cell = ((row + 1) * 32 + (col + 1)) * 2;   /* +1 tile each axis */
            fmv_tilemap[cell]     = (uint8_t)(t & 0xFF);
            fmv_tilemap[cell + 1] = (uint8_t)((t >> 8) & 0x03);  /* 10-bit tile */
        }
    }
    /* the blank tile's CHR is all zeros = colour 0 = black (never regenerated) */
    memset(&fmv_chr[FMV_BLANK_TILE * 32], 0, 32);
}

/* regenerate the frame's CHR into the PSRAM-analog store for the given
 * animation phase (stand-in for decoding a real .fmv frame into PSRAM). */
static void fmv_gen_chr(int phase) {
    for (int t = 0; t < FMV_TILES; ++t) {
        int col = t % 30, row = t / 30;
        fmv_encode_tile(&fmv_chr[t * 32], col * 8, row * 8, phase);
    }
}

static uint32_t rd_u32le(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* open + validate the .fmv (FMV2) file named by HX421_FMV_FILE. On any failure
 * fmv_file stays NULL and the pipeline falls back to the synthetic source. */
static void fmv_file_open(void) {
    const char *path = getenv("HX421_FMV_FILE");
    if (!path || !path[0]) return;
    fmv_file = fopen(path, "rb");
    if (!fmv_file) { fprintf(stderr, "hx421 fmv: cannot open '%s' — synthetic source\n", path); return; }
    uint8_t hdr[32];
    if (fread(hdr, 1, 32, fmv_file) != 32 || memcmp(hdr, "FMV2", 4) != 0) {
        fprintf(stderr, "hx421 fmv: '%s' is not an FMV2 file — synthetic source\n", path);
        fclose(fmv_file); fmv_file = NULL; return;
    }
    fmv_nframes    = rd_u32le(hdr + 12);
    fmv_abytes     = rd_u32le(hdr + 24);
    fmv_unit_bytes = fmv_abytes + FMV_BLOCK;
    fmv_data_off   = 32;
    fmv_cur_frame  = 0;
    if (fmv_unit_bytes > FMV_UNIT_MAX) {           /* sanity vs the static buffer */
        fprintf(stderr, "hx421 fmv: unit %u > max %u — synthetic source\n",
                fmv_unit_bytes, (unsigned)FMV_UNIT_MAX);
        fclose(fmv_file); fmv_file = NULL; return;
    }
    fprintf(stderr, "hx421 fmv: streaming '%s' — %u frames, %u audio B/frame, unit %u B\n",
            path, fmv_nframes, fmv_abytes, fmv_unit_bytes);
    fflush(stderr);
}

/* WASAPI audio-output seam — defined in the audio-pull section below. */
static void hx421_fmv_feed_audio(const int16_t *stereo, uint32_t frames);
static void hx421_audio_worker_start(void);
static void hx421_audio_worker_stop(void);
static void hx421_audio_lock_init(void);
static void hx_produce_fmv_band(void);   /* FMV producer (called from the FRAME_DONE handler) */

/* read the next unit from the file into `dst` (loops at EOF). I/O side — runs
 * on the read-ahead thread (or the caller on the non-threaded fallback). */
static int fmv_read_next_unit(uint8_t *dst) {
    if (fmv_cur_frame >= fmv_nframes ||
        fread(dst, 1, fmv_unit_bytes, fmv_file) != fmv_unit_bytes) {
        fseek(fmv_file, fmv_data_off, SEEK_SET);   /* loop */
        fmv_cur_frame = 0;
        if (fread(dst, 1, fmv_unit_bytes, fmv_file) != fmv_unit_bytes) return 0;
    }
    fmv_cur_frame++;
    return 1;
}

/* ==================== FMV frame FIFO (stream-arbiter core) =============
 * A worker thread decodes COMPLETE frames a few AHEAD into a FIFO; the consumer
 * (FRAME_DONE) pops one per video frame, pushes its audio, and stages its bands.
 * Every buffer is a named depth (docs/stream-arbiter.md). The old raw-unit ring
 * + per-band decode is subsumed; A/V instrumentation logs the frame indices so
 * the offset is a MEASURED number. */

/* one fully decoded, cart-ready frame */
typedef struct {
    uint8_t  cgram[FMV_CGRAM_BYTES];        /* 256              */
    uint8_t  tilemap[2048];                 /* splatted 32x32   */
    uint8_t  chr[(FMV_TILES + 1) * 32];     /* 781 tiles        */
    int16_t  audio[6000];                   /* <= 3000 stereo frames (abytes/4) */
    uint32_t audio_frames;                  /* = abytes / 4     */
} FmvFrame;

static uint64_t  g_fmv_disp;                /* video frames released (A/V instrument)   */
static uint64_t  g_av_pull_base;            /* g_pull_frames at the first video frame   */
static uint64_t  g_av_rend_base;            /* g_audio_rendered at the first video frame */
static uint64_t  g_audio_rendered;          /* frames rendered by the worker (audio clock) */
static int       g_fmv_synctest;            /* HX421_FMV_SYNCTEST=1: flash+click probe  */
static volatile int g_fmv_av_started;       /* 1 after the first FRAME_DONE: audio flows */
static uint64_t  g_fmv_dec;                 /* frames DECODED (worker side)             */
static unsigned  g_fmv_lead = 8;            /* AUDIO_LEAD: frames audio runs ahead of   */
                                            /* video = FIFO depth = video-hold preroll  */

/* copy a decoded frame into the current-frame globals (which the band staging +
 * synthetic generator already use). VIDEO ONLY — the audio for this frame was
 * already pushed by the decode worker, AUDIO_LEAD frames ago. That lead is what
 * cancels the output-pipe latency; pushing here instead would emit audio and
 * video simultaneously and leave the downstream latency uncompensated. */
static void fmv_use_frame(const FmvFrame *f) {
    memcpy(fmv_cgram,   f->cgram,   FMV_CGRAM_BYTES);
    memcpy(fmv_tilemap, f->tilemap, sizeof fmv_tilemap);
    memcpy(fmv_chr,     f->chr,     (FMV_TILES + 1) * 32);
    if (g_fmv_disp == 0) {                                 /* A/V baseline at frame 0 */
        g_av_pull_base = g_pull_frames;
        g_av_rend_base = g_audio_rendered;
    }
    g_fmv_disp++;
}

/* decode one file unit (already read into `unit`) into frame `f`. */
static void fmv_decode_unit_into(FmvFrame *f, const uint8_t *unit) {
    const uint8_t *cg  = unit + fmv_abytes;
    const uint8_t *tms = cg + FMV_CGRAM_BYTES;
    const uint8_t *chr = tms + FMV_TM_SRC_BYTES;
    memcpy(f->cgram, cg, FMV_CGRAM_BYTES);
    for (int i = 0; i < 32 * 32; ++i) {
        f->tilemap[i * 2]     = (uint8_t)(FMV_BLANK_TILE & 0xFF);
        f->tilemap[i * 2 + 1] = (uint8_t)((FMV_BLANK_TILE >> 8) & 0x03);
    }
    for (int r = 0; r < 26; ++r)
        memcpy(&f->tilemap[((r + 1) * 32 + 1) * 2], tms + r * 60, 60);
    memset(&f->chr[FMV_TILES * 32], 0, 32);          /* blank tile (colour 0) */
    memcpy(f->chr, chr, FMV_CHR_SRC_BYTES);
    uint32_t af = fmv_abytes / 4u;
    if (af > 3000u) af = 3000u;
    memcpy(f->audio, unit, (size_t)af * 4u);
    f->audio_frames = af;

    /* SYNC TEST (HX421_FMV_SYNCTEST=1): every ~2 s mark ONE frame — flash its
     * palette white AND put a loud click in that same frame's audio. They travel
     * the pipeline together, so the gap you SEE vs HEAR is the residual A/V
     * error after the lead compensation. Tune HX421_FMV_LEAD until they land
     * together. Injected at DECODE so the click rides with the audio push. */
    if (g_fmv_synctest && (g_fmv_dec % 30u) == 0u) {
        for (int i = 0; i < 128; ++i) {                    /* whole palette -> white */
            f->cgram[i * 2]     = 0xFF;
            f->cgram[i * 2 + 1] = 0x7F;
        }
        for (uint32_t s = 0; s < af && s < 600u; ++s) {    /* ~14 ms click burst */
            int16_t v = (s & 8u) ? 22000 : -22000;
            f->audio[s * 2] = v; f->audio[s * 2 + 1] = v;
        }
    }
    g_fmv_dec++;
}

#define FMV_FIFO_MAX 48u                           /* ~3.2 s of lead @ 15 fps */

#if defined(_WIN32)
#include <windows.h>
#include <stdatomic.h>
static FmvFrame     fmv_fifo[FMV_FIFO_MAX];
static atomic_uint  fmv_fifo_w, fmv_fifo_r;        /* monotonic write/read counts */
static HANDLE       fmv_reader_thread;
static volatile LONG fmv_reader_stop;

static unsigned fmv_apush;                         /* frames whose audio has been pushed */

static DWORD WINAPI fmv_reader_fn(LPVOID arg) {
    (void)arg;
    static uint8_t unit[FMV_UNIT_MAX];             /* worker-local read scratch */
    while (!fmv_reader_stop) {
        /* Audio is HELD until the SNES strobes its first FRAME_DONE (g_fmv_av_started).
         * The reader runs from DLL init, but the console needs time to boot — without
         * this gate the ring plays out and starves during that blank period, putting
         * audio ahead of picture before frame 0 is ever shown. Once video is live we
         * flush the decoded backlog in order, so audio starts WITH it. */
        if (g_fmv_av_started) {
            unsigned wc = atomic_load(&fmv_fifo_w);
            unsigned rc = atomic_load(&fmv_fifo_r);
            if (fmv_apush < rc) fmv_apush = rc;    /* never read a recycled slot */
            while (fmv_apush < wc) {
                FmvFrame *pf = &fmv_fifo[fmv_apush % FMV_FIFO_MAX];
                if (fmv_audio_voice != AUDIO_VOICE_NONE && pf->audio_frames)
                    hx421_fmv_feed_audio(pf->audio, pf->audio_frames);
                fmv_apush++;
            }
        }
        unsigned w = atomic_load(&fmv_fifo_w), r = atomic_load(&fmv_fifo_r);
        if (w - r >= g_fmv_lead) { Sleep(1); continue; }       /* lead satisfied */
        if (!fmv_read_next_unit(unit)) { Sleep(2); continue; }
        fmv_decode_unit_into(&fmv_fifo[w % FMV_FIFO_MAX], unit);
        atomic_store(&fmv_fifo_w, w + 1);
    }
    return 0;
}

/* advance to the next decoded frame: copy it OUT of the FIFO (freeing the slot
 * so the worker can reuse it race-free), then it is the current frame. Returns
 * 0 if the FIFO underran (hold the current frame). */
static int fmv_advance_frame(void) {
    unsigned w = atomic_load(&fmv_fifo_w), r = atomic_load(&fmv_fifo_r);
    if (r >= w) return 0;                          /* FIFO empty -> hold current */
    fmv_use_frame(&fmv_fifo[r % FMV_FIFO_MAX]);    /* copy out (frees the slot); video only */
    atomic_store(&fmv_fifo_r, r + 1);
    return 1;
}

static void fmv_reader_start(void) {
    if (!fmv_file) return;
    fmv_reader_stop = 0;
    fmv_fifo_w = fmv_fifo_r = 0;
    fmv_reader_thread = CreateThread(NULL, 0, fmv_reader_fn, NULL, 0, NULL);
}
static void fmv_reader_stop_join(void) {
    if (!fmv_reader_thread) return;
    fmv_reader_stop = 1;
    WaitForSingleObject(fmv_reader_thread, 2000);
    CloseHandle(fmv_reader_thread);
    fmv_reader_thread = NULL;
}
#else  /* non-Windows fallback: synchronous decode (M3 uses the SD arbiter, not this) */
static int fmv_advance_frame(void) {
    static uint8_t unit[FMV_UNIT_MAX];
    static FmvFrame f;
    if (!fmv_read_next_unit(unit)) return 0;
    fmv_decode_unit_into(&f, unit);
    fmv_use_frame(&f);
    return 1;
}
static void fmv_reader_start(void) {}
static void fmv_reader_stop_join(void) {}
#endif

/* emit the DMA body for band `band` into buffer `base`, filling the hidden
 * BACK buffers (CHR at back_chr, tilemap at back_tm). The displayed FRONT stays
 * up during the fill (band 0 holds BG1SC/BG12NBA on front_sc/front_nba); band 3
 * stages the frame's CGRAM and flips BG1SC+BG12NBA to the back (back_sc/back_nba)
 * so the new frame's CHR+tilemap+palette all appear together in the blank -- no
 * build-in, no tearing, no palette mismatch. */
static void hx_emit_fmv_band(uint32_t base, int band, uint16_t back_chr, uint16_t back_tm,
                             uint8_t back_sc, uint8_t back_nba, uint8_t front_sc, uint8_t front_nba) {
    uint8_t *c = &g_window[base + HX_OFF_DMABODY];
    size_t p = 0;
    e_lda_sta8(c, &p, 0xC0u, HW_A1B0);                 /* A-bus bank = $C0 (once) */
    if (band == 0) {
        e_bg_setup(c, &p);                             /* BGMODE, TM on main */
        e_lda_sta8(c, &p, front_sc,  0x2107u);         /* keep FRONT tilemap shown */
        e_lda_sta8(c, &p, front_nba, 0x210Bu);         /* keep FRONT CHR shown */
    }
    /* Tilemap in 4 chunks of 512 B (256 words), one per band, into the BACK
     * map — safe to fill incrementally because nothing displays it until the
     * band-3 flip, and it keeps 2 KB off band 0. */
    e_dma_vram_slot(c, &p, (uint16_t)(base + FMV_OFF_TILEMAP), 512u,
                    (uint16_t)(back_tm + band * 256));
    /* every band: its CHR chunk -> BACK CHR base + (start_tile * 16 words) */
    int start_tile = 0;
    for (int i = 0; i < band; ++i) start_tile += fmv_band_tiles[i];
    uint16_t chr_bytes = (uint16_t)(fmv_band_tiles[band] * 32);
    uint16_t chr_vword = (uint16_t)(back_chr + start_tile * 16);
    e_dma_vram_slot(c, &p, (uint16_t)(base + FMV_OFF_CHR), chr_bytes, chr_vword);
    if (band == 3) {
        /* frame complete: CGRAM (this frame's palette) + atomic display flip */
        e_dma_cgram_slot(c, &p, (uint16_t)(base + FMV_OFF_CGRAM), FMV_CGRAM_BYTES, 0x00);
        e_lda_sta8(c, &p, back_sc,  0x2107u);          /* FLIP tilemap -> back */
        e_lda_sta8(c, &p, back_nba, 0x210Bu);          /* FLIP CHR -> back */
    }

    /* --- sprite overlay ------------------------------------------------- *
     * Static OBJ assets ride band 1 (an EARLY band, so a long burst can't
     * starve them) as ONE contiguous CHR slot plus both palette blocks. */
    if (band == 1) {
        e_dma_vram_slot(c, &p, HX_OFF_SPR_CHR, HX_SPR_CHR_BYTES, HX_SPR_VRAM_WORD);
        e_dma_cgram_slot(c, &p, HX_OFF_SPR_CGRAM, HX_SPR_CGRAM_BYTES, HX_SPR_CGADD);
        e_dma_cgram_slot(c, &p, HX_OFF_SPR_PAL47, HX_SPR_CGRAM_BYTES, HX_SPR_FFT_CGADD);
    }
    /* OAM: sprites 0-63 every band (60 Hz cursor + bars, 4x the video rate);
     * the full 544 B once per video frame keeps 64-127 parked offscreen. */
    e_dma_oam_slot(c, &p, HX_OFF_SPR_OAM,
                   (band == 3) ? HX_SPR_OAM_BYTES : HX_SPR_OAM_ACTIVE, 0u);

    e8(c, &p, 0x6B);                                   /* rtl */
}

static int      g_back;              /* buffer the DLL writes (0/1); flips on DONE */
static uint64_t g_frames_made;       /* produced-frame counter (for logging)      */
static uint64_t g_done_seen;         /* HX_FRAME_DONE strobes observed            */
static int      g_m2_logged_first;   /* one-shot "first frame produced" log       */
static int      g_fmv_mode;          /* HX421_FMV=1: run the FMV band pipeline     */
static int      g_fmv_band;          /* current subframe band 0..3 (advances /DONE) */
static int      g_fmv_front;         /* displayed CHR buffer: 0=A ($0000), 1=B ($4000) */
static int      g_fmv_phase;         /* animation phase (advances per frame)       */
static int      g_fmv_need_decode;   /* 1 = decode a NEW frame on the next band 0  */
static int      g_fmv_kicked;        /* 1 after the first band is staged (FMV kickoff) */
static int      g_fmv_preroll;       /* SNES frames to hold video black while audio fills */
static int      g_pll_armed;         /* 1 after the drift-PLL baseline is re-armed       */
static int      g_pll_log;           /* feeds since the last A/V diagnostic line          */
static int32_t  g_lead_ema_q8;       /* EMA of the fed-rendered lead in ms (q8)           */
static int      g_ema_seeded;        /* 1 once the EMA has taken its first real sample    */
static uint32_t g_audio_dropped;     /* frames refused by a full ring (audio runs early)  */
static uint32_t g_fmv_stalls;        /* video frames REPEATED because the FIFO underran.  */
                                     /* Each one loses 67 ms of picture time while audio  */
                                     /* keeps streaming = permanent audio lead. Cumulative.*/
static int      g_av_settle;         /* feeds left before capturing the drift setpoint    */
static int      g_av_have_setpoint;  /* 1 once the natural-lead setpoint is captured      */
static int32_t  g_av_setpoint_ms;    /* the natural fed-rendered lead to hold (ms)        */

/* ---- platform file reader (PC / stdio) --------------------------------- */
static void *sr_open(void *ctx, const char *path) { (void)ctx; return fopen(path, "rb"); }
static uint32_t sr_read(void *ctx, void *fh, void *dst, uint32_t n) {
    (void)ctx; return fh ? (uint32_t)fread(dst, 1, n, (FILE *)fh) : 0u;
}
static bool sr_seek(void *ctx, void *fh, uint32_t off) {
    (void)ctx; return fh && fseek((FILE *)fh, (long)off, SEEK_SET) == 0;
}
static void sr_close(void *ctx, void *fh) { (void)ctx; if (fh) fclose((FILE *)fh); }

static AudioFileReader stdio_reader(void) {
    AudioFileReader r;
    r.open = sr_open; r.read = sr_read; r.seek = sr_seek; r.close = sr_close; r.ctx = NULL;
    return r;
}

/* ---- SMOKE-mode diagnostic tone (2 s, 440 Hz, mono16 @ output rate) ----- */
static void start_smoke_tone(uint32_t rate) {
    static int16_t tone[2u * 48000u];           /* room for 2 s up to 48 kHz */
    uint32_t n = 2u * rate;
    if (n > (uint32_t)(sizeof tone / sizeof tone[0])) n = (uint32_t)(sizeof tone / sizeof tone[0]);
    for (uint32_t i = 0; i < n; ++i) {
        double t = (double)i / (double)rate;
        tone[i] = (int16_t)(sin(2.0 * 3.14159265358979323846 * 440.0 * t) * 8000.0);
    }
    AudioObjHandle o = hxa_load_sfx_pcm(g_svc, tone, n * (uint32_t)sizeof tone[0], rate);
    if (o) hxa_trigger_sfx(g_svc, o, Q15_ONE, 0);
}

/* SMOKE-via-mixer: push a continuous 440 Hz sine into the PCM stream voice so
 * the tone is produced by the real mixer path (not a bypass), exercising it at
 * whatever pull granularity the host uses (bsnes pulls 1 frame at a time). */
static void smoke_feed(uint32_t frames) {
    const double TWO_PI = 2.0 * 3.14159265358979323846;
    double inc = TWO_PI * 440.0 / (double)g_smoke_rate;
    int16_t buf[512 * 2];
    while (frames) {
        uint32_t n = frames > 512u ? 512u : frames;
        for (uint32_t i = 0; i < n; ++i) {
            int16_t v = (int16_t)(sin(g_smoke_phase) * 8000.0);
            g_smoke_phase += inc;
            if (g_smoke_phase >= TWO_PI) g_smoke_phase -= TWO_PI;
            buf[i * 2]     = v;
            buf[i * 2 + 1] = v;
        }
        size_t acc = hxa_feed_pcm(g_svc, buf, n);
        if (acc < n) break;   /* ring full — mixer will drain it; resume next pull */
        frames -= n;
    }
}

/* Load a short built-in beep (asset id -> pitch/length) into an SFX slot, so the
 * command channel has something to TRIGGER without external files. */
static void load_builtin_sfx(uint16_t slot, uint32_t asset) {
    static const struct { double freq, secs; } A[] = {
        {440.0, 0.12}, {660.0, 0.12}, {880.0, 0.10}, {330.0, 0.15},
        {523.25, 0.10}, {784.0, 0.10}, {1046.5, 0.08}, {220.0, 0.18},
    };
    if (slot >= HX421_SFX_SLOTS) return;
    uint32_t idx  = asset < (sizeof A / sizeof A[0]) ? asset : 0u;
    uint32_t rate = 44100u;                     /* at output rate (no resample)  */
    static int16_t buf[44100u / 4];             /* up to 0.25 s                  */
    uint32_t n = (uint32_t)(A[idx].secs * rate);
    if (n > (uint32_t)(sizeof buf / sizeof buf[0])) n = (uint32_t)(sizeof buf / sizeof buf[0]);
    const double TWO_PI = 2.0 * 3.14159265358979323846;
    double ph = 0.0, inc = TWO_PI * A[idx].freq / (double)rate;
    uint32_t atk = rate / 200, rel = n / 3;     /* click-free attack/decay       */
    for (uint32_t i = 0; i < n; ++i) {
        double env = 1.0;
        if (i < atk)          env = (double)i / (double)atk;
        else if (i > n - rel) env = (double)(n - i) / (double)rel;
        buf[i] = (int16_t)(sin(ph) * 8000.0 * env);
        ph += inc; if (ph >= TWO_PI) ph -= TWO_PI;
    }
    g_sfx[slot] = hxa_load_sfx_pcm(g_svc, buf, n * (uint32_t)sizeof buf[0], rate);
}

/* ============================ lifecycle ================================= */

HX421_API int hx421_init(const Hx421Config *cfg) {
    if (!cfg) return -22;                                   /* -EINVAL */
    if (cfg->abi_version != HX421_ABI_VERSION) return -22;
    if (cfg->cart_window_size != HX421_CART_WINDOW_BYTES) return -22;
    if (cfg->pad_count > HX421_MAX_PADS) return -22;
    if (cfg->audio_sample_rate != 0 && cfg->audio_sample_rate < 8000) return -22;
    if (g_svc) return -114;                                 /* -EALREADY */

    HxaConfig hc;
    memset(&hc, 0, sizeof hc);
    hc.sample_rate   = cfg->audio_sample_rate ? cfg->audio_sample_rate : HX421_AUDIO_RATE_HZ;
    hc.track_count   = 8;
    hc.pool_bytes    = 0;          /* engine default (4 MiB) */
    hc.headroom_bits = 0;          /* natural level; a single unity voice can't clip.
                                    * (revisit when many loud voices mix — then add
                                    * per-voice gain trims or a bit of headroom back.) */
    hc.reader        = stdio_reader();

    g_svc = hxa_create(&hc);
    if (!g_svc) return -12;                                 /* -ENOMEM */

    hxa_fft_set_enabled(g_svc, true);   /* spectrum analyzer for the player */
    g_fft_next = hc.sample_rate / 2;    /* first ASCII spectrum after ~0.5 s */

    memset(g_window, 0, sizeof g_window);
    g_window[HX421_MB_STATUS & 0xFFFFu]      = 0x01;        /* boot */
    g_window[HX421_MB_FRAME_READY & 0xFFFFu] = 0x00;

    /* Optional standalone boot ROM (SNES-side kernel milestone). If HX421_ROM
     * names a file, load its $8000-$FFFF half into the cart window so the SNES
     * fetches the reset vector from OUR boot image instead of the flat-zero
     * window. The DLL only ever populates $8000-$FFFF, leaving the mailbox/
     * staging region ($0000-$7FFF, incl. the status/frame-ready bytes just set)
     * untouched. A 64 KB image (our hx421.cfg output) supplies bytes
     * [0x8000..0xFFFF]; a <=32 KB image is loaded starting at $8000. When
     * HX421_ROM is unset the existing SMOKE/audio behavior is unchanged. */
    g_rom_loaded = 0;
    {
        const char *rompath = getenv("HX421_ROM");
        if (rompath && rompath[0]) {
            FILE *rf = fopen(rompath, "rb");
            if (!rf) {
                fprintf(stderr, "hx421: HX421_ROM set but cannot open \"%s\"\n", rompath);
            } else {
                long sz = 0;
                if (fseek(rf, 0, SEEK_END) == 0) { sz = ftell(rf); }
                if (sz >= 0x10000L) {
                    /* full 64 KB window image: take the upper half only */
                    fseek(rf, 0x8000L, SEEK_SET);
                    size_t got = fread(&g_window[0x8000], 1, 0x8000u, rf);
                    fprintf(stderr, "hx421: ROM loaded \"%s\" (%ld bytes; upper 32 KB -> "
                            "window $8000-$FFFF, read %zu)\n", rompath, sz, got);
                } else {
                    /* smaller image: load it at $8000 (up to the window top) */
                    fseek(rf, 0, SEEK_SET);
                    size_t got = fread(&g_window[0x8000], 1, 0x8000u, rf);
                    fprintf(stderr, "hx421: ROM loaded \"%s\" (%ld bytes -> window $8000, "
                            "read %zu)\n", rompath, sz, got);
                }
                fclose(rf);
                g_rom_loaded = 1;
                fprintf(stderr, "hx421: reset vector in window = $%02X%02X (RESET -> $%02X%02X)\n",
                        g_window[0xFFFD], g_window[0xFFFC], g_window[0xFFFD], g_window[0xFFFC]);
                fflush(stderr);
            }
        }
    }

    memset(g_pads, 0, sizeof g_pads);
    g_mouse_dx = g_mouse_dy = 0; g_mouse_btn = 0;
    g_now_ns = 0; g_pull_frames = 0; g_resetting = 0;
    g_reset_hold_ns = (uint64_t)(cfg->reset.hold_ms ? cfg->reset.hold_ms : 50u) * 1000000ull;

    g_smoke_tone  = 0;
    g_smoke_mixer = 0;
    g_smoke_rate  = hc.sample_rate;
    g_smoke_phase = 0.0;
    g_music_voice = AUDIO_VOICE_NONE;
    g_cmd_mode    = (getenv("HX421_CMD") != NULL);
    g_fmv_mode    = (getenv("HX421_FMV") != NULL);   /* FMV band pipeline */
    if (g_fmv_mode) {
        g_fmv_need_decode = 1;                        /* decode frame 0 on the first band 0 */
        /* A/B the staging depths without a rebuild. These ARE output latency —
         * raising them buys underrun cushion but puts audio behind the picture
         * (uncapped, the channel runs to 743 ms, which was the original lag).
         * Prefer HX421_FMV_LEAD for cushion: the ring costs no lag. */
        { const char *sb = getenv("HX421_FMV_STREAMBUF");
          const char *cf = getenv("HX421_FMV_CHANFILL");
          size_t sbv = sb && sb[0] ? (size_t)strtoul(sb, NULL, 10) : 0;
          size_t cfv = cf && cf[0] ? (size_t)strtoul(cf, NULL, 10) : 0;
          if (sbv || cfv) {
              hxa_set_lowlat(g_svc, sbv, cfv);
              fprintf(stderr, "hx421 fmv: staging streambuf=%u chanfill=%u frames\n",
                      (unsigned)(sbv ? sbv : 2048u), (unsigned)(cfv ? cfv : 2048u));
          } }
        /* Gunshot SFX for the left click. HX421_BULLET_WAV overrides; otherwise
         * "bullet.wav" beside the .fmv. Missing file just leaves clicks silent
         * (holes still stamp) — a demo asset must never fail the run. */
        g_spr_bullet = 0;
        {
            char bpath[1024];
            const char *bw = getenv("HX421_BULLET_WAV");
            if (bw && bw[0]) {
                snprintf(bpath, sizeof bpath, "%s", bw);
            } else {
                const char *fp = getenv("HX421_FMV_FILE");
                size_t cut = 0;
                if (fp) for (size_t i = 0; fp[i]; ++i)
                    if (fp[i] == '/' || fp[i] == '\\') cut = i + 1;
                snprintf(bpath, sizeof bpath, "%.*sbullet.wav", (int)cut, fp ? fp : "");
            }
            g_spr_bullet = hxa_load_sfx_wav(g_svc, bpath);
            fprintf(stderr, "hx421 spr: bullet SFX %s (\"%s\")\n",
                    g_spr_bullet ? "loaded" : "MISSING - clicks silent", bpath);
        }
        memset(g_spr_hole, 0, sizeof g_spr_hole);
        g_spr_next_hole = HX_SPR_POOL_FIRST;
        g_spr_prev_left = 0; g_spr_cursor_hidden = 0;
        memset(g_fft_lvl, 0, sizeof g_fft_lvl);
        memset(g_fft_h,   0, sizeof g_fft_h);
        memset(g_fft_peak, 0, sizeof g_fft_peak);
        hxa_fft_set_enabled(g_svc, true);             /* band meter for the bars */
        hx_spr_build_assets();                        /* static OBJ tiles + palettes */
        /* Quick eyeball tuning without a rebuild: HX421_CURSOR_CLAMP=l,r,t,b */
        { const char *cl = getenv("HX421_CURSOR_CLAMP");
          int l, r, t, b;
          if (cl && sscanf(cl, "%d,%d,%d,%d", &l, &r, &t, &b) == 4) {
              hx421_cursor_set_clamp(l, r, t, b);
              fprintf(stderr, "hx421 spr: cursor clamp = %d..%d x %d..%d\n", l, r, t, b);
          } }
        g_spr_cx = 124; g_spr_cy = 100;
        hx_spr_write_oam();                           /* cursor centred, rest parked */
        hx421_audio_lock_init();                      /* worker feeds the mixer on both paths */
        /* A/V probe: OFF unless explicitly =1. Value-checked (not mere presence)
         * so HX421_FMV_SYNCTEST=0 actually disables it in a shell that already
         * exported it. Kept as a permanent diagnostic — it flashes one frame
         * white and puts a click in that SAME frame's audio, which measures
         * true end-to-end A/V offset by eye and ear. */
        { const char *st = getenv("HX421_FMV_SYNCTEST");
          g_fmv_synctest = (st && st[0] == '1'); }
        if (g_fmv_synctest) fprintf(stderr, "hx421 fmv: SYNCTEST on (flash+click probe)\n");
        /* Two INDEPENDENT knobs:
         *   HX421_FMV_PREROLL — video hold in SNES frames (16.7 ms each). This is
         *     the only thing that moves A/V sync: offset = L_out - hold, because
         *     the hold sets when the picture starts while audio starts at once.
         *   HX421_FMV_LEAD — audio push-ahead / FIFO depth in video frames. Does
         *     NOT affect sync (the ring re-delays audio by the push-ahead, so it
         *     cancels); it only has to be deep enough that the ring doesn't starve
         *     while the video is held. Keep lead >= hold. */
        { const char *ld = getenv("HX421_FMV_LEAD");
          if (ld && ld[0]) { int v = atoi(ld);
                             if (v < 1) v = 1;
                             if (v > (int)FMV_FIFO_MAX) v = (int)FMV_FIFO_MAX;
                             g_fmv_lead = (unsigned)v; }
          else g_fmv_lead = 4u; }                     /* mgapi's ring depth: 3 frames lookahead */
        g_fmv_preroll = 11;                           /* mgapi's proven value (~183 ms) */
        { const char *pr = getenv("HX421_FMV_PREROLL"); if (pr && pr[0]) g_fmv_preroll = atoi(pr); }
        if (g_fmv_preroll > (int)g_fmv_lead * 4)      /* else the ring starves mid-hold */
            fprintf(stderr, "hx421 fmv: WARNING hold %d > lead %u frames — raise HX421_FMV_LEAD\n",
                    g_fmv_preroll, g_fmv_lead * 4u);
        fprintf(stderr, "hx421 fmv: video hold = %d SNES frames (%d ms), audio lead = %u frames (%u ms)\n",
                g_fmv_preroll, g_fmv_preroll * 1000 / 60, g_fmv_lead, g_fmv_lead * 1000u / 15u);
        g_pll_armed = 0; g_av_have_setpoint = 0;      /* drift PLL: fresh baseline + setpoint */
        g_av_settle = 45;                             /* ~3 s (15 feeds/s) before locking the setpoint */
        fmv_file_open();                              /* NULL => synthetic source */
        if (fmv_file) {                              /* real .fmv: audio voice + read-ahead */
            fmv_audio_voice = hxa_open_pcm_stream(g_svc, 44100u, Q15_ONE, 0);
            fmv_reader_start();                       /* background unit reader (no I/O stalls) */
            /* Audio DEFAULTS to the bsnes path now: with the FRAME_DONE video
             * pacing fix, frame production is steady, so the through-bsnes audio
             * is smooth AND bsnes locks A/V for free (no PLL/preroll needed).
             * The WASAPI decouple is opt-in (HX421_WASAPI=1) for comparison. */
            const char *w = getenv("HX421_WASAPI");
            if (w && w[0] == '1') hx421_audio_worker_start();   /* preroll set above, both paths */
        }
    }
    /* Resolve an autostart WAV: explicit cfg first, else the HX421_WAV env var
     * (a host-side dev convenience for bsnes-plus; unused on the MCU). Streamed
     * looping through the mixer via the file-stream path. Use a 44100 Hz WAV —
     * the file streamer doesn't resample yet. */
    const char *autostart = cfg->autostart_path;
    if (!autostart) {
        const char *e = getenv("HX421_WAV");
        if (e && e[0]) autostart = e;
    }
    if (g_cmd_mode) {
        /* Command-driven (interactive bsnes / host test): no auto-start. Preload
         * a few default SFX slots so TRIGGER plays immediately; everything else
         * comes through hx421_audio_command. */
        for (uint16_t i = 0; i < 4; ++i) load_builtin_sfx(i, i);
    } else if (autostart) {
        hxa_play_stream_wav(g_svc, autostart);
    } else if (cfg->rom_select == HX421_ROM_SMOKE && !(g_fmv_mode && fmv_file)) {
        /* SMOKE drives the tone THROUGH THE MIXER (continuous sine as PCM).
         * Skipped when a real .fmv owns the single push-stream voice. */
        g_smoke_voice = hxa_open_pcm_stream(g_svc, hc.sample_rate, Q15_ONE, 0);
        g_smoke_mixer = (g_smoke_voice != AUDIO_VOICE_NONE);
        (void)start_smoke_tone;   /* one-shot mixer SFX variant, kept for reference */
    }

    fprintf(stderr, "hx421: init ok — cmd_mode=%d, fmv_mode=%d, HX421_WAV=%s\n",
            g_cmd_mode, g_fmv_mode, getenv("HX421_WAV") ? getenv("HX421_WAV") : "(unset)");
    fflush(stderr);
    return 0;
}

HX421_API void hx421_shutdown(void) {
    hx421_audio_worker_stop();      /* stop the render worker before the mixer is destroyed */
    fmv_reader_stop_join();
    if (fmv_file) { fclose(fmv_file); fmv_file = NULL; }
    if (g_svc) { hxa_destroy(g_svc); g_svc = NULL; }
}

HX421_API const char *hx421_version(void) { return "hx421-runtime 0.1 (M0)"; }
HX421_API uint32_t    hx421_abi_version(void) { return HX421_ABI_VERSION; }

/* ============================ cart bus ================================== */

HX421_API uint8_t hx421_cart_read(uint32_t addr) {
    uint16_t a = (uint16_t)(addr & 0xFFFFu);
    if (a == (uint16_t)(HX421_MB_BOOT_RUNTIME & 0xFFFFu))
        g_window[HX421_MB_STATUS & 0xFFFFu] = 0x02;         /* one-shot boot->runtime */

    /* Standalone-boot-ROM trace (milestone plumbing proof, visible with -Log).
     * One-shot markers: the SNES fetching the reset vector proves it is reading
     * OUR boot image; the boot stub's handoff read-strobe ($7FE0) proves the
     * reset code ran through the copy-to-WRAM loop and is jumping to it. Guarded
     * by g_rom_loaded and one-shot flags, so zero cost once each has fired. */
    if (g_rom_loaded) {
        static int seen_rv_lo, seen_rv_hi, seen_exec, seen_handoff;
        if (a == 0xFFFCu && !seen_rv_lo) {
            seen_rv_lo = 1;
            fprintf(stderr, "hx421: SNES fetched RESET vector low  $FFFC = $%02X\n", g_window[a]);
            fflush(stderr);
        } else if (a == 0xFFFDu && !seen_rv_hi) {
            seen_rv_hi = 1;
            fprintf(stderr, "hx421: SNES fetched RESET vector high $FFFD = $%02X "
                    "(reset -> $%02X%02X)\n", g_window[a], g_window[0xFFFD], g_window[0xFFFC]);
            fflush(stderr);
        } else if (a == 0x7FE0u && !seen_handoff) {
            seen_handoff = 1;
            fprintf(stderr, "hx421: boot handoff strobe ($7FE0) — kernel copied to WRAM, "
                    "jumping to it\n");
            fflush(stderr);
        } else if (!seen_exec && a >= 0x8000u && a != 0xFFFCu && a != 0xFFFDu) {
            seen_exec = 1;
            fprintf(stderr, "hx421: SNES executing boot ROM — first opcode fetch in "
                    "$8000-$FFFF at $%04X ($%02X)\n", a, g_window[a]);
            fflush(stderr);
        }
    }
    /* M2 double-buffer handshake: the SNES kernel reads HX_FRAME_DONE_ADDR
     * (the ACCESS is the signal — read-only bus) once it has walked the last
     * DMA slot of the staged frame. On that strobe: flip which buffer we
     * write next (the just-published one is now FRONT/on-screen), and clear
     * the frame-ready flag (consumed). */
    if (g_rom_loaded && a == HX_FRAME_DONE_ADDR) {
        g_done_seen++;
        g_back ^= 1;                                   /* write the other buffer next */
        if (g_fmv_mode) {
            g_fmv_av_started = 1;                      /* console live: audio may flow now */
            hx_spr_tick();                             /* cursor at 60 Hz, before staging */
            g_fmv_band = (g_fmv_band + 1) & 3;         /* advance subframe band */
            if (g_fmv_band == 0) {                     /* completed a video frame */
                g_fmv_front ^= 1;                      /* band 3 flipped display to back */
                g_fmv_phase += 4;                      /* advance the animation */
                g_fmv_need_decode = 1;                 /* next band 0 = a new frame */
            }
            /* Produce the next band HERE, on the SNES's own clock (FRAME_DONE is
             * strobed at a fixed scanline each frame), and re-publish FRAME_READY
             * so a band is always resident before the next VIS_END. This is the
             * fix for the just-in-time-staging judder: production no longer rides
             * hx421_step's audio-sample phase, which drifts vs the SNES frame. */
            hx_produce_fmv_band();
        } else {
            g_window[HX_FRAME_READY_ADDR] = 0x00;      /* M2 bars: consumed; step re-publishes */
        }
        if (g_done_seen <= 5) {
            fprintf(stderr, "hx421 m2: FRAME_DONE #%llu — SNES DMA'd the frame; "
                    "flipping, now writing back=buf%d\n",
                    (unsigned long long)g_done_seen, g_back);
            fflush(stderr);
        }
    }
    /* TODO(M2+): joypad-mailbox readback ($7000-77FF), more DMA slots
     * (CGRAM/OAM), the per-scanline siphon (action code 3). */
    return g_window[a];
}

/* Produce one FMV band into the back buffer: the PSRAM->window band copy +
 * the emitted DMA body. Rides the same per-step / FRAME_DONE handshake as the
 * M2 producer; g_fmv_band advances 0..3 on each FRAME_DONE (see cart_read). */
static void hx_produce_fmv_band(void) {
    if (!g_rom_loaded) return;
    fmv_gen_static();
    uint32_t base = g_back ? HX_BUF1_BASE : HX_BUF0_BASE;
    int band = g_fmv_band & 3;

    /* start of a video frame: decode it into the PSRAM store (real .fmv unit,
     * or the synthetic scroll) — the SD->PSRAM step + the audio push. Guarded
     * so a re-produced band 0 (handshake retry) does NOT read a second unit /
     * push a second audio chunk (that would drop video frames + desync A/V);
     * decode happens exactly once per frame (reset on the band 3->0 wrap). */
    /* Do NOT advance during the preroll: the video must HOLD at frame 0 while the
     * worker streams audio ahead. (Advancing here is what made the old preroll
     * merely skip content instead of delaying the picture.) */
    if (band == 0 && g_fmv_need_decode && g_fmv_preroll == 0) {
        int ok;
        if (fmv_file) ok = fmv_advance_frame();            /* pop next FIFO frame (video only) */
        else        { fmv_gen_chr(g_fmv_phase); ok = 1; }
        if (ok) g_fmv_need_decode = 0;                     /* else retry next cycle (FIFO underran) */
        else    g_fmv_stalls++;                            /* repeated a frame: picture lost 67 ms */
    }

    /* double-buffer: fill the CHR/tilemap bases NOT currently displayed */
    uint16_t back_chr  = g_fmv_front ? FMV_CHR_A_VWORD : FMV_CHR_B_VWORD;
    uint16_t back_tm   = g_fmv_front ? FMV_TM_A_VWORD  : FMV_TM_B_VWORD;
    uint8_t  back_sc   = g_fmv_front ? FMV_BG1SC_A     : FMV_BG1SC_B;
    uint8_t  back_nba  = g_fmv_front ? FMV_BG12NBA_A   : FMV_BG12NBA_B;
    uint8_t  front_sc  = g_fmv_front ? FMV_BG1SC_B     : FMV_BG1SC_A;
    uint8_t  front_nba = g_fmv_front ? FMV_BG12NBA_B   : FMV_BG12NBA_A;

    /* action table: FMV letterbox (8+8), no siphon. During the A/V preroll the
     * whole field is force-blanked (screen black) while audio + the frame
     * pipeline advance, so when video is revealed it lines up with its audio's
     * output-pipe latency instead of racing ahead of it. */
    uint8_t *act = &g_window[base + HX_OFF_ACTION];
    memset(act, 0, 512);
    if (g_fmv_preroll > 0) {
        g_fmv_preroll--;
        for (int v = 0; v < 262; ++v) act[v] = 1;   /* all force-blank = black */
    } else {
        for (int v = 0; v < FMV_TOP_LB; ++v) act[v] = 1;
        act[FMV_TOP_LB] = 2;
        for (int v = FMV_VIS_END; v < 262; ++v) act[v] = 1;
    }
    g_window[base + HX_OFF_HDR_TOP_LB]  = (uint8_t)FMV_TOP_LB;
    g_window[base + HX_OFF_HDR_VIS_END] = (uint8_t)FMV_VIS_END;
    g_window[base + HX_OFF_SIP_BPL + 0] = 0;             /* siphon off */
    g_window[base + HX_OFF_SIP_BPL + 1] = 0;

    /* copy ONLY THIS band from the PSRAM store into the window (BRAM). CHR
     * every band; the tilemap on band 0; the CGRAM on band 3 (with the flip). */
    int start_tile = 0;
    for (int i = 0; i < band; ++i) start_tile += fmv_band_tiles[i];
    uint32_t chr_bytes = (uint32_t)fmv_band_tiles[band] * 32u;
    memcpy(&g_window[base + FMV_OFF_CHR], &fmv_chr[start_tile * 32], chr_bytes);
    /* this band's 512-byte quarter of the tilemap (decoded at band 0, so all
     * four quarters come from the same frame) */
    memcpy(&g_window[base + FMV_OFF_TILEMAP], &fmv_tilemap[band * 512], 512);
    if (band == 3)
        memcpy(&g_window[base + FMV_OFF_CGRAM], fmv_cgram, sizeof fmv_cgram);

    /* emit the band's DMA body (fills back; band 3 flips display) + publish */
    hx_emit_fmv_band(base, band, back_chr, back_tm, back_sc, back_nba, front_sc, front_nba);
    g_window[HX_FRAME_READY_ADDR] = (uint8_t)(g_back + 1);
    g_frames_made++;
    if (!g_m2_logged_first) {
        g_m2_logged_first = 1;
        fprintf(stderr, "hx421 fmv: first band produced (band %d, back=buf%d), "
                "FB top=%d vis_end=%d, %d tiles / 4 bands\n",
                band, g_back, FMV_TOP_LB, FMV_VIS_END, FMV_TILES);
        fflush(stderr);
    }
}

/* ============================ M2 frame production ====================== */

/* Build one frame into the BACK buffer of the served window: the per-line
 * action table (letterbox line-plan), the frame header, one DMA descriptor
 * (tilemap -> VRAM), and the FFT-bar tilemap; then raise the frame-ready
 * flag. Called once per ~60 Hz from hx421_step. Only runs when a real boot
 * ROM (the M2 kernel) is being served — otherwise the non-ROM SMOKE/audio
 * behavior stays byte-identical. */
static void hx_produce_frame(void) {
    if (!g_svc || !g_rom_loaded) return;

    uint32_t base    = g_back ? HX_BUF1_BASE : HX_BUF0_BASE;
    uint32_t tmapoff = base + HX_OFF_TILEMAP;

    /* 1) action table: top letterbox (blank), unblank line, visible band,
     *    bottom letterbox + vblank (blank). action codes: 1=blank 2=unblank. */
    uint8_t *act = &g_window[base + HX_OFF_ACTION];
    memset(act, 0, 512);                          /* default ACT_NONE */
    for (int v = 0; v < HX_TOP_LB; ++v) act[v] = 1;   /* top letterbox */
    act[HX_TOP_LB] = 2;                               /* reveal line   */
    for (int v = HX_VIS_END; v < 262; ++v) act[v] = 1;/* bottom LB + vblank */

    /* 2) frame header (for the kernel's live-V DMA budget) */
    g_window[base + HX_OFF_HDR_TOP_LB]  = (uint8_t)HX_TOP_LB;
    g_window[base + HX_OFF_HDR_VIS_END] = (uint8_t)HX_VIS_END;

    /* 3) emit the baked-immediate 65816 DMA body: one slot pushes the 2 KB
     *    tilemap to VRAM word 0. The kernel jsl's straight into this in the
     *    served window (no descriptor read, no runtime dispatch). */
    hx_emit_dma_body(base, tmapoff);

    /* 4) FFT bars -> BG1 tilemap (tile 1 = solid bar, tile 0 = empty) */
    uint32_t bands[HX_FFT_BARS];
    uint32_t nb = hxa_fft_bands(g_svc, bands, HX_FFT_BARS);
    uint8_t *tm = &g_window[tmapoff];
    memset(tm, 0, 2048);                  /* clear map to tile 0 (word 0)     */
    for (int c = 0; c < HX_TILE_COLS; ++c) {
        int band = nb ? (int)((uint32_t)c * nb / HX_TILE_COLS) : 0;
        if (band >= (int)nb) band = nb ? (int)nb - 1 : 0;
        uint32_t lvl = nb ? (bands[band] > 255u ? 255u : bands[band]) : 0u;
        int h = (int)((lvl * HX_BAR_MAXH) / 255u);
        for (int r = 0; r < h; ++r) {
            int row = HX_BAR_ANCHOR - r;
            if (row < 0 || row >= HX_TILE_ROWS) continue;
            uint32_t cell = ((uint32_t)row * HX_TILE_COLS + (uint32_t)c) * 2u;
            tm[cell]     = 0x01;          /* tile index 1 (low byte)          */
            tm[cell + 1] = 0x00;          /* palette 0 / no attrs (high byte) */
        }
    }

    /* 4b) H-blank siphon demo (the framework proof). Plant a full row of the
     *     siphon tile, mark the visible siphon lines (ACT_SIPHON=3), write the
     *     siphon descriptor, and stage an animated 32-byte tile-2 CHR that the
     *     kernel streams into VRAM during active display. */
    for (int c = 0; c < HX_TILE_COLS; ++c) {
        uint32_t cell = ((uint32_t)HX_SIP_TILE_ROW * HX_TILE_COLS + (uint32_t)c) * 2u;
        tm[cell]     = (uint8_t)HX_SIP_TILE;       /* tile index 2 (low byte)  */
        tm[cell + 1] = 0x00;                       /* palette 0 / no attrs     */
    }
    for (uint32_t i = 0; i < HX_SIP_LINES; ++i)
        act[HX_SIP_FIRST_LINE + i] = 3;            /* ACT_SIPHON on these lines */
    g_window[base + HX_OFF_SIP_BPL + 0] = (uint8_t)(HX_SIP_BPL & 0xFF);
    g_window[base + HX_OFF_SIP_BPL + 1] = (uint8_t)((HX_SIP_BPL >> 8) & 0xFF);
    g_window[base + HX_OFF_SIP_VRAM + 0] = (uint8_t)(HX_SIP_VRAM_WORD & 0xFF);
    g_window[base + HX_OFF_SIP_VRAM + 1] = (uint8_t)((HX_SIP_VRAM_WORD >> 8) & 0xFF);
    {
        uint32_t sip_src = base + HX_OFF_SIPHON_DATA;   /* offset within bank $C0 */
        g_window[base + HX_OFF_SIP_SRC + 0] = (uint8_t)(sip_src & 0xFF);
        g_window[base + HX_OFF_SIP_SRC + 1] = (uint8_t)((sip_src >> 8) & 0xFF);
        /* animated tile-2 CHR (4bpp): color-1 stripes scrolling with the frame
         * counter. plane0 carries the lit-pixel mask; planes 1-3 stay 0. */
        uint8_t *chr = &g_window[sip_src];
        uint32_t phase = (uint32_t)(g_frames_made & 7u);
        memset(chr, 0, HX_SIP_CHR_BYTES);
        for (int r = 0; r < 8; ++r) {
            uint32_t rr = ((uint32_t)r + phase) & 7u;
            chr[2 * r] = ((rr & 3u) < 2u) ? 0xFFu : 0x00u;   /* plane0 = color 1 */
        }
    }

    /* 5) publish: frame-ready = back index + 1 (kernel swaps front at V=0) */
    g_window[HX_FRAME_READY_ADDR] = (uint8_t)(g_back + 1);
    g_frames_made++;
    if (!g_m2_logged_first) {
        g_m2_logged_first = 1;
        fprintf(stderr, "hx421 m2: first frame produced -> back=buf%d, ready=%d "
                "(FB top=%d bot=%d, vis_end=%d)\n",
                g_back, g_back + 1, HX_TOP_LB, HX_BOT_LB, HX_VIS_END);
        fflush(stderr);
    }
}

/* ============================ tick ===================================== */

HX421_API void hx421_step(uint64_t elapsed_ns) {
    g_now_ns += elapsed_ns ? elapsed_ns : (1000000000ull / 60ull);
    /* M2 bars stage one frame per step. FMV production is FRAME_DONE-driven
     * (deterministic pacing, see cart_read) — hx421_step only KICKS the first
     * band once so the very first VIS_END has something to latch. */
    if (g_fmv_mode) {
        if (!g_fmv_kicked) { g_fmv_kicked = 1; hx_produce_fmv_band(); }
    } else {
        hx_produce_frame();
    }

    /* Periodic proof (visible with -Log) that the chainer is being driven:
     * frames produced + FRAME_DONE strobes the SNES kernel fired back. */
    static uint64_t next_log;
    if (g_rom_loaded && g_frames_made >= next_log) {
        next_log = g_frames_made + 120;          /* ~ every 2 s at 60 Hz */
        fprintf(stderr, "hx421 m2: produced %llu frames, %llu done-strobes, "
                "writing back=buf%d\n",
                (unsigned long long)g_frames_made,
                (unsigned long long)g_done_seen, g_back);
        fflush(stderr);
    }
}

/* ==================== WASAPI audio output (DLL/Windows) ================
 * Routing our audio through bsnes-plus's Qt pipeline couples it to bsnes's
 * emulation pacing and stutters (mgapi hit this and moved to its own device).
 * A worker thread renders the mixer paced by our OWN WASAPI device; bsnes gets
 * silence via hx421_audio_pull. Feed (emulation thread) and render (worker) are
 * serialized by g_audio_cs; the drift PLL trims the mixer's rate to the
 * video/feed clock so A/V stay locked without bsnes doing the pacing. */
#if defined(_WIN32)
extern const AudioSinkBackend audio_sink_wasapi;
static CRITICAL_SECTION g_audio_cs;
static int              g_audio_cs_init;

/* The FMV decode worker feeds the mixer on BOTH output paths (bsnes-pull and
 * WASAPI), so the lock is needed whenever FMV is on — not just under WASAPI. */
static void hx421_audio_lock_init(void) {
    if (g_audio_cs_init) return;
    InitializeCriticalSection(&g_audio_cs);
    g_audio_cs_init = 1;
}
static HANDLE           g_audio_thread;
static volatile LONG    g_audio_stop;
static volatile int     g_audio_sink_on;    /* 1 = WASAPI owns output; pull = silence */
static uint64_t         g_audio_fed;        /* frames fed (video clock); both under g_audio_cs */

extern uint32_t hx421_wasapi_device_rate(void);

static DWORD WINAPI hx421_audio_worker(LPVOID arg) {
    (void)arg;
    /* Open at the DEVICE's native rate and resample 44100->native ourselves.
     * Handing Windows a 44.1 kHz stream and letting AUTOCONVERTPCM's SRC convert
     * it is not a reliable real-time clock (mgapi's v1.99 lesson) — the engine
     * drains our buffer at an effective rate that isn't ours, so latency accrues. */
    uint32_t dev_rate = hx421_wasapi_device_rate();
    if (!dev_rate) dev_rate = 44100u;
    void *sink = audio_sink_wasapi.open(dev_rate);
    if (!sink) {
        fprintf(stderr, "hx421: WASAPI open failed — audio stays on the bsnes path\n");
        fflush(stderr);
        return 0;
    }
    g_audio_sink_on = 1;
    fprintf(stderr, "hx421: WASAPI sink open @ %u Hz (mixer 44100 -> %s)\n",
            dev_rate, dev_rate == 44100u ? "direct" : "internal resample");
    fflush(stderr);

    enum { OUT_CHUNK = 512, SRC_MAX = 2048 };
    int16_t  out_buf[OUT_CHUNK * 2];
    int16_t  src_buf[SRC_MAX * 2];
    uint32_t src_n = 0;
    uint64_t pos_q32 = 0;
    const uint64_t step = ((uint64_t)44100u << 32) / dev_rate;   /* src frames per out frame */

    while (!g_audio_stop) {
        if (dev_rate == 44100u) {                                /* rates match: straight through */
            EnterCriticalSection(&g_audio_cs);
            hxa_render(g_svc, out_buf, OUT_CHUNK);
            g_audio_rendered += OUT_CHUNK;
            LeaveCriticalSection(&g_audio_cs);
            if (audio_sink_wasapi.write(sink, out_buf, OUT_CHUNK) < 0) break;
            continue;
        }
        /* drop already-consumed source frames, then top up to what this block needs */
        uint32_t consumed = (uint32_t)(pos_q32 >> 32);
        if (consumed) {
            if (consumed > src_n) consumed = src_n;
            memmove(src_buf, src_buf + (size_t)consumed * 2,
                    (size_t)(src_n - consumed) * 2 * sizeof(int16_t));
            src_n -= consumed;
            pos_q32 &= 0xFFFFFFFFull;
        }
        uint32_t need = (uint32_t)((pos_q32 + (uint64_t)OUT_CHUNK * step) >> 32) + 2u;
        if (need > SRC_MAX) need = SRC_MAX;
        while (src_n < need) {
            uint32_t chunk = need - src_n;
            EnterCriticalSection(&g_audio_cs);
            hxa_render(g_svc, src_buf + (size_t)src_n * 2, chunk);
            g_audio_rendered += chunk;                           /* PLL counts SOURCE frames */
            LeaveCriticalSection(&g_audio_cs);
            src_n += chunk;
        }
        /* linear resample to the device rate */
        for (uint32_t o = 0; o < OUT_CHUNK; ++o) {
            uint32_t i = (uint32_t)(pos_q32 >> 32);
            if (i + 1u >= src_n) i = (src_n >= 2u) ? src_n - 2u : 0u;
            int32_t fr = (int32_t)((pos_q32 >> 16) & 0xFFFFu);   /* q16 fraction */
            for (int ch = 0; ch < 2; ++ch) {
                int32_t a = src_buf[(size_t)i * 2 + ch];
                int32_t b = src_buf[(size_t)(i + 1u) * 2 + ch];
                out_buf[(size_t)o * 2 + ch] = (int16_t)(a + (((b - a) * fr) >> 16));
            }
            pos_q32 += step;
        }
        if (audio_sink_wasapi.write(sink, out_buf, OUT_CHUNK) < 0) break;
    }
    g_audio_sink_on = 0;
    audio_sink_wasapi.close(sink);
    return 0;
}

static void hx421_audio_worker_start(void) {
    hx421_audio_lock_init();          /* usually already done at init; idempotent */
    g_audio_stop = 0;
    g_audio_thread = CreateThread(NULL, 0, hx421_audio_worker, NULL, 0, NULL);
    if (g_audio_thread) SetThreadPriority(g_audio_thread, THREAD_PRIORITY_ABOVE_NORMAL);
}
static void hx421_audio_worker_stop(void) {
    if (g_audio_thread) {
        g_audio_stop = 1;
        WaitForSingleObject(g_audio_thread, 2000);
        CloseHandle(g_audio_thread);
        g_audio_thread = NULL;
    }
    if (g_audio_cs_init) { DeleteCriticalSection(&g_audio_cs); g_audio_cs_init = 0; }
}

/* feed FMV audio + advance the drift PLL, serialized with the worker's render
 * when the WASAPI sink is live (else a plain single-threaded feed). */
static void hx421_fmv_feed_audio(const int16_t *stereo, uint32_t frames) {
    if (g_audio_cs_init) {          /* the decode worker feeds; always serialize */
        EnterCriticalSection(&g_audio_cs);
        if (!g_pll_armed) {                     /* clean baseline when audio starts */
            g_pll_armed = 1;
            g_audio_fed = g_audio_rendered;
            g_lead_ema_q8 = 0;
            g_ema_seeded  = 0;
            hxa_set_drift_ppm(g_svc, 0);
        }
        size_t took = hxa_feed_pcm(g_svc, stereo, frames);
        if (took < frames) g_audio_dropped += (uint32_t)(frames - took);
        g_audio_fed += frames;

        /* ring-level P-controller (mgapi's method): the fed-rendered lead is the
         * pcm buffer fill; it settles to a natural value (the output latency).
         * Correct only DEVIATIONS from that settled setpoint, so steady-state
         * ppm ~ 0 (no runaway to the clamp). */
        long long lead = (long long)g_audio_fed - (long long)g_audio_rendered;
        int32_t lead_ms = (int32_t)(lead * 1000 / 44100);
        /* SEED the EMA with the first sample instead of ramping from zero. Ramping
         * meant the settle window measured the EMA's own rise (~76% after 45 feeds),
         * so the setpoint froze well below the true lead and the P-term ratcheted
         * ppm upward forever chasing an unreachable target. */
        if (!g_ema_seeded) { g_ema_seeded = 1; g_lead_ema_q8 = lead_ms << 8; }
        else g_lead_ema_q8 += ((lead_ms << 8) - g_lead_ema_q8) >> 5;   /* ~2s TC at 15 feeds/s */
        int32_t ema_ms = g_lead_ema_q8 >> 8;
        int32_t ppm = 0;
        if (g_av_settle > 0) {
            g_av_settle--;                       /* let the pipe fill before locking */
        } else if (!g_av_have_setpoint) {
            g_av_have_setpoint = 1;
            g_av_setpoint_ms = ema_ms;           /* hold the natural lead */
        }
        if (g_av_have_setpoint) {
            ppm = 20 * (ema_ms - g_av_setpoint_ms);   /* kp = 20 ppm/ms */
            if (ppm >  18000) ppm =  18000;
            if (ppm < -18000) ppm = -18000;
        }
        hxa_set_drift_ppm(g_svc, ppm);
        if (++g_pll_log >= 15) {                 /* ~1/s diagnostic */
            g_pll_log = 0;
            uint32_t ur = 0, ov = 0, fill = 0;
            hxa_ring_stats(g_svc, &ur, &ov, &fill);
            /* CONTENT SKEW — the measurement that matches what you hear. The ring
             * is fed exactly one video frame's audio per video frame, so
             * rendered/apf is "which frame's audio has been played". Compare to
             * which frame is on screen. Positive = audio content AHEAD of picture
             * (beep leads flash). Growing = true drift; constant = fixed offset. */
            uint32_t apf  = fmv_abytes / 4u;
            long long aidx = apf ? (long long)((g_audio_rendered - g_av_rend_base) / apf) : 0;
            long long skew = aidx - (long long)g_fmv_disp;
            /* drop/underrun are CONTENT losses -> permanent A/V shift (drops =
             * audio early, underruns = audio late). ppm is only rate trim. */
            fprintf(stderr, "hx421 av: lead=%d ms (ema %d, setpoint %d), ppm=%d, "
                            "ring=%u ms, dropped=%u, underruns=%u, vstalls=%u, "
                            "SKEW=%+lld frames (%+lld ms) [aud %lld vs vid %llu]\n",
                    lead_ms, ema_ms, g_av_have_setpoint ? g_av_setpoint_ms : -1, (int)ppm,
                    (unsigned)(fill / 44u), (unsigned)g_audio_dropped, (unsigned)ur,
                    (unsigned)g_fmv_stalls,
                    skew, skew * 67, aidx, (unsigned long long)g_fmv_disp);
            fflush(stderr);
        }
        LeaveCriticalSection(&g_audio_cs);
    } else {
        hxa_feed_pcm(g_svc, stereo, frames);
    }
}
#else
static void hx421_audio_worker_start(void) {}
static void hx421_audio_worker_stop(void) {}
static void hx421_fmv_feed_audio(const int16_t *stereo, uint32_t frames) {
    hxa_feed_pcm(g_svc, stereo, frames);
}
#endif

/* ============================ audio (real pull) ======================== */

HX421_API uint32_t hx421_audio_pull(int16_t *dst_stereo, uint32_t frames) {
    if (!g_svc || !dst_stereo || !frames) return 0;
#if defined(_WIN32)
    if (g_audio_sink_on) {                       /* WASAPI owns output — feed bsnes silence */
        memset(dst_stereo, 0, (size_t)frames * 2u * sizeof(int16_t));
        g_pull_frames += frames;
        return frames;
    }
#endif
    if (g_smoke_mixer) {
        smoke_feed(frames);                      /* keep the push-stream fed with sine */
        hxa_render(g_svc, dst_stereo, frames);   /* the MIXER produces the tone */
    } else if (g_smoke_tone) {
        /* continuous 440 Hz sine on both channels, independent of frame count */
        const double TWO_PI = 2.0 * 3.14159265358979323846;
        double inc = TWO_PI * 440.0 / (double)g_smoke_rate;
        for (uint32_t i = 0; i < frames; ++i) {
            int16_t v = (int16_t)(sin(g_smoke_phase) * 8000.0);
            g_smoke_phase += inc;
            if (g_smoke_phase >= TWO_PI) g_smoke_phase -= TWO_PI;
            dst_stereo[i * 2]     = v;
            dst_stereo[i * 2 + 1] = v;
        }
    } else {
        hxa_render(g_svc, dst_stereo, frames);   /* engine silence-pads underruns */
    }
    g_pull_frames += frames;                     /* master-clock reference for drift */

    /* Periodic ASCII spectrum to stderr (visible with -Log): one bar char per
     * band, redrawn ~once/second, so the FFT can be watched reacting to audio
     * before the on-screen (SNES) analyzer exists. */
    if (g_pull_frames >= g_fft_next) {
        g_fft_next = g_pull_frames + (g_smoke_rate ? g_smoke_rate / 2u : 22050u);
        uint32_t bands[16];
        uint32_t nb = hxa_fft_bands(g_svc, bands, 16);
        static const char LV[] = " .:-=+*#%@";   /* 10 levels, quiet -> loud */
        char line[17];
        uint32_t i;
        for (i = 0; i < nb && i < 16; ++i) {
            uint32_t lvl = bands[i] > 255u ? 255u : bands[i];
            line[i] = LV[(lvl * 9u) / 255u];
        }
        line[i] = '\0';
        fprintf(stderr, "hx421 fft [%s]\n", line);
        /* A/V instrument: video frames released vs audio frames output, and the
         * decode FIFO fill. offset = where the audio sits relative to video. */
        if (fmv_file && fmv_abytes) {
            uint32_t apf = fmv_abytes / 4u;                 /* audio frames / video frame */
            long long aout = (long long)((g_pull_frames - g_av_pull_base) / apf);
#if defined(_WIN32)
            unsigned fifo = (unsigned)(atomic_load(&fmv_fifo_w) - atomic_load(&fmv_fifo_r));
#else
            unsigned fifo = 0;
#endif
            fprintf(stderr, "hx421 av2: video_disp=%llu audio_out=%lld (offset %lld frames) fifo=%u\n",
                    (unsigned long long)g_fmv_disp, aout,
                    (long long)g_fmv_disp - aout, fifo);
        }
        fflush(stderr);
    }

    return frames;
}

/* ============================ audio command channel ==================== */

HX421_API int32_t hx421_audio_command(const Hx421AudioCmd *cmd) {
    if (!g_svc || !cmd) return -1;
    switch (cmd->opcode) {
    case HX421_ACMD_LOAD_SFX:
        load_builtin_sfx(cmd->slot, cmd->arg);
        return (cmd->slot < HX421_SFX_SLOTS && g_sfx[cmd->slot]) ? 0 : -1;

    case HX421_ACMD_TRIGGER: {
        if (cmd->slot >= HX421_SFX_SLOTS || !g_sfx[cmd->slot]) return -1;
        int32_t gain = cmd->gain ? cmd->gain : Q15_ONE;
        return (int32_t)hxa_trigger_sfx(g_svc, g_sfx[cmd->slot], gain, cmd->pan);
    }

    case HX421_ACMD_PLAY_MUSIC: {
        const char *w = getenv("HX421_WAV");
        if (w && w[0]) {
            g_music_voice = hxa_play_stream_wav(g_svc, w);
            fprintf(stderr, "hx421: PLAY_MUSIC wav=\"%s\" -> voice %u\n",
                    w, (unsigned)g_music_voice);
        } else if (!g_smoke_mixer) {
            /* no WAV configured: a built-in continuous tone through the mixer */
            g_smoke_phase = 0.0;
            g_smoke_voice = hxa_open_pcm_stream(g_svc, g_smoke_rate, Q15_ONE, 0);
            g_smoke_mixer = (g_smoke_voice != AUDIO_VOICE_NONE);
            g_music_voice = g_smoke_voice;
            fprintf(stderr, "hx421: PLAY_MUSIC (built-in tone, no HX421_WAV) -> voice %u\n",
                    (unsigned)g_music_voice);
        }
        fflush(stderr);
        return (int32_t)g_music_voice;
    }

    case HX421_ACMD_STOP:
        hxa_stop_voice(g_svc, (AudioVoiceHandle)cmd->arg);
        if ((AudioVoiceHandle)cmd->arg == g_music_voice) g_music_voice = AUDIO_VOICE_NONE;
        return 0;

    case HX421_ACMD_STOP_ALL:
        if (g_music_voice) hxa_stop_voice(g_svc, g_music_voice);
        g_music_voice = AUDIO_VOICE_NONE;
        if (g_smoke_mixer && g_smoke_voice) hxa_stop_voice(g_svc, g_smoke_voice);
        g_smoke_mixer = 0; g_smoke_voice = AUDIO_VOICE_NONE;
        return 0;

    case HX421_ACMD_SET_MASTER_VOL:
        return 0;   /* v1 stub — wire to a service master gain later */

    default:
        return -1;
    }
}

/* ============================ FFT spectrum ============================= */

HX421_API uint32_t hx421_fft_bands(uint32_t *out, uint32_t n) {
    if (!g_svc || !out || !n) return 0;
    return hxa_fft_bands(g_svc, out, n);
}

/* ============================ input ==================================== */

HX421_API void hx421_post_joypads(const uint16_t pads[HX421_MAX_PADS]) {
    if (pads) memcpy(g_pads, pads, sizeof g_pads);
}
HX421_API void hx421_post_mouse(int dx, int dy, unsigned buttons) {
    g_mouse_dx += dx; g_mouse_dy += dy; g_mouse_btn = buttons;
}

HX421_API void hx421_cursor_set_clamp(int left, int right, int top, int bottom) {
    if (right < left) { int t = left; left = right; right = t; }
    if (bottom < top) { int t = top;  top = bottom; bottom = t; }
    g_spr_clamp_l = left;  g_spr_clamp_r = right;
    g_spr_clamp_t = top;   g_spr_clamp_b = bottom;
    /* re-clamp immediately so a shrink can't leave the cursor outside */
    if (g_spr_cx < left)   g_spr_cx = left;
    if (g_spr_cx > right)  g_spr_cx = right;
    if (g_spr_cy < top)    g_spr_cy = top;
    if (g_spr_cy > bottom) g_spr_cy = bottom;
}

HX421_API void hx421_cursor_get_clamp(int *left, int *right, int *top, int *bottom) {
    if (left)   *left   = g_spr_clamp_l;
    if (right)  *right  = g_spr_clamp_r;
    if (top)    *top    = g_spr_clamp_t;
    if (bottom) *bottom = g_spr_clamp_b;
}

HX421_API void hx421_cursor_get_pos(int *x, int *y) {
    if (x) *x = g_spr_cx;
    if (y) *y = g_spr_cy;
}

/* ============================ reset ==================================== */

HX421_API void hx421_cart_reset_begin(void) {
    g_resetting = 1;
    g_reset_deadline_ns = g_now_ns + g_reset_hold_ns;
    g_window[HX421_MB_STATUS & 0xFFFFu]      = 0x01;   /* back to boot */
    g_window[HX421_MB_FRAME_READY & 0xFFFFu] = 0x00;
}
HX421_API int hx421_cart_reset_ready(void) {
    return (g_resetting && g_now_ns >= g_reset_deadline_ns) ? 1 : 0;
}
HX421_API void hx421_cart_reset_end(void) { g_resetting = 0; }
