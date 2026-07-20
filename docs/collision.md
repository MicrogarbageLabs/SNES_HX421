# 3D object registry and collision — design

Follows the TBDR base. Meshes live in PSRAM and are *registered* to objects, giving "3D sprites":
an object is a mesh reference plus a transform, so one mesh serves many instances. The coprocessor
owns transforms and collision so the STM32 spends its cycles on game logic rather than vector math.

## The registry is SHARED with the renderer

This is the part worth getting right first. The renderer already needs an object table with
transforms; collision needs the same table. **One registry, two consumers** — otherwise the ARM
maintains two copies and they drift.

```
Mesh (PSRAM, shared by all instances):
  vertices, triangles            -> renderer
  bounding sphere  (centre, r)   -> broad phase
  AABB             (min, max)    -> mid phase
  collision planes (N x normal,d)-> deflection, N small (6-12)

Object (registry, ~48 B each, resident):
  mesh_id
  position   x,y,z    Q16.16
  rotation   3x3 or quaternion
  scale
  velocity            (for response hints)
  flags               visible / collidable / solid-vs-passthrough
  layer + mask        who may collide with whom
```

Mesh reuse is then free: 200 rocks referencing one rock mesh cost 200 x 48 B of registry and one
copy of the geometry.

## Tiered collision — never per-polygon

```
broad   sphere vs sphere      3 mul + compare      cull almost everything
mid     AABB or OBB overlap   6 compares / SAT     confirm
narrow  plane set             dot products         deflection normal only
```

Cost is trivial at this scale: 64 objects pairwise is 2016 pairs, and a sphere test is ~10 cycles,
so a naive all-pairs broad phase is **~0.5 ms at 40 MHz**. A spatial hash cuts it further but is not
needed until object counts are much higher — worth *not* building until measured.

The FPGA's **56 idle 18x18 multipliers** are what makes this cheap: dot products and transforms map
straight onto them without consuming LEs.

## Deflection without per-polygon tests

Each mesh carries a small set of **collision planes** (normal + distance), authored or derived at
bake time — the large flat surfaces that matter for bouncing, not the full hull.

### Contacts are a SET, not a winner

Picking the single plane that most opposes the velocity is wrong wherever two surfaces meet: the
object reflects off one wall straight into the other, then off that one back into the first. The
symptom is **jitter in corners, or tunnelling through the seam** — and corners are exactly where
players drive into walls.

Resolve all simultaneous contacts together:

1. **Collect** every plane penetrated this frame (cap at 4; more than that is a wedge).
2. **Deduplicate** normals within ~15 degrees of each other. A curved surface approximated by
   several planes would otherwise apply nearly the same correction repeatedly and launch the object.
3. **Orthogonalise** what remains (Gram-Schmidt over 2-3 vectors).
4. **Reflect against all of them at once:**

```
v' = v - 2 * SUM_i (v . n_i) n_i
```

For walls meeting at a right angle — the common case — this is exact, and it is the same cost as
one reflection per contact: a dot product and a scaled subtract each.

Degenerate cases fall out naturally. **One contact** reduces to the plain `v - 2(v.n)n`.
**Three or more** after dedup means wedged: zero the velocity rather than resolving, since any
reflection just re-penetrates something.

Sliding instead of bouncing is the same machinery with the reflection coefficient dropped from 2 to
1 (`v' = v - (v.n)n` removes the into-surface component instead of reversing it), so a character
controller and a bouncing projectile share one path.

### Who resolves

The collision record returns **the normal set**, not just a resolved vector, so the game chooses
bounce, slide, or stop per object. The coprocessor can also return the precomputed `v'` for the
common case, saving the ARM the vector math — but the set is the source of truth, since response
policy is game logic rather than geometry.

**Crash-through** is a per-object flag rather than a separate path: `solid` reflects,
`passthrough` reports the hit and lets the object continue, which is what breakable scenery and
trigger volumes both need.

## Command interface (ARM -> coprocessor)

```
mesh_register(psram_addr, bounds)      -> mesh_id
object_spawn(mesh_id, flags)           -> object_id
object_set_transform(id, pos, rot)     (the hot path, one per moving object per frame)
object_set_velocity(id, v)
object_despawn(id)
collisions_read()                      -> list of {a, b, normals[<=4], count, depth}
```

Transforms are the only per-frame traffic: 48 B per moving object over SPI at ~380 KB/s current
utilisation leaves room for a few hundred. The collision list comes back the same way, and — like
the metatile queries — should be **pipelined a frame ahead** rather than waited on, so the ARM never
blocks on it.

## Why this belongs on the coprocessor

- It **shares the transform pipeline** with the renderer: the same matrix that positions a mesh for
  drawing positions its bounds for collision. Doing it on the ARM would duplicate that work.
- It is **embarrassingly parallel** and fixed-function — exactly what fabric is good at and what a
  general-purpose core is wasteful for.
- It removes the per-frame vector math from the STM32, which is running game logic out of 64 KB and
  has no cycles to spare on 2000 sphere tests.

## 2D actor collider — 1-bit sprite masks

Scheduled after the TBDR base and audio validation. The 3D path above handles meshes; most of what
a game actually collides is **sprites**, and for those a 1-bit-per-pixel mask gives true per-pixel
accuracy for less memory than the artwork itself.

### Why the masks are nearly free

```
mask, 1 bit/px          8x8    8 B     16x16   32 B     32x32  128 B     64x64  512 B
the sprite's own CHR    8x8   32 B     16x16  128 B     32x32  512 B
                                       -> the mask is 1/4 the size of the art
```

64 actors at 16x16 is **2 KB of masks** — resident in BRAM with room to spare, and no PSRAM fetch
at all during collision. That is the point: collision never touches the CHR, so it does not compete
with the renderer for PSRAM bandwidth. Masks for the full actor set live in PSRAM and are pulled
into BRAM when an actor spawns, not per frame.

### Test

Broad phase first, mask test only on the survivors:

```
1. AABB overlap                      ~10 cycles, culls almost everything
2. clip to the overlapping rect
3. per row: shift one mask by dx, AND with the other, test non-zero
```

A 16x16 pair is at most 16 shift-AND-test operations. In fabric a barrel shifter aligns the row and
the non-zero test is an OR tree, so a 32-wide row resolves in a cycle. With ~20 AABB overlaps in a
busy frame that is a few hundred operations — beneath measurement.

Return the **first set bit's position** as well as the boolean: that gives a contact point for
spawning impact effects, which AABB collision cannot provide and which is most of why per-pixel
looks better.

### Masks are DERIVED, never authored

Generate them in the asset pipeline from the sprite's own CHR — any non-zero (non-transparent)
pixel becomes a 1. Hand-authored masks drift from the art the moment a sprite is redrawn, and the
resulting bug is "hits register slightly off the picture", which is maddening to trace back to an
asset. Deriving them makes drift impossible.

### Shares the registry

A 2D actor is the same registry entry as a 3D object with a mask instead of a mesh: position,
flags, layer and mask fields are identical. One table, one broad phase, two narrow phases chosen by
what the entry carries — so a bullet can test against both sprite actors and mesh geometry without
the game holding two worlds.

## Build order

Sequenced after the TBDR base and audio validation.

1. **Object registry + transform** — shared with the renderer from the start, not retrofitted.
2. **Broad phase + collision list readback** — sphere only; measure the real pair count before
   deciding whether a spatial hash is warranted.
3. **2D actor masks** — derived in the asset pipeline, AABB then shift-AND. Cheapest piece with
   the most immediate gameplay payoff, and it exercises the registry before the 3D maths lands.
4. **AABB/OBB mid phase.**
5. **Collision planes + deflection normals.**
6. **Passthrough flags, layers and masks.**

Steps 1-2 are the ones that prove the interface; everything after refines precision rather than
changing shape.
