# FPGA audio mixer — plan and co-simulation

## Architecture (corrected 2026-07-24)

The mix moved **into the FPGA**. Earlier notes (`firmware/audio/mixer.md`) put the 8-channel mix on
the STM32 with the FPGA only clocking samples out; that predates dropping the RISC-V soft core.
Reclaiming that fabric — and freeing STM32 memory and cycles — let the mix become hardware.

```
STM32                                   FPGA
-----                                   ----
arbitrates STREAMED sources:            8-channel MIX from PSRAM-resident samples:
 - FMV audio channel                     - per-voice native rate, cubic interpolation
 - 2 PCM sources from SD                 - per-voice volume + pan
 SD -> DMA -> hands blocks to FPGA       - blends in the STM32's streamed sources
                                         - accumulate wide, headroom shift, saturate
                                         - feeds the existing I2S DAC (dac.v)
```

The DAC output path already exists and is proven: `dac.v` is the sd2snes MSU-1 serializer — I2S with
a CIC upsampling filter, playing 44.1 kHz off the SNES master clock. The mixer feeds it; it is not
rebuilt.

## The C reference is the golden model

`engine/audio/audio_mixer.c` is the full software mixer (control plane + datapath). The FPGA
implements its **datapath**; create/destroy/write/sync stay on the STM32 side. The RTL is built to
match the C **bit-for-bit**, which keeps the C a valid test oracle: any divergence is a caught bug,
not a mystery that only shows up as a faint artifact on hardware.

### Co-simulation

`fpga/cores/mixer/sim/` runs the same inputs through the C and the RTL and diffs:

1. `gen_cubic_vectors.c` embeds the golden function *verbatim* and emits `{ inputs : expected }`.
2. `tb_cubic.v` replays the vectors through the DUT and flags any mismatch.
3. `run-cosim.ps1` drives it with **Icarus Verilog** (license-free; the Questa FSE bundled with
   Quartus Lite needs a separately-provisioned Starter license, and this combinational co-sim runs
   identically on iverilog).

## Bit-exactness includes overflow — on purpose

The cubic's `a * t3` is `int32 x int32 -> int32` in the C. With full-scale alternating taps `a`
reaches ~2^18 and `t3` ~2^15, so the product is ~2^33 and **wraps in the C's int32**. The RTL
reproduces the wrap deliberately (32-bit signed intermediates, truncate each product to 32 bits
before an arithmetic `>>>`) rather than "fixing" it with wider math. Matching the C exactly is what
makes the C an oracle; the input range over which the cubic is trustworthy is a separate question,
characterised rather than papered over. Real audio taps do not alternate full-scale, so the wrap is
latent — but it is now a *known* latent, in both C and RTL, not a surprise waiting in one of them.

## Build order (each step co-simulated before the next)

1. **Cubic interpolator** — `hx_cubic.v`. **DONE**, bit-exact (2026-07-24). The most arithmetic-heavy
   stage, done first because it is the most likely to diverge.
2. **Linear interpolator** — `hx_lerp.v` vs `interp_linear_q15`. **DONE**, bit-exact.
3. **Volume/pan multiply + finalize** — `hx_scale.v` vs `q15_sat_mul` (volume and pan-gain
   application), `hx_finalize.v` vs `finalize_output` (headroom shift, q15 saturate, output-format
   shift/offset/clamp — tested generally over 16s/12u/8u/8s formats). **DONE**, bit-exact. The pan
   GAINS themselves (`compute_pan_gains`: `Q15_ONE - |pan|`, note `Q15_ONE == 0x7FFF`) are per-channel
   control logic, not per-sample, so they live on the STM32 side or a trivial combinational block.
4. **One-channel datapath** — phase accumulator (q32.32 step), tap-window fetch, the three produce
   paths (fast / resample / loop). Co-sim against `produce_channel_sample` + one-channel render.

All four primitives run under `sim/run-cosim.ps1` (Icarus), ~200k vectors total, every one
bit-exact. The remaining steps compose these; the arithmetic they rest on is now proven.
5. **8-channel + accumulate** — the full render against `mixer_render` with a multi-channel scene.
6. **PSRAM sample fetch + the DAC seam** — the first part that needs hardware; everything above is
   proven in simulation first.

Only step 6 needs the board. Steps 1-5 are pure RTL-vs-C, which is why the mixer can be built and
trusted long before it is integrated.
