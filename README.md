# SNES_HX_421

The **HX-421** — a coprocessor cart built on the **FXPak Pro** (SD2SNES Pro, mk3 hardware), repurposing
it as a coprocessor for a homebrew SNES game. Sibling in concept to the `SNES_HX_420` board (same HX
coprocessor family, same PC-side drift-correction idea) but on an off-the-shelf FXPak instead of a
custom H745 board.

**Standalone repo — no dependency on microgarbage.** A few small sources may be copied in and adapted
as a starting point (tracked in `engine/PROVENANCE.md`); after that they are HX-421's own code. No
submodule, no shared library, no build/runtime coupling.

**Status:** scaffolding. Architecture is settled (see `docs/`); no HW code written yet.

**PC development is via `ares`** (more cycle-accurate than bsnes-plus — validates the timing-critical
release/NMI-DMA handshake), with the coprocessor logic as a stable-ABI DLL behind a **cartridge-seam
contract** implemented by both ares and the FPGA. See `docs/emulation-seam.md`.

## The one-paragraph idea

The FXPak Pro's Cyclone IV FPGA (`EP4CE22F17C8N`) keeps doing what it already does well — present a
ROM window to the SNES and bridge to the MCU — and we bolt on our own logic. The onboard MCU (the
"M3") becomes an **8-channel audio mixer** with SNES-master-locked, drift-free output. The FPGA owns
the master clock and the final DAC stage; the M3 owns mixing, the stream arbiter, and game logic. The
game itself is the same VM guest that runs on `microgarbage` and on the `SNES_HX_420` board, so it is
substrate-independent.

## Why this is buildable and safe

- The mk3 Cyclone IV base is **open**: fork `mrehkopf/sd2snes` → `verilog/sd2snes_mini` (see
  `fpga/base/VENDOR.md`).
- The toolchain is **free**: `alttpo/sd2snes-build-docker` builds `fpga_mini.bi3` + `firmware.im3`
  with Intel Quartus Lite 20.1 + ARM GCC (see `docs/build.md`).
- Deployment is **reversible**: standard SD-card `.im3`/`.bi3` drop, no signing, no bootloader touch.

## Layout

```
include/
  hx421.h        THE cartridge-seam contract (flat-C dual-target ABI; host ↔ runtime)
docs/            architecture, audio, memory budget, build, pc-build, emulation seam
engine/          portable core — mixer, sound RAM, arbiter, FFT, drift correction
                 (HX-421's own code; built into BOTH firmware and the ares DLL)
  service.{h,c}  runtime owner (hxa_* API): pool+mixer+arbiter+players+fft as one unit
  demo/player.c  end-to-end render demo → out.wav
runtime/
  hx421_runtime.c  implements include/hx421.h over hxa_* → hx421.dll (or M3 static)
tools/
  hx421_host_test.c  LoadLibrary harness proving the ABI (build: engine `make dll dlltest`)
fpga/
  base/          forked sd2snes_mini (bus decode + MCU/SPI bridge + PLL)  [vendor]
  cores/
    mixer_out/   flagship: master/487 clock + FIFO + DAC final stage      [skeleton]
    (riscv/, compositor/ — see fpga/README.md; add when needed)
firmware/        M3-specific: SD/FAT, USB, HAL, main — links engine/
  audio/         audio-processor design notes
host/            ares fork: coprocessor board + DLL shim (the PC seam)   [plan: host/README.md]
tools/build/     docker build + deploy notes
```

`sd2snes_mini` (the FPGA base) is upstream — a fork/vendor of `mrehkopf/sd2snes`, unrelated to
microgarbage. The **only** external code lineage is: the sd2snes base (its own license) and whatever
`engine/PROVENANCE.md` records.

## First moves (before writing code)

1. `git submodule`/vendor `sd2snes_mini`, build the **unmodified** baseline via the docker to prove
   the toolchain end-to-end (see `fpga/base/VENDOR.md`).
2. Read two numbers off that build: the **device line in `main.qsf`** (confirm it targets your
   `EP4CE22`) and the **MCU flash/SRAM from the mk3 `.map`** (the budget game logic + SFX palette
   share).
3. Only then start extending — flagship first (`fpga/cores/mixer_out`, `firmware/audio`).
