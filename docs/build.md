# Build & deploy

## Toolchain (free, reproducible)

Use `alttpo/sd2snes-build-docker` — it installs **Intel Quartus Lite 20.1** + **ARM GCC** (no Xilinx
ISE) and builds the mk3 target end-to-end:

- `make mk3` → `fpga_base.bi3` **and** `fpga_mini.bi3` (Cyclone IV bitstreams, via Quartus)
- `make CONFIG=config-mk3` → `firmware.im3` (ARM GCC)

For iterative dev, the sd2snes build supports forcing a reflash every boot via `CONFIG_FWVER`
(`config-mk3`) — set it so the bootloader always takes your new `firmware.im3`.

> Baseline first: build the **unmodified** `sd2snes_mini` + `config-mk3` and confirm it runs before
> changing anything. That proves the toolchain and gives a known-good rollback point.

## Deploy (non-destructive, reversible)

Copy `firmware.im3` + the `.bi3` FPGA image(s) into the SD card's `/sd2snes/` folder; the cart
flashes/loads them at boot. No signing, no bootloader modification. **Restore stock** = drop the
official images back. Two SD cards (stock vs. coprocessor) = clean dual-mode with zero risk.

## Two numbers to capture from the first build

1. **Device** — the target line in `verilog/sd2snes_mini/main.qsf`. Confirm it's `EP4CE22F17C8N`
   (vs. the `sd2snes_base` / Cyclone-V image). This tells you which image is *your* board.
2. **MCU flash/SRAM** — from the mk3 firmware `.map`. This is the budget game logic + SFX palette
   share, and the free-flash headroom if you ever want an in-flash dual-mode section.

## Notes

- Cyclone IV E has **no partial reconfiguration** — full bitstream per game, loaded at game-load only
  (reconfig drops the SNES bus, so never mid-game).
- Keep the base (`sd2snes_mini`) as an upstream-tracking vendor/submodule so you can pull fixes; put
  our cores in a separate hierarchy that the base instantiates. See `fpga/base/VENDOR.md`.
