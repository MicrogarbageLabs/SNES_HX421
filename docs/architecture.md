# Architecture

## Hardware (FXPak Pro / SD2SNES Pro, mk3)

| Part | Detail |
|---|---|
| FPGA | Cyclone IV `EP4CE22F17C8N` — 22,320 LE, 66 M9K (594 Kbit ≈ 74 KB), 66 mult (9×9), 4 PLL, C8 (slow) grade |
| MCU ("M3") | STM32 Cortex-M (exact part **varies by board rev** — Rev D swapped it; read the marking / mk3 `.map`) |
| PSRAM | `MT45W8MW16BGX-701` — 16 MB, 70 ns random access, 104 MHz burst, page mode |
| MCU↔FPGA | SPI command bridge, ~21.5 MHz → ~2 MB/s effective (control path, **not** a data firehose) |
| SNES master clk | 21.477 MHz on cart pin 1 — wired to the **FPGA** (not the MCU) |
| Audio out | cart audio-in pins (analog), fed by a DAC on the FPGA side (MSU-1 path) |

The FPGA is constant across board revs; the MCU is not. => keep game/geometry logic in the portable
VM (rev-independent); keep MCU-specific code to a thin shim.

## Division of labor

```
SNES bus ── FPGA (fork of sd2snes_mini) ── SPI ── MCU (M3)
             │  address decode / ROM window                │  8-ch mixer (cubic)
             │  MCU command bridge                         │  stream arbiter (FMV/music)
             │  PLL: master/487 = 44101 Hz audio tick ─────┼─► feeds mixer sample clock
             │  audio FIFO + DAC (final stage)  ◄──────────┤  mixed output
             │  [per-game core: RISC-V / compositor]       │  game logic (or drives VM)
             └── PSRAM (ROM window, sound RAM, maps, FMV) ──┘
```

- **FPGA owns everything timing-critical & clock-locked:** SNES bus response, the master-derived
  audio clock, the DAC. This is why a weaker MCU is acceptable — the FPGA carries the hard parts.
- **MCU owns everything flexible:** mixing, arbitration, game logic. Steady, not fast.

## The ROM-window / staging model (same topology as SNES_HX_420)

A hardware ROM-window presenter (the FPGA base, served from PSRAM at full SNES speed — costs **zero
BRAM**) is fed by a shared region that game logic + any compositor write. The SNES DMAs newly-presented
frame data out of that window. Identical shape to the HX_420 design; only the substrate differs.

## Memory discipline (the thing that makes PSRAM tolerable)

PSRAM random access is 70 ns — fatal only for a *large* random working set. Keep it viable by:

- **Bulk = sequential.** Tile maps via strip fetch from **dual-encoded** maps (row-major for
  horizontal seams, column-major for vertical) → burst reads, not 70 ns random.
- **Random = small & capped.** A few actors × ~128 B/frame ≈ <1% of a 16.6 ms frame. Fine.

See `memory-budget.md`.

## What we fork vs. write

- **Fork (solved):** SNES bus interface, MCU/SPI bridge, PLL — `verilog/sd2snes_mini`. The genuinely
  hard, timing-critical part.
- **Write (ours):** audio final-stage core (`fpga/cores/mixer_out`), MCU audio processor
  (`firmware/audio`), optional soft RISC-V and 3D compositor cores.

## Open items to resolve at first build

- Confirm `sd2snes_mini`'s `main.qsf` device == `EP4CE22F17C8N` (vs. the `base`/Cyclone-V image).
- MCU flash/SRAM from the mk3 `.map` — sets the SFX-palette + game-logic-working-set ceiling.
- Base FPGA resource usage (fitter report) — confirm headroom for our cores (expected large: base is
  ~6 small files and coexists with SA-1/GSU today).
