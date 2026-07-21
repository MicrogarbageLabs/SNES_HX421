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
mid     OBB overlap (SAT)     15 axes              confirm
narrow  plane set             dot products         deflection normal only
```

**Mid phase built** (`hx421_obb_test`, `hx421_midphase`). Boxes are **oriented, not axis-aligned**:
these objects rotate, and an AABB around a tumbling ship grows and shrinks with its heading, which
reads in play as collision getting mysteriously sloppy at 45 degrees. The discriminating test in the
suite is two unit cubes 3.39 apart on a diagonal with one yawed 45 degrees — the bounding spheres
say maybe, an AABB would say hit, the oriented boxes correctly say no.

All 15 axes decide the boolean; an edge-edge case separates boxes that every face axis calls
overlapping, so dropping the 9 cross products would give phantom hits. The returned normal is the
shallowest of the **6 face axes only**, which are unit vectors — cross-product axes are not, and
comparing their raw overlaps picks the wrong winner unless each is normalised, which is nine square
roots to shave a little off a push-out distance. Any axis with its overlap is a valid separation, so
this trades minimality for cost deliberately rather than by accident.

Bounds are **derived, never authored** — `hx421_mesh_fit_bounds` computes half-extents, centre and
radius from the vertex list, for the same reason sprite masks are derived: hand-set bounds that
disagree with the geometry give collisions slightly off the visible shape. The sphere is centred on
the box centre rather than the model origin, so an off-origin mesh does not get a needlessly fat
sphere that the mid phase then has to reject.

Cost is small at this scale: 64 objects pairwise is 2016 pairs. A spatial hash cuts it further but
is not needed until object counts are much higher — worth *not* building until measured.

**Built and measured** (`runtime/hx421_collide.c`, `tools/hx421_collide_test.c`): a full sweep over
64 objects performs exactly 2016 tests, confirming the pair count. The per-test cost estimate was
optimistic, though — the reject path is 3 shifts, 3 multiplies, 3 adds and a compare, plus the two
layer/mask checks, so **~20 cycles rather than ~10**, putting a full sweep nearer **1 ms at 40 MHz**
than 0.5 ms. Still affordable once per frame, and still not a reason to build the spatial hash yet,
but the real figure is worth carrying rather than the estimate.

The reject is deliberately square-root-free: `hx421_sqrt_q16` runs only on pairs that actually
overlap, which in a typical frame is a handful. Filtering is layer/mask based and **both sides must
accept the other** — a one-sided rule makes the relationship asymmetric, and "the bullet hits the
wall but the wall does not hit the bullet" is a miserable thing to debug.

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
the non-zero test is an OR tree, so a 32-wide row resolves in a cycle.

**Built and measured** (`runtime/hx421_mask.c`, `tools/hx421_mask_test.c`): 64 actors of 16x16
scattered over a 256x224 screen give 2016 AABB tests of which **27 survive (1.3%)** — so the per-
pixel work runs on ~1 pair in 75. Worst case that is 27 x 16 = **~432 row operations per frame**,
which confirms the estimate: beneath measurement, and the broad phase is doing exactly the culling
it exists for.

Rows are loaded into a single 64-bit accumulator with pixel 0 at bit 63, so aligning B into A's
column space is one shift in the same direction as screen space. Bits past `w` are masked off on
load — rows are byte-padded, and a mask staged from elsewhere with dirty padding would otherwise
produce phantom hits in a region no artwork explains.

Return the **first set bit's position** as well as the boolean: that gives a contact point for
spawning impact effects, which AABB collision cannot provide and which is most of why per-pixel
looks better. It is free — the row scan already found it — but only if the API carries it out;
retrofitting a position onto a boolean means re-running the test.

Scanning order decides which contact you get. Top-left-first is the natural fall-out and is fine
for impacts; if a projectile wants the contact nearest its travel direction, scan rows in the
direction of motion instead. Worth fixing the convention early, since it is invisible until
effects start spawning on the wrong side of a hit.

### Masks are DERIVED, never authored

Any non-zero (non-transparent) pixel becomes a 1. Hand-authored masks drift from the art the moment
a sprite is redrawn, and the resulting bug is "hits register slightly off the picture", which is
maddening to trace back to an asset. Deriving them makes drift impossible.

**`derive_mask` runs per ACTOR, at PSRAM stage time — not per sprite and not per frame.** An actor
is usually several OAM sprites (a 48x64 character might be six 16x32s), and testing each separately
would mean N tests and N results to reconcile. Instead, when the actor's CHR is staged into PSRAM,
walk its constituent sprites and composite them into ONE mask over the actor's bounding box, at
their OAM-relative offsets. One entry, one test, one answer.

```
load actor -> stage CHR to PSRAM -> derive_mask(actor) -> mask in PSRAM
                                                       -> pulled to BRAM on spawn
```

Because it happens at load, the cost is irrelevant: a full 64x64 actor is 4096 pixel tests once,
against nothing per frame.

### Variable mask sizes

SNES sprites come in size pairs (8x8/16x16/32x32/64x64, plus the 16x32 and 32x64 variants), and an
actor's bounding box is whatever its sprites span. So a mask carries its own dimensions rather than
assuming one size:

```
size     rows x bytes/row      mask
8x8       8 x 1                  8 B
16x16    16 x 2                 32 B
16x32    32 x 2                 64 B
32x32    32 x 4                128 B
32x64    64 x 4                256 B
64x64    64 x 8                512 B
```

Rows are padded to whole bytes and the width is stored, so an odd actor size (say 48 wide) costs
6 B/row rather than being rounded to 64. A **uniform 64-wide row** would simplify the shifter but
waste 8x on small sprites — 64 actors at 64x64 uniform is 32 KB, well past what BRAM should hold —
so natural width is the right trade, and the shifter handles 1/2/4/8-byte rows.

Widths up to 64 fit a single 64-bit row accumulator, which covers every SNES sprite size and most
actors. Wider actors need multi-word rows; worth deferring until something actually needs one.

### Shares the registry

A 2D actor is the same registry entry as a 3D object with a mask instead of a mesh: position,
flags, layer and mask fields are identical. One table, one broad phase, two narrow phases chosen by
what the entry carries — so a game with sprite actors and mesh geometry does not hold two worlds.

**Built.** `hx421_actor_spawn` claims an object slot with `kind = HX421_BODY_MASK`, and
`hx421_broadphase` dispatches on kind: mesh pairs take the sphere test, mask pairs take AABB then
shift-AND. Two decisions worth recording:

- **A mask body's `pos` is Q16.16 SCREEN PIXELS**, not world units — integer pixel is `pos.x >> 16`,
  and `z` is free for the game to use as a sort key. Keeping actors in fixed point rather than plain
  ints means sub-pixel motion accumulates properly and the same `translate`/`move_local` commands
  work on them.
- **Mesh-vs-mask pairs are skipped and COUNTED (`cross_skipped`), not tested.** A 3D bounding sphere
  against a 2D screen-pixel mask has no defined meaning without a projection, and inventing one
  would produce confidently wrong hits. If a game ever needs it, the projection is the design
  question to answer first — the counter makes the gap visible instead of letting it look supported.

Actors spawn with `visible = 0` and the renderer re-checks `kind`, because a mask body's `mesh`
index is meaningless and reading it would rasterise whatever mesh 0 happens to be.

## Build order

Sequenced after the TBDR base and audio validation.

1. **Object registry + transform** — shared with the renderer from the start, not retrofitted.
2. **Broad phase + collision list readback** — sphere only; measure the real pair count before
   deciding whether a spatial hash is warranted.
3. **2D actor masks** — derived in the asset pipeline, AABB then shift-AND. Cheapest piece with
   the most immediate gameplay payoff, and it exercises the registry before the 3D maths lands.
   DONE: `hx421_derive_mask` composites an actor's parts into one mask (honouring the SNES OBJ
   16-tile row stride, which is the detail that silently derives a wrong mask), and
   `hx421_mask_overlap` returns the contact point alongside the boolean. Not yet attached to a
   registry entry — a 2D actor should become the same entry as a 3D object with a mask instead of
   a mesh, which is step 6's layer/mask work.
4. **AABB/OBB mid phase.**
5. **Collision planes + deflection normals.**
6. **Passthrough flags, layers and masks.**

Steps 1-2 are the ones that prove the interface; everything after refines precision rather than
changing shape.
