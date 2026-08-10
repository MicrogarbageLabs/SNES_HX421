# Tilemap accelerator — design and SNES constraints

Working in C against bsnes-plus (`HX421_MAP=1` / `-Map`), and the specification for the
FPGA block. Reference implementation: `runtime/hx421_metatile.{h,c}` (portable, integer-only)
plus the staging path in `runtime/hx421_runtime.c`.

## The primitive

```c
hx_map_layer_goto(layer, x, y)     /* the caller moves a layer.            */
                                   /* the engine derives the tile strips.  */
```

Nothing outside this names a column or a row. That is the whole point: the SPI-level command
from the STM32 is "move layer N by dx,dy" (or to x,y) and everything else — which strips are
stale, where they land in VRAM, whether a reseed is cheaper — is the accelerator's business.

## Why it is worth building

A scrolling tilemap costs a full 2 KB push per layer per frame if done naively. Expanding
metatiles and pushing only what entered the view costs a column and/or a row:

```
full tilemap, per layer          2048 B
one column seam                    64 B
one row seam (spans two screens)  128 B
2 layers, measured in the demo    ~288 B/frame   vs 8192 B    ~28x
```

That reduction is what lets several layers, sprites and FMV share one vblank burst.

## Model: a sliding window

Each layer's tilemap holds a window of world tiles:

```
columns [win_l, win_l+63]     rows [win_t, win_t+31]
invariant: tilemap column (T & 63) holds world column T
           tilemap row    (Ty & 31) holds world row    Ty
```

The window is kept **centred on the camera** so travel in either direction has lookahead.
Moving slides the window; whatever entered is expanded and pushed. Consequences:

- **Multi-tile deltas work.** Move 4 tiles in a frame and 4 columns are emitted. Code that
  assumes one strip per frame silently leaves gaps as soon as the camera moves quickly.
- **Large moves reseed.** Past `MAP_MAX_COLS`/`MAP_MAX_ROWS` the strips cost more than a full
  refill, so it reseeds — which also covers teleports, camera cuts and scene changes with no
  special case.
- **Both directions are real paths.** The demo's camera uses triangle waves with different
  periods per axis so it reverses; otherwise only right/down would ever execute and left/up
  would be untested code that looks correct.

## SNES constraints these cost real time to learn

**A 32x32 tilemap cannot scroll smoothly.** The viewport is 256 px = 32 tiles, but at any
scroll offset that is not a multiple of 8 a partial tile shows at *both* edges — **33 distinct
columns**. In a 32-wide map, column `(T+32) & 31` IS column `T & 31`, so the column entering at
the right and the one still visible at the left are the same tilemap slot: writing the new one
corrupts the old one on screen. Symptom is a jumpy left edge. **Any layer that scrolls
horizontally needs a 64-wide tilemap.** Vertically 32 rows is fine (224 px = 28 tiles + 1).

**A tilemap column is one DMA, not 32 writes.** Entries in a 32x32 screen are 32 words apart,
so `VMAIN` step-32 (`0x81`) walks straight down a column. That addressing mode exists for
exactly this.

**A row spans both screens** of a 64-wide map, so it is two DMAs — which is why rows cost 128 B
and columns 64 B.

**BGnVOFS must be -1**, not 0: the PPU shows BG line `VOFS+1` on the first visible scanline.
See [[snes-bg-vofs-off-by-one]] — this cost the project a hidden top pixel row for months.

**Screen layout by size** (`Hx421TilemapGeom` / `tm_word()`), each screen 0x400 words, ordered
left-to-right then top-to-bottom:

```
32x32 -> 1 screen    64x32 -> 2 side by side
32x64 -> 2 stacked   64x64 -> 4 in a 2x2 block
```

Getting the screen index wrong is **silent** — the DMA lands in a valid but wrong slot, and
tiles misplace only at particular scroll offsets. Hence one helper, not inline arithmetic.

## Parallax and multiple layers

Each layer carries its own map, geometry, scroll registers and parallax ratio. Cameras derive
from an **unbounded master** before wrapping at the map size; dividing an already-wrapped
camera makes slower layers jump at the *master's* rollover instead of their own.

Transparency: colour index 0 is transparent, so a front layer needs tiles that are entirely
index 0 where the layer behind should show. The demo reserves **tile 0 as fully transparent**
and gives the front layer metatiles built from it.

**Four layers is possible** — Mode 0 gives 4 BGs at 2bpp (CGRAM groups 0-31, 32-63, 64-95,
96-127). Budget at 4 layers scrolling diagonally:

```
per layer  column 64 + row 128     =  192 B
4 layers                           =  768 B/frame   (~12% of a ~6.2 KB vblank)
VRAM: 4 tilemaps @64x32 16 KB + CHR 2bpp x4 16 KB  =  32 KB of 64 KB
```

DMA is not the constraint; **VRAM is**. Note Mode 1 (BG1/BG2 4bpp + BG3 2bpp) often looks
better than four 2bpp layers — 15 colours on the layers carrying artwork usually beats a fourth
3-colour parallax band. The accelerator does not care which mode is chosen.

## RPG target: Mode 1, three independent-camera layers

The RPG core runs the engine as a **3-layer Mode 1 driver**, one context per BG:

- **BG1 + BG2 (4bpp)** share the same **1024-tile CHR set** (`MAP_BG12NBA = 0x22`, both CHR base
  0x2000), but each has its **own tilemap pointer and its own camera** — so the two 4bpp planes
  scroll independently (parallax, or one static while the other moves) off one tile set.
- **BG3 (2bpp)** has its own tiles and camera. Usual job: ambience / water-transparency behind or
  in front of the action. Second job: a **HUD** — the SNES uses an **H-IRQ mid-frame `BG3SC` swap**
  to point BG3 at a static HUD tilemap for the status region and back to the scrolling map for the
  rest of the frame. So BG3 needs **two tilemap targets** (scrolling ambience + static HUD); the
  engine scrolls the first and leaves the second alone (a clamped/static layer, below).

**Per-layer camera → auto strips, or clamp.** Each layer's camera is set independently; the engine
derives the strips from the *delta* (the `hx_map_layer_goto` model: `dl/dt` vs the window, seams in
the direction of travel, reseed past `MAP_MAX_COLS/ROWS`). A layer set to **clamp** holds its camera
at a limit and emits nothing — the mode for static screens (a fixed room, a menu backdrop, the HUD
sub-map). No special-case code: clamp is just `cam` pinned so `dl = dt = 0`.

**Memory.** Three layers, each with a row-major *and* transposed column-major expanded map in PSRAM,
plus their def sets — a lot of PSRAM, but trivial against ~16 MB (`map_rows`/`map_cols` are two base
addresses per layer, §PSRAM layout). Only VRAM is tight (Mode 1: 2×4bpp + 1×2bpp CHR + 3 tilemaps),
per §Parallax.

## Actor ↔ layer metatile queries (the map's only read path for the SNES)

The same fetch datapath that builds strips is a point lookup — "what tile/metatile is at world
(x,y) on layer L" is exactly the two-level metatile→def fetch, just addressed by an actor position
instead of a window walk. So the strip engine doubles as a **metatile query service**, and there is a
hard reason to expose it: **the SNES never reads PSRAM** (core architecture) and the metatile map
lives in PSRAM, so a query is the *only* way SNES game logic can learn what is under an actor. Any
map-aware actor behaviour — tile collision, terrain type (water/ledge/wall), triggers, "am I standing
on X" — goes through it.

- An actor is assigned **one or more layers**; the engine answers the metatile/tile it intersects on
  each (foreground collision on BG1, water on BG3, …). Point or small-rect footprint.
- **Coordinate model: actors live in ONE world frame — the top (reference) layer's camera.** An actor
  carries a single `(world_x, world_y)`, not one per layer. From it everything derives:
  sprite/OAM screen position = `world − top_cam` (what the actor-priority path already uses); a
  **top-layer** query is direct (`top_cam` cancels → lookup at `world`); a query on a **parallax**
  layer L maps through the camera difference → lookup at `world + (L_cam − top_cam)`. The engine
  already holds every layer's camera in context, so the parallax mapping is one adder on the query
  address — the FPGA does it, the actor never tracks per-layer positions.
- Reuses the measured fetch core (721 LE datapath); the added cost is a query request port + a result
  mailbox, not a second engine. The strip walk and actor queries **share the one datapath + PSRAM
  port**, so they arbitrate: strip building is the per-frame bulk load, queries interleave or run in a
  separate phase.
- This is **tile/metatile-granularity map query, NOT the dropped pixel-perfect mask collider** — it
  accelerates the cheap tile-grid lookup that would otherwise sit on the 65816, and it exists because
  the 65816 *cannot* do the PSRAM lookup itself.

Status: designed, not yet in RTL. It is a request-port + result-buffer extension of
`fpga/cores/strip/hx_strip.v`; open semantics to pin before building — what it returns (raw metatile
index / full tilemap entry / a derived terrain attribute), point vs rect, and queries/frame budget.

## CHR is RESIDENT, not streamed

The accelerator moves **tilemap entries only**. Tile graphics live in VRAM for the duration of
an area and are replaced in bulk on an area transition — during a fade or forced blank, where
throughput does not matter.

Per-tile CHR streaming was considered and rejected. It would need a VRAM slot allocator,
reference counting per tile, and tilemap entries rewritten whenever a tile relocated — a lot of
machinery to solve a problem metatiles already avoid, since heavy tile reuse means a modest set
covers a large map:

```
512 tiles 4bpp   16 KB      2 tilemaps @64x32    8 KB
                            sprites, spare      ~8 KB
                            ------------------------
                            ~32 KB of 64 KB VRAM
```

If an area ever needs more tiles than fit, the answer is to split it into sub-areas with their
own tile sets — a load-time decision, not a per-frame one. PSRAM holds every area's set, so a
swap is a bulk transfer, not a stream.

## Torus vs bounded maps

`Hx421MapLayer.wrap` selects: set, coordinates wrap modulo the map and a camera may advance
without bound; clear, out-of-range yields `oob_entry` (point it at a black tile). For a wrap to
be seamless on screen the map's tile dimensions must be a multiple of the tilemap wrap (64
wide, 32 high) so world tile T and T+map_tiles land in the same slot.

A camera wrapped at `(map size - viewport)` instead of the full map size teleports backwards by
most of the map in one frame, leaving the screen stale for ~96 frames while the seams repair it
one column at a time. That was a real bug; symptom was a glitch roughly every 13 seconds.

## PSRAM layout on hardware

Each layer's map is stored **twice — row-major and column-major**. A strided column fetch costs
~70 ns per row (a 32-tile column ~3 us of pure latency); the transposed copy makes it a
sequential burst. `map_rows` and `map_cols` become two PSRAM base addresses per layer, and the
strip fetch picks whichever gives sequential access. Doubling storage is free against 16 MB.

`hx421_metatile_test.c` pins that both paths produce **byte-identical** output, including under
wrap — a transposition bug would be data-dependent and would surface as occasional wrong tiles
at a scroll seam, months later.

## What becomes RTL

`hx421_metatile.c` is deliberately dependency-free and integer-only, with `mt_side` restricted
to powers of two so the tile-to-metatile decomposition is a shift and a mask. It is both the
reference implementation and the spec: the fabric block can be diffed against it in simulation
the same way the mixer will be.

**Status.** The fetch/expand datapath exists as `fpga/cores/strip/hx_strip.v` — the hardware form of
`mt_lookup` (two-level metatile→def fetch, 1-deep metatile cache, req/ack PSRAM read port, strip
write port), measured at **721 LE / 2 DSP / 0 M9K** on the EP4CE15. Still to add for the RPG target:
**per-layer context (×3)** time-multiplexed over that one datapath (mixer load-context style),
**camera-delta → direction seams + clamp** (`hx_map_layer_goto`), reseed, the transposed-column path,
and per-layer tilemap-dest tagging (incl. BG3's static HUD sub-map). Those are control/state around
the measured datapath; the standalone-synth harness (`cores/strip/synth`) measures each increment.
