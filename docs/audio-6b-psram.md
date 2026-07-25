# H4 step 6b — the mixer reads samples from real PSRAM

H4a (audio seam) and H4b (real mixer → DAC, playing a baked sine ROM) are both
**hardware-proven**. The only thing between H4b and runtime audio is the sample
*source*: today it's a 128×16 ROM baked into the bitstream; it needs to be real
sample data in the shared PSRAM, streamed in at runtime. This is 6b.

## The real arbitration seam (corrects an earlier assumption)

`hx_psram_arb` (in `fpga/cores/mixer/`) was built + cosim-proven for a standalone
model: mixer vs. a generic "renderer", mixer-priority. **That is not the arbiter
the hardware actually uses.** On the sd2snes/FXPak the PSRAM *is* the ROM store
(`ROM_ADDR[22:0]`, `ROM_DATA[15:0]`, two 16-bit chips selected by `ROM_ADDR22`),
and access is arbitrated by the base core's own state machine in `main.v`:

- `free_slot = (SNES_cycle_end | free_strobe) & ~SD_DMA_TO_ROM` — the ROM bus is
  free this cycle (the SNES isn't mid-access).
- `STATE` (11-bit one-hot) sits in `ST_IDLE`; when `free_slot`, it serves the
  highest-priority pending requestor — CTX, then MCU rd/wr, then DMA rd/wr — each
  for `ROM_CYCLE_LEN = 7` cycles (`ST_*_RD_ADDR` counts down `ST_MEM_DELAYr`,
  captures `ROM_DATA` at the end, → `ST_*_RD_END` → `ST_IDLE`).
- `ROM_ADDR` is a priority mux: `SD_DMA_TO_ROM > CTX_HIT > DMA_HIT > MCU_HIT >
  MAPPED_SNES_ADDR`. The SNES is the *default* (lowest-priority) path, but it's
  never starved because non-SNES requestors only run inside a `free_slot`.

**So the mixer becomes a fifth requestor in exactly this pattern** — NOT a
separate arbiter. Its read port (`rd_req/rd_addr/rd_ack/rd_data`, already
latency-tolerant and proven) maps onto a `MIX_RD` request the state machine
serves in free slots, at lowest priority, so it can never delay a SNES ROM read.

### Concretely, the base-core changes (main.v)
1. Widen `STATE` (currently all 11 one-hot bits used) to add `ST_MIX_RD_ADDR` /
   `ST_MIX_RD_END`.
2. A request latch: on the mixer's `rd_req`, set `MIX_RD_PENDr`, latch
   `MIX_ROM_ADDRr <= {mixer sample addr}`.
3. `ST_IDLE`: after CTX/MCU/DMA, `else if (MIX_RD_PENDr)` → `ST_MIX_RD_ADDR`,
   `ST_MEM_DELAYr <= ROM_CYCLE_LEN`.
4. `ST_MIX_RD_ADDR`: countdown; at 0 capture `ROM_DATA` → `MIX_DINr`, →
   `ST_MIX_RD_END`; there assert `rd_ack`, clear `MIX_RD_PENDr`.
5. Add `MIX_HIT ? MIX_ROM_ADDRr : …` as the lowest-priority entry in the
   `ROM_ADDR` / `ROM_ADDR0` / `ROM_ADDR22` muxes.

### Address mapping
The mixer's `rd_addr` (24-bit sample position) maps to a PSRAM word address in a
region agreed with the loader. For the first increment the sample data lives in
the loaded ROM image (the cart ROM *is* PSRAM), so `MIX_ROM_ADDRr` is a fixed
base + `rd_addr`. `ROM_DATA[15:0]` is a 16-bit word = one sample (matches the
mixer's `rd_data[15:0]`), so no byte assembly.

## Bandwidth (already fits)
The mixer uses ~190 of 2177 cycles/frame at 44.1 kHz (measured, latency-tolerant
cosim). Each PSRAM read is `ROM_CYCLE_LEN = 7` cycles. `free_slot`s are abundant:
the SNES touches ROM at ~3.58 MHz against the 96 MHz bus, so the vast majority of
cycles are free. Worst case is a DMA-heavy game monopolizing free slots, but the
mixer's read latency tolerance (proven to 12+ cycles) absorbs bursts; if a frame
ever can't complete, `hx_audio_top.underrun` latches and the previous sample is
held (a click, not drift) — the same guarantee already in the design.

## Why this is not a one-shot to hardware
Every prior step degraded gracefully on error (silence). This one edits the ROM
state machine that serves **every** SNES access — a bug there corrupts ROM reads
and crashes any game, on a sealed cart with only the screen as a scope. So it
must be simulation-gated first. The needed harness (next build):
`tb_rom_mix.v` — a PSRAM model + a SNES-ROM-access pattern generator driving the
extracted arbiter + mixer requestor, asserting two properties:
1. **Safety:** the mixer never drives `ROM_ADDR` during a SNES access window
   (no SNES read ever sees mixer data).
2. **Liveness + correctness:** the mixer's reads return the addressed PSRAM words
   and the audio frames stay bit-exact to the baked-ROM reference.

## Increments
- **6b.1 — mixer reads a wavetable from PSRAM (ROM image).** Add the `MIX_RD`
  requestor; put the sine in the ROM image; mixer reads it from PSRAM instead of
  the baked BRAM. Hear the same sine, now PSRAM-sourced. Proves the read seam.
- **6b.2 — bulk sample load.** Reuse the FPGA SD→PSRAM DMA (`SD_DMA_TGT`) or MCU
  writes to place multi-channel sample banks in a PSRAM region; mixer plays
  several channels from it.
- **6b.3 — STM32 streaming.** The STM32 streams the FMV audio + 2× PCM (SD) into
  ring buffers in PSRAM and updates per-channel play pointers; the FPGA mixes
  those with the resident banks. Matches the MCU-arbitrates-streams / FPGA-mixes
  architecture ([[hx421-stm32-pivot]], [[hx421-fpga-mixer]]).

Related: [[hx421-fpga-mixer]], [[hx421-dma-budget-measured]], [[hx421-hardware-facts]].
