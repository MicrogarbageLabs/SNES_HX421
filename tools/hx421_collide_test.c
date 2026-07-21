/* hx421_collide_test.c — broad-phase checks, plus the pair-count measurement
 * that decides whether a spatial hash is warranted. */

#include <stdio.h>
#include <stdlib.h>
#include "../runtime/hx421_collide.h"

static int fails = 0, checks = 0;
static void ck(int cond, const char *what) {
    checks++;
    if (!cond) { printf("  FAIL: %s\n", what); fails++; }
}
static void ck_near(int32_t got, int32_t want, int32_t tol, const char *what) {
    checks++;
    int32_t d = got - want; if (d < 0) d = -d;
    if (d > tol) { printf("  FAIL: %s (got %d want %d tol %d)\n", what, got, want, tol); fails++; }
}

#define ONE 65536
#define V(a,b,c) ((Hx421Vec){ (a)*ONE, (b)*ONE, (c)*ONE })

/* A single-triangle mesh; only `radius` matters to the broad phase. */
static const Hx421Vec pv[3] = {{0,0,0},{ONE,0,0},{0,ONE,0}};
static const uint16_t pf[3] = {0,1,2};
static const uint8_t  pc[1] = {1};

/* Are two unit normals within ~5 degrees of each other? Used to assert that a
 * cube's six derived planes stay distinct. */
static int dot_ok(Hx421Vec a, Hx421Vec b) {
    int64_t d = ((int64_t)a.x*b.x + (int64_t)a.y*b.y + (int64_t)a.z*b.z) >> 16;
    return d > 65100;
}
/* Normalise in the test so a hand-written near-duplicate is actually unit. */
static void normalize_test(Hx421Vec *v) {
    int64_t x = v->x >> 8, y = v->y >> 8, z = v->z >> 8;
    int32_t l = hx421_sqrt_q16(x*x + y*y + z*z);
    if (l < 64) return;
    v->x = (int32_t)(((int64_t)v->x << 16) / l);
    v->y = (int32_t)(((int64_t)v->y << 16) / l);
    v->z = (int32_t)(((int64_t)v->z << 16) / l);
}

static Hx421Mesh mesh_r(int32_t radius) {
    Hx421Mesh m = { pv, 3, pf, pc, 1, radius, {radius, radius, radius}, {0,0,0} };
    return m;
}

int main(void) {
    printf("hx421 collide tests\n");

    /* ---- sqrt ---- */
    ck(hx421_sqrt_q16(0) == 0,           "sqrt(0) == 0");
    ck_near(hx421_sqrt_q16(4*ONE), 2*ONE, 2,   "sqrt(4) == 2");
    ck_near(hx421_sqrt_q16(2*ONE), 92682, 2,   "sqrt(2) == 1.4142");
    ck_near(hx421_sqrt_q16(10000LL*ONE), 100*ONE, 4, "sqrt(10000) == 100");
    ck(hx421_sqrt_q16(-5) == 0,          "sqrt of negative clamps to 0");
    /* monotonic: a rounding bug in the restoring loop shows up as a step back */
    int mono = 1;
    for (int32_t v = 1; v < 4000; v += 7)
        if (hx421_sqrt_q16((int64_t)v * ONE) < hx421_sqrt_q16((int64_t)(v-1) * ONE)) { mono = 0; break; }
    ck(mono, "sqrt is monotonic");

    /* ---- sphere test ---- */
    Hx421Vec n; int32_t d;
    ck(hx421_sphere_test(V(0,0,0), ONE, V(5,0,0), ONE, &n, &d) == 0, "far apart: no overlap");
    ck(hx421_sphere_test(V(0,0,0), ONE, V(2,0,0), ONE, &n, &d) == 0, "exactly touching is not overlap");
    ck(hx421_sphere_test(V(0,0,0), ONE, V(1,0,0), ONE, &n, &d) == 1, "overlapping pair detected");
    ck_near(d, ONE, 64, "penetration depth is r_sum - distance");
    ck_near(n.x, ONE, 64, "normal points from a toward b");
    ck(n.y == 0 && n.z == 0, "normal has no off-axis component");

    /* normal direction must flip with the argument order */
    hx421_sphere_test(V(1,0,0), ONE, V(0,0,0), ONE, &n, &d);
    ck_near(n.x, -ONE, 64, "normal reverses when the pair is swapped");

    /* diagonal: the normal must be UNIT length, not the raw offset */
    hx421_sphere_test(V(0,0,0), 2*ONE, V(1,1,0), 2*ONE, &n, &d);
    {
        int64_t l = (int64_t)n.x*n.x + (int64_t)n.y*n.y + (int64_t)n.z*n.z;
        ck_near((int32_t)(l >> 16), ONE, 512, "diagonal normal is unit length");
    }

    /* concentric objects must not divide by zero */
    ck(hx421_sphere_test(V(3,3,3), ONE, V(3,3,3), ONE, &n, &d) == 1, "concentric pair overlaps");
    ck(n.x || n.y || n.z, "concentric pair still yields a direction");

    /* ---- the sweep ---- */
    Hx421Scene *s = calloc(1, sizeof *s);
    Hx421ContactList *cl = calloc(1, sizeof *cl);
    hx421_scene_init(s);
    Hx421Mesh m = mesh_r(ONE);
    int mid = hx421_mesh_register(s, &m);

    int a = hx421_object_spawn(s, mid);
    int b = hx421_object_spawn(s, mid);
    hx421_object_set_pos(s, a, V(0,0,0));
    hx421_object_set_pos(s, b, V(10,0,0));
    ck(hx421_broadphase(s, cl, HX421_SCAN_TOPLEFT) == 0, "separated objects produce no contacts");
    ck(cl->tests == 1, "one pair, one test");

    hx421_object_set_pos(s, b, V(1,0,0));
    ck(hx421_broadphase(s, cl, HX421_SCAN_TOPLEFT) == 1, "overlapping objects produce a contact");
    ck(cl->c[0].a == (uint16_t)a && cl->c[0].b == (uint16_t)b, "contact names both objects");
    ck(cl->overflow == 0, "no overflow at two objects");

    /* an object must never collide with itself */
    ck(cl->count == 1, "self-pairing does not happen");

    /* ---- filtering ---- */
    s->obj[b].collidable = 0;
    ck(hx421_broadphase(s, cl, HX421_SCAN_TOPLEFT) == 0, "collidable=0 opts out");
    ck(cl->tests == 0, "opted-out object is not even tested");
    s->obj[b].collidable = 1;

    s->obj[a].layer = 1; s->obj[b].mask = 0;      /* b accepts nothing */
    ck(hx421_broadphase(s, cl, HX421_SCAN_TOPLEFT) == 0, "mask 0 collides with nothing");
    s->obj[b].mask = 0xFF;
    ck(hx421_broadphase(s, cl, HX421_SCAN_TOPLEFT) == 1, "restoring the mask restores the contact");

    /* asymmetric masks must NOT produce a contact — both sides have to agree */
    s->obj[a].layer = 1; s->obj[a].mask = 0xFF;
    s->obj[b].layer = 2; s->obj[b].mask = 0x02;   /* b accepts layer 1 */
    ck(hx421_broadphase(s, cl, HX421_SCAN_TOPLEFT) == 1, "both sides accepting -> contact");
    s->obj[b].mask = 0x01;                        /* b no longer accepts layer 1 */
    ck(hx421_broadphase(s, cl, HX421_SCAN_TOPLEFT) == 0, "one side refusing kills the contact");
    s->obj[a].layer = 0; s->obj[a].mask = 0xFF;
    s->obj[b].layer = 0; s->obj[b].mask = 0xFF;

    /* despawned objects drop out of the sweep entirely */
    hx421_object_despawn(s, b);
    ck(hx421_broadphase(s, cl, HX421_SCAN_TOPLEFT) == 0, "despawned object is not swept");
    ck(cl->tests == 0, "despawned object costs no tests");

    /* ---- overflow is reported, not silent ---- */
    hx421_scene_init(s);
    Hx421Mesh big = mesh_r(4 * ONE);
    int bid = hx421_mesh_register(s, &big);
    for (unsigned i = 0; i < HX421_MAX_OBJ; ++i) {
        int o = hx421_object_spawn(s, bid);
        if (o < 0) break;
        hx421_object_set_pos(s, o, V(0,0,0));     /* all mutually overlapping */
    }
    int got = hx421_broadphase(s, cl, HX421_SCAN_TOPLEFT);
    ck(got == (int)HX421_MAX_CONTACTS, "contact list saturates at the cap");
    ck(cl->overflow > 0, "dropped pairs are reported, not silently lost");

    /* ---- 2D actors share the table, the filter and the sweep ---- */
    hx421_scene_init(s);
    {
        /* a solid 16x16 actor mask; bits live outside the registry */
        static uint8_t solid16[2 * 16];
        for (unsigned i = 0; i < sizeof solid16; ++i) solid16[i] = 0xFF;
        Hx421Mask am = { 16, 16, 2, solid16 };
        int mk = hx421_mask_register(s, &am);
        ck(mk == 0, "first mask id is 0");
        ck(hx421_actor_spawn(s, 99) < 0, "actor spawn rejects a bad mask id");

        int p = hx421_actor_spawn(s, mk);
        int q = hx421_actor_spawn(s, mk);
        ck(p >= 0 && q >= 0, "actors spawn into the object table");
        ck(s->obj[p].kind == HX421_BODY_MASK, "an actor is a MASK body");
        ck(s->obj[p].visible == 0, "an actor is not drawn by the 3D renderer");

        /* positions are Q16.16 SCREEN PIXELS for a mask body */
        Hx421Vec pa = { 10 * ONE, 10 * ONE, 0 };
        Hx421Vec pb = { 100 * ONE, 10 * ONE, 0 };
        hx421_object_set_pos(s, p, pa);
        hx421_object_set_pos(s, q, pb);
        ck(hx421_broadphase(s, cl, HX421_SCAN_TOPLEFT) == 0, "separated actors miss");

        Hx421Vec near = { 18 * ONE, 14 * ONE, 0 };
        hx421_object_set_pos(s, q, near);
        ck(hx421_broadphase(s, cl, HX421_SCAN_TOPLEFT) == 1, "overlapping actors hit");
        ck(cl->c[0].kind == HX421_BODY_MASK, "contact records the pair kind");
        ck(cl->c[0].px == 18 && cl->c[0].py == 14, "contact point is in world pixels");
        ck(cl->c[0].depth == 0, "a mask pair reports no fabricated depth");

        /* sub-pixel motion must accumulate rather than round away each frame */
        Hx421Vec half = { ONE / 2, 0, 0 };
        Hx421Vec before = s->obj[q].pos;
        for (int k = 0; k < 2; ++k) hx421_object_translate(s, q, half);
        ck(s->obj[q].pos.x == before.x + ONE, "two half-pixel steps make one pixel");

        /* the layer filter works identically for actors */
        s->obj[q].mask = 0;
        ck(hx421_broadphase(s, cl, HX421_SCAN_TOPLEFT) == 0, "actors honour the layer mask");
        s->obj[q].mask = 0xFF;

        /* scan order picks the contact for actors too */
        Hx421MaskHit unused; (void)unused;
        hx421_broadphase(s, cl, HX421_SCAN_BOTTOMRIGHT);
        ck(cl->count == 1, "actor pair still hits under a different scan order");
        ck(cl->c[0].px != 18 || cl->c[0].py != 14,
           "a different scan order reports a different contact pixel");

        /* ---- mixed kinds: counted, never guessed ---- */
        Hx421Mesh sm = mesh_r(50 * ONE);          /* huge, would overlap anything */
        int smid = hx421_mesh_register(s, &sm);
        int o3 = hx421_object_spawn(s, smid);
        hx421_object_set_pos(s, o3, s->obj[p].pos);
        hx421_broadphase(s, cl, HX421_SCAN_TOPLEFT);
        ck(cl->cross_skipped == 2, "mesh-vs-mask pairs are skipped and counted");
        ck(cl->count == 1, "and produce no contact — a 3D sphere vs 2D pixels is undefined");

        /* the 3D renderer must not try to draw the actors */
        Hx421Tri *tr = calloc(HX421_MAX_TRIS, sizeof *tr);
        Hx421Vec cpos = { 0, 0, -400 * ONE };
        hx421_camera_set_active(s, hx421_camera_register(s, cpos, 128 * ONE));
        s->obj[p].visible = 1;                    /* force the guard to matter */
        int nt = hx421_scene_render(s, tr, HX421_MAX_TRIS, 256, 208);
        ck(nt == 1, "render emits the mesh object only, not the visible actor");
        free(tr);
    }

    /* ---- mid phase: oriented boxes ---- */
    {
        /* A unit cube's 8 corners, so fit_bounds has something to fit. */
        static const Hx421Vec cube[8] = {
            {-ONE,-ONE,-ONE},{ ONE,-ONE,-ONE},{ ONE, ONE,-ONE},{-ONE, ONE,-ONE},
            {-ONE,-ONE, ONE},{ ONE,-ONE, ONE},{ ONE, ONE, ONE},{-ONE, ONE, ONE},
        };
        Hx421Mesh cm = { cube, 8, pf, pc, 1, 0, {0,0,0}, {0,0,0} };
        hx421_mesh_fit_bounds(&cm);
        ck(cm.half.x == ONE && cm.half.y == ONE && cm.half.z == ONE,
           "fit_bounds derives the half-extents");
        ck(cm.centre.x == 0 && cm.centre.y == 0 && cm.centre.z == 0,
           "a symmetric mesh centres on the origin");
        /* farthest vertex is a corner at sqrt(3) ~= 1.732 */
        ck(cm.radius > 113000 && cm.radius < 114100, "fit_bounds derives the sphere radius");

        /* an off-origin mesh must not get a fat sphere */
        static const Hx421Vec off[2] = {{ 10*ONE, 0, 0 }, { 12*ONE, 0, 0 }};
        Hx421Mesh om = { off, 2, pf, pc, 1, 0, {0,0,0}, {0,0,0} };
        hx421_mesh_fit_bounds(&om);
        ck(om.centre.x == 11*ONE, "off-origin mesh centres on its own bounds");
        ck(om.radius <= ONE + 64, "sphere is about the centre, not the origin");

        /* --- the SAT itself --- */
        Hx421Mat I = {{ 32768,0,0, 0,32768,0, 0,0,32768 }};
        Hx421Vec h = { ONE, ONE, ONE }, n; int32_t dep;

        ck(hx421_obb_test(V(0,0,0), I, h, V(5,0,0), I, h, &n, &dep) == 0,
           "separated boxes do not overlap");
        ck(hx421_obb_test(V(0,0,0), I, h, V(1,0,0), I, h, &n, &dep) == 1,
           "overlapping boxes overlap");
        ck_near(dep, ONE, 256, "penetration depth on the shallow axis");
        ck_near(n.x, ONE, 512, "normal is the separating axis, a toward b");
        ck(hx421_obb_test(V(0,0,0), I, h, V(0,0,3), I, h, &n, &dep) == 0,
           "separation on z is found too");

        /* the normal must reverse with the argument order */
        hx421_obb_test(V(1,0,0), I, h, V(0,0,0), I, h, &n, &dep);
        ck_near(n.x, -ONE, 512, "normal reverses when the pair is swapped");

        /* THE case axis-aligned boxes get wrong: two unit cubes 2.4 apart on a
         * diagonal. An AABB grown to enclose a 45-degree-rotated cube reaches
         * sqrt(2) ~= 1.41 per side and would report a hit; the oriented boxes
         * do not touch. This is why the mid phase is OBB and not AABB. */
        Hx421Scene *t = calloc(1, sizeof *t);
        hx421_scene_init(t);
        int cid = hx421_mesh_register(t, &cm);
        int o1 = hx421_object_spawn(t, cid);
        int o2 = hx421_object_spawn(t, cid);
        Hx421Vec far45 = { 2400 * (ONE/1000), 2400 * (ONE/1000), 0 };
        hx421_object_set_pos(t, o1, V(0,0,0));
        hx421_object_set_pos(t, o2, far45);
        hx421_object_set_rot(t, o2, 128, 0, 0);        /* 45 degrees of yaw */

        Hx421ContactList *tl = calloc(1, sizeof *tl);
        int bp = hx421_broadphase(t, tl, HX421_SCAN_TOPLEFT);
        ck(bp == 1, "spheres (radius sqrt3 each) say maybe at 3.39 apart");
        ck(hx421_midphase(t, tl) == 0, "oriented boxes say no — the sphere was optimistic");

        /* and when they really do overlap, the mid phase keeps them and
         * replaces the sphere normal with the box normal */
        hx421_object_set_pos(t, o2, V(1,0,0));
        hx421_object_set_rot(t, o2, 0, 0, 0);
        hx421_broadphase(t, tl, HX421_SCAN_TOPLEFT);
        ck(hx421_midphase(t, tl) == 1, "genuinely overlapping boxes survive");
        ck_near(tl->c[0].normal.x, ONE, 512, "surviving contact carries the box normal");
        ck_near(tl->c[0].depth, ONE, 256, "and the box depth");

        /* a rotation that does NOT separate them must still report a hit --
         * otherwise the test above passes for the wrong reason */
        hx421_object_set_rot(t, o2, 128, 0, 0);
        hx421_broadphase(t, tl, HX421_SCAN_TOPLEFT);
        ck(hx421_midphase(t, tl) == 1, "a 45-degree box at 1.0 apart still overlaps");

        /* mask pairs pass through the mid phase untouched */
        hx421_scene_init(t);
        {
            static uint8_t s16[2 * 16];
            for (unsigned i = 0; i < sizeof s16; ++i) s16[i] = 0xFF;
            Hx421Mask am2 = { 16, 16, 2, s16 };
            int mk2 = hx421_mask_register(t, &am2);
            int a1 = hx421_actor_spawn(t, mk2), a2 = hx421_actor_spawn(t, mk2);
            hx421_object_set_pos(t, a1, V(0,0,0));
            Hx421Vec p2 = { 8*ONE, 8*ONE, 0 };
            hx421_object_set_pos(t, a2, p2);
            hx421_broadphase(t, tl, HX421_SCAN_TOPLEFT);
            ck(tl->count == 1, "actor pair found by the broad phase");
            ck(hx421_midphase(t, tl) == 1, "mask pairs survive the mid phase untouched");
            ck(tl->c[0].px == 8 && tl->c[0].py == 8, "and keep their contact point");
        }
        free(tl); free(t);
    }

    /* ---- narrow phase: plane sets and deflection ---- */
    {
        static const Hx421Vec cube[8] = {
            {-ONE,-ONE,-ONE},{ ONE,-ONE,-ONE},{ ONE, ONE,-ONE},{-ONE, ONE,-ONE},
            {-ONE,-ONE, ONE},{ ONE,-ONE, ONE},{ ONE, ONE, ONE},{-ONE, ONE, ONE},
        };
        static const uint16_t cf[36] = {
            0,2,1, 0,3,2,  4,5,6, 4,6,7,  0,1,5, 0,5,4,
            3,6,2, 3,7,6,  0,4,7, 0,7,3,  1,2,6, 1,6,5,
        };
        static const uint8_t cc12[12] = {1,1,2,2,3,3,4,4,5,5,6,6};
        Hx421Plane planes[HX421_MAX_PLANES];
        Hx421Mesh cm = { cube, 8, cf, cc12, 12, 0, {0,0,0}, {0,0,0}, 0, 0 };
        hx421_mesh_fit_bounds(&cm);

        unsigned np = hx421_mesh_fit_planes(&cm, planes, HX421_MAX_PLANES);
        ck(np == 6, "a cube's 12 faces merge into 6 planes");
        ck(cm.pcount == 6 && cm.planes == planes, "fit_planes wires the mesh up");

        /* every plane must be unit length and at offset 1 from the origin */
        int unit_ok = 1, off_ok = 1;
        for (unsigned i = 0; i < np; ++i) {
            int64_t l = (int64_t)planes[i].n.x*planes[i].n.x
                      + (int64_t)planes[i].n.y*planes[i].n.y
                      + (int64_t)planes[i].n.z*planes[i].n.z;
            int32_t lq = (int32_t)(l >> 16);
            if (lq < ONE - 700 || lq > ONE + 700) unit_ok = 0;
            int32_t dd = planes[i].d - ONE; if (dd < 0) dd = -dd;
            if (dd > 700) off_ok = 0;
        }
        ck(unit_ok, "derived plane normals are unit length");
        ck(off_ok, "a unit cube's planes all sit at offset 1");

        /* the six normals must be distinct — a slab's opposite faces must NOT
         * merge, which is what the offset check in the merge exists to prevent */
        int distinct = 1;
        for (unsigned i = 0; i < np && distinct; ++i)
            for (unsigned j = i + 1; j < np; ++j)
                if (dot_ok(planes[i].n, planes[j].n)) { distinct = 0; break; }
        ck(distinct, "opposite faces stay separate planes");

        /* fit_planes must respect a cap smaller than the plane count */
        Hx421Plane few[2];
        Hx421Mesh cm2 = cm;
        ck(hx421_mesh_fit_planes(&cm2, few, 2) == 2, "fit_planes honours the cap");

        /* --- narrow phase fills the set --- */
        Hx421Scene *t = calloc(1, sizeof *t);
        Hx421ContactList *tl = calloc(1, sizeof *tl);
        hx421_scene_init(t);
        int cid = hx421_mesh_register(t, &cm);
        int mover = hx421_object_spawn(t, cid);
        int wall  = hx421_object_spawn(t, cid);
        hx421_object_set_pos(t, wall, V(0,0,0));

        /* Mover overlapping the wall's +x face. Unit cubes, so the boxes span
         * [0.5, 2.5] and [-1, 1] — 0.5 of overlap. (At 2.5 they would be a full
         * half-unit APART; the mid phase rejecting that is correct.) */
        Hx421Vec near_x = { ONE + ONE/2, 0, 0 };
        hx421_object_set_pos(t, mover, near_x);
        hx421_broadphase(t, tl, HX421_SCAN_TOPLEFT);
        hx421_midphase(t, tl);
        ck(tl->count == 1, "pair survives to the narrow phase");
        hx421_narrowphase(t, tl);
        /* A flat face-on contact must report exactly ONE normal. Asserting only
         * ">= 1" hides the failure that matters: over-reporting normals makes
         * the resolver see three independent contacts, call it a wedge, and
         * ZERO the velocity — so driving straight into a wall would stop dead
         * instead of bouncing. */
        ck(tl->c[0].nnormals == 1, "a face-on contact reports exactly one normal");
        ck_near(tl->c[0].normals[0].x, ONE, 512, "and it is the face it actually hit");
        {
            Hx421Vec into = { -ONE, 0, 0 }, back;
            ck(hx421_resolve(tl->c[0].normals, tl->c[0].nnormals, into,
                             HX421_RESPONSE_BOUNCE, &back) == 1,
               "a flat wall is not a wedge");
            ck_near(back.x, ONE, 512, "and it bounces straight back");
        }

        /* An edge-on contact must report exactly TWO — the case a single
         * winning plane gets wrong. */
        {
            Hx421Vec corner = { ONE + ONE/2, ONE + ONE/2, 0 };
            hx421_object_set_pos(t, mover, corner);
            hx421_broadphase(t, tl, HX421_SCAN_TOPLEFT);
            hx421_midphase(t, tl);
            hx421_narrowphase(t, tl);
            ck(tl->count == 1, "corner pair survives the mid phase");
            ck(tl->c[0].nnormals == 2, "an edge-on contact reports exactly two normals");
        }
        hx421_object_set_pos(t, mover, near_x);

        /* a contact always carries at least one normal, even with no planes */
        {
            Hx421Mesh bare = cm; bare.planes = 0; bare.pcount = 0;
            Hx421Scene *u = calloc(1, sizeof *u);
            hx421_scene_init(u);
            int bid3 = hx421_mesh_register(u, &bare);
            int x1 = hx421_object_spawn(u, bid3), x2 = hx421_object_spawn(u, bid3);
            hx421_object_set_pos(u, x1, V(0,0,0));
            hx421_object_set_pos(u, x2, V(1,0,0));
            Hx421ContactList *ul = calloc(1, sizeof *ul);
            hx421_broadphase(u, ul, HX421_SCAN_TOPLEFT);
            hx421_midphase(u, ul);
            hx421_narrowphase(u, ul);
            ck(ul->count == 1 && ul->c[0].nnormals == 1,
               "a plane-less mesh falls back to the box normal");
            free(ul); free(u);
        }

        /* --- resolution --- */
        Hx421Vec nx = { ONE, 0, 0 }, ny = { 0, ONE, 0 }, nz = { 0, 0, ONE };
        Hx421Vec vin = { -2 * ONE, 3 * ONE, 0 }, vout;

        /* one contact: the plain v - 2(v.n)n */
        ck(hx421_resolve(&nx, 1, vin, HX421_RESPONSE_BOUNCE, &vout) == 1, "single contact resolves");
        ck_near(vout.x, 2 * ONE, 256, "bounce reverses the into-surface component");
        ck_near(vout.y, 3 * ONE, 256, "and leaves the tangent untouched");

        /* slide merely removes it */
        hx421_resolve(&nx, 1, vin, HX421_RESPONSE_SLIDE, &vout);
        ck_near(vout.x, 0, 256, "slide removes the into-surface component");
        ck_near(vout.y, 3 * ONE, 256, "slide keeps the tangent");

        /* THE case a single winning plane gets wrong: two walls at a right
         * angle. Resolving against only one sends the object into the other. */
        {
            Hx421Vec pair[2] = { nx, ny };
            Hx421Vec vdiag = { -2 * ONE, -2 * ONE, 0 };
            ck(hx421_resolve(pair, 2, vdiag, HX421_RESPONSE_BOUNCE, &vout) == 1,
               "a two-wall corner resolves");
            ck_near(vout.x, 2 * ONE, 256, "corner reflects out on x");
            ck_near(vout.y, 2 * ONE, 256, "and on y at the same time");
        }

        /* near-duplicate normals (an approximated curve) must NOT double-apply.
         * Six copies of the same wall must give the same answer as one. */
        {
            Hx421Vec dupes[4] = { nx, nx, nx, nx };
            dupes[1].y = ONE / 32;  normalize_test(&dupes[1]);   /* ~2 degrees off */
            dupes[2].y = -ONE / 32; normalize_test(&dupes[2]);
            hx421_resolve(dupes, 4, vin, HX421_RESPONSE_BOUNCE, &vout);
            ck_near(vout.x, 2 * ONE, 3000, "near-duplicate normals dedup to one");
            ck(vout.x < 4 * ONE, "and do not multiply the correction");
        }

        /* three independent contacts = wedged: stop, do not resolve */
        {
            Hx421Vec three[3] = { nx, ny, nz };
            Hx421Vec vany = { -ONE, -ONE, -ONE };
            ck(hx421_resolve(three, 3, vany, HX421_RESPONSE_BOUNCE, &vout) == 0,
               "three independent contacts report wedged");
            ck(vout.x == 0 && vout.y == 0 && vout.z == 0, "and zero the velocity");
        }

        /* an empty set is a no-op, not a crash or a zeroing */
        ck(hx421_resolve(0, 0, vin, HX421_RESPONSE_BOUNCE, &vout) == 1, "empty set resolves");
        ck(vout.x == vin.x && vout.y == vin.y, "empty set leaves the velocity alone");

        /* passthrough is a flag on the entry, not a separate path: the contact
         * is still reported so a trigger volume can fire. */
        t->obj[mover].passthrough = 1;
        hx421_broadphase(t, tl, HX421_SCAN_TOPLEFT);
        hx421_midphase(t, tl);
        ck(tl->count == 1, "a passthrough object still reports its hit");

        free(tl); free(t);
    }

    /* ---- the measurement docs/collision.md asks for ---- */
    hx421_scene_init(s);
    {
        Hx421Mesh big2 = mesh_r(4 * ONE);
        int bid2 = hx421_mesh_register(s, &big2);
        for (unsigned i = 0; i < HX421_MAX_OBJ; ++i) {
            int o = hx421_object_spawn(s, bid2);
            if (o < 0) break;
            hx421_object_set_pos(s, o, V(0,0,0));
        }
        hx421_broadphase(s, cl, HX421_SCAN_TOPLEFT);
    }
    unsigned n_obj = 0;
    for (unsigned i = 0; i < HX421_MAX_OBJ; ++i) if (s->obj[i].active) n_obj++;
    printf("\n  measurement: %u objects -> %u pair tests "
           "(n(n-1)/2 = %u), %u contacts, %u dropped\n",
           n_obj, cl->tests, n_obj * (n_obj - 1) / 2, cl->count, cl->overflow);
    ck(cl->tests == n_obj * (n_obj - 1) / 2, "all-pairs sweep tests exactly n(n-1)/2");

    free(cl); free(s);
    printf("%d checks, %d failures\n", checks, fails);
    return fails ? 1 : 0;
}
