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

static Hx421Mesh mesh_r(int32_t radius) {
    Hx421Mesh m = { pv, 3, pf, pc, 1, radius };
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
    ck(hx421_broadphase(s, cl) == 0, "separated objects produce no contacts");
    ck(cl->tests == 1, "one pair, one test");

    hx421_object_set_pos(s, b, V(1,0,0));
    ck(hx421_broadphase(s, cl) == 1, "overlapping objects produce a contact");
    ck(cl->c[0].a == (uint16_t)a && cl->c[0].b == (uint16_t)b, "contact names both objects");
    ck(cl->overflow == 0, "no overflow at two objects");

    /* an object must never collide with itself */
    ck(cl->count == 1, "self-pairing does not happen");

    /* ---- filtering ---- */
    s->obj[b].collidable = 0;
    ck(hx421_broadphase(s, cl) == 0, "collidable=0 opts out");
    ck(cl->tests == 0, "opted-out object is not even tested");
    s->obj[b].collidable = 1;

    s->obj[a].layer = 1; s->obj[b].mask = 0;      /* b accepts nothing */
    ck(hx421_broadphase(s, cl) == 0, "mask 0 collides with nothing");
    s->obj[b].mask = 0xFF;
    ck(hx421_broadphase(s, cl) == 1, "restoring the mask restores the contact");

    /* asymmetric masks must NOT produce a contact — both sides have to agree */
    s->obj[a].layer = 1; s->obj[a].mask = 0xFF;
    s->obj[b].layer = 2; s->obj[b].mask = 0x02;   /* b accepts layer 1 */
    ck(hx421_broadphase(s, cl) == 1, "both sides accepting -> contact");
    s->obj[b].mask = 0x01;                        /* b no longer accepts layer 1 */
    ck(hx421_broadphase(s, cl) == 0, "one side refusing kills the contact");
    s->obj[a].layer = 0; s->obj[a].mask = 0xFF;
    s->obj[b].layer = 0; s->obj[b].mask = 0xFF;

    /* despawned objects drop out of the sweep entirely */
    hx421_object_despawn(s, b);
    ck(hx421_broadphase(s, cl) == 0, "despawned object is not swept");
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
    int got = hx421_broadphase(s, cl);
    ck(got == (int)HX421_MAX_CONTACTS, "contact list saturates at the cap");
    ck(cl->overflow > 0, "dropped pairs are reported, not silently lost");

    /* ---- the measurement docs/collision.md asks for ---- */
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
