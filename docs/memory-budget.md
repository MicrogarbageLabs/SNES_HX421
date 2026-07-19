# Memory budget & discipline

Three tiers, three owners. The 74 KB is only the FPGA's on-chip pool — it is **not** anyone's main RAM.

| Memory | Size | Owner | Role |
|---|---|---|---|
| MCU internal SRAM | rev-dependent (read `.map`) | STM32 | game logic working set, mixer state, SFX palette + lookahead rings |
| FPGA M9K BRAM | **74 KB total, shared** | fabric | RISC-V cache/TCM, compositor tile buffer, audio FIFOs, glue |
| PSRAM | 16 MB, 70 ns random | FPGA-arbitrated | ROM window, sound RAM, dual-encoded maps, FMV buffers |

## FPGA BRAM (74 KB) — per-game partition

The ROM window costs **0 BRAM** (served from PSRAM by the base, like stock sd2snes). Frame DMA staging
can also be PSRAM-served. So BRAM goes to: RISC-V I/D cache (~8–16 K, if used), audio FIFOs (~KBs),
compositor tile buffer (~10–20 K, 3D games only), glue/FIFOs.

Because cores load **per-game** (full bitstream at game-load; Cyclone IV has **no partial reconfig**),
each game's bitstream partitions the 74 KB for its own needs — an RPG bitstream gives BRAM to
map/game RAM + audio; a 3D bitstream gives it to the compositor. They never coexist, so each fits.
Note: no mid-game core swap (reconfig drops the bus) — bundle FMV into the RPG bitstream or do FMV
staging in software.

## PSRAM access rules (non-negotiable)

- **Bulk = sequential.** Strip-fetch tile maps from **dual-encoded** copies (row-major + column-major)
  so both seam directions read as bursts (~130–200 MB/s), never 70 ns random.
- **Random = small & capped.** Actor/off-screen metatile fetches ≤ ~128 B each, a handful/frame →
  <1% of frame. The 70 ns penalty is a rounding error when the budget is bounded.
- Hold this discipline and PSRAM is fine; break it (random-access a whole tilemap) and you fall off
  the cliff.

## If MCU SRAM turns out tight

Bulk-stream the active region (level/room) into MCU SRAM sequentially, random-access *from* SRAM.
Same "working set in fast RAM, stream the rest" pattern as the map LRU screens.
