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

## Next steps

1. **PC first.** Split `hx421.dll` internally into "STM32 side" and "FPGA side" so the boundary that
   must eventually become RTL is explicit and testable against bsnes-plus. Prototype the metatile
   renderer there — biggest win, far easier to get right in C than in Verilog.
2. **Mailbox**: add `hx421_cart_write` + the bsnes-plus write path.
3. **Mixer in Verilog**, simulated against golden output from the existing C engine before it touches
   fabric. A bit-exact reference to diff against is a much better bring-up position than most.
4. **Baseline build** (Docker) still needed for the base's LE/M9K consumption — the one estimate
   everything above rests on.
