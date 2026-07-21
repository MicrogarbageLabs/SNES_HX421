/* hx421_scene3d_test.c — checks for the 3D object registry and transforms.
 *
 * Built alongside hx421_raster_test. Everything here is exact integer maths, so
 * the assertions are on values, with a tolerance only where the Q15 rounding of
 * a rotation genuinely accumulates.
 */

#include <stdio.h>
#include <stdlib.h>
#include "../runtime/hx421_scene3d.h"

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

#define V(a,b,c) ((Hx421Vec){ (a)*65536, (b)*65536, (c)*65536 })
#define ONE 65536

/* v' = M * v, Q15 matrix against a Q16.16 vector — the same operation the
 * renderer performs, replicated so the test can check its result directly. */
static Hx421Vec mat_apply_test(const Hx421Mat *m, Hx421Vec v) {
    Hx421Vec r;
    r.x = (int32_t)(((int64_t)m->m[0]*v.x + (int64_t)m->m[1]*v.y + (int64_t)m->m[2]*v.z) >> 15);
    r.y = (int32_t)(((int64_t)m->m[3]*v.x + (int64_t)m->m[4]*v.y + (int64_t)m->m[5]*v.z) >> 15);
    r.z = (int32_t)(((int64_t)m->m[6]*v.x + (int64_t)m->m[7]*v.y + (int64_t)m->m[8]*v.z) >> 15);
    return r;
}

/* A unit cube centred on the origin, 12 triangles. */
static const Hx421Vec cube_v[8] = {
    {-ONE,-ONE,-ONE},{ ONE,-ONE,-ONE},{ ONE, ONE,-ONE},{-ONE, ONE,-ONE},
    {-ONE,-ONE, ONE},{ ONE,-ONE, ONE},{ ONE, ONE, ONE},{-ONE, ONE, ONE},
};
static const uint16_t cube_f[36] = {
    0,1,2, 0,2,3,  4,6,5, 4,7,6,  0,4,5, 0,5,1,
    3,2,6, 3,6,7,  0,3,7, 0,7,4,  1,5,6, 1,6,2,
};
static const uint8_t cube_c[12] = {1,1,2,2,3,3,4,4,5,5,6,6};

static Hx421Mesh cube_mesh(void) {
    Hx421Mesh m;
    m.verts = cube_v; m.vcount = 8;
    m.faces = cube_f; m.colors = cube_c; m.fcount = 12;
    m.radius = 113512;                       /* sqrt(3) in Q16.16 */
    return m;
}

int main(void) {
    printf("hx421 scene3d tests\n");

    /* ---- trig ---- */
    ck(hx421_sin_q15(0) == 0,        "sin(0) == 0");
    ck(hx421_sin_q15(256) == 32768,  "sin(quarter) == 1.0");
    ck(hx421_sin_q15(512) == 0,      "sin(half) == 0");
    ck(hx421_sin_q15(768) == -32768, "sin(three quarter) == -1.0");
    ck(hx421_cos_q15(0) == 32768,    "cos(0) == 1.0");
    ck(hx421_sin_q15(1024 + 128) == hx421_sin_q15(128), "angle wraps by mask");
    ck(hx421_sin_q15(-256) == -32768, "negative angle wraps");
    /* sin^2 + cos^2 == 1 across the whole turn, which catches a table typo that
     * a handful of spot values would sail past. */
    int pyth_ok = 1;
    for (int a = 0; a < 1024; ++a) {
        int64_t s = hx421_sin_q15(a), c = hx421_cos_q15(a);
        int64_t sum = (s*s + c*c) >> 15;
        if (sum < 32700 || sum > 32800) { pyth_ok = 0; break; }
    }
    ck(pyth_ok, "sin^2 + cos^2 == 1 over the full turn");

    /* ---- registry lifecycle ---- */
    Hx421Scene *s = calloc(1, sizeof *s);
    hx421_scene_init(s);
    Hx421Mesh cm = cube_mesh();
    int mid = hx421_mesh_register(s, &cm);
    ck(mid == 0, "first mesh id is 0");

    int a = hx421_object_spawn(s, mid);
    int b = hx421_object_spawn(s, mid);
    ck(a == 0 && b == 1, "objects spawn into ascending slots");
    ck(hx421_object_spawn(s, 99) < 0, "spawn rejects a bad mesh id");

    /* mesh reuse is the whole point of the registry — two objects, one copy */
    ck(s->obj[a].mesh == s->obj[b].mesh, "instances share one mesh");

    hx421_object_despawn(s, a);
    int c2 = hx421_object_spawn(s, mid);
    ck(c2 == a, "a despawned slot is reused");

    /* ---- absolute vs relative ---- */
    hx421_object_set_pos(s, b, V(10, 0, 0));
    ck(s->obj[b].pos.x == 10*ONE, "set_pos is absolute");
    hx421_object_set_pos(s, b, V(3, 0, 0));
    ck(s->obj[b].pos.x == 3*ONE, "set_pos replaces, does not accumulate");

    hx421_object_translate(s, b, V(1, 0, 0));
    hx421_object_translate(s, b, V(1, 0, 0));
    ck(s->obj[b].pos.x == 5*ONE, "translate accumulates");

    /* move_local must follow the object's own axes: yaw a quarter turn and
     * "forward" (+z) becomes world +x. This is the distinction that makes the
     * registry worth having — the caller never rotates its own motion vector. */
    hx421_object_set_pos(s, b, V(0,0,0));
    hx421_object_set_rot(s, b, 256, 0, 0);            /* yaw +90 deg */
    hx421_object_move_local(s, b, V(0, 0, 1));
    ck_near(s->obj[b].pos.x, ONE, 64, "move_local +z after yaw90 -> world +x");
    ck_near(s->obj[b].pos.z, 0,   64, "move_local +z after yaw90 -> world z ~0");

    /* ---- the matrix must stay a ROTATION over a long session ----
     * Composing Q15 matrices truncates, and the demo composes one per object per
     * frame. Unrenormalised this drifted to row lengths of 0.72/1.04 with rows
     * 0.23 out of orthogonal after a minute of play, which SHEARS AND SQUASHES
     * every object — it looks like a projection or aspect bug, not like matrix
     * error, which is exactly why it needs a standing test.
     *
     * 20000 iterations is ~5.5 minutes at 60 Hz. Checking after a handful of
     * rotations proves nothing: the error is cumulative. */
    hx421_object_set_rot(s, b, 0, 0, 0);
    for (int i = 0; i < 20000; ++i) hx421_object_rotate(s, b, 3, 2, 0);
    {
        const Hx421Mat *r = &s->obj[b].rot;
        int rigid = 1;
        for (int i = 0; i < 3; ++i) {
            int64_t len = 0;
            for (int k = 0; k < 3; ++k) len += (int64_t)r->m[i*3+k] * r->m[i*3+k];
            len >>= 15;                                   /* -> Q15 */
            if (len < 32768 - 200 || len > 32768 + 200) rigid = 0;
        }
        ck(rigid, "rows stay unit length over 20000 rotations");

        int orthogonal = 1;
        for (int i = 0; i < 3 && orthogonal; ++i)
            for (int j = i + 1; j < 3; ++j) {
                int64_t d = 0;
                for (int k = 0; k < 3; ++k) d += (int64_t)r->m[i*3+k] * r->m[j*3+k];
                d >>= 15;
                if (d < -200 || d > 200) { orthogonal = 0; break; }
            }
        ck(orthogonal, "rows stay mutually orthogonal over 20000 rotations");

        /* And the consequence that actually matters: a cube still projects with
         * the aspect it started with, rather than progressively squashing. */
        Hx421Vec vtest = { ONE, 0, 0 };
        Hx421Vec rot = mat_apply_test(r, vtest);
        int64_t l2 = ((int64_t)rot.x*rot.x + (int64_t)rot.y*rot.y + (int64_t)rot.z*rot.z) >> 16;
        ck_near((int32_t)l2, ONE, 600, "a rotated unit vector keeps unit length");
    }

    /* four 90-degree relative rotations must return to identity */
    hx421_object_set_rot(s, b, 0, 0, 0);
    for (int i = 0; i < 4; ++i) hx421_object_rotate(s, b, 256, 0, 0);
    ck_near(s->obj[b].rot.m[0], 32768, 8, "4x rotate(90) returns to identity");
    ck_near(s->obj[b].rot.m[2], 0,     8, "4x rotate(90) leaves no residue");

    /* ---- point_at ---- */
    hx421_object_set_pos(s, b, V(0,0,0));
    hx421_object_point_at(s, b, V(0, 0, 10), 1);
    ck_near(s->obj[b].rot.m[0], 32768, 32, "point_at +z is identity yaw");
    hx421_object_point_at(s, b, V(10, 0, 0), 1);
    /* facing +x means local forward +z maps to world +x: column 2 row 0 == 1 */
    ck_near(s->obj[b].rot.m[2], 32768, 64, "point_at +x yaws a quarter turn");

    /* ---- camera ---- */
    int cam = hx421_camera_register(s, V(0, 0, -8), 128*ONE);   /* focal in PIXELS */
    ck(cam == 0, "first camera id is 0");
    hx421_camera_set_active(s, cam);

    hx421_camera_translate(s, cam, V(0, 1, 0));
    ck(s->cam[cam].pos.y == ONE, "camera translate accumulates");
    hx421_camera_set_pos(s, cam, V(0, 0, -8));
    ck(s->cam[cam].pos.y == 0, "camera set_pos is absolute");

    /* ---- render ---- */
    for (unsigned i = 0; i < HX421_MAX_OBJ; ++i) s->obj[i].active = 0;
    int o = hx421_object_spawn(s, mid);
    hx421_object_set_pos(s, o, V(0, 0, 0));
    hx421_object_set_rot(s, o, 0, 0, 0);

    Hx421Tri tris[HX421_MAX_TRIS];
    int n = hx421_scene_render(s, tris, HX421_MAX_TRIS, 256, 208);
    ck(n == 12, "a cube in front of the camera emits all 12 triangles");

    /* Projected geometry must land on screen and straddle the centre. */
    int32_t minx = 1<<30, maxx = -(1<<30);
    for (int i = 0; i < n; ++i)
        for (int k = 0; k < 3; ++k) {
            if (tris[i].v[k].x < minx) minx = tris[i].v[k].x;
            if (tris[i].v[k].x > maxx) maxx = tris[i].v[k].x;
        }
    ck(minx < 128*HX421_ONE && maxx > 128*HX421_ONE, "cube straddles screen centre");
    ck(minx > 0 && maxx < 256*HX421_ONE, "cube projects inside the viewport");

    /* SIZE, not just position. `focal` is in pixels; passing it a world-scale
     * number instead collapses the scene to a couple of pixels, which still
     * straddles the centre and still sits inside the viewport — both checks
     * above pass while the screen is empty. A unit cube at depth 8 with
     * focal 128 spans roughly 2*128/8 = 32 px, so assert it is at least half
     * that rather than merely non-zero. */
    ck((maxx - minx) > 16*HX421_ONE, "cube projects at a plausible pixel size");

    /* ---- pixel aspect ----
     * SNES pixels are ~1.167x wider than tall (8:7 on a 4:3 display), so a
     * world-space cube must project NARROWER in pixels than it is tall for it to
     * look square on the TV. Asserting equal pixel extents would enforce exactly
     * the distortion this corrects — the check has to be in physical space. */
    {
        int32_t miny = 1<<30, maxy = -(1<<30);
        for (int i = 0; i < n; ++i)
            for (int k = 0; k < 3; ++k) {
                if (tris[i].v[k].y < miny) miny = tris[i].v[k].y;
                if (tris[i].v[k].y > maxy) maxy = tris[i].v[k].y;
            }
        const int32_t wpx = maxx - minx, hpx = maxy - miny;
        ck(wpx < hpx, "a cube spans fewer pixels across than down");
        /* physical width = wpx * PAR; should match the height within a few % */
        const int32_t phys_w = (int32_t)(((int64_t)wpx * HX421_PAR_SNES) >> 16);
        const int32_t err = (phys_w > hpx ? phys_w - hpx : hpx - phys_w);
        ck(err * 20 < hpx, "and is square once the pixel aspect is applied");

        /* square-pixel output must fall back to equal extents */
        hx421_camera_set_par(s, cam, HX421_PAR_SQUARE);
        n = hx421_scene_render(s, tris, HX421_MAX_TRIS, 256, 208);
        int32_t mny = 1<<30, mxy = -(1<<30), mnx = 1<<30, mxx = -(1<<30);
        for (int i = 0; i < n; ++i)
            for (int k = 0; k < 3; ++k) {
                if (tris[i].v[k].y < mny) mny = tris[i].v[k].y;
                if (tris[i].v[k].y > mxy) mxy = tris[i].v[k].y;
                if (tris[i].v[k].x < mnx) mnx = tris[i].v[k].x;
                if (tris[i].v[k].x > mxx) mxx = tris[i].v[k].x;
            }
        const int32_t d = (mxx-mnx) > (mxy-mny) ? (mxx-mnx)-(mxy-mny) : (mxy-mny)-(mxx-mnx);
        ck(d * 20 < (mxy - mny), "square pixels give equal pixel extents");
        hx421_camera_set_par(s, cam, HX421_PAR_SNES);
    }

    /* ---- field of view ---- */
    {
        hx421_camera_set_fov_preset(s, cam, HX421_FOV_NORMAL, 240);
        const int32_t fx_norm = s->cam[cam].focal_x;
        const int32_t fy_norm = s->cam[cam].focal_y;
        ck(fy_norm > fx_norm, "the aspect correction survives a FOV change");

        hx421_camera_set_fov_preset(s, cam, HX421_FOV_NARROW, 240);
        ck(s->cam[cam].focal_x > fx_norm, "a narrower FOV is a longer focal length");
        hx421_camera_set_fov_preset(s, cam, HX421_FOV_VERY_WIDE, 240);
        ck(s->cam[cam].focal_x < fx_norm, "a wider FOV is a shorter focal length");

        /* 90 degrees on a 240-wide screen must give focal == half the width,
         * which is the one value that can be checked by hand. */
        ck_near(s->cam[cam].focal_x, 120 * ONE, ONE, "90 deg -> focal = half width");

        /* the aspect ratio must be PRESERVED across FOV changes, not recomputed
         * from the new focal_x against the stale focal_y */
        const int32_t par_now = (int32_t)(((int64_t)s->cam[cam].focal_y << 16)
                                          / s->cam[cam].focal_x);
        ck_near(par_now, HX421_PAR_SNES, 600, "FOV changes preserve the pixel aspect");

        /* degenerate inputs must not produce a zero focal (a collapsed scene) */
        hx421_camera_set_fov(s, cam, 0, 240);
        ck(s->cam[cam].focal_x > 0, "a zero FOV is clamped, not accepted");
        hx421_camera_set_fov(s, cam, 5000, 240);
        ck(s->cam[cam].focal_x > 0, "an absurd FOV is clamped, not accepted");

        hx421_camera_set_fov_preset(s, cam, HX421_FOV_NORMAL, 240);
    }

    /* Perspective: the near face must project WIDER than the far face. A bug
     * that drops the divide entirely still passes an on-screen check.
     * Split on the observed midpoint rather than a magic depth — a hardcoded
     * threshold silently put every vertex on one side, which made this pass
     * while measuring nothing. */
    int32_t zlo = 0xFFFF, zhi = 0;
    for (int i = 0; i < n; ++i)
        for (int k = 0; k < 3; ++k) {
            if (tris[i].v[k].z < zlo) zlo = tris[i].v[k].z;
            if (tris[i].v[k].z > zhi) zhi = tris[i].v[k].z;
        }
    ck(zhi > zlo, "the cube spans a range of depths");

    const int32_t zmid = (zlo + zhi) / 2;
    int32_t near_x = 0, far_x = 0, near_n = 0, far_n = 0;
    for (int i = 0; i < n; ++i)
        for (int k = 0; k < 3; ++k) {
            int32_t dx = tris[i].v[k].x - 128*HX421_ONE; if (dx < 0) dx = -dx;
            if (tris[i].v[k].z < zmid) { if (dx > near_x) near_x = dx; near_n++; }
            else                       { if (dx > far_x)  far_x  = dx; far_n++;  }
        }
    ck(near_n > 0 && far_n > 0, "both depth ranges are populated");
    ck(near_x > far_x, "near face projects wider than far face");

    /* Moving the object behind the camera must cull it entirely. */
    hx421_object_set_pos(s, o, V(0, 0, -40));
    ck(hx421_scene_render(s, tris, HX421_MAX_TRIS, 256, 208) == 0,
       "object behind the camera is culled");

    /* An invisible object emits nothing but stays registered. */
    hx421_object_set_pos(s, o, V(0, 0, 0));
    s->obj[o].visible = 0;
    ck(hx421_scene_render(s, tris, HX421_MAX_TRIS, 256, 208) == 0, "visible=0 emits nothing");
    ck(s->obj[o].active == 1, "invisible object stays registered");
    s->obj[o].visible = 1;

    /* The output cap must be honoured — the rasteriser's array is fixed. */
    ck(hx421_scene_render(s, tris, 5, 256, 208) == 5, "render honours max");

    /* Camera motion and object motion must be equivalent in view space: pushing
     * the camera back by 1 matches pulling the object forward by 1. */
    hx421_camera_set_pos(s, cam, V(0, 0, -8));
    hx421_scene_render(s, tris, HX421_MAX_TRIS, 256, 208);
    int32_t ref = tris[0].v[0].x;
    hx421_camera_translate(s, cam, V(1, 0, 0));
    hx421_object_translate(s, o, V(1, 0, 0));
    hx421_scene_render(s, tris, HX421_MAX_TRIS, 256, 208);
    ck_near(tris[0].v[0].x, ref, 2, "equal camera and object translation cancel");

    free(s);
    printf("%d checks, %d failures\n", checks, fails);
    return fails ? 1 : 0;
}
