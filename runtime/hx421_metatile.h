/* ============================================================
 *  hx421_metatile.h — metatile map expansion (FPGA-side function)
 *
 *  A map is stored as a grid of METATILE indices; each metatile expands to an
 *  NxN block of SNES BG tilemap entries. This is what lets per-frame DMA be
 *  edge seams only: as the camera scrolls one tile, we expand and push a single
 *  column or row (64-128 B) instead of the whole 2 KB tilemap.
 *
 *      full tilemap             2048 B/frame
 *      edge seam (col or row)     64-128 B/frame
 *      3 layers                  384 B vs 6144 B      ~16x
 *
 *  This code is the reference implementation AND the specification for the RTL
 *  block that will eventually do it in fabric. Keep it dependency-free and
 *  integer-only so the two can be diffed against each other.
 *
 *  TRANSPOSED STORAGE: maps may be held twice in PSRAM — row-major AND
 *  column-major — so that BOTH strip directions are sequential bursts rather
 *  than strided random access (a 32-tile strided column costs ~3 us of pure
 *  PSRAM latency; sequential is a burst). Supply `map_cols` and column
 *  extraction uses it; leave it NULL and columns fall back to striding
 *  `map_rows`. Both paths MUST produce identical output — see the test.
 *
 *  Public domain (CC0). No warranty.
 * ============================================================ */

#ifndef HX421_METATILE_H
#define HX421_METATILE_H

#include <stdint.h>
#include <stddef.h>

/* SNES BG tilemap entry: tile 0-9, palette 10-12, priority 13, hflip 14,
 * vflip 15. Metatile definitions store whole entries, so palette and flips
 * are per sub-tile. */
typedef uint16_t Hx421TileEntry;

#define HX421_MT_MAX_SIDE 8u        /* 2, 4 or 8 tiles per metatile side */

typedef struct {
    /* Metatile index grid. `map_rows` is required (row-major, map_w x map_h).
     * `map_cols` is the optional transposed copy (column-major, map_h x map_w)
     * used to make column extraction sequential; NULL = stride map_rows. */
    const uint16_t  *map_rows;
    const uint16_t  *map_cols;
    uint16_t         map_w;         /* width  in metatiles */
    uint16_t         map_h;         /* height in metatiles */

    /* Metatile definitions: `def_count` entries, each mt_side*mt_side tilemap
     * entries in row-major order. */
    const Hx421TileEntry *defs;
    uint16_t         def_count;
    uint8_t          mt_side;       /* tiles per side: 2, 4 or 8 */

    /* Entry substituted when a coordinate falls outside the map. */
    Hx421TileEntry   oob_entry;
} Hx421MapLayer;

/* Validate a layer: non-NULL map_rows/defs, mt_side in {2,4,8}, non-zero
 * dimensions. Returns 1 if usable. Cheap; call once at layer setup, not per
 * strip. */
int hx421_metatile_layer_valid(const Hx421MapLayer *layer);

/* Expand a vertical strip: TILE column `tx`, tile rows `ty0 .. ty0+count-1`,
 * into `out[0..count-1]`. Uses `map_cols` when present (sequential reads).
 * Coordinates are in TILES, not metatiles or pixels. Out-of-range rows and
 * columns yield `oob_entry`. */
void hx421_metatile_column(const Hx421MapLayer *layer, int tx, int ty0,
                           int count, Hx421TileEntry *out);

/* Expand a horizontal strip: TILE row `ty`, tile columns `tx0 .. tx0+count-1`.
 * Always sequential in `map_rows`. */
void hx421_metatile_row(const Hx421MapLayer *layer, int ty, int tx0,
                        int count, Hx421TileEntry *out);

/* Expand a rectangular region — used for the initial full-screen fill on a
 * scene change, where seams are not enough. `out` is `w` entries per row,
 * `h` rows, with `out_stride` entries between row starts (so it can write
 * straight into a 32x32 SNES tilemap). */
void hx421_metatile_rect(const Hx421MapLayer *layer, int tx0, int ty0,
                         int w, int h, Hx421TileEntry *out, int out_stride);

/* Build the transposed (column-major) copy of a row-major metatile map.
 * `dst` must hold map_w*map_h uint16. Done once at map load, in PSRAM. */
void hx421_metatile_transpose(const uint16_t *src_rows, uint16_t map_w,
                              uint16_t map_h, uint16_t *dst_cols);

#endif /* HX421_METATILE_H */
