/* ============================================================
 *  hx421_raster_test.c — TBDR rasteriser checks
 *
 *  The load-bearing properties: the depth test actually orders overlapping
 *  geometry, classification identifies solid tiles (which is what sets the
 *  frame rate), the binning predicate agrees with what the rasteriser draws,
 *  and the 4bpp pack round-trips.
 *
 *  Also measures the SOLID FRACTION of a synthetic scene, since that number —
 *  not fill rate — determines how many frames per second the DMA budget allows.
 *
 *  Build: gcc -std=c11 -Wall -O2 -I runtime tools/hx421_raster_test.c \
 *             runtime/hx421_raster.c -o raster_test
 *
 *  Public domain (CC0). No warranty.
 * ============================================================ */

#include <stdio.h>
#include <string.h>

#include "hx421_raster.h"

static int failures = 0;
static void check(int cond, const char *what) {
    if (cond) printf("  PASS %s\n", what);
    else    { printf("  FAIL %s\n", what); failures++; }
}

#define PX(v) ((int32_t)((v) * HX421_ONE))

static Hx421Tri quad_tri(int x0, int y0, int x1, int y1, uint16_t z, uint8_t c, int second) {
    Hx421Tri t;
    /* Wound so the signed area is POSITIVE (front-facing) with screen Y down. */
    if (!second) {
        t.v[0] = (Hx421Vtx){ PX(x0), PX(y0), z };
        t.v[1] = (Hx421Vtx){ PX(x1), PX(y1), z };
        t.v[2] = (Hx421Vtx){ PX(x0), PX(y1), z };
    } else {
        t.v[0] = (Hx421Vtx){ PX(x0), PX(y0), z };
        t.v[1] = (Hx421Vtx){ PX(x1), PX(y0), z };
        t.v[2] = (Hx421Vtx){ PX(x1), PX(y1), z };
    }
    t.color = c;
    return t;
}

int main(void) {
    uint8_t  idx[HX421_TPIX];
    uint16_t zb[HX421_TPIX];
    uint8_t  solid = 0;

    printf("==== TBDR rasteriser ====\n");

    /* 1. an empty tile stays empty */
    hx421_tile_clear(idx, zb);
    check(hx421_tile_classify(idx, &solid) == HX421_TILE_EMPTY, "cleared tile classifies EMPTY");

    /* 2. a quad covering the whole tile classifies SOLID with its colour */
    {
        Hx421Tri q[2] = { quad_tri(0, 0, 8, 8, 100, 5, 0), quad_tri(0, 0, 8, 8, 100, 5, 1) };
        hx421_tile_clear(idx, zb);
        int w = hx421_raster_tile(q, 2, 0, 0, idx, zb);
        check(w == (int)HX421_TPIX, "full-cover quad writes all 64 pixels");
        check(hx421_tile_classify(idx, &solid) == HX421_TILE_SOLID && solid == 5,
              "full-cover quad classifies SOLID with its colour");
    }

    /* 3. partial cover is MIXED — the case that costs a CHR upload */
    {
        Hx421Tri half[2] = { quad_tri(0, 0, 4, 8, 100, 3, 0), quad_tri(0, 0, 4, 8, 100, 3, 1) };
        hx421_tile_clear(idx, zb);
        hx421_raster_tile(half, 2, 0, 0, idx, zb);
        check(hx421_tile_classify(idx, &solid) == HX421_TILE_MIXED, "half-cover classifies MIXED");
        check(idx[0] == 3 && idx[7] == 0, "half-cover fills the left, leaves the right");
    }

    /* 4. THE DEPTH TEST: draw far then near, and near must win — then repeat
     *    in the opposite submission order and get the same picture. Order
     *    independence is the whole point of having a z buffer. */
    {
        Hx421Tri far_[2]  = { quad_tri(0, 0, 8, 8, 400, 7, 0), quad_tri(0, 0, 8, 8, 400, 7, 1) };
        Hx421Tri near_[2] = { quad_tri(0, 0, 8, 8, 100, 9, 0), quad_tri(0, 0, 8, 8, 100, 9, 1) };

        Hx421Tri a[4]; memcpy(a, far_, sizeof far_); memcpy(a + 2, near_, sizeof near_);
        hx421_tile_clear(idx, zb);
        hx421_raster_tile(a, 4, 0, 0, idx, zb);
        hx421_tile_classify(idx, &solid);
        int near_wins_1 = (solid == 9);

        Hx421Tri b[4]; memcpy(b, near_, sizeof near_); memcpy(b + 2, far_, sizeof far_);
        hx421_tile_clear(idx, zb);
        hx421_raster_tile(b, 4, 0, 0, idx, zb);
        hx421_tile_classify(idx, &solid);
        int near_wins_2 = (solid == 9);

        check(near_wins_1, "near occludes far (far submitted first)");
        check(near_wins_2, "near occludes far (near submitted first) — order independent");
    }

    /* 5. back-facing triangles are culled, not drawn inverted */
    {
        Hx421Tri back = quad_tri(0, 0, 8, 8, 100, 6, 0);
        Hx421Vtx sw = back.v[1]; back.v[1] = back.v[2]; back.v[2] = sw;   /* flip winding */
        hx421_tile_clear(idx, zb);
        int w = hx421_raster_tile(&back, 1, 0, 0, idx, zb);
        check(w == 0, "back-facing triangle is culled");
    }

    /* 6. the binning predicate agrees with what actually gets drawn — a
     *    mismatch here silently drops or duplicates geometry */
    {
        Hx421Tri t = quad_tri(0, 0, 8, 8, 100, 4, 0);
        int disagree = 0;
        for (int ty = -1; ty < 4; ++ty)
            for (int tx = -1; tx < 4; ++tx) {
                hx421_tile_clear(idx, zb);
                int drew  = hx421_raster_tile(&t, 1, tx, ty, idx, zb) > 0;
                int binned = hx421_tri_touches_tile(&t, tx, ty);
                if (drew && !binned) disagree++;    /* drawn but not binned = dropped */
            }
        check(disagree == 0, "binning predicate covers everything the rasteriser draws");
    }

    /* 6b. INTERPOLATED depth: a quad sloping in z must vary across the tile,
     *     and a near sloping surface must beat a far flat one only where it is
     *     actually nearer. Constant-z geometry cannot catch a broken gradient. */
    {
        Hx421Tri slope;
        slope.v[0] = (Hx421Vtx){ PX(0), PX(0), 100 };
        slope.v[1] = (Hx421Vtx){ PX(8), PX(0), 900 };
        slope.v[2] = (Hx421Vtx){ PX(0), PX(8), 100 };
        slope.color = 4;
        hx421_tile_clear(idx, zb);
        hx421_raster_tile(&slope, 1, 0, 0, idx, zb);
        int left = zb[0], right = zb[7];
        check(left < right && left < 300 && right > 600,
              "depth interpolates across the tile (left near, right far)");

        /* a flat plane at mid depth should win on the left, lose on the right */
        Hx421Tri flat[2] = { quad_tri(0, 0, 8, 8, 500, 8, 0), quad_tri(0, 0, 8, 8, 500, 8, 1) };
        hx421_tile_clear(idx, zb);
        hx421_raster_tile(&slope, 1, 0, 0, idx, zb);
        hx421_raster_tile(flat, 2, 0, 0, idx, zb);
        check(idx[0] == 4 && idx[7] == 8,
              "z-test picks the nearer surface per pixel, not per triangle");
    }

    /* 7. 4bpp pack round-trips */
    {
        for (unsigned i = 0; i < HX421_TPIX; ++i) idx[i] = (uint8_t)(i & 0x0Fu);
        uint8_t packed[32];
        hx421_tile_pack4bpp(idx, packed);
        int bad = 0;
        for (unsigned y = 0; y < 8; ++y)
            for (unsigned x = 0; x < 8; ++x) {
                uint8_t bit = (uint8_t)(0x80u >> x);
                uint8_t c = (uint8_t)(((packed[2*y]     & bit) ? 1 : 0)
                                    | ((packed[2*y+1]   & bit) ? 2 : 0)
                                    | ((packed[16+2*y]  & bit) ? 4 : 0)
                                    | ((packed[16+2*y+1]& bit) ? 8 : 0));
                if (c != idx[y * 8 + x]) bad++;
            }
        check(bad == 0, "4bpp pack round-trips all 16 indices");
    }

    /* 8. SOLID FRACTION on a synthetic scene — the number that sets the frame
     *    rate. A big ground quad plus scattered small quads, over 30x25 tiles. */
    {
        printf("-- solid fraction (30x25 = 750 tiles) --\n");
        Hx421Tri scene[64];
        int n = 0;
        scene[n++] = quad_tri(0, 120, 240, 200, 500, 2, 0);   /* ground */
        scene[n++] = quad_tri(0, 120, 240, 200, 500, 2, 1);
        for (int i = 0; i < 10 && n < 62; ++i) {              /* objects */
            int x = 12 + i * 22, y = 60 + (i % 3) * 25;
            scene[n++] = quad_tri(x, y, x + 14, y + 14, (uint16_t)(200 + i), (uint8_t)(3 + i % 6), 0);
            scene[n++] = quad_tri(x, y, x + 14, y + 14, (uint16_t)(200 + i), (uint8_t)(3 + i % 6), 1);
        }
        int empty = 0, solid_n = 0, mixed = 0;
        for (int ty = 0; ty < 25; ++ty)
            for (int tx = 0; tx < 30; ++tx) {
                hx421_tile_clear(idx, zb);
                hx421_raster_tile(scene, n, tx, ty, idx, zb);
                switch (hx421_tile_classify(idx, &solid)) {
                    case HX421_TILE_EMPTY: empty++;   break;
                    case HX421_TILE_SOLID: solid_n++; break;
                    default:               mixed++;   break;
                }
            }
        int uploaded = mixed;                    /* only MIXED needs CHR */
        double kb = uploaded * 32.0 / 1024.0;
        printf("   empty %3d  solid %3d  mixed %3d\n", empty, solid_n, mixed);
        printf("   CHR upload: %d tiles = %.1f KB -> %.1f vblanks @6.2 KB -> ~%d fps\n",
               uploaded, kb, kb / 6.2, kb > 0.0 ? (int)(60.0 / ((kb / 6.2) < 1.0 ? 1.0 : (kb / 6.2))) : 60);
        check(empty + solid_n + mixed == 750, "every tile classified");
        check(mixed < 750, "solid/empty compression is doing something");
    }

    printf("\n==== RASTER %s (%d failures) ====\n", failures ? "FAILED" : "OK", failures);
    return failures ? 1 : 0;
}
