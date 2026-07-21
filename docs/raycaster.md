# Strip raycaster — design and VRAM budget

A second, separate render path from the [TBDR](tbdr.md). Where the polygon renderer generates CHR
every frame and pays for it in vblank bursts, the raycaster keeps its entire tileset **resident** and
rewrites only tilemaps. That is what buys a locked 60 fps.

Resolution is **240x208**, an 8/8 letterbox, matching the FMV path — the kernel already handles that
letterbox and it is proven on hardware.

```
                                              CHR         tilemap per frame
BG1  4bpp  wall strips, pre-scaled, RESIDENT  ~19-31 KB   1664 B  (rewritten)
BG2  4bpp  16 solid shade tiles, RESIDENT        512 B    1664 B  (rewritten)
BG3  2bpp  floor/ceiling, RESIDENT                64 B       0 B  (STATIC)
OBJ        enemies, pickups, weapon
           main = BG1 + BG3 + OBJ,  sub = BG2,  colour math subtracts
```

```
per-frame DMA   3328 B
blank budget    54 lines x 163 B = 8802 B     (measured, snes/dma_rate_test.s)
                -> 38% used, 5474 B spare for OAM
```

No CHR upload at all in the steady state, and enough headroom left that BG3 could be made dynamic
later (+1664 B, still only 57%) if a scrolling floor pattern ever turns out to be worth it.

**208 vs 200 is a small cost, not a saving.** A 32x32 tilemap occupies 2 KB whatever the visible
height, so the extra row costs no VRAM — only 64 more bytes of tilemap DMA per layer, 8 fewer blank
lines to do it in, and one more wall scale bucket (18.9 KB vs 17.0 KB of strips at four textures).
Against 224 it is a real saving on both counts; against 200 it buys 8 more pixels of view for about
2 KB of strips. With 38% of the DMA budget in use either way, take the view.

## Why the tileset can be resident

A raycaster column is a vertical slice of a wall texture, scaled by distance. Pre-bake every
(texture, scale bucket, slice) combination and the runtime never generates a tile — it only chooses
one. The obvious objection is that this explodes combinatorially, and the obvious count says it
does: 8 textures x 12 scales x 25 slices x 8 horizontal variants is ~5000 tiles, well past the 1904
that fit.

**Deduplication is what makes it work, and only a bake can measure it.** A wall at one distance and
the same wall one bucket nearer share most of their 8x8 slices; far buckets collapse almost
entirely. `tools/hx421_raybake.c` bakes the real set and counts distinct tiles.

## Measured (tools/hx421_raybake.c)

VRAM 64 KB, minus two tilemaps (4 KB) and the shade CHR (512 B), leaves **60.9 KB = 1904 tiles**.

```
config                                  generated  distinct  survive    CHR
8 textures, 2 h-variants, 16 px steps      2496      1011      40%     31.6 KB   <- comfortable
4 textures, 2 h-variants,  8 px steps      2400       913      38%     28.5 KB
4 textures, 4 h-variants                   2496       970      39%     30.3 KB
4 textures, 8 h-variants                   4992      1608      32%     50.2 KB
8 banded (horizontally uniform)            1248       189      15%      5.9 KB
3 pure-noise textures, 4 h-variants        1872      1800      96%     56.2 KB   <- the limit
4 pure-noise textures, 4 h-variants        2496      2400      96%     75.0 KB   TOO BIG
```

Three conclusions:

- **Horizontal detail is the multiplier, not texture count.** 1 -> 8 horizontal variants takes four
  textures from 9.6 KB to 50.2 KB; 1 -> 8 *textures* at two variants only reaches 31.6 KB. The
  design wants many textures with modest horizontal detail — which is what the Wolfenstein-era look
  wants anyway.
- **8-pixel height steps are affordable** (28.5 KB vs 17.0 KB for 16-pixel steps). 16-pixel
  quantisation keeps a wall symmetric about the horizon AND tile-aligned at both ends, so it was the
  expected choice; the measurement says the smoother option fits easily. Take it.
- **Only high-frequency noise breaks the budget**, at 96% survival — dedup cannot compress random
  data. Real wall art is far from noise, but a texture with per-pixel detail is the thing to avoid,
  and the bake reports survival so it is visible before it is a problem.

## Shading via colour math

The shade layer is 16 SOLID tiles — 512 B that never changes — with the tilemap choosing a shade per
8x8 cell. SNES colour math is add/subtract between main and sub screen (CGWSEL/CGADSUB), optionally
halved, so **subtract darkens**: distance fog and per-face lighting both fall out of picking a shade
index per cell.

Granularity is 8x8, not per-pixel. For distance shading in a raycaster that is the natural
granularity anyway, since a whole 8-pixel column is one ray bucket.

### One shade layer covers walls AND floor

BG2 sits on the sub screen, so it shades whatever the main screen puts under it — walls from BG1,
floor and ceiling from BG3 — without caring which. Filling its tilemap is then one rule per cell:

```
for each cell (col, row):
    if row is inside this column's wall span -> shade from the WALL's ray distance
    else                                     -> shade from the ROW  (floor/ceiling)
```

**Floor shading is a function of screen row alone.** For a flat floor at fixed eye height the
distance to the floor point on row y goes as 1/(y - horizon), so the floor's shade bands are
horizontal — and they do not change as the player moves. That is why extending the shade layer over
the floor is free: those cells were being written anyway, and their values are a static per-row
table the ray loop never has to compute.

### Why BG3 can be static

The same argument makes the floor layer itself static. Without per-pixel floor casting there is no
perspective floor texture to scroll, so BG3 carries a floor/ceiling colour split about the horizon —
a handful of solid 2bpp tiles, a tilemap uploaded once at level load, and zero per-frame DMA. All
the apparent depth comes from BG2's shade bands on top of it.

Layer order works out without a fight: in Mode 1, BG3 is the lowest priority unless the BGMODE
priority bit is set, so walls draw over the floor by default. BG2 never enters main-screen priority
at all, since it only exists on the sub screen.

Sprites can be included in colour math too (CGADSUB has an OBJ bit), so a distant enemy darkens with
the wall behind it rather than sitting on top at full brightness.

## Enemies: streamed, scaled sprites

SNES OBJ hardware cannot scale, so a billboarded enemy needs its CHR re-rendered whenever its
on-screen size changes. The coprocessor's 2D scaler does that from the source art in PSRAM into the
staging buffer, reusing the same path the TBDR uses for tile CHR.

The naive budget says this is tight: a 64x64 sprite is 2048 B against 4930 B per frame, so two.
**That is the wrong number.** An enemy only needs re-uploading when its scale bucket changes or its
animation frame advances — not every frame. Measured over a minute of simulated play
(`tools/hx421_spritestream.c`):

```
scenario                     mean    p95    p99   worst   frames over budget
8 enemies,  8 fps anim       355 B  2432   3264   3840    0 / 3600
12 enemies, 8 fps anim       510 B  2944   3712   4800    0 / 3600
8 enemies, 30 fps anim      1152 B  3296   3680   4352    0 / 3600
8 enemies, charging fast     492 B  2304   3744   5920   11 / 3600  (0.3%)
```

**The median is zero** — in most frames nothing crosses a bucket or advances a pose. Eight enemies
cost about 7% of the CHR budget on average.

- **Animation rate dominates, not movement.** 8 -> 30 fps animation triples the mean; slow -> fast
  movement adds only 30%. An enemy walking toward the player holds each scale bucket for many frames.
- **The resident pool is small**: 6.2 KB peak at 12 enemies, against a hard 16 KB cap — SNES OBJ
  addresses only two tables of 256 tiles, so that ceiling holds no matter how much VRAM is spare.
- **No double buffering needed.** Worst case fits inside one blank period, so an upload always
  completes before anything displays.

### Deferral, for the 0.3%

When everything charges at once the frame can want more than 4930 B. The response is to defer, not
to find more bandwidth: sort pending updates by on-screen size and drop the smallest past the
budget. A distant enemy showing its previous scale bucket for one frame is invisible; a near one
tearing is not. This is the same shape as the FMV band scheduling, and the deferred count should be
reported rather than silently dropped — a silent cap reads as "it always keeps up".

### The per-scanline limit is the real cap

Bandwidth is not what limits enemy count on screen — **the OBJ per-scanline limit is**. The SNES
renders at most 32 sprites and 34 8x8 slivers per scanline. A 64x64 sprite is 8 slivers wide on each
of its 64 lines, so **four large enemies abreast already reach the sliver limit** and the fifth drops
out. Mid-distance 32x32 enemies are 4 slivers, giving eight abreast.

This bites exactly when a raycaster is most dramatic — several enemies close and level with the
player — so encounter design should spread them in depth, and the engine should drop by distance
rather than letting the PPU drop by OAM index.

## What this does NOT need

- **No scrolling.** 30x25 tiles fits inside one 32x32 tilemap, and the whole map is rewritten each
  frame. The [tilemap accelerator](tilemap-accelerator.md)'s seam machinery and the 33-column
  sampling problem are both irrelevant here.
- **No TBDR.** This path bypasses the polygon renderer entirely, so the two can coexist: raycast
  walls at 60 fps, polygon objects on the same screen at whatever rate their complexity allows.

## Two budgets, not one

Worth stating plainly because trims get aimed at the wrong one:

- **SNES VRAM (64 KB, in the console)** — wall strips, shade tiles, tilemaps, OBJ CHR. Trimmed by
  reducing textures, horizontal variants or scale buckets.
- **FPGA BRAM and logic (on the cart)** — the mixer, the 2D sprite collider, staging buffers.
  Trimmed by dropping features like the sprite masks or mixer channels.

Dropping the 2D collider frees no VRAM. Dropping a wall texture frees no logic elements.

## Open questions

- Wall height quantisation makes walls "pop" between scale buckets as the player moves. 8-pixel steps
  soften it and fit (31.4 KB at four textures); whether that reads as acceptable is an eyeball
  question, not a measurement.
- BG3 gives 3 colours plus transparent. Whether floor and ceiling can share that convincingly, or
  whether the ceiling should just be the backdrop, is an art decision.
- Whether the strip bake belongs in the asset pipeline (baked to PSRAM at build time) or is generated
  on the cart at level load. PSRAM has room either way; build time is simpler.
- Doors and thin walls. A door at a different depth from its wall plane needs its own scale bucket
  set, which is cheap, but sliding doors need a horizontal offset the strip scheme does not have.
  Unresolved and worth settling before the bake format is fixed.
