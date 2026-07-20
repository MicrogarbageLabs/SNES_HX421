/* ============================================================
 *  hx421_raster.h — TBDR tile rasteriser (reference + RTL spec)
 *
 *  Bins and rasterises at 8x8, the SNES CHR tile size, so a rasterised tile
 *  IS the output rather than something to convert. The per-tile working set is
 *  therefore 160 B — 32 B of colour and 128 B of full 16-bit z — instead of a
 *  screen-sized framebuffer. That is the whole reason TBDR suits this hardware.
 *
 *  The classify step is not an optimisation, it is the mechanism: a tile whose
 *  64 pixels share one index needs NO CHR upload at all, only a tilemap entry
 *  pointing at a resident solid tile. CHR-to-VRAM DMA is the binding constraint
 *  (750 unique tiles = 24 KB against a ~6.2 KB vblank), so the solid fraction
 *  of a scene sets the frame rate. See docs/tbdr.md.
 *
 *  Dependency-free and integer-only so the fabric block can be diffed against
 *  it in simulation, exactly like hx421_metatile.c.
 *
 *  Public domain (CC0). No warranty.
 * ============================================================ */

#ifndef HX421_RASTER_H
#define HX421_RASTER_H

#include <stdint.h>

#define HX421_TILE    8u                          /* SNES CHR tile side      */
#define HX421_TPIX    (HX421_TILE * HX421_TILE)   /* 64 pixels               */
#define HX421_SUBPX   8                           /* fractional bits on x,y  */
#define HX421_ONE     (1 << HX421_SUBPX)          /* 1.0 in screen fixed pt  */
#define HX421_ZFAR    0xFFFFu                     /* cleared depth (farthest)*/

/* Screen-space vertex. x,y carry HX421_SUBPX fractional bits; z is 16-bit
 * with SMALLER MEANING NEARER, so the depth test is a plain less-than. */
typedef struct {
    int32_t  x, y;
    uint16_t z;
} Hx421Vtx;

/* Flat-shaded untextured triangle. `color` is a 4bpp palette index 1..15 —
 * index 0 is reserved for "nothing drawn here", which is what lets a tile be
 * classified EMPTY and skipped entirely. */
typedef struct {
    Hx421Vtx v[3];
    uint8_t  color;
} Hx421Tri;

typedef enum {
    HX421_TILE_EMPTY = 0,   /* nothing drawn — no CHR, no tilemap change    */
    HX421_TILE_SOLID,       /* all 64 pixels one index — resident solid tile */
    HX421_TILE_MIXED        /* needs a unique CHR upload                     */
} Hx421TileKind;

/* Clear a tile's colour and depth. Colour 0 = untouched, depth = ZFAR. */
void hx421_tile_clear(uint8_t idx[HX421_TPIX], uint16_t zbuf[HX421_TPIX]);

/* Rasterise every triangle in `tris` into the tile whose top-left pixel is
 * (tx*8, ty*8). Triangles not covering the tile are rejected cheaply, so the
 * caller may pass an unbinned list; binning only saves the reject cost.
 * Depth-tests and writes both buffers. Returns pixels written (post-z). */
int hx421_raster_tile(const Hx421Tri *tris, int count, int tx, int ty,
                      uint8_t idx[HX421_TPIX], uint16_t zbuf[HX421_TPIX]);

/* Classify a rasterised tile. On SOLID, *solid_color receives the index.
 * EMPTY means every pixel is still 0. */
Hx421TileKind hx421_tile_classify(const uint8_t idx[HX421_TPIX], uint8_t *solid_color);

/* Pack 64 colour indices into one SNES 4bpp tile (32 B): bitplanes 0/1
 * interleaved per row in the first 16 bytes, planes 2/3 in the second 16. */
void hx421_tile_pack4bpp(const uint8_t idx[HX421_TPIX], uint8_t out[32]);

/* Does a triangle's bounding box touch this tile? The binning predicate,
 * exposed so the caller can build tile lists with the same test the
 * rasteriser uses — a mismatch between them drops or duplicates geometry. */
int hx421_tri_touches_tile(const Hx421Tri *t, int tx, int ty);

#endif /* HX421_RASTER_H */
