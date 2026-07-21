# Strip raycaster — design and VRAM budget

A second, separate render path from the [TBDR](tbdr.md). Where the polygon renderer generates CHR
every frame and pays for it in vblank bursts, the raycaster keeps its entire tileset **resident** and
rewrites only tilemaps. That is what buys a locked 60 fps.

```
BG1  4bpp  wall strips, pre-scaled, RESIDENT       tilemap rewritten per frame (2 KB)
BG2  4bpp  16 solid shade tiles, RESIDENT (512 B)  tilemap rewritten per frame (2 KB)
           colour math blends them
OBJ        enemies, pickups, weapon
```

Per-frame cost is **4 KB of tilemap** against a measured ~6.2 KB vblank budget, leaving ~2 KB for
OAM. No CHR upload at all in the steady state.

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

- Floors and ceilings. Mode 1 gives BG3 as a 2bpp layer, which could carry a horizon gradient, but
  it competes for colour-math participation. Unresolved.
- Wall height quantisation makes walls "pop" between scale buckets as the player moves. 8-pixel steps
  soften it; whether that reads as acceptable is an eyeball question, not a measurement.
- Whether the strip bake belongs in the asset pipeline (baked to PSRAM at build time) or is generated
  on the cart at level load. PSRAM has room either way; build time is simpler.
