# Firmware (MCU / M3)

A fork of the sd2snes `config-mk3` firmware where the MCU is repurposed: instead of the flashcart
menu/manager, it becomes the **audio processor + stream arbiter**, and (optionally) the game-logic
host driving the portable VM guest.

## Modes

Coprocessor mode = our `firmware.im3` + our `fpga_mini.bi3`. It is a **whole-cart mode**, not just an
MCU jump — the FPGA must be running our bitstream (only it has the DAC/clock/mixer path). Cleanest
non-destructive dual-mode = "mode is which SD card" (stock vs. coprocessor). A single-card boot-time
picker is a later nicety.

## Responsibilities

- **Audio** (`audio/`): 8-channel cubic mixer clocked by the FPGA's master/487 tick; scene SFX palette
  with primed heads; block-based fragmentation-free sound RAM; stream arbiter with stream-priority
  refill. Drive PSRAM refills over SPI via **DMA** so they don't steal mixing cycles.
- **Stream arbiter**: FMV frames + music, prefetch-ahead; primed stream heads for immediate playback.
- **Game host** *(if the VM runs on the MCU)*: dynamic-load exec format (no reflash to load a game),
  bare-metal passthrough where available. Keep MCU-specific code thin — it varies by board rev.

## Do not assume the MCU part

Rev D changed the STM32. Read flash/SRAM from the mk3 `.map` and the chip marking; size the SFX
palette and game working set against the real number (see `docs/memory-budget.md`).

## Edit hygiene

Mark our changes to forked firmware with `/* FXCOPRO */` so upstream sd2snes updates merge cleanly.
Prefer adding modules over editing stock paths in place.
