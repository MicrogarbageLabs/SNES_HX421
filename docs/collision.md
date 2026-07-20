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
bake time — the large flat surfaces that matter for bouncing, not the full hull. On a hit:

1. pick the plane whose normal most opposes the incoming velocity
2. return that normal with the collision record
3. the game reflects velocity about it: `v' = v - 2(v . n)n`

That gives believable bounces off walls, floors and ramps at a fraction of hull-test cost, and it
degrades gracefully — a mesh with one plane still behaves like a wall.

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
collisions_read()                      -> list of {a, b, normal, depth}
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

## Build order

1. **Object registry + transform** — shared with the renderer from the start, not retrofitted.
2. **Broad phase + collision list readback** — sphere only; measure the real pair count before
   deciding whether a spatial hash is warranted.
3. **AABB/OBB mid phase.**
4. **Collision planes + deflection normals.**
5. **Passthrough flags, layers and masks.**

Steps 1-2 are the ones that prove the interface; everything after refines precision rather than
changing shape.
