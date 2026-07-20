/* ============================================================
 *  hx421_metatile_test.c — metatile expansion checks
 *
 *  The load-bearing property is that column extraction via the TRANSPOSED map
 *  and via striding the row-major map produce byte-identical output. If they
 *  ever diverge, the bug is data-dependent and shows up as occasional wrong
 *  tiles at a scroll seam — miserable to find later, trivial to catch here.
 *
 *  Build: gcc -std=c11 -Wall -O2 tools/hx421_metatile_test.c \
 *             runtime/hx421_metatile.c -I runtime -o metatile_test
 *
 *  Public domain (CC0). No warranty.
 * ============================================================ */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "hx421_metatile.h"

static int failures = 0;
static void check(int cond, const char *what) {
    if (cond) { printf("  PASS %s\n", what); }
    else      { printf("  FAIL %s\n", what); failures++; }
}

#define MAP_W 20
#define MAP_H 14
#define DEFS  64
#define MAXSIDE 8

static uint16_t       map_rows[MAP_W * MAP_H];
static uint16_t       map_cols[MAP_W * MAP_H];
static Hx421TileEntry defs[DEFS * MAXSIDE * MAXSIDE];

/* Each definition entry encodes (metatile, sub-y, sub-x) so the expected value
 * at any tile coordinate is computable in closed form. */
static Hx421TileEntry encode(int mt, int sy, int sx) {
    return (Hx421TileEntry)(((mt & 0xFF) << 8) | ((sy & 0xF) << 4) | (sx & 0xF));
}

static void build(int side) {
    for (int mt = 0; mt < DEFS; ++mt)
        for (int sy = 0; sy < side; ++sy)
            for (int sx = 0; sx < side; ++sx)
                defs[mt * side * side + sy * side + sx] = encode(mt, sy, sx);
    for (int y = 0; y < MAP_H; ++y)
        for (int x = 0; x < MAP_W; ++x)
            map_rows[y * MAP_W + x] = (uint16_t)((y * MAP_W + x) % DEFS);
    hx421_metatile_transpose(map_rows, MAP_W, MAP_H, map_cols);
}

static Hx421TileEntry expected(int tx, int ty, int side, Hx421TileEntry oob) {
    int shift = (side == 2) ? 1 : (side == 4) ? 2 : 3;
    int mtx = tx >> shift, mty = ty >> shift;
    if (tx < 0 || ty < 0 || mtx >= MAP_W || mty >= MAP_H) return oob;
    int mt = map_rows[mty * MAP_W + mtx];
    return encode(mt, ty & (side - 1), tx & (side - 1));
}

int main(void) {
    const Hx421TileEntry OOB = 0xBEEF;
    printf("==== metatile expansion ====\n");

    for (int si = 0; si < 3; ++si) {
        const int side = (si == 0) ? 2 : (si == 1) ? 4 : 8;
        build(side);

        Hx421MapLayer L = {
            .map_rows = map_rows, .map_cols = map_cols,
            .map_w = MAP_W, .map_h = MAP_H,
            .defs = defs, .def_count = DEFS,
            .mt_side = (uint8_t)side, .oob_entry = OOB,
        };
        Hx421MapLayer L_norows = L;      /* same layer, no transposed copy */
        L_norows.map_cols = NULL;

        printf("-- mt_side = %d --\n", side);
        check(hx421_metatile_layer_valid(&L), "layer validates");

        /* 1. transposed vs strided column extraction — the key invariant */
        int mismatch = 0, oob_seen = 0;
        for (int tx = -2; tx < MAP_W * side + 2; ++tx) {
            Hx421TileEntry a[40], b[40];
            hx421_metatile_column(&L,        tx, -1, 40, a);   /* uses map_cols */
            hx421_metatile_column(&L_norows, tx, -1, 40, b);   /* strides rows  */
            if (memcmp(a, b, sizeof a) != 0) mismatch++;
            for (int i = 0; i < 40; ++i) if (a[i] == OOB) oob_seen++;
        }
        check(mismatch == 0, "column: transposed == strided");
        check(oob_seen > 0,  "column: out-of-range yields oob_entry");

        /* 2. values match the closed form */
        int wrong = 0;
        for (int tx = 0; tx < MAP_W * side; ++tx) {
            Hx421TileEntry col[32];
            hx421_metatile_column(&L, tx, 0, 32, col);
            for (int i = 0; i < 32; ++i)
                if (col[i] != expected(tx, i, side, OOB)) wrong++;
        }
        check(wrong == 0, "column: matches closed-form expectation");

        wrong = 0;
        for (int ty = 0; ty < MAP_H * side; ++ty) {
            Hx421TileEntry row[32];
            hx421_metatile_row(&L, ty, 0, 32, row);
            for (int i = 0; i < 32; ++i)
                if (row[i] != expected(i, ty, side, OOB)) wrong++;
        }
        check(wrong == 0, "row: matches closed-form expectation");

        /* 3. rect agrees with row/column at the same coordinates, and honours
         *    out_stride (written into a 32-wide SNES tilemap) */
        static Hx421TileEntry rect[32 * 32];
        for (int i = 0; i < 32 * 32; ++i) rect[i] = 0;
        hx421_metatile_rect(&L, 3, 5, 28, 26, rect, 32);
        wrong = 0;
        for (int y = 0; y < 26; ++y)
            for (int x = 0; x < 28; ++x)
                if (rect[y * 32 + x] != expected(3 + x, 5 + y, side, OOB)) wrong++;
        check(wrong == 0, "rect: matches expectation with out_stride");

        int untouched = 1;                       /* columns 28..31 must be clear */
        for (int y = 0; y < 26; ++y)
            for (int x = 28; x < 32; ++x)
                if (rect[y * 32 + x] != 0) untouched = 0;
        check(untouched, "rect: does not write past w within the stride");

        /* 4. a metatile index beyond def_count degrades to oob, not UB */
        map_rows[0] = DEFS + 7;
        hx421_metatile_transpose(map_rows, MAP_W, MAP_H, map_cols);
        Hx421TileEntry one[1];
        hx421_metatile_column(&L, 0, 0, 1, one);
        check(one[0] == OOB, "out-of-range metatile index yields oob_entry");
        map_rows[0] = 0;
    }

    /* 5. torus mode: never yields oob, and is periodic in both axes */
    {
        const int side = 2;
        build(side);
        Hx421MapLayer W = {
            .map_rows = map_rows, .map_cols = map_cols,
            .map_w = MAP_W, .map_h = MAP_H,
            .defs = defs, .def_count = DEFS,
            .mt_side = (uint8_t)side, .oob_entry = OOB, .wrap = 1,
        };
        Hx421MapLayer W_norows = W; W_norows.map_cols = NULL;

        printf("-- torus (wrap = 1) --\n");
        const int period_x = MAP_W * side, period_y = MAP_H * side;

        int any_oob = 0, aperiodic = 0, mismatch = 0;
        for (int tx = -2 * period_x; tx < 2 * period_x; tx += 3) {
            Hx421TileEntry a[16], b[16], shifted[16];
            hx421_metatile_column(&W,        tx, -5, 16, a);
            hx421_metatile_column(&W_norows, tx, -5, 16, b);
            hx421_metatile_column(&W, tx + period_x, -5, 16, shifted);
            if (memcmp(a, b, sizeof a) != 0)       mismatch++;
            if (memcmp(a, shifted, sizeof a) != 0) aperiodic++;
            for (int i = 0; i < 16; ++i) if (a[i] == OOB) any_oob++;
        }
        check(any_oob == 0,   "torus: negative and past-end coords never yield oob");
        check(aperiodic == 0, "torus: column at tx == column at tx + period");
        check(mismatch == 0,  "torus: transposed == strided");

        int rows_aperiodic = 0;
        for (int ty = -period_y; ty < period_y; ty += 3) {
            Hx421TileEntry a[16], shifted[16];
            hx421_metatile_row(&W, ty, -7, 16, a);
            hx421_metatile_row(&W, ty + period_y, -7, 16, shifted);
            if (memcmp(a, shifted, sizeof a) != 0) rows_aperiodic++;
        }
        check(rows_aperiodic == 0, "torus: row at ty == row at ty + period");
    }

    /* 6. bad configurations are rejected rather than crashing */
    {
        Hx421MapLayer bad = { .map_rows = map_rows, .map_w = MAP_W, .map_h = MAP_H,
                              .defs = defs, .def_count = DEFS, .mt_side = 3 };
        check(!hx421_metatile_layer_valid(&bad), "mt_side=3 rejected (not power of two)");
        bad.mt_side = 4; bad.defs = NULL;
        check(!hx421_metatile_layer_valid(&bad), "NULL defs rejected");
    }

    /* 6. the DMA saving this whole exercise exists for */
    printf("-- seam vs full tilemap --\n");
    printf("   full 32x32 tilemap        : %d B\n", 32 * 32 * 2);
    printf("   one 32-entry column seam  : %d B\n", 32 * 2);
    printf("   3 layers, seam only       : %d B  (vs %d B)\n", 3 * 32 * 2, 3 * 32 * 32 * 2);

    printf("\n==== METATILE %s (%d failures) ====\n",
           failures ? "FAILED" : "OK", failures);
    return failures ? 1 : 0;
}
