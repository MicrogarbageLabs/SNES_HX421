/* ============================================================
 *  hx421_raybake.c — VRAM budget probe for the strip raycaster
 *
 *  The design: wall textures are pre-scaled into vertical STRIPS and left
 *  resident in VRAM, so a frame costs only TILEMAP writes (2 KB per layer)
 *  rather than CHR uploads. That is what buys 60 fps. The question this tool
 *  answers is whether the strip tileset actually fits in the VRAM left over.
 *
 *  Everything here turns on DEDUPLICATION. A wall at distance N and the same
 *  wall at distance N+1 share most of their 8x8 slices, and far-away scales
 *  collapse almost entirely. Counting generated tiles says the design is
 *  impossible; counting DISTINCT tiles is the real number, and only a bake can
 *  produce it.
 *
 *  Public domain (CC0). No warranty.
 * ============================================================ */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#define TEX_W      64
#define TEX_H      64
#define SCREEN_W  240
#define SCREEN_H  208                   /* 8/8 letterbox, matching the FMV path */
#define TILE        8
#define ROWS      (SCREEN_H / TILE)     /* 26 */
#define COLS      (SCREEN_W / TILE)     /* 30 */
#define MAX_WALL_H ROWS                 /* a wall can fill the view */

/* SNES VRAM and what the rest of the design costs.
 *
 * A 32x32 tilemap occupies 2 KB whatever the visible height, so 208 vs 200 costs
 * no VRAM — only 64 more bytes of tilemap DMA per layer, and 8 fewer blank
 * lines to do it in. BG3 (floor/ceiling) is STATIC, so its tilemap is uploaded
 * once and never counts against the per-frame budget. */
#define VRAM_BYTES     65536
#define TILEMAP_BYTES   2048            /* 32x32 words, per layer */
#define SHADE_CHR       (16 * 32)       /* 16 solid shade tiles, 4bpp */
#define FLOOR_CHR       (4 * 16)        /* 4 solid tiles, 2bpp BG3    */
#define N_TILEMAPS      3               /* BG1 walls, BG2 shade, BG3 floor */

/* Per-frame DMA. Only BG1 and BG2 are rewritten; BG3 is static. Rows are
 * written full-width (32) because a partial row is not contiguous. */
#define DYN_TILEMAPS    2
#define DMA_PER_FRAME   (DYN_TILEMAPS * ROWS * 32 * 2)
/* Measured blank-line DMA rate from snes/dma_rate_test.s on bsnes-accuracy. */
#define BYTES_PER_LINE  163
#define BLANK_LINES     (262 - SCREEN_H)

/* ---- synthetic wall textures, 15 colours + transparent ------------------ */

static uint8_t tex[8][TEX_H][TEX_W];
static int n_tex;

/* Each maker takes a `variant` so N textures are genuinely DISTINCT. Building
 * eight textures from four makers made the texture-count sweep meaningless —
 * dedup collapsed the duplicates and the distinct count simply stopped moving,
 * which reads as "textures are free" rather than "the harness repeated itself". */
static void make_brick(uint8_t t[TEX_H][TEX_W], int variant) {
    const int bh = 6 + (variant % 3) * 2;          /* brick height   */
    const int bw = 12 + (variant % 4) * 4;         /* brick width    */
    for (int y = 0; y < TEX_H; ++y)
        for (int x = 0; x < TEX_W; ++x) {
            const int off = ((y / bh) & 1) ? bw / 2 : 0;
            const int mortar = (y % bh == 0) || (((x + off) % bw) == 0);
            t[y][x] = mortar ? (uint8_t)(3 + variant % 2)
                             : (uint8_t)(6 + ((x + y * 3 + variant) % 3));
        }
}
static void make_panel(uint8_t t[TEX_H][TEX_W], int variant) {
    const int pw = 12 + (variant % 3) * 6;
    for (int y = 0; y < TEX_H; ++y)
        for (int x = 0; x < TEX_W; ++x) {
            const int edge = (x % pw < 2) || (x % pw > pw - 3) || y < 2 || y > TEX_H - 3;
            t[y][x] = edge ? (uint8_t)(4 + variant % 3)
                           : (uint8_t)(9 + ((y / (3 + variant % 3)) % 2));
        }
}
/* Horizontally UNIFORM: bands only. The cheap case — every 8-px column of the
 * texture is identical, so horizontal variants collapse to one. */
static void make_bands(uint8_t t[TEX_H][TEX_W], int variant) {
    const int band = 2 + (variant % 5);
    for (int y = 0; y < TEX_H; ++y)
        for (int x = 0; x < TEX_W; ++x)
            t[y][x] = (uint8_t)(2 + ((y / band + variant) % 7));
}
static void make_noise(uint8_t t[TEX_H][TEX_W], int variant) {
    uint32_t s = 12345u + (uint32_t)variant * 7919u;
    for (int y = 0; y < TEX_H; ++y)
        for (int x = 0; x < TEX_W; ++x) {
            s = s * 1103515245u + 12345u;
            t[y][x] = (uint8_t)(1 + ((s >> 16) % 15));
        }
}

/* Fill slot i with a distinct texture, cycling the makers. */
static void make_distinct(int i) {
    switch (i % 4) {
        case 0: make_brick(tex[i], i); break;
        case 1: make_panel(tex[i], i); break;
        case 2: make_bands(tex[i], i); break;
        default: make_noise(tex[i], i); break;
    }
}

/* ---- tile set with dedup ------------------------------------------------ */

#define MAX_TILES 40000
static uint8_t  set[MAX_TILES][64];
static unsigned n_set;
static unsigned n_generated;

/* Linear scan is fine: this is a bake tool, not a hot path. */
static void add_tile(const uint8_t px[64]) {
    n_generated++;
    for (unsigned i = 0; i < n_set; ++i)
        if (!memcmp(set[i], px, 64)) return;
    if (n_set < MAX_TILES) memcpy(set[n_set++], px, 64);
}

/* Bake one wall texture at every scale bucket.
 *
 * `hstep` is the wall-height quantisation in TILES. 2 means heights of 2, 4, 6
 * ... tiles, i.e. 16-pixel steps, which keeps the wall symmetric about the
 * horizon AND tile-aligned at both ends. 1 allows 8-pixel steps but then one end
 * lands mid-tile unless the wall is off-centre.
 *
 * `hvariants` is how many distinct 8-px-wide columns of the texture are used.
 * A horizontally uniform texture needs 1; full detail on a 64-wide texture
 * needs 8. This is the multiplier that decides whether the design fits. */
static void bake(const uint8_t t[TEX_H][TEX_W], int hstep, int hvariants, int max_h) {
    uint8_t px[64];
    for (int h = hstep; h <= max_h; h += hstep) {          /* wall height, tiles */
        const int wall_px = h * TILE;
        for (int c = 0; c < hvariants; ++c) {
            const int tx0 = (c * TEX_W) / hvariants;
            for (int r = 0; r < h; ++r) {                  /* tile row in the wall */
                for (int py = 0; py < TILE; ++py) {
                    const int sy = r * TILE + py;          /* pixel down the wall */
                    const int v  = (sy * TEX_H) / wall_px; /* texture row */
                    for (int pxx = 0; pxx < TILE; ++pxx)
                        px[py * TILE + pxx] = t[v % TEX_H][(tx0 + pxx) % TEX_W];
                }
                add_tile(px);
            }
        }
    }
}

static void report(const char *label, int ntex, int hstep, int hvar, int max_h) {
    n_set = 0; n_generated = 0;
    for (int i = 0; i < ntex; ++i) bake(tex[i], hstep, hvar, max_h);

    const unsigned chr = n_set * 32u;
    const unsigned other = N_TILEMAPS * TILEMAP_BYTES + SHADE_CHR + FLOOR_CHR;
    const unsigned avail = VRAM_BYTES - other;
    printf("%-26s | %2d tex | step %d | %d hvar | %6u gen -> %5u distinct "
           "(%4.1f%%) | %5.1f KB CHR | %s\n",
           label, ntex, hstep, hvar, n_generated, n_set,
           100.0 * n_set / (n_generated ? n_generated : 1),
           chr / 1024.0,
           chr <= avail ? "FITS" : "TOO BIG");
}

int main(void) {
    for (int i = 0; i < 8; ++i) make_distinct(i);
    n_tex = 8;

    const unsigned other = N_TILEMAPS * TILEMAP_BYTES + SHADE_CHR + FLOOR_CHR;
    printf("%dx%d (%dx%d tiles)\n", SCREEN_W, SCREEN_H, COLS, ROWS);
    printf("VRAM %u B - %d tilemaps (%u) - shade CHR (%u) - floor CHR (%u)"
           " = %u B for wall strips (%u tiles)\n",
           VRAM_BYTES, N_TILEMAPS, N_TILEMAPS * TILEMAP_BYTES, SHADE_CHR, FLOOR_CHR,
           VRAM_BYTES - other, (VRAM_BYTES - other) / 32u);
    printf("per-frame DMA: %u B (BG1+BG2 tilemaps; BG3 floor is STATIC)\n",
           (unsigned)DMA_PER_FRAME);
    printf("blank-line budget: %d lines x %d B = %u B  -> %s, %u B spare for OAM\n\n",
           BLANK_LINES, BYTES_PER_LINE, (unsigned)(BLANK_LINES * BYTES_PER_LINE),
           (BLANK_LINES * BYTES_PER_LINE) > DMA_PER_FRAME ? "FITS at 60 fps" : "OVER BUDGET",
           (unsigned)(BLANK_LINES * BYTES_PER_LINE - DMA_PER_FRAME));

    printf("-- horizontal detail is the multiplier that decides this --\n");
    for (int hv = 1; hv <= 8; hv *= 2) report("mixed textures", 4, 2, hv, MAX_WALL_H);

    printf("\n-- height quantisation: 16 px steps vs 8 px steps --\n");
    report("16 px steps (aligned)", 4, 2, 2, MAX_WALL_H);
    report("8 px steps", 4, 1, 2, MAX_WALL_H);

    printf("\n-- how many textures fit, at 2 horizontal variants --\n");
    for (int nt = 1; nt <= 8; ++nt) report("texture count", nt, 2, 2, MAX_WALL_H);

    printf("\n-- the cheap case: horizontally uniform (banded) textures only --\n");
    for (int i = 0; i < 8; ++i) make_bands(tex[i], i);
    for (int nt = 2; nt <= 8; nt += 2) report("banded only", nt, 2, 1, MAX_WALL_H);

    printf("\n-- the expensive case: distinct high-frequency noise --\n");
    for (int i = 0; i < 8; ++i) make_noise(tex[i], i);
    for (int nt = 1; nt <= 8; ++nt) report("noise", nt, 2, 4, MAX_WALL_H);

    return 0;
}
