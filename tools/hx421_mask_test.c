/* hx421_mask_test.c — 1-bit actor mask derivation and per-pixel overlap. */

#include <stdio.h>
#include <string.h>
#include "../runtime/hx421_mask.h"

static int fails = 0, checks = 0;
static void ck(int cond, const char *what) {
    checks++;
    if (!cond) { printf("  FAIL: %s\n", what); fails++; }
}

/* Build one 4bpp tile whose non-zero pixels come from an 8-row bitmask. Only
 * plane 0 is set, which is enough: derivation asks "is the index non-zero". */
static void tile_from_rows(uint8_t tile[32], const uint8_t rows[8]) {
    memset(tile, 0, 32);
    for (int r = 0; r < 8; ++r) tile[r * 2] = rows[r];
}

/* A tile that is only set in plane 3 — index 8, still non-transparent. If
 * derivation only looked at plane 0 this comes back empty, and the bug reads as
 * "collision misses parts of the sprite" long after the art is forgotten. */
static void tile_plane3(uint8_t tile[32], const uint8_t rows[8]) {
    memset(tile, 0, 32);
    for (int r = 0; r < 8; ++r) tile[16 + r * 2 + 1] = rows[r];
}

int main(void) {
    printf("hx421 mask tests\n");

    /* ---- derivation: one 8x8 tile ---- */
    static const uint8_t solid[8]  = {0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF};
    static const uint8_t diag[8]   = {0x80,0x40,0x20,0x10,0x08,0x04,0x02,0x01};
    static const uint8_t empty8[8] = {0};

    uint8_t tile[32], bits[64 * 8];
    Hx421SpritePart part = { tile, 1, 1, 0, 0, 0 };

    tile_from_rows(tile, solid);
    ck(hx421_derive_mask(&part, 1, 8, 8, bits, sizeof bits) == 8, "8x8 mask is 8 bytes");
    Hx421Mask m8 = { 8, 8, 1, bits };
    ck(hx421_mask_get(&m8, 0, 0) && hx421_mask_get(&m8, 7, 7), "solid tile fills the mask");

    tile_from_rows(tile, diag);
    hx421_derive_mask(&part, 1, 8, 8, bits, sizeof bits);
    ck(hx421_mask_get(&m8, 0, 0) == 1 && hx421_mask_get(&m8, 1, 1) == 1, "diagonal set where art is");
    ck(hx421_mask_get(&m8, 1, 0) == 0 && hx421_mask_get(&m8, 0, 1) == 0, "diagonal clear elsewhere");

    tile_from_rows(tile, empty8);
    hx421_derive_mask(&part, 1, 8, 8, bits, sizeof bits);
    ck(hx421_mask_get(&m8, 0, 0) == 0, "a fully transparent tile derives an empty mask");

    /* every plane counts, not just plane 0 */
    tile_plane3(tile, solid);
    hx421_derive_mask(&part, 1, 8, 8, bits, sizeof bits);
    ck(hx421_mask_get(&m8, 3, 3) == 1, "a plane-3-only pixel is non-transparent");

    /* index 0 must stay clear even when neighbours are set */
    ck(hx421_mask_get(&m8, -1, 0) == 0 && hx421_mask_get(&m8, 8, 0) == 0,
       "out-of-bounds reads are 0, not garbage");

    /* ---- derivation: a multi-part actor ---- */
    /* Two 8x8 parts stacked to make an 8x16 actor. Compositing them into ONE
     * mask is the whole point — two masks would mean two tests to reconcile. */
    uint8_t ta[32], tb[32], abits[16];
    tile_from_rows(ta, solid);
    tile_from_rows(tb, diag);
    Hx421SpritePart actor[2] = {
        { ta, 1, 1, 0, 0, 0 },
        { tb, 1, 1, 0, 0, 8 },
    };
    ck(hx421_derive_mask(actor, 2, 8, 16, abits, sizeof abits) == 16, "8x16 actor is 16 bytes");
    Hx421Mask ma = { 8, 16, 1, abits };
    ck(hx421_mask_get(&ma, 4, 3) == 1, "upper part composited");
    ck(hx421_mask_get(&ma, 0, 8) == 1, "lower part composited at its offset");
    ck(hx421_mask_get(&ma, 4, 8) == 0, "lower part keeps its own shape");

    /* SNES OBJ row stride: vertically adjacent tiles are 16 tiles apart, not
     * tiles_w apart. Using the wrong stride reads the wrong tile and produces a
     * mask that is silently wrong rather than obviously broken. */
    uint8_t vram[17 * 32];
    memset(vram, 0, sizeof vram);
    tile_from_rows(vram, solid);            /* tile 0  = top-left  */
    tile_from_rows(vram + 16 * 32, diag);   /* tile 16 = below it  */
    uint8_t vbits[16];
    Hx421SpritePart hw = { vram, 1, 2, 16, 0, 0 };
    hx421_derive_mask(&hw, 1, 8, 16, vbits, sizeof vbits);
    Hx421Mask mv = { 8, 16, 1, vbits };
    ck(hx421_mask_get(&mv, 4, 3) == 1, "hardware stride reads the top tile");
    ck(hx421_mask_get(&mv, 0, 8) == 1 && hx421_mask_get(&mv, 4, 8) == 0,
       "hardware stride reads tile 16 as the row below");

    /* size guards */
    ck(hx421_derive_mask(&part, 1, 8, 8, bits, 4) == 0, "a short buffer is refused");
    ck(hx421_derive_mask(&part, 1, 200, 8, bits, sizeof bits) == 0, "over-wide mask is refused");

    /* ---- AABB ---- */
    uint8_t sbits[8];
    tile_from_rows(tile, solid);
    hx421_derive_mask(&part, 1, 8, 8, sbits, sizeof sbits);
    Hx421Mask sq = { 8, 8, 1, sbits };

    ck(hx421_mask_aabb(&sq, 0, 0, &sq, 4, 4) == 1, "AABB overlap detected");
    ck(hx421_mask_aabb(&sq, 0, 0, &sq, 8, 0) == 0, "AABB touching edge-to-edge is not overlap");
    ck(hx421_mask_aabb(&sq, 0, 0, &sq, 100, 0) == 0, "AABB far apart");

    /* ---- overlap: solid vs solid ---- */
    Hx421MaskHit hit;
    ck(hx421_mask_overlap(&sq, 0, 0, &sq, 4, 4, HX421_SCAN_TOPLEFT, &hit) == 1,
       "two solid squares overlap");
    ck(hit.x == 4 && hit.y == 4, "top-left scan reports the top-left contact pixel");

    ck(hx421_mask_overlap(&sq, 0, 0, &sq, 8, 0, HX421_SCAN_TOPLEFT, &hit) == 0,
       "adjacent squares do not overlap");

    /* negative offset: B to the LEFT of A exercises the opposite shift */
    ck(hx421_mask_overlap(&sq, 4, 0, &sq, 0, 0, HX421_SCAN_TOPLEFT, &hit) == 1,
       "overlap works with B left of A");
    ck(hit.x == 4 && hit.y == 0, "left-side contact is at A's own left edge");

    /* ---- overlap: shapes that AABB alone would get WRONG ---- */
    /* Two opposing diagonals whose boxes overlap fully but whose pixels never
     * do. This is the case that justifies per-pixel at all: AABB says hit. */
    static const uint8_t anti[8] = {0x01,0x02,0x04,0x08,0x10,0x20,0x40,0x80};
    uint8_t dbits[8], abits2[8];
    tile_from_rows(tile, diag);  hx421_derive_mask(&part, 1, 8, 8, dbits, sizeof dbits);
    tile_from_rows(tile, anti);  hx421_derive_mask(&part, 1, 8, 8, abits2, sizeof abits2);
    Hx421Mask md = { 8, 8, 1, dbits }, mant = { 8, 8, 1, abits2 };

    /* md has a pixel at (r, r); mant placed at dx has one at (dx + 7 - r, r).
     * They share a pixel only when r = (dx + 7) / 2 has an integer solution,
     * i.e. only for ODD dx. Even offsets slip between the two lines. Both cases
     * matter: the miss proves per-pixel is doing work AABB cannot, and the hit
     * proves the test is actually sensitive rather than just returning 0. */
    ck(hx421_mask_aabb(&md, 0, 0, &mant, 2, 0) == 1, "diagonals' boxes overlap");
    ck(hx421_mask_overlap(&md, 0, 0, &mant, 2, 0, HX421_SCAN_TOPLEFT, &hit) == 0,
       "but at an even offset their pixels do not — AABB alone would false-positive");
    ck(hx421_mask_overlap(&md, 0, 0, &mant, 0, 0, HX421_SCAN_TOPLEFT, &hit) == 0,
       "aligned, 8-wide diagonals cross between pixels, not on one");
    ck(hx421_mask_overlap(&md, 0, 0, &mant, 1, 0, HX421_SCAN_TOPLEFT, &hit) == 1,
       "at an odd offset the lines do share a pixel");
    ck(hit.x == 4 && hit.y == 4, "and it is the crossing pixel");

    /* ---- scan order decides WHICH contact, not WHETHER ---- */
    /* A horizontal bar over a solid square: the overlap is a whole row, so the
     * four scan orders must agree on hit-ness and differ only in position. */
    static const uint8_t bar[8] = {0x00,0x00,0x00,0xFF,0x00,0x00,0x00,0x00};
    uint8_t bbits[8];
    tile_from_rows(tile, bar); hx421_derive_mask(&part, 1, 8, 8, bbits, sizeof bbits);
    Hx421Mask mbar = { 8, 8, 1, bbits };

    Hx421MaskHit tl, tr;
    ck(hx421_mask_overlap(&sq, 0, 0, &mbar, 0, 0, HX421_SCAN_TOPLEFT, &tl) == 1, "bar hits");
    ck(hx421_mask_overlap(&sq, 0, 0, &mbar, 0, 0, HX421_SCAN_TOPRIGHT, &tr) == 1,
       "bar hits regardless of scan order");
    ck(tl.y == tr.y && tl.y == 3, "both find the bar's row");
    ck(tl.x == 0 && tr.x == 7, "left scan takes the leftmost pixel, right scan the rightmost");

    /* Two bars at different heights: top-scan must find the upper one, bottom
     * the lower. This is what a projectile needs to spawn its impact on the
     * side it arrived from. */
    static const uint8_t bars2[8] = {0x00,0xFF,0x00,0x00,0x00,0x00,0xFF,0x00};
    uint8_t b2[8];
    tile_from_rows(tile, bars2); hx421_derive_mask(&part, 1, 8, 8, b2, sizeof b2);
    Hx421Mask m2 = { 8, 8, 1, b2 };
    Hx421MaskHit up, dn;
    hx421_mask_overlap(&sq, 0, 0, &m2, 0, 0, HX421_SCAN_TOPLEFT, &up);
    hx421_mask_overlap(&sq, 0, 0, &m2, 0, 0, HX421_SCAN_BOTTOMLEFT, &dn);
    ck(up.y == 1, "top scan reports the upper bar");
    ck(dn.y == 6, "bottom scan reports the lower bar");

    /* ---- symmetry: swapping the pair must agree on hit-ness ---- */
    int f1 = hx421_mask_overlap(&sq, 3, 5, &md, 0, 0, HX421_SCAN_TOPLEFT, &hit);
    int f2 = hx421_mask_overlap(&md, 0, 0, &sq, 3, 5, HX421_SCAN_TOPLEFT, &hit);
    ck(f1 == f2, "overlap is symmetric under argument order");

    /* ---- non-byte-aligned width: the padding bits must not register ---- */
    /* A 12-wide mask has 4 pad bits per row. Deliberately dirty them, since a
     * mask staged from elsewhere may not be clean, and phantom hits in the pad
     * region are unexplainable from the artwork. */
    uint8_t wide[2 * 8];
    memset(wide, 0, sizeof wide);
    for (int y = 0; y < 8; ++y) wide[y * 2 + 1] = 0x0F;   /* only pad bits set */
    Hx421Mask mpad = { 12, 8, 2, wide };
    ck(hx421_mask_get(&mpad, 11, 0) == 0, "no real pixel is set");
    ck(hx421_mask_overlap(&mpad, 0, 0, &sq, 8, 0, HX421_SCAN_TOPLEFT, &hit) == 0,
       "padding bits past the width never register as a hit");

    /* ---- what a busy frame actually costs ----
     * 64 actors of 16x16 scattered over a 256x224 screen by a fixed LCG, so the
     * figure is reproducible. Counts the AABB rejects against the mask tests
     * that survive them, which is the ratio the broad phase exists to create. */
    {
        uint8_t a16[2 * 16];
        memset(a16, 0xFF, sizeof a16);              /* a solid 16x16 actor */
        Hx421Mask act = { 16, 16, 2, a16 };

        int px[64], py[64];
        uint32_t seed = 12345u;
        for (int i = 0; i < 64; ++i) {
            seed = seed * 1103515245u + 12345u;
            px[i] = (int)((seed >> 16) % 240u);
            seed = seed * 1103515245u + 12345u;
            py[i] = (int)((seed >> 16) % 208u);
        }
        unsigned pairs = 0, survived = 0, hits = 0;
        for (int i = 0; i < 64; ++i)
            for (int j = i + 1; j < 64; ++j) {
                pairs++;
                if (!hx421_mask_aabb(&act, px[i], py[i], &act, px[j], py[j])) continue;
                survived++;
                if (hx421_mask_overlap(&act, px[i], py[i], &act, px[j], py[j],
                                       HX421_SCAN_TOPLEFT, &hit)) hits++;
            }
        printf("\n  measurement: 64 actors (16x16) -> %u AABB tests, %u survive "
               "(%.1f%%), %u overlap\n"
               "  worst case per survivor is 16 shift-AND-tests, so ~%u row ops/frame\n",
               pairs, survived, 100.0 * survived / pairs, hits, survived * 16u);
    }

    printf("%d checks, %d failures\n", checks, fails);
    return fails ? 1 : 0;
}
