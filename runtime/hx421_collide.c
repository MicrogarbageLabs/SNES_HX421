/* ============================================================
 *  hx421_collide.c — broad-phase collision over the shared object registry
 *
 *  See the header for why this reads the renderer's registry rather than
 *  keeping its own copy.
 *
 *  Public domain (CC0). No warranty.
 * ============================================================ */

#include "hx421_collide.h"

/* Integer sqrt of a Q16.16 value -> Q16.16.
 *
 * sqrt(v * 2^16) in raw units is sqrt(v) * 2^8, so the raw result needs another
 * <<8 to land back in Q16.16. Doing that AFTER the sqrt would throw away the low
 * 8 bits of precision, so shift the input left by 16 first and take the sqrt of
 * a Q32.32 value instead — the intermediate needs 64 bits, which is why this is
 * not simply a 32-bit Newton loop. */
int32_t hx421_sqrt_q16(int64_t v_q16) {
    if (v_q16 <= 0) return 0;
    uint64_t v = (uint64_t)v_q16 << 16;          /* Q16.16 -> Q32.32 */
    /* bit-by-bit restoring square root: no division, no table, and it maps
     * directly onto fabric as a shift/compare chain. */
    uint64_t rem = 0, root = 0;
    for (int i = 0; i < 32; ++i) {
        rem = (rem << 2) | (v >> 62);
        v <<= 2;
        root <<= 1;
        uint64_t trial = (root << 1) | 1;
        if (rem >= trial) { rem -= trial; root |= 1; }
    }
    if (root > 0x7FFFFFFFull) root = 0x7FFFFFFFull;
    return (int32_t)root;
}

int hx421_sphere_test(Hx421Vec pa, int32_t ra, Hx421Vec pb, int32_t rb,
                      Hx421Vec *normal, int32_t *depth) {
    const int64_t dx = (int64_t)pb.x - pa.x;
    const int64_t dy = (int64_t)pb.y - pa.y;
    const int64_t dz = (int64_t)pb.z - pa.z;

    /* Square in a REDUCED scale (Q16.8) so three squares cannot overflow 64
     * bits even at the far edge of the Q16.16 world. dsq is then Q32.16. */
    const int64_t sx = dx >> 8, sy = dy >> 8, sz = dz >> 8;
    const int64_t dsq = sx*sx + sy*sy + sz*sz;
    const int64_t r   = (int64_t)ra + rb;
    const int64_t rs  = r >> 8;
    const int64_t rsq = rs * rs;

    if (dsq >= rsq) return 0;                    /* no overlap: the common exit */

    /* Only now is the square root worth paying for — the reject above is 3
     * multiplies and a compare, which is the entire point of a broad phase. */
    const int32_t len = hx421_sqrt_q16(dsq);     /* Q32.16 in -> Q16.16 out */

    if (len == 0) {
        /* Concentric. There is no separating direction, so pick one rather than
         * dividing by zero; +y pushes the pair apart predictably instead of
         * leaving them welded together. */
        if (normal) { normal->x = 0; normal->y = 65536; normal->z = 0; }
        if (depth)  *depth = (int32_t)r;
        return 1;
    }
    if (normal) {
        normal->x = (int32_t)((dx << 16) / len);
        normal->y = (int32_t)((dy << 16) / len);
        normal->z = (int32_t)((dz << 16) / len);
    }
    if (depth) *depth = (int32_t)(r - len);
    return 1;
}

int hx421_broadphase(const Hx421Scene *s, Hx421ContactList *out) {
    if (!s || !out) return 0;
    out->count = 0; out->overflow = 0; out->tests = 0;

    for (unsigned i = 0; i < HX421_MAX_OBJ; ++i) {
        const Hx421Object *a = &s->obj[i];
        if (!a->active || !a->collidable) continue;
        const int32_t ra = s->mesh[a->mesh].radius;

        for (unsigned j = i + 1; j < HX421_MAX_OBJ; ++j) {
            const Hx421Object *b = &s->obj[j];
            if (!b->active || !b->collidable) continue;

            /* BOTH must accept the other. Requiring only one side makes the
             * relationship asymmetric, and "why does the bullet hit the wall but
             * the wall not hit the bullet" is a miserable thing to debug. */
            if (!((a->mask >> b->layer) & 1u)) continue;
            if (!((b->mask >> a->layer) & 1u)) continue;

            out->tests++;
            Hx421Vec n; int32_t d;
            if (!hx421_sphere_test(a->pos, ra, b->pos, s->mesh[b->mesh].radius, &n, &d))
                continue;

            if (out->count >= HX421_MAX_CONTACTS) { out->overflow++; continue; }
            Hx421Contact *c = &out->c[out->count++];
            c->a = (uint16_t)i; c->b = (uint16_t)j;
            c->normal = n; c->depth = d;
        }
    }
    return (int)out->count;
}
