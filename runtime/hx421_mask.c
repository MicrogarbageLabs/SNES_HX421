/* ============================================================
 *  hx421_mask.c — 1-bit sprite masks, per-pixel 2D actor collision
 *
 *  See the header for why masks are derived rather than authored, and why
 *  derivation is per-actor rather than per-sprite.
 *
 *  Public domain (CC0). No warranty.
 * ============================================================ */

#include "hx421_mask.h"

/* ---- pixel access ------------------------------------------------------- */

int hx421_mask_get(const Hx421Mask *m, int x, int y) {
    if (!m || !m->bits) return 0;
    if (x < 0 || y < 0 || x >= (int)m->w || y >= (int)m->h) return 0;
    return (m->bits[(unsigned)y * m->stride + ((unsigned)x >> 3)] >> (7 - (x & 7))) & 1;
}

static void mask_set(uint8_t *bits, unsigned stride, int x, int y) {
    bits[(unsigned)y * stride + ((unsigned)x >> 3)] |= (uint8_t)(0x80u >> (x & 7));
}

/* ---- derivation --------------------------------------------------------- */

/* SNES 4bpp tile: 32 B. Bytes 0..15 are bitplanes 0/1 interleaved per row,
 * bytes 16..31 are planes 2/3. We only need "is the index non-zero", so OR the
 * four planes for the row and test the bit — no need to assemble the index. */
static unsigned tile_row_nonzero(const uint8_t *tile, int row) {
    return (unsigned)(tile[row * 2] | tile[row * 2 + 1]
                    | tile[16 + row * 2] | tile[16 + row * 2 + 1]);
}

unsigned hx421_derive_mask(const Hx421SpritePart *parts, unsigned n,
                           uint8_t w, uint8_t h, uint8_t *bits, unsigned cap) {
    if (!parts || !bits || !w || !h) return 0;
    if (w > HX421_MASK_MAX_W || h > HX421_MASK_MAX_H) return 0;

    const unsigned stride = ((unsigned)w + 7u) / 8u;
    const unsigned bytes  = stride * (unsigned)h;
    if (bytes > cap) return 0;

    for (unsigned i = 0; i < bytes; ++i) bits[i] = 0;

    for (unsigned p = 0; p < n; ++p) {
        const Hx421SpritePart *sp = &parts[p];
        if (!sp->chr || !sp->tiles_w || !sp->tiles_h) continue;
        const unsigned rs = sp->row_stride_tiles ? sp->row_stride_tiles : sp->tiles_w;

        for (unsigned ty = 0; ty < sp->tiles_h; ++ty) {
            for (unsigned tx = 0; tx < sp->tiles_w; ++tx) {
                const uint8_t *tile = sp->chr + (ty * rs + tx) * 32u;
                for (int row = 0; row < 8; ++row) {
                    const unsigned nz = tile_row_nonzero(tile, row);
                    if (!nz) continue;                       /* whole row clear */
                    const int y = sp->oy + (int)(ty * 8u) + row;
                    if (y < 0 || y >= (int)h) continue;
                    for (int col = 0; col < 8; ++col) {
                        if (!((nz >> (7 - col)) & 1u)) continue;
                        const int x = sp->ox + (int)(tx * 8u) + col;
                        if (x < 0 || x >= (int)w) continue;
                        mask_set(bits, stride, x, y);
                    }
                }
            }
        }
    }
    return bytes;
}

/* ---- overlap ------------------------------------------------------------ */

int hx421_mask_aabb(const Hx421Mask *ma, int ax, int ay,
                    const Hx421Mask *mb, int bx, int by) {
    if (!ma || !mb) return 0;
    if (ax + (int)ma->w <= bx || bx + (int)mb->w <= ax) return 0;
    if (ay + (int)ma->h <= by || by + (int)mb->h <= ay) return 0;
    return 1;
}

/* Load one mask row into a 64-bit accumulator, pixel 0 at bit 63. Aligning to
 * the TOP of the word is what lets the alignment shift below be a plain shift
 * in the same direction as screen space. */
static uint64_t row_word(const Hx421Mask *m, int y) {
    uint64_t acc = 0;
    const uint8_t *r = m->bits + (unsigned)y * m->stride;
    for (unsigned i = 0; i < m->stride && i < 8u; ++i)
        acc |= (uint64_t)r[i] << (56 - 8 * i);
    /* Clear everything past column w-1. Rows are byte-padded, so a 12-wide mask
     * has 4 junk bits per row; hx421_derive_mask never sets them, but a mask
     * built elsewhere might, and the result would be phantom hits in the pad
     * region that no artwork explains. */
    if (m->w < 64u) acc &= ~0ull << (64u - m->w);
    return acc;
}

/* Index of the highest set bit (bit 63 -> 0, bit 0 -> 63) — i.e. the leftmost
 * overlapping pixel, since pixel 0 sits at bit 63. */
static int first_left(uint64_t v) {
    int n = 0;
    if (!(v & 0xFFFFFFFF00000000ull)) { n += 32; v <<= 32; }
    if (!(v & 0xFFFF000000000000ull)) { n += 16; v <<= 16; }
    if (!(v & 0xFF00000000000000ull)) { n +=  8; v <<=  8; }
    if (!(v & 0xF000000000000000ull)) { n +=  4; v <<=  4; }
    if (!(v & 0xC000000000000000ull)) { n +=  2; v <<=  2; }
    if (!(v & 0x8000000000000000ull)) { n +=  1; }
    return n;
}
/* Index of the lowest set bit, expressed in the same pixel-column space. */
static int first_right(uint64_t v) {
    int n = 63;
    if (!(v & 0x00000000FFFFFFFFull)) { n -= 32; v >>= 32; }
    if (!(v & 0x000000000000FFFFull)) { n -= 16; v >>= 16; }
    if (!(v & 0x00000000000000FFull)) { n -=  8; v >>=  8; }
    if (!(v & 0x000000000000000Full)) { n -=  4; v >>=  4; }
    if (!(v & 0x0000000000000003ull)) { n -=  2; v >>=  2; }
    if (!(v & 0x0000000000000001ull)) { n -=  1; }
    return n;
}

int hx421_mask_overlap(const Hx421Mask *ma, int ax, int ay,
                       const Hx421Mask *mb, int bx, int by,
                       Hx421ScanOrder order, Hx421MaskHit *hit) {
    if (!ma || !mb || !ma->bits || !mb->bits) return 0;
    if (ma->w > HX421_MASK_MAX_W || mb->w > HX421_MASK_MAX_W) return 0;
    if (!hx421_mask_aabb(ma, ax, ay, mb, bx, by)) return 0;   /* the cheap reject */

    /* Clip to the overlapping band, expressed in A's rows. */
    const int dx = bx - ax;
    const int dy = by - ay;
    int y0 = (dy > 0) ? dy : 0;
    int y1 = dy + (int)mb->h;                        /* exclusive */
    if (y1 > (int)ma->h) y1 = (int)ma->h;

    const int down = (order == HX421_SCAN_TOPLEFT || order == HX421_SCAN_TOPRIGHT);
    const int left = (order == HX421_SCAN_TOPLEFT || order == HX421_SCAN_BOTTOMLEFT);

    for (int k = y0; k < y1; ++k) {
        /* Scanning bottom-up is the same band walked in reverse; mirroring the
         * index rather than duplicating the loop keeps the two orders provably
         * identical apart from which contact they report. */
        const int y = down ? k : (y1 - 1 - (k - y0));

        const uint64_t wa = row_word(ma, y);
        if (!wa) continue;
        uint64_t wb = row_word(mb, y - dy);
        if (!wb) continue;

        /* Align B into A's column space. B's pixel j sits at A-column dx+j, and
         * pixel 0 is at bit 63, so a positive dx is a right shift. */
        wb = (dx >= 0) ? ((dx >= 64) ? 0 : (wb >> dx))
                       : ((-dx >= 64) ? 0 : (wb << -dx));

        const uint64_t both = wa & wb;
        if (!both) continue;

        if (hit) {
            const int col = left ? first_left(both) : first_right(both);
            hit->x = (int16_t)(ax + col);
            hit->y = (int16_t)(ay + y);
        }
        return 1;
    }
    return 0;
}
