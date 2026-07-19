# FPGA

## What we keep vs. add

- **`base/`** — a vendored fork of `mrehkopf/sd2snes` → `verilog/sd2snes_mini` (the mk3 Cyclone IV
  project). This is the SNES bus interface + MCU/SPI bridge + PLL. **The hard, timing-critical part —
  do not rewrite; fork and extend.** See `base/VENDOR.md`.
- **`cores/mixer_out/`** — our audio final stage: master/487 clock, sample FIFO from the MCU, DAC
  drive. The flagship. Skeleton present.
- **`cores/riscv/`** *(add when needed)* — soft RISC-V (picorv32-class) *if* game logic runs in-fabric
  rather than on the MCU. Needs a BRAM I/D cache (uncached PSRAM fetch = ~10–15 MHz effective). Likely
  unnecessary if the MCU hosts the VM; decide per the MCU SRAM budget.
- **`cores/compositor/`** *(add when needed, 3D games only)* — pure output compositor: blend 4bpp/2bpp
  layers, pack bitplanes, hand result to the ROM-window staging. **Never** gate it onto the cart bus
  directly, and it hosts no memory/logic (that reintroduces the PSRAM penalty). Measure whether
  software compositing fits the frame first — it may make this core unnecessary.

## Instantiation shape

The base (`sd2snes_mini/main.v`) is the top. Our cores hang off it:

- `mixer_out` taps the base PLL (master-derived clock) and the MCU/SPI path (sample stream), drives
  the audio DAC pins.
- A per-game core (riscv/compositor) reads/writes the shared PSRAM region the base already arbitrates.

Keep edits to base files minimal and marked (`// FXCOPRO:`) so upstream `sd2snes_mini` updates merge
cleanly. Prefer instantiating our cores from a thin wrapper over editing base internals.

## Resource expectation

Base is ~6 small Verilog files (`main`, `address`, `mcu_cmd`, `spi`, `dcm`, `pll`) and coexists with
SA-1/GSU-sized cores today → expect the bulk of 22,320 LE / 74 KB BRAM / 66 mult free for our cores.
Confirm with the fitter report on the baseline build.
