/* gen_vectors.c — co-sim vector generator for hx_strip.v.
 *
 * Uses the RTL's own C reference (runtime/hx421_metatile.c) to produce:
 *   psram.hex   — PSRAM word contents: metatile map at word 0, defs at 0x1000
 *   golden.hex  — the 64x32 reseed window (column-major) mt_lookup expects
 * The testbench loads psram.hex into its PSRAM model, drives hx_strip to reseed,
 * and diffs its staging writes against golden.hex. Same map, same defs, same
 * addressing => the RTL fetch/expand must match the reference bit-for-bit. CC0. */

#include "hx421_metatile.h"
#include <stdio.h>
#include <string.h>

#define MW   32          /* map width  in metatiles */
#define MH   32          /* map height in metatiles */
#define SIDE 2           /* tiles per metatile side (shift=1) */
#define DEFC 16          /* metatile definitions */
#define DEFS_BASE 0x1000 /* PSRAM word base of the defs (matches the TB config) */

static uint16_t map_rows[MW*MH];
static Hx421TileEntry defs[DEFC*SIDE*SIDE];

int main(void) {
    /* deterministic, recognizable contents */
    for (int i = 0; i < MW*MH; i++)             map_rows[i] = (uint16_t)(i % DEFC);
    for (int i = 0; i < DEFC*SIDE*SIDE; i++)     defs[i]     = (uint16_t)(0xA000 + i);

    Hx421MapLayer L;
    memset(&L, 0, sizeof L);
    L.map_rows = map_rows; L.map_cols = NULL;   /* NULL -> row-major, matches RTL */
    L.map_w = MW; L.map_h = MH;
    L.defs = defs; L.def_count = DEFC; L.mt_side = SIDE;
    L.oob_entry = 0xFFFF; L.wrap = 0;

    /* PSRAM image: word 0.. = map (row-major), word DEFS_BASE.. = defs */
    FILE *p = fopen("psram.hex", "w");
    int maxw = DEFS_BASE + DEFC*SIDE*SIDE;
    for (int w = 0; w < maxw; w++) {
        uint16_t v = 0;
        if (w < MW*MH)                                       v = map_rows[w];
        else if (w >= DEFS_BASE && w < DEFS_BASE + DEFC*SIDE*SIDE) v = defs[w-DEFS_BASE];
        fprintf(p, "%04x\n", v);
    }
    fclose(p);

    /* golden: the reseed window is want_l=0, want_t=0 (camera x=128,y=16):
     * columns tx=0..63, each rows ty=0..31, emitted column-major. */
    FILE *g = fopen("golden.hex", "w");
    for (int c = 0; c < 64; c++) {
        Hx421TileEntry col[32];
        hx421_metatile_column(&L, c, 0, 32, col);
        for (int r = 0; r < 32; r++) fprintf(g, "%04x\n", col[r]);
    }
    fclose(g);
    printf("gen_vectors: psram.hex + golden.hex (2048 entries) written\n");
    return 0;
}
