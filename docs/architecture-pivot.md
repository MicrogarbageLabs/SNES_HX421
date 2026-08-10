# Architecture: STM32 runs game logic, FPGA is a fixed-function toolbox

Decided 2026-07-19, replacing the earlier "soft RISC-V in the FPGA runs game logic" plan.

## Why

BRAM was the binding constraint on every path. EP4CE15 has **56 M9K blocks (63 KB)**, and a soft
core wanted ~20 of them for IMEM/DMEM/I-cache before staging, tile buffers or rendering cores got
anything. Dropping the core returns **~20 blocks and ~3-5K LEs** to the pool that needs them.

Secondary wins:

- **The STM32F401 is likely the faster machine anyway** — M4F at 84 MHz against a VexRiscv at 40 MHz
  XIP-ing through a small cache off contended PSRAM. The soft core was always going to be
  memory-stalled; that was the honest weakness of the previous plan.
- **Real debugging** (SWD, breakpoints) and a toolchain already in hand, versus soft-core bring-up.
- **Matches HX-420**, where the MCU also runs game logic, so game code is portable across boards.
- **The PC seam is unchanged**, so `hx421.dll` + bsnes-plus remains the development path — our DLL
  already *is* the "MCU runs logic, emits staging" model.

## Split

| | owns |
|---|---|
| **STM32F401** | game logic; pre-mixes 2 SD PCM streams to 1; issues commands/queries over SPI |
| **FPGA** | audio mixer + drift correction + DAC; metatile rendering; PSRAM bulk transfer; DMA staging; SNES mailbox |
| **PSRAM** | maps (stored twice: row-major AND column-major), samples, primed heads, FMV |
| **SNES** | sees BRAM only — vectors, boot payload, staging, writable mailbox. **Never touches PSRAM.** |

Game code is **SD-loaded into SRAM**, not resident in flash: keeps firmware generic, keeps the game
a file rather than something baked into the cart, avoids flash write wear during development, and is
actually faster (F401 SRAM is single-cycle; even ART-accelerated flash is not).

## Budgets

### FPGA BRAM (56 blocks / 63 KB)

Revised after the baseline build measured the base at **1 block, not ~5** (see
`docs/hardware-budget.md`), and after the audio decisions below changed.

| region | blocks |
|---|---|
| vectors (permanent, hard-partitioned from payload) | 1 |
| boot payload (WRAM loader) | 2 |
| per-frame DMA staging, single-buffered (sized by the SNES's ~7.4 KB burst) | 12 |
| SNES↔FPGA mailbox + doorbell | 1 |
| mixer: 8 × PSRAM prefetch FIFOs @512 B | 4 |
| mixer: **2 × SPI stream input FIFOs @4 KB** (was 1 pre-mixed) | 8 |
| mixer: DAC output FIFO | 1 |
| metatile cache + expansion buffers | 5 |
| PSRAM read prefetch | 2 |
| sd2snes base (**measured**) | 1 |
| **used / spare** | **~37 / ~19** |

TBDR tile buffer later wants ~5 blocks, leaving ~14 spare.

**Worth reconsidering: ping-pong staging.** Single-buffering was chosen when BRAM looked scarce. It
costs 12 more blocks to double-buffer, which now fits. Single-buffer works because the SNES bursts
during vblank while the FPGA fills during active display — naturally time-separated — but there is a
race at the boundary that ping-pong removes outright. Decide when the staging path is written; the
blocks are available either way.

### STM32 SRAM (64 KB, one bank)

| region | size |
|---|---|
| game code + rodata (SD-loaded, overlay-swappable) | 16-24 KB |
| collision region window (64×64, 2 bits — **not** a resident whole-world map) | 1 KB |
| SD stream buffers (2 × 4 KB) | 8 KB |
| ~~pre-mix output buffer~~ — **dropped, see audio below** | — |
| actor + game state | 4-8 KB |
| FatFs sector buffer + FS state | ~2 KB |
| SPI DMA staging | ~2 KB |
| stack + heap | 4-8 KB |
| **used / spare** | **37-53 KB / 11-27 KB** |

A **region window** beats a resident collision map: 1 KB reloaded on scroll rather than 16 KB
resident, and it scales to any world size instead of growing quadratically.

## Key mechanisms

**Metatile rendering is the biggest rendering win.** The FPGA expands metatiles, so per-frame DMA is
only edge seams as the camera scrolls:

```
full tilemap             2048 B/frame
edge seam (row/column)     64-128 B/frame
3 layers                 384 B vs 6144 B      ~16x reduction
```

Combined with a fixed low-overhead staging area (no slot walking), this is what lets several layers,
sprites and FMV coexist inside the ~7.4 KB burst budget.

**SNES → cart mailbox.** The read-only bus was our choice, not a hardware limit — cart writes are how
save SRAM and every enhancement chip work. Use a **256 B writable BRAM mailbox** for payloads
(joypad, command blocks) and keep **address-strobes** for pure signals (`FRAME_DONE` already works
this way). Optionally a doorbell offset that triggers on write, so the FPGA needn't poll.

Costs on the PC side: a new ABI export `hx421_cart_write` plus forwarding in `hx421_chip.cpp`
(currently a no-op) — an ABI bump and a bsnes-plus rebuild.

**Audio: uniform 44.1 kHz, no pre-mixing.** Both earlier decisions here (22 kHz SFX, STM32 pre-mix)
were made against the *pre-pivot* SPI budget, when the STM32 read samples over the link. Once the
FPGA mixes from PSRAM directly, SFX never cross SPI and neither constraint exists.

- **All voices 44.1 kHz**, SFX mono. Costs 2.5% of PSRAM instead of 1.2% — irrelevant. The win is
  that with every voice at the output rate there is **no per-channel resampler at all**: the mixer is
  gain, sum, saturate. Drift correction then happens **once on the mixed output** rather than eight
  times. That is the difference between a mixer verifiable against the C reference in an afternoon
  and one you chase rounding differences in for a week. Also: 22 kHz would have put our SFX *below*
  the SPC700's own 32 kHz, which is the wrong side of the line for a coprocessor selling itself on
  audio.
- **The STM32 sends its two streams raw.** Pre-mixing spent 4 KB of the *scarce* resource (STM32
  SRAM, which also holds game code) to save ~4 blocks of the *plentiful* one (BRAM). SPI goes from
  ~12% to ~21% — fine. The STM32 then needs **no mixer code at all**: read SD, push to SPI, manage
  stream lifecycle. Per-stream gain/pan/ducking become FPGA register writes.
- The FPGA sees **one uniform voice abstraction** — 8 voices at 44.1, some fed from PSRAM (SFX), some
  from SPI FIFOs (streams), all handled identically. One RTL path, not two.

Two stream handles is right: enough for a seamless intro→loop transition with both open across the
boundary.

**Consequence for the C engine.** With mixing in fabric, the STM32 does not run `engine/`. Its role
becomes the **PC-side reference model and the source of truth for the RTL mixer** — golden output to
diff bit-exactly against in simulation. That is a better use for it than a second implementation.

**Metatile queries are pipelined, never synchronous.** Issue frame N's query list at the end of N's
logic; consume the response at the start of N+1. ~93 µs of work against 16.7 ms of availability
(~180x margin), and the CPU never waits.

Required discipline, or it silently breaks:

1. **SPI DMA, never poll** — polling reintroduces the stall.
2. **Double-buffer the response** — the DMA must not write the buffer the CPU is reading. Failure
   presents as intermittently wrong collision results and reads as a physics bug.
3. **Priority queue on SPI** — PCM has a deadline, queries do not. Queries fill the gaps.
4. **Sequence number in the response**, verified by the consumer. Catches all three above at the
   point of failure rather than at the point of symptom.

## SPI budget (18 MHz, ~1.8 MB/s sustained)

| traffic | rate |
|---|---|
| **2 raw PCM streams** to FPGA (was 1 pre-mixed) | 352 KB/s |
| metatile query req+resp (50 tiles, batched) | ~13 KB/s |
| commands, joypad mailbox, status | ~30 KB/s |
| **total** | **~395 KB/s (~21%)** |

FMV bulk still uses `fpga_sddma` (SD → PSRAM direct), and SFX are mixed in the FPGA from PSRAM, so
neither crosses the link. The pivot leaves the SPI link mostly idle.

## Decision audit after measurement (2026-07-19)

Several choices here were made against constraints that the baseline build and the pivot itself then
dissolved. Re-examined every one:

| decision | original driver | verdict |
|---|---|---|
| drop soft RISC-V for game logic | BRAM scarcity | **stands, reasoning corrected** — base is 1 block not ~5, so ~26 would have been free. Holds on M4F speed, SWD debugging, toolchain, HX-420 portability instead |
| 22 kHz SFX | pre-pivot SPI budget (SFX crossed the link) | **REVERSED → 44.1 kHz** — FPGA reads PSRAM directly; also removes all per-channel resamplers |
| STM32 pre-mixes 2 streams → 1 | SPI bandwidth, FPGA FIFO BRAM, resampler count | **REVERSED** — spent scarce SRAM to save plentiful BRAM; uniform 44.1 removed the resampler argument entirely |
| single-buffered staging | BRAM scarcity | **open** — ping-pong now affordable (12 blocks); decide when the staging path is written |
| collision = 1 KB region window | STM32 SRAM scarcity | **stands** — SRAM is still the tight resource, and it scales to any world size |
| game code in SRAM, not flash | firmware genericity, flash wear | **stands** — owner's call, unaffected by measurement |
| writable 256 B mailbox | read-only bus was our choice | **stands** |
| pipelined metatile queries | SPI round-trip latency | **stands, more margin** |
| metatile edge-seam rendering | DMA budget | **stands** — still the biggest rendering win |
| no microheads | PSRAM fetch ≪ deadline | **stands, more strongly** — the FPGA now reads PSRAM at ~70 ns directly rather than over SPI |
| primed heads ~200 ms in PSRAM | worst-case SD arbiter wait | **stands** — sized by SD, which did not change |

**Pattern worth noting:** three decisions in one session were invalidated not by being wrong when
made, but by the architecture moving underneath them. Any future "we chose X because Y is tight"
should be re-checked against measured numbers before it drives RTL — assumptions here have run 5x
off in both directions.

### Optional add-ons using the headroom (measured)
The RPG core leaves ~34% LE and most of the DSP/M9K free, so two optional coprocessor services fit
cheaply, both built + verified:

- **Actor→map metatile query** (`fpga/cores/actorq`) — bulk per-actor map lookups (collision, terrain,
  triggers) the SNES can't do (it never reads PSRAM). **433 LE, 1 M9K** (store shared with the actor
  engine → marginal cost tiny). §"Actor ↔ layer metatile queries".
- **DSP math coprocessor** (`fpga/cores/dsp`) — the DSP-1/CX4 pattern: the 65816 byte-writes a
  function + operands to the cart mailbox, NOPs a fixed latency, reads the result bytes by index.
  Functions: MUL (signed 16×16→32) / MAC+MACINIT (dot products) / DIV (iterative 32÷16) / **SIN, COS**
  (257-entry quarter-wave Q1.15 table + quadrant folding, angle 0..1023 = full circle; `sintab.vh`),
  plus a `ready` byte. **358 LE, 2 DSP, 1 M9K** (the sine ROM) — nearly free (2 of the ~88 spare
  multipliers), room for atan2/sqrt/vector transforms. Co-sim: 14 ops incl. edge cases (max-neg
  squared, ÷0, sin/cos at 0/45/90/180/270°) match a C golden.

## RPG re-scope (2026-08-06): SNES runs the game, FPGA accelerates, ARM minimal

A deliberate scope cut to ship an RPG. The premise above (STM32 runs game logic, FPGA is a toolbox)
is **reversed for the RPG**: the SNES 65816 runs the game in its 128 KB WRAM (plenty for an RPG), the
FPGA is a feature accelerator, and the ARM is nearly idle — its only jobs are loading the core and
running the audio stream arbiter. The ARM + streamer are **reserved for a future 3D title**, not the RPG.

### Feature-selectable core
The per-game core-load mechanism (now `FPGA_HX421`, carttype `$E4` → `/sd2snes/fpga_hx421.bi3`)
generalizes to **variants**: each is a different feature subset packed to fit the EP4CE15, selected at
**load time** by carttype (`$E4` = RPG core, a later `$E5` = 3D core, …). Load-time, not runtime —
Cyclone IV partial reconfig isn't worth it and a full reconfig blanks the fabric.

### RPG core feature set
- **Scene engine** — screen/layer composition.
- **Actor priority** — **sprite-vs-sprite** OAM depth-sort + flicker-rotation (NOT sprite-vs-BG:
  the sprite's OBJ priority bits stay game-controlled and pass through, enabling BG canopy / bridge
  multi-tier tricks — see docs/tilemap-accelerator.md). Offloads the 65816's
  per-frame sprite sort; spreads the 32-sprite/scanline dropout across frames instead of vanishing).
- **Auto map strip builder + metatile fetch** — scroll-triggered VRAM fill from the metatile map. The
  biggest CPU win for a scrolling RPG: the 65816 no longer rebuilds the wrapping edge column/row on
  every tile boundary. Hardware form of the sengine metatile engine + the `layer_goto` sliding-window work.
- **FMV prep engine (NMI DMA staging)** — reads decoded frames from PSRAM (streamed there by the
  arbiter) and lays them into the BRAM staging area in the exact chunked layout the SNES NMI handler
  DMAs to VRAM, per-sub-frame (the "fabric does fixed-function staging" of `docs/fmv-engine.md`). For
  RPG cutscenes. The BRAM it fills is already the per-frame staging budget (12 blocks) — the engine
  is the sequencer + offset/length math, ~no new M9K.
- **8-ch mixer** — the existing RTL, fed by the arbiter.

**Strip builder ⇄ FMV prep can share one staging core.** Both are PSRAM→BRAM staging engines feeding
NMI DMA, and they are **never active at the same time** (FMV during cutscenes, strip builder during
scrolling gameplay). If LE gets tight, build one PSRAM→BRAM staging core with two modes rather than
paying for both — a natural fallback that also shares the staging BRAM.

**No collider.** RPG collision is cheap on the 65816 (tile-grid lookups); pixel-perfect is
action-game overkill. Its M9K is reallocated to the metatile cache.

### Memory model: PSRAM bulk, BRAM staging
Everything bulk lives in **PSRAM** — the **metatile map is PSRAM-resident, not SD-streamed** — with
**BRAM (M9K) as the staging area for everything**. This is the fix for the timing conflict found when
the SNES and the sound engine both hit PSRAM directly: consumers touch BRAM (fast, contention-free)
and BRAM is refilled from PSRAM on a schedule. Strip builder: metatile PSRAM→BRAM→VRAM. Mixer: audio
PSRAM→BRAM→DAC.

### Stream arbiter (audio + FMV), core-gated
The ARM stream arbiter (built + host-tested) streams audio/FMV **files** SD→PSRAM rings via FatFs
(fragmentation-tolerant, seek-by-name) — chosen over a raw contiguous blob for flexibility and
because it feeds the high-quality mixer. Firmware coupling is small and additive:
- **Core-keyed switch:** `load_rom` sets an `audio_core_active` flag when the loaded core is an HX-421
  audio variant; the main loop runs the arbiter only when set. Stock cores / menu / ROM-load / save
  states are untouched.
- **Command mailbox:** the SNES writes play/seek/stop to an FPGA register; the ARM polls it and drives
  the arbiter. Same shape as the copro command channel.

### Simplified address decode + execute-from-WRAM
The SNES sees only a small **BRAM boot stub mirrored across all banks** — decode is just
**CARTSEL + /RD + A[15:0]**, ignoring the top 8 address bits and all bank/HiROM/LoROM/region logic.
That sheds a meaningful chunk of mapper LEs. It is the scheme already proven on the H745 kernel
(`snes-kernel.md`: 16-bit decode, mirror, run-from-WRAM) and the execute-from-WRAM model:
1. On HX-421 core load, the FPGA loads a selected SD file into BRAM — this **is** the ROM the SNES
   boots (the boot stub); no firmware ROM-load path needed.
2. The stub copies/streams the engine into WRAM and `jmp`s there; the engine runs from WRAM.
3. PSRAM is left to the FPGA (map, audio, FMV); a small register window carries SNES→FPGA commands.

Keep the BRAM stub **small** — total M9K is only 63 KB, shared with the metatile cache + audio
staging, so a full 64K mirror is out; mirror a small stub and stream the rest to WRAM.

**Net LE/M9K:** simplified decode frees LEs; dropping the collider frees M9K for the metatile cache;
a small boot stub keeps M9K for staging.

### LE/M9K tally (2026-08-06) — base MEASURED, RPG core fits
Full `h2_base` = **9,091 LE (59%)** but already bundles the mixer (`hx_mixer_dac` ~3,050) **and** stock
sd2snes features the RPG doesn't use — bsx 762, cheat 550, ctx 790, rtc+srtc 989, msu 148.

**Stripped base measured (2026-08-06), not estimated.** A resource probe replaced those six cores with
port-matched zero-output stubs (so Quartus drops them *and* their now-dead decode/mux glue in main.v)
and ran a full fit on the EP4CE15 (Quartus 25.1std). Result:

| component | LE | source |
|---|---|---|
| **stripped ROM-load base (+ mirror-64K decode) + 8-ch mixer** | **5,010 (33%)** | **MEASURED (stub-strip fit)** |
| scene engine (DMA-body / opcode composer) | **415 (3%), 3 M9K** | **MEASURED** (`fpga/cores/scene`) |
| actor priority (depth-sort + overload flicker) | **827 (5%), 3 M9K** | **MEASURED** (`fpga/cores/actor`) |
| map strip builder + metatile fetch (3-layer) | **2,060 (13%), 0 M9K** | **MEASURED** (`fpga/cores/strip`) |
| FMV prep engine (NMI DMA staging) | **624 (4%), 0 M9K** | **MEASURED** (`fpga/cores/fmv`) |
| **full RPG core (sum of ALL FIVE)** | **8,936 (58%)** | **all measured — fits** |

**ALL FIVE components measured (2026-08-06).** Every feature estimate retired via standalone EP4CE15
fits (Questa-clean RTL in `fpga/cores/{strip,actor,scene,fmv}` + the measured base/mixer).

**INTEGRATED BUILD + COMBINED FIT (2026-08-06).** `fpga/cores/rpg_top/hx_rpg_top.v` wires all five
engines together with a shared **PSRAM read arbiter** (mixer vs a strip/FMV-muxed renderer port —
they never run at once) and a **shared staging BRAM** with write arbitration among the four writers.
Combined fit on the EP4CE15:

| | integrated | note |
|---|---|---|
| **Logic elements** | **8,240 / 15,408 (53%)** | 5 engines + arbiter + staging + glue |
| M9K | 17 / 56 (30%) | staging BRAM + actor store + scene bank + mixer FIFOs |
| DSP (9-bit mult) | 24 / 112 (21%) | all the mixer's cubic interp |

**Integration overhead is negligible — ~78 LE (~1%)**, not the +15–25% budgeted. Per-instance
(comb. ALUTs): mixer 3,170, strip 1,586, actor 638, fmv 505, scene 299, **arbiter 35**. The
strip/FMV renderer-port sharing avoided a third arbiter port; the staging mux is trivial. This
integrated 8,240 LE does **not** include the SNES-bus/ROM-load decode from the base (~1,960 LE), so
the **full cart core ≈ 10,200 LE ≈ 66%**, with ~34% headroom. M9K 30% and DSP 21% both have ample
room. **The RPG core fits the EP4CE15 comfortably, on fully measured numbers.**

**Timing (2026-08-06, constrained at 96 MHz).** Combined-domain Fmax started at **64.9 MHz** — the
limiter was the strip engine computing `mty*map_w + mtx` combinationally straight into the PSRAM
address and through the arbiter (a shift+multiply+add+mux in one cycle). Pipelining that (register the
tile decode, then the multiply, then `rd_addr` — the mixer's "no compute crosses a cycle" discipline)
lifted it to **79.3 MHz** (+43 LE); the next limiter is the actor engine's histogram-BRAM→`oidx`
arithmetic, the same cheap fix if wanted. **But the per-frame engines do not need 96 MHz** — only the
mixer does (continuous audio). Strip/actor/scene/FMV do per-frame work (~12k cycles/frame total ≈ **2%
utilization even at 40 MHz**; a frame is ~1.6M cycles at 96 MHz), so the real design is **two clock
domains** — mixer at 96 MHz, the per-frame engines in a slower domain with CDC on the shared PSRAM
arbiter — and 79 MHz is already ample for them. Single-domain 96 MHz on the per-frame engines is an
artificial constraint; no further pipelining is required for correctness or throughput.

**Strip builder measured (2026-08-06):** `fpga/cores/strip/hx_strip.v` — the hardware form of
`runtime/hx421_metatile.c` + `hx_map_layer_goto`. The core fetch datapath (two-level metatile→def
fetch, 1-deep cache) alone was **721 LE**; the full **3-layer** engine — per-layer context
(map/defs pointers, camera, sliding window), camera-delta → direction seams, clamp (static
screens/HUD), and reseed — measures **2,060 LE (13%), 2 DSP, 0 M9K** on the EP4CE15, landing at the
midpoint of the earlier 1.5–2.5K estimate. The `mty*map_w` multiply is **shared across layers**
(time-multiplexed) so it stays 2 DSP; per-layer context is in registers (moving it to BRAM trades
~400 LE for 1 M9K). Compiles clean under Questa. Follow-ons (small): transposed-column path select,
DMA-descriptor emission, BG3 HUD dual-map tagging.

**Actor priority measured (2026-08-06):** `fpga/cores/actor/hx_actor.v` — world→screen transform via
the top-layer camera, an **O(N) Y-counting-sort** (histogram → prefix → scatter, not a comparison
network), OAM low-table emission in depth order, and per-band flicker rotation by frame phase.
Measured **1,176 LE (8%), 2 M9K, 0 DSP** (the 2 M9K is the 128×64b actor store). A cautionary
measurement: the first cut sorted by every scanline (256-entry histogram + occupancy arrays), which
synthesized to **6,389 LE** of muxes rather than BRAM — the fix was to depth-sort by **8-line Y-band**
(~30 bands; visually fine for sprite draw order), a 5.4× LE cut.

**Overload-flicker rewrite (2026-08-06):** the first cut's flicker was inadequate — it dropped the
same sort-tail every frame (no fairness) and half-duplicated the PPU (which already drops sprites past
32/line by OAM index order). Corrected model: the engine assigns **OAM indices** — a **priority-lock**
band puts must-show actors (player/boss) at the lowest indices (survive per-line dropout always), and
the **crowd is admitted through a per-frame rotating window** over the depth-sorted list, so both the
per-line PPU dropout and the >128-slot overflow rotate fairly and every crowd actor cycles through
visibility. The per-line occupancy array was removed (the PPU does per-line). Re-measured: **827 LE
(5%), 3 M9K** — *smaller* than the flawed 1,176-LE first cut, because dropping the redundant per-line
array outweighed the admission logic. Follow-ons: OAM high-table packing; on-hardware tuning of the
depth-vs-fairness rotation rate (heavy flicker slightly disturbs strict occlusion order — inherent to
the technique). Three of five core components (base, strip, actor) are now measured.

**Scene engine measured (2026-08-06):** `fpga/cores/scene/hx_scene.v` — composes the per-frame SNES
DMA body the 65816 executes at NMI (execute-from-window): a PPU register preamble (`lda #imm; sta
abs` per bank entry) then a VRAM DMA slot per descriptor (the strip/actor/FMV engines fill the
descriptor list). Hardware form of `hx421_runtime.c`'s `e_lda_sta*`/`e_dma_vram_slot` opcode emitters
— a byte-emitter FSM + register bank + descriptor list. **415 LE (3%), 3 M9K (bank+list in BRAM),
0 DSP** — the smallest core, under the estimate.

**FMV prep measured (2026-08-06):** `fpga/cores/fmv/hx_fmv.v` — reads a decoded frame's sub-frame
slice from PSRAM (CHR + tilemap) and stages it into the BRAM the SNES NMI DMAs, computing the
double-buffered VRAM destination (CHR-overlap buffer A/B by parity) and emitting descriptors to the
scene engine. A chunked PSRAM→BRAM bulk copy with sub-frame + double-buffer bookkeeping (shares the
copy shape with the strip engine, no metatile lookup). **624 LE (4%), 0 M9K (staging external), 0 DSP**
— under the 1–2K estimate. **All five core components are now measured.**

The measured base came in **~840 LE below the earlier subtraction estimate (5,850)** — the fitter also
removed dead glue the per-entity subtraction couldn't. Other measured facts from the probe: **all 24
DSP multipliers (21%) belong to the mixer** (cubic interp); base+mixer needs only **4 M9K** (feature
staging/cache/FIFO buffers add on top per the BRAM budget — projected ~37/56 with everything, not the
bottleneck); **pins stay the tight resource at 144/166 (87%)** but the features are internal logic and
add ~no I/O. If the top-end feature estimate gets close, the strip-builder ⇄ FMV-prep shared-staging-
core fallback (above) reclaims most of the FMV-prep line. Reproduce: stub the six cores + `quartus_fit`
(the probe was a scratch build, not committed — the real stripped base is a later functional RTL step).

### Saves: mailbox to an SD file, no cart RAM (decided 2026-08-06)
**No cart RAM at all** — not a PSRAM window, not BRAM. Working memory is the SNES's 128 KB WRAM,
full stop. This keeps the decode at just `CARTSEL + /RD + A[15:0]` (no `RAMSEL`, even less logic) and
sidesteps the fact that the FXPak has no separate cart-RAM chip (its memory is two bus-sharing PSRAM
chips, 2 × 64 Mbit; `hardware-budget.md`).

Saves are **serialized to / deserialized from an SD file over a byte mailbox** — the write-direction
twin of the existing joypad **read**-mailbox:
- **Save:** at a save point the game serializes its state and writes bytes sequentially to a
  **data-write mailbox**; the ARM drains it through FatFs into an SD file. Explicit serialize (not a
  raw WRAM dump) → a versionable save format.
- **Load:** reverse — the ARM reads the file, the SNES pulls bytes from a read-mailbox (same
  mechanism as joypad reads).

Notes: FatFs lives on the ARM, so the save mailbox adds one small ARM job (core-load + audio arbiter
+ save drain — still minimal). SD is muxed, so a save-write is just another consumer the stream
arbiter serializes; it happens at a save point (menu/pause), so it doesn't fight active music/FMV.
Throughput is a non-issue — an RPG save is a few KB, rare, and a save screen tolerates a few ms of
byte-mailboxing.

## Next steps

1. **PC first.** Split `hx421.dll` internally into "STM32 side" and "FPGA side" so the boundary that
   must eventually become RTL is explicit and testable against bsnes-plus. Prototype the metatile
   renderer there — biggest win, far easier to get right in C than in Verilog.
2. **Mailbox**: add `hx421_cart_write` + the bsnes-plus write path.
3. **Mixer in Verilog**, simulated against golden output from the existing C engine before it touches
   fabric. A bit-exact reference to diff against is a much better bring-up position than most.
4. **Baseline build** (Docker) still needed for the base's LE/M9K consumption — the one estimate
   everything above rests on.
