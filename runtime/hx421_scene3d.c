/* ============================================================
 *  hx421_scene3d.c — object registry, cameras, transform pipeline
 *
 *  Integer-only. Positions Q16.16, rotations Q15 3x3, angles in 1024ths of a
 *  turn so wrapping is a mask. See the header for why absolute and relative
 *  command forms both exist.
 *
 *  Public domain (CC0). No warranty.
 * ============================================================ */

#include "hx421_scene3d.h"

#define Q15_ONE   32768
#define ANG_MASK  1023

/* Quarter-turn sine table, Q15, 257 entries so index 256 == 1.0 exactly.
 * sin_q[i] = round(sin(i/256 * pi/2) * 32768). One quarter plus symmetry is all
 * that is stored — in fabric this is a single M9K. */
static const int32_t sin_q[257] = {
        0,   201,   402,   603,   804,  1005,  1206,  1407,  1608,  1809,
     2009,  2210,  2411,  2611,  2811,  3012,  3212,  3412,  3612,  3812,
     4011,  4211,  4410,  4609,  4808,  5007,  5205,  5404,  5602,  5800,
     5998,  6195,  6393,  6590,  6787,  6983,  7180,  7376,  7571,  7767,
     7962,  8157,  8351,  8546,  8740,  8933,  9127,  9319,  9512,  9704,
     9896, 10088, 10279, 10469, 10660, 10850, 11039, 11228, 11417, 11605,
    11793, 11980, 12167, 12354, 12540, 12725, 12910, 13095, 13279, 13463,
    13646, 13828, 14010, 14192, 14373, 14553, 14733, 14912, 15091, 15269,
    15447, 15624, 15800, 15976, 16151, 16326, 16500, 16673, 16846, 17018,
    17190, 17361, 17531, 17700, 17869, 18037, 18205, 18372, 18538, 18703,
    18868, 19032, 19195, 19358, 19520, 19681, 19841, 20001, 20160, 20318,
    20475, 20632, 20788, 20943, 21097, 21251, 21403, 21555, 21706, 21856,
    22006, 22154, 22302, 22449, 22595, 22740, 22884, 23028, 23170, 23312,
    23453, 23593, 23732, 23870, 24008, 24144, 24279, 24414, 24548, 24680,
    24812, 24943, 25073, 25202, 25330, 25457, 25583, 25708, 25833, 25956,
    26078, 26199, 26320, 26439, 26557, 26674, 26791, 26906, 27020, 27133,
    27246, 27357, 27467, 27576, 27684, 27791, 27897, 28002, 28106, 28209,
    28311, 28411, 28511, 28610, 28707, 28803, 28899, 28993, 29086, 29178,
    29269, 29359, 29448, 29535, 29622, 29707, 29792, 29875, 29957, 30038,
    30118, 30196, 30274, 30350, 30425, 30499, 30572, 30644, 30715, 30784,
    30853, 30920, 30986, 31050, 31114, 31177, 31238, 31298, 31357, 31415,
    31471, 31527, 31581, 31634, 31686, 31737, 31786, 31834, 31881, 31927,
    31972, 32015, 32058, 32099, 32138, 32177, 32214, 32251, 32286, 32319,
    32352, 32383, 32413, 32442, 32470, 32496, 32522, 32546, 32568, 32590,
    32610, 32629, 32647, 32664, 32679, 32693, 32706, 32718, 32729, 32738,
    32746, 32753, 32758, 32762, 32766, 32767, 32768,
};

/* sin over 1024 steps, built from the quarter table by symmetry. */
int32_t hx421_sin_q15(int32_t a) {
    a &= ANG_MASK;
    if (a < 256)  return  sin_q[a];
    if (a < 512)  return  sin_q[512 - a];
    if (a < 768)  return -sin_q[a - 512];
    return               -sin_q[1024 - a];
}
int32_t hx421_cos_q15(int32_t a) { return hx421_sin_q15(a + 256); }

static int32_t qmul15(int32_t a, int32_t b) { return (int32_t)(((int64_t)a * b) >> 15); }

static Hx421Mat mat_identity(void) {
    Hx421Mat r = {{ Q15_ONE,0,0, 0,Q15_ONE,0, 0,0,Q15_ONE }};
    return r;
}
static Hx421Mat mat_mul(Hx421Mat a, Hx421Mat b) {
    Hx421Mat r;
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j)
            r.m[i*3+j] = qmul15(a.m[i*3+0], b.m[0*3+j])
                       + qmul15(a.m[i*3+1], b.m[1*3+j])
                       + qmul15(a.m[i*3+2], b.m[2*3+j]);
    return r;
}
static Hx421Mat mat_euler(int32_t yaw, int32_t pitch, int32_t roll) {
    const int32_t sy = hx421_sin_q15(yaw),   cy = hx421_cos_q15(yaw);
    const int32_t sp = hx421_sin_q15(pitch), cp = hx421_cos_q15(pitch);
    const int32_t sr = hx421_sin_q15(roll),  cr = hx421_cos_q15(roll);
    Hx421Mat Y = {{ cy,0,sy,  0,Q15_ONE,0,  -sy,0,cy }};
    Hx421Mat P = {{ Q15_ONE,0,0,  0,cp,-sp,  0,sp,cp }};
    Hx421Mat R = {{ cr,-sr,0,  sr,cr,0,  0,0,Q15_ONE }};
    return mat_mul(mat_mul(Y, P), R);
}
/* v' = M * v, v in Q16.16, M in Q15 */
static Hx421Vec mat_apply(Hx421Mat m, Hx421Vec v) {
    Hx421Vec r;
    r.x = (int32_t)(((int64_t)m.m[0]*v.x + (int64_t)m.m[1]*v.y + (int64_t)m.m[2]*v.z) >> 15);
    r.y = (int32_t)(((int64_t)m.m[3]*v.x + (int64_t)m.m[4]*v.y + (int64_t)m.m[5]*v.z) >> 15);
    r.z = (int32_t)(((int64_t)m.m[6]*v.x + (int64_t)m.m[7]*v.y + (int64_t)m.m[8]*v.z) >> 15);
    return r;
}
/* transpose == inverse for a rotation matrix; that is how the camera's
 * world->view transform is formed without a general matrix inverse. */
static Hx421Mat mat_transpose(Hx421Mat a) {
    Hx421Mat r;
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j) r.m[i*3+j] = a.m[j*3+i];
    return r;
}

static Hx421Vec vsub(Hx421Vec a, Hx421Vec b) { Hx421Vec r = { a.x-b.x, a.y-b.y, a.z-b.z }; return r; }

/* integer atan2 over the 1024-step turn, ~1 step accurate — enough to aim. */
static int32_t atan2_1024(int32_t y, int32_t x) {
    if (x == 0 && y == 0) return 0;
    int32_t ax = x < 0 ? -x : x, ay = y < 0 ? -y : y;
    int32_t a;
    if (ax >= ay) a = (int32_t)(((int64_t)ay * 128) / (ax ? ax : 1));
    else          a = 256 - (int32_t)(((int64_t)ax * 128) / (ay ? ay : 1));
    if (x < 0) a = 512 - a;
    if (y < 0) a = -a;
    return a & ANG_MASK;
}

/* ---- lifecycle ---------------------------------------------------------- */

void hx421_scene_init(Hx421Scene *s) {
    if (!s) return;
    for (unsigned i = 0; i < HX421_MAX_OBJ; ++i) { s->obj[i].active = 0; s->obj[i].rot = mat_identity(); }
    for (unsigned i = 0; i < HX421_MAX_CAM; ++i) { s->cam[i].active = 0; s->cam[i].rot = mat_identity(); }
    s->mesh_count = 0;
    s->active_cam = 0;
}
int hx421_mesh_register(Hx421Scene *s, const Hx421Mesh *m) {
    if (!s || !m || s->mesh_count >= HX421_MAX_MESH) return -1;
    s->mesh[s->mesh_count] = *m;
    return (int)s->mesh_count++;
}
int hx421_object_spawn(Hx421Scene *s, int mesh_id) {
    if (!s || mesh_id < 0 || mesh_id >= (int)s->mesh_count) return -1;
    for (unsigned i = 0; i < HX421_MAX_OBJ; ++i) {
        if (s->obj[i].active) continue;
        s->obj[i].active = 1; s->obj[i].visible = 1;
        s->obj[i].mesh = (uint16_t)mesh_id;
        s->obj[i].pos.x = s->obj[i].pos.y = s->obj[i].pos.z = 0;
        s->obj[i].rot = mat_identity();
        return (int)i;
    }
    return -1;
}
void hx421_object_despawn(Hx421Scene *s, int id) {
    if (s && id >= 0 && id < (int)HX421_MAX_OBJ) s->obj[id].active = 0;
}

static Hx421Object *obj_of(Hx421Scene *s, int id) {
    if (!s || id < 0 || id >= (int)HX421_MAX_OBJ || !s->obj[id].active) return 0;
    return &s->obj[id];
}
static Hx421Camera *cam_of(Hx421Scene *s, int id) {
    if (!s || id < 0 || id >= (int)HX421_MAX_CAM || !s->cam[id].active) return 0;
    return &s->cam[id];
}

/* ---- absolute ----------------------------------------------------------- */

void hx421_object_set_pos(Hx421Scene *s, int id, Hx421Vec p) {
    Hx421Object *o = obj_of(s, id); if (o) o->pos = p;
}
void hx421_object_set_rot(Hx421Scene *s, int id, int32_t yaw, int32_t pitch, int32_t roll) {
    Hx421Object *o = obj_of(s, id); if (o) o->rot = mat_euler(yaw, pitch, roll);
}

/* ---- relative / integral ------------------------------------------------ */

void hx421_object_translate(Hx421Scene *s, int id, Hx421Vec d) {
    Hx421Object *o = obj_of(s, id);
    if (o) { o->pos.x += d.x; o->pos.y += d.y; o->pos.z += d.z; }
}
void hx421_object_move_local(Hx421Scene *s, int id, Hx421Vec d) {
    Hx421Object *o = obj_of(s, id);
    if (!o) return;
    Hx421Vec w = mat_apply(o->rot, d);       /* d is in the object's own axes */
    o->pos.x += w.x; o->pos.y += w.y; o->pos.z += w.z;
}
void hx421_object_rotate(Hx421Scene *s, int id, int32_t dyaw, int32_t dpitch, int32_t droll) {
    Hx421Object *o = obj_of(s, id);
    /* Compose onto the existing matrix rather than tracking Euler angles: the
     * caller never has to accumulate, and there is no angle to wrap or drift. */
    if (o) o->rot = mat_mul(o->rot, mat_euler(dyaw, dpitch, droll));
}

/* ---- facing ------------------------------------------------------------- */

/* Yaw-then-pitch aim. Roll comes out zero either way: with only a target point
 * there is no reference to roll ABOUT, so roll_lock is accepted for API
 * symmetry and a free-roll variant would need an explicit up hint. Wiring one
 * in later is additive; faking it now would silently orient aircraft wrong. */
static Hx421Mat look_rot(Hx421Vec from, Hx421Vec to, int roll_lock) {
    (void)roll_lock;
    Hx421Vec d = vsub(to, from);
    const int32_t yaw = atan2_1024(d.x, d.z);
    /* horizontal run for the pitch leg; >>8 keeps the square inside 64 bits */
    const int64_t hx = d.x >> 8, hz = d.z >> 8;
    int64_t hsq = hx*hx + hz*hz, h = 0;
    while ((h+1)*(h+1) <= hsq) h++;                 /* integer sqrt, exact */
    const int32_t pitch = -atan2_1024((int32_t)(d.y >> 8), (int32_t)h);
    return mat_euler(yaw, pitch, 0);
}
void hx421_object_point_at(Hx421Scene *s, int id, Hx421Vec target, int roll_lock) {
    Hx421Object *o = obj_of(s, id);
    if (o) o->rot = look_rot(o->pos, target, roll_lock);
}

/* ---- cameras ------------------------------------------------------------ */

int hx421_camera_register(Hx421Scene *s, Hx421Vec pos, int32_t focal) {
    if (!s) return -1;
    for (unsigned i = 0; i < HX421_MAX_CAM; ++i) {
        if (s->cam[i].active) continue;
        s->cam[i].active = 1; s->cam[i].pos = pos;
        s->cam[i].rot = mat_identity();
        /* Default: 128 px, a 90-degree FOV on a 256-wide screen. In PIXELS. */
        s->cam[i].focal = focal ? focal : (128 << 16);
        return (int)i;
    }
    return -1;
}
void hx421_camera_set_active(Hx421Scene *s, int id) {
    if (s && id >= 0 && id < (int)HX421_MAX_CAM) s->active_cam = (uint8_t)id;
}
void hx421_camera_set_pos(Hx421Scene *s, int id, Hx421Vec p) {
    Hx421Camera *c = cam_of(s, id); if (c) c->pos = p;
}
void hx421_camera_translate(Hx421Scene *s, int id, Hx421Vec d) {
    Hx421Camera *c = cam_of(s, id);
    if (c) { c->pos.x += d.x; c->pos.y += d.y; c->pos.z += d.z; }
}
void hx421_camera_move_local(Hx421Scene *s, int id, Hx421Vec d) {
    Hx421Camera *c = cam_of(s, id);
    if (!c) return;
    Hx421Vec w = mat_apply(c->rot, d);
    c->pos.x += w.x; c->pos.y += w.y; c->pos.z += w.z;
}
void hx421_camera_rotate(Hx421Scene *s, int id, int32_t dy, int32_t dp, int32_t dr) {
    Hx421Camera *c = cam_of(s, id);
    if (c) c->rot = mat_mul(c->rot, mat_euler(dy, dp, dr));
}
void hx421_camera_point_at(Hx421Scene *s, int id, Hx421Vec target, int roll_lock) {
    Hx421Camera *c = cam_of(s, id);
    if (c) c->rot = look_rot(c->pos, target, roll_lock);
}

/* ---- the per-frame transform -------------------------------------------- */

int hx421_scene_render(const Hx421Scene *s, Hx421Tri *out, int max,
                       int screen_w, int screen_h) {
    if (!s || !out || max <= 0) return 0;
    const Hx421Camera *cam = &s->cam[s->active_cam];
    if (!cam->active) return 0;

    const Hx421Mat view = mat_transpose(cam->rot);   /* inverse of a rotation */
    /* Screen centre in the rasteriser's HX421_SUBPX fixed point, NOT Q16.16 —
     * projection results get shifted down into these units before adding. */
    const int32_t cx = (screen_w / 2) * HX421_ONE;
    const int32_t cy = (screen_h / 2) * HX421_ONE;
    const int32_t NEAR = 1 << 14;                    /* 0.25 world units      */
    int n = 0;

    for (unsigned i = 0; i < HX421_MAX_OBJ && n < max; ++i) {
        const Hx421Object *o = &s->obj[i];
        if (!o->active || !o->visible) continue;
        const Hx421Mesh *m = &s->mesh[o->mesh];
        if (!m->verts || !m->faces) continue;

        /* object centre in view space; cull behind the near plane by radius */
        Hx421Vec oc = mat_apply(view, vsub(o->pos, cam->pos));
        if (oc.z + m->radius < NEAR) continue;

        for (uint16_t f = 0; f < m->fcount && n < max; ++f) {
            Hx421Tri t;
            int behind = 0;
            for (int k = 0; k < 3; ++k) {
                Hx421Vec vm = m->verts[m->faces[f*3+k]];
                Hx421Vec vw = mat_apply(o->rot, vm);
                vw.x += o->pos.x; vw.y += o->pos.y; vw.z += o->pos.z;
                Hx421Vec vv = mat_apply(view, vsub(vw, cam->pos));
                if (vv.z < NEAR) { behind = 1; break; }

                /* Perspective divide. Q16.16 * Q16.16 / Q16.16 leaves pixels in
                 * Q16.16, so shift into HX421_SUBPX BEFORE adding the centre —
                 * the two are different fixed-point scales and mixing them puts
                 * the whole scene off by a factor of 256. */
                const int64_t sx = ((int64_t)vv.x * cam->focal) / vv.z;
                const int64_t sy = ((int64_t)vv.y * cam->focal) / vv.z;
                t.v[k].x = cx + (int32_t)(sx >> (16 - HX421_SUBPX));
                t.v[k].y = cy - (int32_t)(sy >> (16 - HX421_SUBPX));

                /* Depth: near maps small, so the rasteriser's plain less-than
                 * test means nearer wins. >>8 puts one unit at 1/256 of a world
                 * unit, so the 16-bit buffer covers 0..256 world units — past
                 * that everything saturates to ZFAR and stops z-sorting. Worth
                 * knowing before choosing a world scale. */
                int64_t z = vv.z >> 8;
                if (z < 0) z = 0;
                if (z > HX421_ZFAR) z = HX421_ZFAR;
                t.v[k].z = (uint16_t)z;
            }
            if (behind) continue;              /* whole-triangle near clip */
            t.color = m->colors ? m->colors[f] : 1;
            out[n++] = t;
        }
    }
    return n;
}
