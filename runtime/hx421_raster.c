/* ============================================================
 *  hx421_raster.c — TBDR tile rasteriser
 *
 *  Half-space edge functions, evaluated incrementally. For each edge
 *      E(x,y) = A*(x - x0) + B*(y - y0),  A = y1-y0, B = -(x1-x0)
 *  a pixel is inside when all three E >= 0 (counter-clockwise winding). Because
 *  E is affine, stepping one pixel right adds A*ONE and stepping down adds
 *  B*ONE — no multiplies in the inner loop, which is what makes this cheap in
 *  fabric as well as in C.
 *
 *  Depth is interpolated from the triangle's plane the same way: dz/dx and
 *  dz/dy computed once at setup, then added per step.
 *
 *  Public domain (CC0). No warranty.
 * ============================================================ */

#include "hx421_raster.h"

/* Edge values are products of two screen-space fixed-point deltas. At 240x200
 * with 8 fractional bits those reach ~61440 each, so the product overflows
 * int32 — hence int64 here. A fabric implementation does not need the full
 * width: the useful range is bounded by the tile, and the incremental form
 * means only the accumulator must hold it. */
typedef int64_t edge_t;

void hx421_tile_clear(uint8_t idx[HX421_TPIX], uint16_t zbuf[HX421_TPIX]) {
    for (unsigned i = 0; i < HX421_TPIX; ++i) { idx[i] = 0; zbuf[i] = HX421_ZFAR; }
}

int hx421_tri_touches_tile(const Hx421Tri *t, int tx, int ty) {
    if (!t) return 0;
    int32_t minx = t->v[0].x, maxx = minx, miny = t->v[0].y, maxy = miny;
    for (int i = 1; i < 3; ++i) {
        if (t->v[i].x < minx) minx = t->v[i].x;
        if (t->v[i].x > maxx) maxx = t->v[i].x;
        if (t->v[i].y < miny) miny = t->v[i].y;
        if (t->v[i].y > maxy) maxy = t->v[i].y;
    }
    const int32_t x0 = (int32_t)(tx * (int)HX421_TILE) * HX421_ONE;
    const int32_t y0 = (int32_t)(ty * (int)HX421_TILE) * HX421_ONE;
    const int32_t x1 = x0 + (int32_t)HX421_TILE * HX421_ONE;
    const int32_t y1 = y0 + (int32_t)HX421_TILE * HX421_ONE;
    return !(maxx < x0 || minx >= x1 || maxy < y0 || miny >= y1);
}

int hx421_raster_tile(const Hx421Tri *tris, int count, int tx, int ty,
                      uint8_t idx[HX421_TPIX], uint16_t zbuf[HX421_TPIX]) {
    if (!tris || !idx || !zbuf || count <= 0) return 0;
    int written = 0;

    /* Pixel CENTRES: sampling at the centre rather than the corner is what
     * keeps shared edges from double-covering or leaving gaps. */
    const int32_t ox = (int32_t)(tx * (int)HX421_TILE) * HX421_ONE + HX421_ONE / 2;
    const int32_t oy = (int32_t)(ty * (int)HX421_TILE) * HX421_ONE + HX421_ONE / 2;

    for (int n = 0; n < count; ++n) {
        const Hx421Tri *t = &tris[n];
        if (t->color == 0) continue;                  /* 0 means "not drawn" */
        if (!hx421_tri_touches_tile(t, tx, ty)) continue;

        const int32_t x0 = t->v[0].x, y0 = t->v[0].y;
        const int32_t x1 = t->v[1].x, y1 = t->v[1].y;
        const int32_t x2 = t->v[2].x, y2 = t->v[2].y;

        /* Signed area x2. Zero is degenerate; negative is back-facing, which
         * we cull rather than draw with inverted edge tests. */
        const edge_t area = (edge_t)(x1 - x0) * (y2 - y0) - (edge_t)(y1 - y0) * (x2 - x0);
        if (area <= 0) continue;

        /* E(p) = cross(v1-v0, p-v0), so "inside" (E >= 0) agrees in sign with
         * `area` above. Writing A = y1-y0 instead negates E and inverts the
         * test — every front-facing triangle then rasterises as empty. */
        const int32_t A0 = -(y1 - y0), B0 = (x1 - x0);
        const int32_t A1 = -(y2 - y1), B1 = (x2 - x1);
        const int32_t A2 = -(y0 - y2), B2 = (x0 - x2);

        edge_t e0 = (edge_t)A0 * (ox - x0) + (edge_t)B0 * (oy - y0);
        edge_t e1 = (edge_t)A1 * (ox - x1) + (edge_t)B1 * (oy - y1);
        edge_t e2 = (edge_t)A2 * (ox - x2) + (edge_t)B2 * (oy - y2);

        /* Depth plane: solve z as a linear function of screen x,y from the
         * three vertices. Scaled by `area` so the divide happens once. */
        const int32_t dz1 = (int32_t)t->v[1].z - (int32_t)t->v[0].z;
        const int32_t dz2 = (int32_t)t->v[2].z - (int32_t)t->v[0].z;
        const edge_t zdx_num = (edge_t)dz1 * (y2 - y0) - (edge_t)dz2 * (y1 - y0);
        const edge_t zdy_num = (edge_t)dz2 * (x1 - x0) - (edge_t)dz1 * (x2 - x0);

        const edge_t stepx = (edge_t)HX421_ONE;
        for (unsigned py = 0; py < HX421_TILE; ++py) {
            edge_t r0 = e0, r1 = e1, r2 = e2;
            for (unsigned px = 0; px < HX421_TILE; ++px) {
                if ((r0 | r1 | r2) >= 0) {            /* all three non-negative */
                    /* z at this pixel, from the plane, rounded toward zero */
                    const edge_t dx = (edge_t)(ox + (edge_t)px * HX421_ONE - x0);
                    const edge_t dy = (edge_t)(oy + (edge_t)py * HX421_ONE - y0);
                    /* zdx_num*dx is (dz * Q8 * Q8) and area is Q8^2, so the
                     * quotient is already in depth units — dividing by an
                     * extra HX421_ONE would flatten every gradient by 256x,
                     * which constant-z test geometry would never reveal. */
                    edge_t zz = (edge_t)t->v[0].z
                              + (zdx_num * dx + zdy_num * dy) / area;
                    if (zz < 0)               zz = 0;
                    if (zz > HX421_ZFAR)      zz = HX421_ZFAR;

                    const unsigned o = py * HX421_TILE + px;
                    if ((uint16_t)zz < zbuf[o]) {     /* smaller = nearer */
                        zbuf[o] = (uint16_t)zz;
                        idx[o]  = t->color;
                        written++;
                    }
                }
                r0 += (edge_t)A0 * stepx;
                r1 += (edge_t)A1 * stepx;
                r2 += (edge_t)A2 * stepx;
            }
            e0 += (edge_t)B0 * stepx;
            e1 += (edge_t)B1 * stepx;
            e2 += (edge_t)B2 * stepx;
        }
    }
    return written;
}

Hx421TileKind hx421_tile_classify(const uint8_t idx[HX421_TPIX], uint8_t *solid_color) {
    if (!idx) return HX421_TILE_EMPTY;
    const uint8_t first = idx[0];
    for (unsigned i = 1; i < HX421_TPIX; ++i)
        if (idx[i] != first) return HX421_TILE_MIXED;
    if (first == 0) return HX421_TILE_EMPTY;
    if (solid_color) *solid_color = first;
    return HX421_TILE_SOLID;
}

void hx421_tile_pack4bpp(const uint8_t idx[HX421_TPIX], uint8_t out[32]) {
    if (!idx || !out) return;
    for (int i = 0; i < 32; ++i) out[i] = 0;
    for (unsigned y = 0; y < HX421_TILE; ++y) {
        uint8_t p0 = 0, p1 = 0, p2 = 0, p3 = 0;
        for (unsigned x = 0; x < HX421_TILE; ++x) {
            const uint8_t c   = idx[y * HX421_TILE + x] & 0x0Fu;
            const uint8_t bit = (uint8_t)(0x80u >> x);   /* pixel 0 is the MSB */
            if (c & 1u) p0 |= bit;
            if (c & 2u) p1 |= bit;
            if (c & 4u) p2 |= bit;
            if (c & 8u) p3 |= bit;
        }
        out[2 * y]          = p0;
        out[2 * y + 1]      = p1;
        out[16 + 2 * y]     = p2;
        out[16 + 2 * y + 1] = p3;
    }
}
