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
4. **One-channel datapath** — `hx_chan.v`. **DONE**, bit-exact (2026-07-24). q32.32 phase
   accumulator + sliding tap window feeding the proven kernels, resample / non-loop path (mono).
   Co-simulated against the SEQUENCE from the real `mixer_render` (one mono channel at unity gain, so
   its output is the raw interpolated stream) across 5 resample ratios x cubic/linear, 2000 frames,
   0 mismatches. Model: the resample path is a sliding window over linearly-consumed source; the
   phase carry says how many samples to slide (clamped to tap count, exactly as the C — including the
   >4x-downsample source skip). Priming is its own cycle (no output, no phase change) so the sequence
   still matches the C priming inside its first produce. Loop mode and stereo (a second data lane
   sharing this control) are the remaining extensions.

Everything through step 4 runs under `sim/run-cosim.ps1` (Icarus): four combinational primitives
(~200k vectors) plus the stateful channel (2000 frames), every one bit-exact against the shipping C.
5. **8-channel time-multiplexed engine** — `hx_mixer.v`. **DONE**, bit-exact (2026-07-24). One
   datapath iterates all channels per output frame (the win from dropping the RISC-V core — a single
   cubic/lerp/scale/finalize instance serves every voice), per-channel state in arrays. Composes the
   proven pieces: resample -> volume -> pan (both with the C's Q15_ONE unity bypass) -> accumulate ->
   finalize. Co-simulated against a full `mixer_render` scene — cubic + linear, up/down-sample,
   per-channel volume, pan L/R, a muted channel, an inactive one — 150 stereo frames, 0 mismatches.
   Mono-source simplification: one tap window per channel (tap_l == tap_r for mono), L/R split at the
   pan multiply. Sequencer: PRIME all active channels, then per render frame PRODUCE each (1 cycle)
   and FINAL. At 96 MHz that is ~10 cycles/frame against a 22.7 us sample period — the headroom the
   bandwidth analysis predicted.
6. **PSRAM sample fetch + the DAC seam** — the first part that needs hardware; everything above is
   proven in simulation first.

Steps 1-5 are pure RTL-vs-C and all pass. The entire mixer arithmetic, resampling control, and
multi-channel accumulate are bit-exact to the shipping C, built and trusted before any integration.
Remaining before hardware: loop mode (wrapping source read), and the fast 1:1 path (`source_rate ==
output` skips the interpolator — a pop, not a resample). Then step 6: the source read becomes a real
PSRAM fetch (with the arbitration priority — mixer's hard deadline over renderer bursts), and the
finalized stream drives `dac.v`.
