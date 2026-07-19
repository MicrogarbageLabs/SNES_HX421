# FFT (spectrum analysis)

Wanted both as a feature (audio-player spectrum bars) and as a personal tool. Feasible and cheap on
the MCU.

## Feasibility

- 1024-point FFT at **display refresh (30–60 Hz)**, over a window of recent output samples — NOT
  per-sample. ~a few % of a 100 MHz M3.
- Cortex-M3 has **no FPU / no DSP SIMD** → use **Q15 fixed-point**. Options: CMSIS-DSP
  `arm_rfft_q15` (drop-in, well-tuned), or a compact radix-2 fixed-point FFT. If the Pro's STM32 is
  actually M4/M7-class (DSP/FPU), it's cheaper still.
- Combined budget: mixer ~15–20% + FFT ~few–10% → M3 stays comfortable.

## Pipeline

1. Tap the mixed output ring (or a selected voice) → window (Hann) a frame of N samples.
2. `rfft_q15` → N/2 complex bins.
3. Magnitude (approx: `|re|+|im|` or `max+min/2`, or true `sqrt(re²+im²)`), optional log scale.
4. Down-bin to the display's bar count (e.g. 16–32 bars), smooth/decay over frames.
5. Ship bars to the SNES via the ROM-window/mailbox → 65816 renders (reuse the FMV FFT-overlay path).

## Notes

- Run the FFT in the "spare" time between audio blocks; it's not on the sample-critical path.
- Analysis is display-only — never let it perturb the mix timing (which is master/487-locked).
- Keep it in the portable audio engine so the same spectrum works on PC (M0) and cart (M4).
