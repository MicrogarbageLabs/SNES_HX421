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

/* ---- oriented box overlap (SAT) ----------------------------------------- */

#define Q15_ONE 32768

static int32_t qm15(int32_t a, int32_t b) { return (int32_t)(((int64_t)a * b) >> 15); }
static int32_t qabs(int32_t v) { return v < 0 ? -v : v; }

/* Row i of a Q15 rotation matrix is that box's i-th axis in world space. */
static Hx421Vec mat_row(Hx421Mat m, int i) {
    Hx421Vec r = { m.m[i*3+0], m.m[i*3+1], m.m[i*3+2] };
    return r;
}
static int32_t dot_q15_q16(Hx421Vec axis_q15, Hx421Vec v_q16) {
    return (int32_t)(((int64_t)axis_q15.x * v_q16.x
                    + (int64_t)axis_q15.y * v_q16.y
                    + (int64_t)axis_q15.z * v_q16.z) >> 15);
}

int hx421_obb_test(Hx421Vec pa, Hx421Mat ra, Hx421Vec ha,
                   Hx421Vec pb, Hx421Mat rb, Hx421Vec hb,
                   Hx421Vec *normal, int32_t *depth) {
    /* R[i][j] = a_i . b_j, the relative rotation. Q15. */
    int32_t R[3][3], absR[3][3];
    for (int i = 0; i < 3; ++i) {
        const Hx421Vec ai = mat_row(ra, i);
        for (int j = 0; j < 3; ++j) {
            const Hx421Vec bj = mat_row(rb, j);
            R[i][j] = qm15(ai.x, bj.x) + qm15(ai.y, bj.y) + qm15(ai.z, bj.z);
            /* The epsilon matters when two boxes are near-parallel: the cross
             * products of nearly-collinear edges are near zero, and without it
             * the resulting axis is numerical noise that reports a separation
             * where there is none. */
            absR[i][j] = qabs(R[i][j]) + 16;
        }
    }

    /* translation, in A's frame */
    const Hx421Vec d = { pb.x - pa.x, pb.y - pa.y, pb.z - pa.z };
    int32_t t[3];
    for (int i = 0; i < 3; ++i) t[i] = dot_q15_q16(mat_row(ra, i), d);

    const int32_t ea[3] = { ha.x, ha.y, ha.z };
    const int32_t eb[3] = { hb.x, hb.y, hb.z };

    int32_t best = 0x7FFFFFFF;
    int      best_axis = 0, best_from_b = 0, best_sign = 1;

    /* --- 6 face axes: unit vectors, so their overlaps are comparable --- */
    for (int i = 0; i < 3; ++i) {                      /* A's axes */
        const int32_t ra_i = ea[i];
        const int32_t rb_i = qm15(eb[0], absR[i][0]) + qm15(eb[1], absR[i][1])
                           + qm15(eb[2], absR[i][2]);
        const int32_t over = ra_i + rb_i - qabs(t[i]);
        if (over <= 0) return 0;
        if (over < best) { best = over; best_axis = i; best_from_b = 0;
                           best_sign = (t[i] < 0) ? -1 : 1; }
    }
    for (int j = 0; j < 3; ++j) {                      /* B's axes */
        const int32_t ra_j = qm15(ea[0], absR[0][j]) + qm15(ea[1], absR[1][j])
                           + qm15(ea[2], absR[2][j]);
        const int32_t tj = qm15(t[0], R[0][j]) + qm15(t[1], R[1][j]) + qm15(t[2], R[2][j]);
        const int32_t over = ra_j + eb[j] - qabs(tj);
        if (over <= 0) return 0;
        if (over < best) { best = over; best_axis = j; best_from_b = 1;
                           best_sign = (tj < 0) ? -1 : 1; }
    }

    /* --- 9 edge-cross axes: BOOLEAN ONLY (see the header) --- */
    static const int u[3] = {1, 2, 0}, v[3] = {2, 0, 1};
    for (int i = 0; i < 3; ++i) {
        for (int j = 0; j < 3; ++j) {
            const int32_t r0 = qm15(ea[u[i]], absR[v[i]][j]) + qm15(ea[v[i]], absR[u[i]][j]);
            const int32_t r1 = qm15(eb[u[j]], absR[i][v[j]]) + qm15(eb[v[j]], absR[i][u[j]]);
            const int32_t tt = qabs(qm15(t[v[i]], R[u[i]][j]) - qm15(t[u[i]], R[v[i]][j]));
            if (tt > r0 + r1) return 0;
        }
    }

    if (normal) {
        Hx421Vec n = best_from_b ? mat_row(rb, best_axis) : mat_row(ra, best_axis);
        /* Point it from a toward b, so callers can use it without re-deriving
         * the sense from the object order. */
        n.x *= best_sign; n.y *= best_sign; n.z *= best_sign;
        /* Q15 axis -> Q16.16 unit vector */
        n.x <<= 1; n.y <<= 1; n.z <<= 1;
        *normal = n;
    }
    if (depth) *depth = best;
    return 1;
}

int hx421_midphase(const Hx421Scene *s, Hx421ContactList *io) {
    if (!s || !io) return 0;
    unsigned keep = 0;
    for (unsigned k = 0; k < io->count; ++k) {
        Hx421Contact *c = &io->c[k];
        if (c->kind != HX421_BODY_MESH) { io->c[keep++] = *c; continue; }

        const Hx421Object *a = &s->obj[c->a], *b = &s->obj[c->b];
        const Hx421Mesh *ma = &s->mesh[a->mesh], *mb = &s->mesh[b->mesh];

        /* The box is centred on the mesh's own centre, which need not be the
         * model origin — rotate that offset into world space before testing. */
        Hx421Vec ca = a->pos, cb = b->pos;
        ca.x += (int32_t)(((int64_t)ma->centre.x * a->rot.m[0]
                         + (int64_t)ma->centre.y * a->rot.m[1]
                         + (int64_t)ma->centre.z * a->rot.m[2]) >> 15);
        ca.y += (int32_t)(((int64_t)ma->centre.x * a->rot.m[3]
                         + (int64_t)ma->centre.y * a->rot.m[4]
                         + (int64_t)ma->centre.z * a->rot.m[5]) >> 15);
        ca.z += (int32_t)(((int64_t)ma->centre.x * a->rot.m[6]
                         + (int64_t)ma->centre.y * a->rot.m[7]
                         + (int64_t)ma->centre.z * a->rot.m[8]) >> 15);
        cb.x += (int32_t)(((int64_t)mb->centre.x * b->rot.m[0]
                         + (int64_t)mb->centre.y * b->rot.m[1]
                         + (int64_t)mb->centre.z * b->rot.m[2]) >> 15);
        cb.y += (int32_t)(((int64_t)mb->centre.x * b->rot.m[3]
                         + (int64_t)mb->centre.y * b->rot.m[4]
                         + (int64_t)mb->centre.z * b->rot.m[5]) >> 15);
        cb.z += (int32_t)(((int64_t)mb->centre.x * b->rot.m[6]
                         + (int64_t)mb->centre.y * b->rot.m[7]
                         + (int64_t)mb->centre.z * b->rot.m[8]) >> 15);

        Hx421Vec n; int32_t dep;
        if (!hx421_obb_test(ca, a->rot, ma->half, cb, b->rot, mb->half, &n, &dep))
            continue;                       /* sphere said maybe, boxes say no */
        c->normal = n; c->depth = dep;
        io->c[keep++] = *c;
    }
    io->count = (uint16_t)keep;
    return (int)keep;
}

/* ---- narrow phase: plane sets and deflection ---------------------------- */

#define Q16_ONE 65536

static Hx421Vec vsub3(Hx421Vec a, Hx421Vec b) { Hx421Vec r = {a.x-b.x, a.y-b.y, a.z-b.z}; return r; }
static int32_t  dot16(Hx421Vec a, Hx421Vec b) {
    return (int32_t)(((int64_t)a.x*b.x + (int64_t)a.y*b.y + (int64_t)a.z*b.z) >> 16);
}
static Hx421Vec cross16(Hx421Vec a, Hx421Vec b) {
    Hx421Vec r;
    r.x = (int32_t)(((int64_t)a.y*b.z - (int64_t)a.z*b.y) >> 16);
    r.y = (int32_t)(((int64_t)a.z*b.x - (int64_t)a.x*b.z) >> 16);
    r.z = (int32_t)(((int64_t)a.x*b.y - (int64_t)a.y*b.x) >> 16);
    return r;
}
static Hx421Vec scale16(Hx421Vec v, int32_t s) {
    Hx421Vec r;
    r.x = (int32_t)(((int64_t)v.x * s) >> 16);
    r.y = (int32_t)(((int64_t)v.y * s) >> 16);
    r.z = (int32_t)(((int64_t)v.z * s) >> 16);
    return r;
}
/* Length of a Q16.16 vector. Reduces to Q16.8 before squaring so three squares
 * cannot overflow, exactly as the sphere test does. */
static int32_t len16(Hx421Vec v) {
    const int64_t x = v.x >> 8, y = v.y >> 8, z = v.z >> 8;
    return hx421_sqrt_q16(x*x + y*y + z*z);
}
static int normalize16(Hx421Vec *v) {
    const int32_t l = len16(*v);
    if (l < 64) return 0;                 /* degenerate: no usable direction */
    v->x = (int32_t)(((int64_t)v->x << 16) / l);
    v->y = (int32_t)(((int64_t)v->y << 16) / l);
    v->z = (int32_t)(((int64_t)v->z << 16) / l);
    return 1;
}

unsigned hx421_mesh_fit_planes(Hx421Mesh *m, Hx421Plane *out, unsigned cap) {
    if (!m || !out || !cap || !m->verts || !m->faces) return 0;
    if (cap > HX421_MAX_PLANES) cap = HX421_MAX_PLANES;

    Hx421Plane p[HX421_MAX_PLANES];
    int64_t    area[HX421_MAX_PLANES];
    unsigned   n = 0;

    for (uint16_t f = 0; f < m->fcount; ++f) {
        const Hx421Vec v0 = m->verts[m->faces[f*3+0]];
        const Hx421Vec v1 = m->verts[m->faces[f*3+1]];
        const Hx421Vec v2 = m->verts[m->faces[f*3+2]];
        Hx421Vec nrm = cross16(vsub3(v1, v0), vsub3(v2, v0));
        const int32_t twice_area = len16(nrm);      /* |cross| = 2 * area */
        if (!normalize16(&nrm)) continue;           /* degenerate triangle */
        const int32_t d = dot16(nrm, v0);

        /* Merge coplanar faces: same normal within ~5 degrees AND the same
         * offset. Without the offset check, the two opposite faces of a slab
         * would merge into one plane and the far side would stop existing. */
        unsigned hit = n;
        for (unsigned k = 0; k < n; ++k) {
            if (dot16(p[k].n, nrm) < 65100) continue;          /* ~5 deg */
            int32_t dd = p[k].d - d; if (dd < 0) dd = -dd;
            if (dd > (Q16_ONE / 64)) continue;
            hit = k; break;
        }
        if (hit < n) { area[hit] += twice_area; continue; }
        if (n < HX421_MAX_PLANES) { p[n].n = nrm; p[n].d = d; area[n] = twice_area; n++; }
    }

    /* Rank by area, keep the largest `cap`: the big flat surfaces are what a
     * deflection should come off, and slivers only add near-duplicate normals
     * for the dedup pass to throw away again. Selection sort — n <= 12. */
    for (unsigned i = 0; i < n; ++i) {
        unsigned best = i;
        for (unsigned j = i + 1; j < n; ++j) if (area[j] > area[best]) best = j;
        if (best != i) {
            Hx421Plane tp = p[i]; p[i] = p[best]; p[best] = tp;
            int64_t ta = area[i]; area[i] = area[best]; area[best] = ta;
        }
    }
    if (n > cap) n = cap;
    for (unsigned i = 0; i < n; ++i) out[i] = p[i];
    m->planes = out;
    m->pcount = (uint8_t)n;
    return n;
}

/* Rotate a model-space vector into world space with a Q15 matrix. */
static Hx421Vec rot_apply(Hx421Mat m, Hx421Vec v) {
    Hx421Vec r;
    r.x = (int32_t)(((int64_t)m.m[0]*v.x + (int64_t)m.m[1]*v.y + (int64_t)m.m[2]*v.z) >> 15);
    r.y = (int32_t)(((int64_t)m.m[3]*v.x + (int64_t)m.m[4]*v.y + (int64_t)m.m[5]*v.z) >> 15);
    r.z = (int32_t)(((int64_t)m.m[6]*v.x + (int64_t)m.m[7]*v.y + (int64_t)m.m[8]*v.z) >> 15);
    return r;
}

int hx421_narrowphase(const Hx421Scene *s, Hx421ContactList *io) {
    if (!s || !io) return 0;
    for (unsigned k = 0; k < io->count; ++k) {
        Hx421Contact *c = &io->c[k];
        c->nnormals = 0;
        if (c->kind != HX421_BODY_MESH) continue;

        const Hx421Object *a = &s->obj[c->a], *b = &s->obj[c->b];
        const Hx421Mesh *mb = &s->mesh[b->mesh];
        const int32_t ra = s->mesh[a->mesh].radius;

        /* a's centre relative to b, in b's model frame: transpose is the inverse
         * of a rotation, so no general matrix inverse is needed. */
        const Hx421Vec w = vsub3(a->pos, b->pos);
        Hx421Mat bt;
        for (int i = 0; i < 3; ++i)
            for (int j = 0; j < 3; ++j) bt.m[i*3+j] = b->rot.m[j*3+i];
        const Hx421Vec lp = rot_apply(bt, w);

        /* Pick the SHALLOWEST plane(s), not every plane the sphere reaches.
         *
         * Testing |dist| < radius against each plane independently is the
         * obvious formulation and it is wrong: a sphere big enough to touch the
         * front face of a box also reaches its side faces, so a flat face-on
         * contact comes back with four or five normals. The resolver then sees
         * three independent contacts, calls it a wedge, and ZEROES the velocity
         * — driving straight into a wall stops dead instead of bouncing.
         *
         * For a convex solid the contact face is the one the object has
         * penetrated LEAST. Ties inside a small tolerance are genuine
         * simultaneous contacts, which is exactly how an edge gives two normals
         * and a corner three, without any of them being invented. */
        int32_t best = -0x7FFFFFFF;
        for (unsigned pi = 0; pi < mb->pcount; ++pi) {
            const int32_t dist = dot16(mb->planes[pi].n, lp) - mb->planes[pi].d;
            if (dist > best) best = dist;
        }
        if (mb->pcount && best <= ra) {
            const int32_t tol = ra / 8;
            for (unsigned pi = 0; pi < mb->pcount && c->nnormals < HX421_MAX_NORMALS; ++pi) {
                const int32_t dist = dot16(mb->planes[pi].n, lp) - mb->planes[pi].d;
                if (dist < best - tol) continue;
                c->normals[c->nnormals++] = rot_apply(b->rot, mb->planes[pi].n);
            }
        }
        /* Always at least one normal, so a caller never has to special-case a
         * contact that carries none. */
        if (!c->nnormals) { c->normals[0] = c->normal; c->nnormals = 1; }
    }
    return (int)io->count;
}

int hx421_resolve(const Hx421Vec *normals, unsigned n, Hx421Vec v,
                  Hx421Response mode, Hx421Vec *out) {
    Hx421Vec keep[HX421_MAX_NORMALS];
    unsigned nk = 0;

    /* 1. Dedup within ~15 degrees. cos(15) = 0.9659 -> 63303 in Q16.16. An
     *    approximated curve is several planes with nearly the same normal;
     *    applying each in turn multiplies one correction and launches the
     *    object. */
    for (unsigned i = 0; i < n && nk < HX421_MAX_NORMALS; ++i) {
        Hx421Vec ni = normals[i];
        if (!normalize16(&ni)) continue;
        unsigned dup = 0;
        for (unsigned j = 0; j < nk; ++j) if (dot16(keep[j], ni) > 63303) { dup = 1; break; }
        if (!dup) keep[nk++] = ni;
    }
    if (!nk) { if (out) *out = v; return 1; }

    /* 2. Orthogonalise (Gram-Schmidt). Two walls at a right angle are already
     *    orthogonal and pass through untouched; oblique ones stop double-
     *    counting their shared component. */
    unsigned no = 0;
    Hx421Vec ortho[HX421_MAX_NORMALS];
    for (unsigned i = 0; i < nk; ++i) {
        Hx421Vec e = keep[i];
        for (unsigned j = 0; j < no; ++j) {
            const int32_t pr = dot16(e, ortho[j]);
            const Hx421Vec sub = scale16(ortho[j], pr);
            e.x -= sub.x; e.y -= sub.y; e.z -= sub.z;
        }
        if (normalize16(&e)) ortho[no++] = e;      /* wholly dependent -> drop */
    }

    /* 3. Three or more independent contacts is a WEDGE: any reflection just
     *    re-penetrates something, so stop rather than resolve. */
    if (no >= 3) {
        if (out) { out->x = out->y = out->z = 0; }
        return 0;
    }

    /* 4. Reflect against all of them at once. For right-angle walls — the
     *    common case — this is exact, and it costs one dot and one scaled
     *    subtract per contact, the same as resolving them one at a time. */
    const int32_t k = (mode == HX421_RESPONSE_BOUNCE) ? 2 : 1;
    Hx421Vec r = v;
    for (unsigned i = 0; i < no; ++i) {
        const int32_t pr = dot16(v, ortho[i]);
        const Hx421Vec sub = scale16(ortho[i], pr * k);
        r.x -= sub.x; r.y -= sub.y; r.z -= sub.z;
    }
    if (out) *out = r;
    return 1;
}

/* A mask actor's position is Q16.16 SCREEN PIXELS; the mask maths is integer. */
static int px_of(int32_t q) { return (int)(q >> 16); }

int hx421_broadphase(const Hx421Scene *s, Hx421ContactList *out,
                     Hx421ScanOrder order) {
    if (!s || !out) return 0;
    out->count = 0; out->overflow = 0; out->tests = 0; out->cross_skipped = 0;

    for (unsigned i = 0; i < HX421_MAX_OBJ; ++i) {
        const Hx421Object *a = &s->obj[i];
        if (!a->active || !a->collidable) continue;

        for (unsigned j = i + 1; j < HX421_MAX_OBJ; ++j) {
            const Hx421Object *b = &s->obj[j];
            if (!b->active || !b->collidable) continue;

            /* BOTH must accept the other. Requiring only one side makes the
             * relationship asymmetric, and "why does the bullet hit the wall but
             * the wall not hit the bullet" is a miserable thing to debug. */
            if (!((a->mask >> b->layer) & 1u)) continue;
            if (!((b->mask >> a->layer) & 1u)) continue;

            if (a->kind != b->kind) { out->cross_skipped++; continue; }

            Hx421Contact cand;
            cand.a = (uint16_t)i; cand.b = (uint16_t)j; cand.kind = a->kind;
            cand.normal.x = cand.normal.y = cand.normal.z = 0;
            cand.depth = 0; cand.px = cand.py = 0;

            out->tests++;
            if (a->kind == HX421_BODY_MESH) {
                if (!hx421_sphere_test(a->pos, s->mesh[a->mesh].radius,
                                       b->pos, s->mesh[b->mesh].radius,
                                       &cand.normal, &cand.depth)) continue;
            } else {
                const Hx421Mask *ma = &s->mask[a->mask_id];
                const Hx421Mask *mb = &s->mask[b->mask_id];
                Hx421MaskHit hit;
                if (!hx421_mask_overlap(ma, px_of(a->pos.x), px_of(a->pos.y),
                                        mb, px_of(b->pos.x), px_of(b->pos.y),
                                        order, &hit)) continue;
                cand.px = hit.x; cand.py = hit.y;
            }

            if (out->count >= HX421_MAX_CONTACTS) { out->overflow++; continue; }
            out->c[out->count++] = cand;
        }
    }
    return (int)out->count;
}
