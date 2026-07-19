/* ============================================================
 *  hx421_metatile.c — metatile map expansion
 *
 *  Reference implementation and RTL specification. See the header.
 *
 *  Structure note: every lookup decomposes a TILE coordinate into
 *  (metatile index, sub-tile offset) by division and modulo. mt_side is
 *  restricted to powers of two so both become a shift and a mask — which is
 *  what makes this cheap in fabric as well as in C.
 *
 *  Public domain (CC0). No warranty.
 * ============================================================ */

#include "hx421_metatile.h"

/* log2 of a power-of-two side (2->1, 4->2, 8->3); 0 if not a valid side. */
static uint8_t mt_shift(uint8_t side) {
    switch (side) {
        case 2: return 1;
        case 4: return 2;
        case 8: return 3;
        default: return 0;
    }
}

int hx421_metatile_layer_valid(const Hx421MapLayer *layer) {
    if (!layer || !layer->map_rows || !layer->defs) return 0;
    if (!layer->map_w || !layer->map_h || !layer->def_count) return 0;
    if (!mt_shift(layer->mt_side)) return 0;
    return 1;
}

/* Fetch the tilemap entry at TILE coordinate (tx, ty).
 * `prefer_cols` selects the transposed copy when available — the caller knows
 * which access pattern it is walking, the lookup does not. */
static Hx421TileEntry mt_lookup(const Hx421MapLayer *L, int tx, int ty,
                                uint8_t shift, int prefer_cols) {
    const int mtx = tx >> shift;                 /* metatile coords */
    const int mty = ty >> shift;
    if (mtx < 0 || mty < 0 || mtx >= (int)L->map_w || mty >= (int)L->map_h)
        return L->oob_entry;

    uint16_t mt = prefer_cols && L->map_cols
                ? L->map_cols[(size_t)mtx * L->map_h + (size_t)mty]   /* transposed */
                : L->map_rows[(size_t)mty * L->map_w + (size_t)mtx];
    if (mt >= L->def_count) return L->oob_entry;

    const int side = (int)L->mt_side;
    const int sx   = tx & (side - 1);            /* sub-tile within metatile */
    const int sy   = ty & (side - 1);
    return L->defs[(size_t)mt * (size_t)side * (size_t)side
                 + (size_t)sy * (size_t)side + (size_t)sx];
}

void hx421_metatile_column(const Hx421MapLayer *layer, int tx, int ty0,
                           int count, Hx421TileEntry *out) {
    if (!out || count <= 0) return;
    if (!hx421_metatile_layer_valid(layer)) return;
    const uint8_t shift = mt_shift(layer->mt_side);
    for (int i = 0; i < count; ++i)
        out[i] = mt_lookup(layer, tx, ty0 + i, shift, /*prefer_cols=*/1);
}

void hx421_metatile_row(const Hx421MapLayer *layer, int ty, int tx0,
                        int count, Hx421TileEntry *out) {
    if (!out || count <= 0) return;
    if (!hx421_metatile_layer_valid(layer)) return;
    const uint8_t shift = mt_shift(layer->mt_side);
    for (int i = 0; i < count; ++i)
        out[i] = mt_lookup(layer, tx0 + i, ty, shift, /*prefer_cols=*/0);
}

void hx421_metatile_rect(const Hx421MapLayer *layer, int tx0, int ty0,
                         int w, int h, Hx421TileEntry *out, int out_stride) {
    if (!out || w <= 0 || h <= 0) return;
    if (!hx421_metatile_layer_valid(layer)) return;
    const uint8_t shift = mt_shift(layer->mt_side);
    for (int y = 0; y < h; ++y) {
        Hx421TileEntry *row = out + (size_t)y * (size_t)out_stride;
        for (int x = 0; x < w; ++x)
            row[x] = mt_lookup(layer, tx0 + x, ty0 + y, shift, /*prefer_cols=*/0);
    }
}

void hx421_metatile_transpose(const uint16_t *src_rows, uint16_t map_w,
                              uint16_t map_h, uint16_t *dst_cols) {
    if (!src_rows || !dst_cols) return;
    for (uint16_t y = 0; y < map_h; ++y)
        for (uint16_t x = 0; x < map_w; ++x)
            dst_cols[(size_t)x * map_h + y] = src_rows[(size_t)y * map_w + x];
}
