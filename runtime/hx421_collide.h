/* ============================================================
 *  hx421_collide.h — broad-phase collision over the shared object registry
 *
 *  The SECOND consumer of Hx421Scene. The matrix that positions a mesh for
 *  drawing positions its bounds for collision, so there is one table and it
 *  cannot drift — see docs/collision.md.
 *
 *  This is the broad phase only: sphere vs sphere, all pairs. The mid phase
 *  (AABB/OBB) and the narrow phase (plane sets and deflection) come after, and
 *  deliberately after MEASURING what the pair count actually is — the estimate
 *  says a naive all-pairs sweep over 64 objects is ~0.5 ms, which would make a
 *  spatial hash premature. `tests` in the result exists to check that estimate
 *  against reality rather than trusting it.
 *
 *  Integer-only Q16.16, like the rest of the runtime, because this is the RTL
 *  spec as much as it is the reference.
 *
 *  Public domain (CC0). No warranty.
 * ============================================================ */

#ifndef HX421_COLLIDE_H
#define HX421_COLLIDE_H

#include <stdint.h>
#include "hx421_scene3d.h"

#define HX421_MAX_CONTACTS  64u

/* One overlapping pair. `normal` points from a toward b and is the direction
 * that separates them; `depth` is how far they interpenetrate.
 *
 * The narrow phase will return a SET of normals per pair rather than one, since
 * picking a single winning plane jitters or tunnels wherever two surfaces meet.
 * A sphere overlap genuinely has only one, so this carries one and the set
 * arrives with the plane phase instead of being faked here. */
typedef struct {
    uint16_t a, b;          /* object ids */
    uint8_t  kind;          /* Hx421BodyKind of the pair — selects which fields below are meaningful */

    /* MESH pairs: the separating direction and how deep they interpenetrate. */
    Hx421Vec normal;        /* unit vector a -> b, Q16.16 */
    int32_t  depth;         /* penetration along the normal, Q16.16 */

    /* MASK pairs: the first overlapping pixel, in world pixels. A per-pixel
     * sprite overlap has a contact POINT but no meaningful normal — faking one
     * from the AABB would put impact effects on the wrong edge of concave art,
     * so normal/depth stay zero and the point is the answer. */
    int16_t  px, py;
} Hx421Contact;

typedef struct {
    Hx421Contact c[HX421_MAX_CONTACTS];
    uint16_t     count;
    uint16_t     overflow;  /* pairs found past the cap — dropped, not silent */
    uint32_t     tests;     /* pair tests performed this sweep (the measurement) */
    /* Mesh-vs-mask pairs passed the layer filter but were not tested: a 3D
     * bounding sphere against a 2D screen-pixel mask has no defined meaning
     * without a projection, and inventing one would give confidently wrong
     * hits. Counted rather than skipped silently, so the gap is visible if a
     * game starts relying on it. */
    uint32_t     cross_skipped;
} Hx421ContactList;

/* Sweep every collidable pair. Returns the contact count (== out->count).
 *
 * ONE sweep over ONE table serves both kinds: mesh pairs take the sphere test,
 * mask pairs take AABB then per-pixel, and the narrow phase is chosen by what
 * the entry carries. That is the point of unifying them — a game with sprites
 * and mesh geometry does not maintain two worlds.
 *
 * Filtering is layer/mask based: a and b collide only if each one's `mask` has
 * the other's `layer` bit set, so "bullets hit scenery but not other bullets"
 * costs no special cases. An object with mask 0 collides with nothing, which is
 * how a purely visual object opts out without leaving the registry.
 *
 * `order` picks which contact pixel a mask pair reports; it is ignored by mesh
 * pairs. */
int hx421_broadphase(const Hx421Scene *s, Hx421ContactList *out,
                     Hx421ScanOrder order);

/* Sphere-vs-sphere for one pair, exposed so a caller can re-test a specific
 * pair without a full sweep (and so the test suite can check the maths
 * directly). Returns 1 on overlap and fills *normal / *depth. */
int hx421_sphere_test(Hx421Vec pa, int32_t ra, Hx421Vec pb, int32_t rb,
                      Hx421Vec *normal, int32_t *depth);

/* Integer square root of a Q16.16 value, returning Q16.16. Exposed because the
 * collision maths and gameplay should agree on it exactly. */
int32_t hx421_sqrt_q16(int64_t v_q16);

#endif /* HX421_COLLIDE_H */
