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

Steps 1-5 are pure RTL-vs-C and all pass. The entire mixer — arithmetic, resampling control,
multi-channel accumulate, AND every source path — is bit-exact to the shipping C, built and trusted
before any integration. Two paths that turned out NOT to need new RTL or were folded in:

- **Fast 1:1 path is FREE.** A channel at `source_rate == output` produces `step == 2^32`, which the
  resample path advances exactly one sample per frame at `frac == 0` — and both interpolators return
  the exact source sample there. So it is bit-identical to the C's fast-path pop with no separate
  logic. Confirmed by a 1:1 channel in the scene matching. One fewer path to build and verify on
  silicon.
- **Loop mode is the same sliding window** with the source read wrapped mod `loop_len` and no
  underrun. The next-load index tracks identically to non-loop; only the address wraps (one
  compare-subtract, valid for `loop_len >= 4`). Verified with three looping channels wrapping small
  buffers several times inside one render. `hx_mixer` exposes four per-tap read addresses so the
  module owns the wrap.

### Step 6a — latency-tolerant read path (DONE in sim, 2026-07-24)

`hx_mixer_seq.v`: the synthesizable evolution. The combinational 4-wide read becomes a **single
request/ack port** that tolerates real PSRAM latency. The unification that makes it clean: priming
and advancing are the same "shift one sample into the window from src_pos" operation, each one read —
so the whole datapath is a stream of single-sample reads through one port, which is exactly what the
shared PSRAM bus offers.

Co-simulated against the same golden `mixer_render` scene with a PSRAM model answering reads late.
**Bit-identical at 1, 7, and 12 cycles of latency** — latency changes timing, not values. Measured
cost per output frame:

```
read latency (cyc)   cyc/frame   of 2177 available (96 MHz / 44.1 kHz)
        1                64          2.9%
        7 (real PSRAM)  112          5.1%
       12               152          7.0%
```

So the mixer holds its 22.7 us deadline with ~95% of the PSRAM port left for the renderer and
tilemap — the bandwidth analysis, now confirmed from the mixer side. This is why the arbiter only
needs to give the mixer a small guaranteed slice at top priority; there is no contention pressure.

### Synthesis check (2026-07-24) — fits, but the datapath needs pipelining

Synthesized `hx_mixer_seq` standalone for the EP4CE15 (`synth/`, virtual pins, 96 MHz constraint).
Two results, one of them the kind only synthesis finds:

- **Fits comfortably.** 4245 LE (28%), 22 nine-bit multipliers (20%), 2416 registers (16%), 0 M9K.
  Alongside the `base` core's 6359 LE that is ~69% of the device — room to spare.
- **Timing FAILS at 96 MHz.** Setup slack −38.9 ns against the 10.4 ns period; Fmax ≈ 20 MHz. The
  whole produce path is one combinational cloud in a single cycle: 8:1 tap-array muxes -> the cubic's
  chained multiplies (t -> t² -> t³ -> products -> sum) -> volume -> pan -> accumulate. Sim could
  never see this; the co-sim is functional, not timed.

A real synthesis-only bug was caught first: `phase`/`tap`/`src_pos` were reset in the config
always-block and written in the sequencer block — a multiple-driver net, illegal for synthesis,
which iverilog had silently allowed. Fixed by giving the sequencer sole ownership of the state
registers.

### Pipelining the datapath (2026-07-24) — 20 -> 74 MHz, still bit-exact

`hx_produce.v`: the produce datapath split into one-multiply-deep registered stages (latency 7) —
input latch; cubic t²; t³ + coeffs; the three products; cubic finish + lerp finish + select; volume;
pan. The finalize was then pipelined into two stages (headroom-shift+saturate, then
shift+offset+clamp). And `phase` was narrowed from a 64-bit q32.32 to a 32-bit fractional register
(the C zeroes the integer part each frame, so a 32-bit add + carry is exactly equivalent) to drop a
64-bit adder off the control path. Every change re-verified bit-exact by the same co-sim — pipelining
is timing, not values — with the frame cost rising only 112 -> 176 cycles (of 2177).

Pipelining alone took setup slack −38.9 -> −3.2 ns (Fmax ~20 -> ~74 MHz), but not to 96 MHz. The
remaining gap was a systematic pattern, not one path: every per-channel state update ran
`ci -> channel mux -> compute -> array demux`, and a few compute chains sat at ~13.5 ns through the mux.

### Load-context restructure (2026-07-24) — MEETS 96 MHz

The definitive fix, and it closes timing. Each channel is processed through flat WORKING registers:
`S_LOAD` latches `arrays[cur]` into `w_*` (the ONLY channel mux, a mux->reg), all arithmetic reads
and writes `w_*` (no mux anywhere in a compute path), `S_STORE` writes `w_*` back to `arrays[cur]`
(the ONLY demux, a reg->demux). Every compute chain is then reg->reg. The finalize was also split to
three stages (headroom+saturate; out_shift+offset; clamp) to clear the last 0.5 ns.

**Setup slack +0.291 ns (85 C) / +1.147 ns (0 C) — MET.** Still bit-exact across every co-sim and all
read latencies. Resources 4197 LE (27%), 22 mult (20%), 2904 registers (19%), 211 M9K bits. Frame cost
185 cycles of 2177 at the realistic 7-cycle read latency (+9 from the load/store cycles — nothing).

The full arc: **−38.9 ns -> +0.291 ns**, unsynthesizable-at-speed to meeting the memory clock, with
the output never once diverging from the golden C. The synthesizable, timing-closed 8-channel mixer
is done; only the board seam (step 6b) remains.

### Free-running audio subsystem (2026-07-24) — done in sim

`hx_audio_top.v`: the integration wrapper. A sample tick (master/TICK_DIV ≈ 44.1 kHz) fires one mixer
render; the finalized frame is latched as the next DAC sample (`audio_l/r` + `audio_stb`); the read
port passes through to the PSRAM arbiter. Its reason to exist is the guarantee that the mixer always
finishes before the next tick, so no sample is missed — `underrun` latches sticky if a tick arrives
while the mixer is busy, and on a miss the tick is still consumed and the last sample held (a click,
never drift).

Co-simulated with the golden scene at **TICK_DIV = 512** — far tighter than the real ~2177 — over 300
samples: bit-exact stream, **underrun = 0**. Synthesized as top: **setup slack +0.409 ns at 96 MHz**,
28% LE. So the whole audio subsystem holds timing and never misses a deadline with vast margin.

### Step 6b — the DAC seam (needs the board)

What remains and genuinely needs hardware: instantiate `hx_audio_top` in the `base` fork, wire its
read port to the real PSRAM arbiter (mixer at top priority), and `audio_l/r` + `audio_stb` into
`dac.v` (the proven MSU-1 I2S serializer). That is the H4 hardware bring-up — and by construction a
mixer arithmetic, control, or timing bug is already ruled out, so only the arbiter wiring and the
DAC/analog path are new variables.
