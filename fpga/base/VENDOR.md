# Vendoring the sd2snes_mini base

This directory holds the forked mk3 Cyclone IV base. It is intentionally empty in the scaffold — pull
the real source here as the first concrete step.

## Recommended: submodule (tracks upstream)

```sh
# from repo root
git submodule add https://github.com/mrehkopf/sd2snes.git third_party/sd2snes
# the base project lives at: third_party/sd2snes/verilog/sd2snes_mini
```

Then reference `third_party/sd2snes/verilog/sd2snes_mini` from the Quartus project, or copy just that
subtree here if you prefer a flat vendor. Submodule is preferred so upstream fixes pull cleanly.

## What's in sd2snes_mini (confirmed present, Cyclone IV / Quartus)

- Project: `main.qsf`, `sd2snes_mini.qpf`, `main.sdc`, `pll.qip`, `pll.ppf`
- Verilog: `main.v`, `address.v`, `mcu_cmd.v`, `spi.v`, `dcm.v`, `pll.v`
- (also carries Xilinx `.xise`/`.ucf` for the mk2 variant — ignore for our target)

## Do this first

1. Vendor the source (above).
2. Build the **unmodified** base + `config-mk3` firmware via `alttpo/sd2snes-build-docker`
   (`docs/build.md`). Prove the toolchain, get a known-good `fpga_mini.bi3` + `firmware.im3`.
3. Record the device from `main.qsf` (== `EP4CE22F17C8N`?) and the MCU flash/SRAM from the `.map`.
4. Only then start `cores/mixer_out` + `firmware/audio`.

## License note

`mrehkopf/sd2snes` is under its own license — keep it isolated under `third_party/`/here and honor its
terms for anything derived. Our original cores/firmware live outside the vendored tree.
