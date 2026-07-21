/* ============================================================
 *  hx421_scene3d.h — 3D object registry, cameras, transform pipeline
 *
 *  The registry the renderer and the collision system SHARE. An object is a
 *  mesh reference plus a transform, so one mesh serves many instances ("3D
 *  sprites"): 200 rocks cost 200 registry entries and one copy of the geometry.
 *
 *  The point of it living here rather than on the ARM is that the ARM then
 *  issues INTENT — move, rotate, face that point — instead of recomputing
 *  matrices and re-uploading vertices every frame. Both absolute and relative
 *  forms exist because both are natural to a game: absolute for placement and
 *  teleports, relative for per-frame motion where the caller should not have to
 *  track accumulated state.
 *
 *  Fixed point: positions are Q16.16 world units, rotation is a 3x3 matrix of
 *  Q15 fractions. No floating point anywhere — this is the RTL spec as much as
 *  the reference, like hx421_raster.c.
 *
 *  Public domain (CC0). No warranty.
 * ============================================================ */

#ifndef HX421_SCENE3D_H
#define HX421_SCENE3D_H

#include <stdint.h>
#include "hx421_raster.h"
#include "hx421_mask.h"

#define HX421_MAX_MESH     16u
#define HX421_MAX_MASK     32u
#define HX421_MAX_OBJ      64u
#define HX421_MAX_CAM       4u
#define HX421_MAX_TRIS    512u    /* per frame, after culling */

/* What an entry's geometry IS, which selects its narrow phase. A 2D actor is
 * the same registry entry as a 3D object with a mask instead of a mesh, so one
 * table and one broad phase serve both and a game never holds two worlds. */
typedef enum {
    HX421_BODY_MESH = 0,   /* 3D: bounding sphere -> mesh/plane narrow phase */
    HX421_BODY_MASK        /* 2D actor: AABB -> per-pixel mask narrow phase  */
} Hx421BodyKind;

typedef struct { int32_t x, y, z; } Hx421Vec;      /* Q16.16 world units */

/* Row-major 3x3, Q15. Identity is 0x8000 on the diagonal. */
typedef struct { int32_t m[9]; } Hx421Mat;

/* A mesh's geometry. Vertices are model-space Q16.16; each face is three
 * vertex indices plus a palette index. Faces are flat-shaded — the renderer is
 * untextured, so colour is per face, not per vertex. */
/* A collision plane in MODEL space: points with dot(n, p) > d are outside.
 * A mesh carries only the few large flat surfaces that matter for bouncing, not
 * its full hull — the narrow phase is a handful of dot products, never a
 * per-polygon sweep. */
typedef struct {
    Hx421Vec n;      /* unit normal, Q16.16, pointing OUT of the solid */
    int32_t  d;      /* plane offset along n, Q16.16                   */
} Hx421Plane;

#define HX421_MAX_PLANES   12u   /* per mesh; more is a hull, not a plane set */

typedef struct {
    const Hx421Vec *verts;
    uint16_t        vcount;
    const uint16_t *faces;      /* 3 indices per face */
    const uint8_t  *colors;     /* 1 per face, palette index 1..15 */
    uint16_t        fcount;
    /* Bounds. Prefer hx421_mesh_fit_bounds() over setting these by hand: bounds
     * that disagree with the geometry give collisions slightly off the visible
     * shape, which is the same drift problem authored sprite masks have. */
    int32_t         radius;     /* bounding sphere, Q16.16 — broad phase + cull */
    Hx421Vec        half;       /* OBB half-extents about `centre`, Q16.16      */
    Hx421Vec        centre;     /* box centre in model space (verts may be off-origin) */
    /* Narrow-phase plane set. Storage belongs to the caller (see
     * hx421_mesh_fit_planes), so the mesh holds no geometry it did not already
     * hold. pcount == 0 means the narrow phase falls back to the box normal. */
    const Hx421Plane *planes;
    uint8_t           pcount;
} Hx421Mesh;

/* Derive radius, half-extents and centre from the vertex list. Call after
 * filling verts/vcount and before registering. The sphere is centred on the box
 * centre rather than the model origin, so an off-origin mesh does not get a
 * needlessly fat bounding sphere. */
void hx421_mesh_fit_bounds(Hx421Mesh *m);

/* One registry entry, 3D object or 2D actor.
 *
 * `pos` means different things per kind, and this is the one place the two
 * worlds actually touch:
 *   MESH — world units, Q16.16, all three axes.
 *   MASK — SCREEN PIXELS in Q16.16 (integer pixel = pos.x >> 16) at the mask's
 *          top-left corner. z is unused by collision and free for the game to
 *          use as a sort key.
 * Keeping actors in Q16.16 pixels rather than plain ints means sub-pixel motion
 * accumulates properly and the same translate/move_local commands work on them. */
typedef struct {
    uint8_t    kind;        /* Hx421BodyKind                                  */
    uint16_t   mesh;        /* MESH: index into scene.mesh                    */
    uint16_t   mask_id;     /* MASK: index into scene.mask                    */
    uint8_t    active;
    uint8_t    visible;     /* drawn by the renderer                          */
    uint8_t    collidable;  /* considered by the collision sweep              */
    uint8_t    layer;       /* which layer THIS object belongs to (0..7)      */
    uint8_t    mask;        /* bitmask of layers this object collides WITH    */
    /* Passthrough reports the hit and lets the object keep going — breakable
     * scenery and trigger volumes both need it, and making it a FLAG rather
     * than a separate path means they share one sweep with everything else. */
    uint8_t    passthrough;
    Hx421Vec   pos;
    Hx421Vec   vel;         /* world units per tick, Q16.16 — response input  */
    Hx421Mat   rot;
} Hx421Object;

typedef struct {
    uint8_t    active;
    Hx421Vec   pos;
    Hx421Mat   rot;
    /* Focal length in PIXELS, Q16.16 — not world units. A point one world unit
     * off-axis at one unit of depth projects `focal` pixels from the centre, so
     * focal_x == half the screen width is a 90-degree horizontal field of view.
     * Seeding it with a world-scale number (5 rather than 128) shrinks the whole
     * scene to a few pixels, which reads as "nothing is being drawn".
     *
     * SEPARATE PER AXIS because SNES pixels are not square: a 256x224 frame on a
     * 4:3 display gives an 8:7 pixel aspect, so pixels are ~1.167x wider than
     * tall. One focal for both axes renders every cube 1.167x too wide — a
     * world-space cube arrives on screen as a brick. focal_y carries the
     * correction (see hx421_camera_set_par). */
    int32_t    focal_x, focal_y;
} Hx421Camera;

typedef struct {
    Hx421Mesh   mesh[HX421_MAX_MESH];
    Hx421Mask   mask[HX421_MAX_MASK];
    Hx421Object obj[HX421_MAX_OBJ];
    Hx421Camera cam[HX421_MAX_CAM];
    uint8_t     mesh_count;
    uint8_t     mask_count;
    uint8_t     active_cam;
} Hx421Scene;

/* ---- lifecycle ---- */
void     hx421_scene_init(Hx421Scene *s);
int      hx421_mesh_register(Hx421Scene *s, const Hx421Mesh *m);   /* -> mesh id, <0 fail */
int      hx421_object_spawn(Hx421Scene *s, int mesh_id);           /* -> object id, <0 fail */
void     hx421_object_despawn(Hx421Scene *s, int id);

/* 2D actors share the object table, the layer/mask filter and the sweep. The
 * mask descriptor is COPIED; its bits are not, so they stay wherever they were
 * staged (PSRAM for the full set, BRAM for the spawned ones) and the registry
 * holds no artwork. */
int      hx421_mask_register(Hx421Scene *s, const Hx421Mask *m);   /* -> mask id, <0 fail   */
int      hx421_actor_spawn(Hx421Scene *s, int mask_id);            /* -> object id, <0 fail */

/* ---- ABSOLUTE: set state outright. Placement, teleports, authored keyframes. */
void hx421_object_set_pos(Hx421Scene *s, int id, Hx421Vec p);
void hx421_object_set_rot(Hx421Scene *s, int id, int32_t yaw, int32_t pitch, int32_t roll);
void hx421_object_set_vel(Hx421Scene *s, int id, Hx421Vec v);

/* ---- RELATIVE (integral): accumulate onto current state. Per-frame motion,
 * where making the caller track accumulated position or angle is exactly the
 * bookkeeping this registry exists to remove. */
void hx421_object_translate(Hx421Scene *s, int id, Hx421Vec d);     /* world axes  */
void hx421_object_move_local(Hx421Scene *s, int id, Hx421Vec d);    /* object axes */
void hx421_object_rotate(Hx421Scene *s, int id, int32_t dyaw, int32_t dpitch, int32_t droll);

/* ---- FACING: orient toward a world point, no angles required of the caller.
 * `roll_lock` keeps the object's up axis world-up, which is what almost
 * everything except aircraft wants. */
void hx421_object_point_at(Hx421Scene *s, int id, Hx421Vec target, int roll_lock);

/* ---- field of view --------------------------------------------------------
 *
 * Pixel aspect ratio: physical pixel width / height, Q16.16. The SNES value is
 * the default on every camera, so correct geometry is what you get without
 * asking; square-pixel output (a PC preview window) needs HX421_PAR_SQUARE. */
#define HX421_PAR_SNES    76800    /* 8:7 — 256x224 on a 4:3 display */
#define HX421_PAR_SQUARE  65536    /* 1:1 */

/* Named fields of view, because "what focal length looks right" is not a
 * question a game should have to answer. Horizontal, since that is the axis a
 * player judges speed by. */
typedef enum {
    HX421_FOV_NARROW = 0,   /* ~40 deg — long lens; least edge distortion   */
    HX421_FOV_NORMAL,       /* ~60 deg — the safe default for most scenes   */
    HX421_FOV_WIDE,         /* ~75 deg — fast movement, strong perspective  */
    HX421_FOV_VERY_WIDE     /* ~90 deg — dramatic, noticeably stretched     */
} Hx421FovPreset;

/* Set the horizontal FOV; focal lengths and the aspect correction follow.
 * `hfov` is in the same 1024-step turn used everywhere else (1024 = full turn,
 * so 60 degrees = 171). `screen_w` is the render width in pixels. */
void hx421_camera_set_fov(Hx421Scene *s, int id, int32_t hfov, int screen_w);
void hx421_camera_set_fov_preset(Hx421Scene *s, int id, Hx421FovPreset p, int screen_w);

/* Change the pixel aspect ratio, recomputing focal_y from focal_x. Setting this
 * does NOT change the horizontal field of view — only how tall things look. */
void hx421_camera_set_par(Hx421Scene *s, int id, int32_t par);

/* ---- cameras: same vocabulary, several registered, one active ---- */
/* `focal_x` is in PIXELS (0 for a sensible default). focal_y is derived from
 * the SNES pixel aspect; call hx421_camera_set_fov for the friendlier route. */
int  hx421_camera_register(Hx421Scene *s, Hx421Vec pos, int32_t focal_x);  /* -> cam id */
void hx421_camera_set_active(Hx421Scene *s, int id);
void hx421_camera_set_pos(Hx421Scene *s, int id, Hx421Vec p);
void hx421_camera_translate(Hx421Scene *s, int id, Hx421Vec d);
void hx421_camera_move_local(Hx421Scene *s, int id, Hx421Vec d);
void hx421_camera_rotate(Hx421Scene *s, int id, int32_t dyaw, int32_t dpitch, int32_t droll);
void hx421_camera_point_at(Hx421Scene *s, int id, Hx421Vec target, int roll_lock);

/* ---- the per-frame job ----
 * Transform every visible object through the active camera, project, cull, and
 * emit screen-space triangles for the rasteriser. Returns the count written.
 * Angles are 0..1023 (a 1024-step turn), which keeps the sin table small and
 * the wrap a mask rather than a modulo. */
int hx421_scene_render(const Hx421Scene *s, Hx421Tri *out, int max,
                       int screen_w, int screen_h);

/* Q15 sine/cosine over the 1024-step turn. Exposed because gameplay wants the
 * same angle convention the renderer uses. */
int32_t hx421_sin_q15(int32_t a1024);
int32_t hx421_cos_q15(int32_t a1024);

#endif /* HX421_SCENE3D_H */
