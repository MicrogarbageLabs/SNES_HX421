# HX-421 audio demo — STM32 streaming arbiter + FPGA mixing

The demo that exercises the whole audio system: **two WAV streams from SD, mixed
by the FPGA, controlled by the SNES joystick, with a live FFT spectrum**, running
as an "HX421 mode" in the FXPak (mk3 / STM32F401) firmware. After it runs we get
a real STM32 SRAM figure to size the rest of the chip's job.

## Architecture — the M4 never touches an audio byte

Decision (2026-07-25): **mix on the FPGA, not the M4.** The M4 only orchestrates;
bulk audio is FPGA-direct. This is what the hardware is built for — the SD-DMA
offload (`sdnative.c` → `fpga_set_sddma_range` + `fpga_sddma(tgt,…)`) streams SD
data straight into FPGA memory without the MCU reading it. `SD_DMA_TGT=0` targets
PSRAM (the same path ROMs load through), so a WAV chunk lands directly in a PSRAM
ring buffer. A software mix on the M4 would instead pull audio into MCU RAM and
push it back over the **per-byte, no-TX-DMA** FPGA SPI — the one weak spot — so it
is explicitly rejected.

```
 SD card ──(FPGA SD-DMA offload, tgt=PSRAM)──▶ PSRAM ring buf A ─┐
        └─(offload, tgt=PSRAM)──────────────▶ PSRAM ring buf B ─┼─▶ FPGA 8-ch mixer ─▶ dac_mix ─▶ I2S ─▶ SNES
                                    PSRAM bleep bank ───────────┘        │
 M4: open/seek WAVs, arbitrate refills, read drain ptrs, FFT ◀── capture window (SPI read)
 SNES ROM: read joystick ──▶ snescmd ──▶ M4 ;  read FFT bands ◀── ; draw sprite bars
```

## PSRAM memory map (above the loaded ROM)
- `RING_A`, `RING_B` — per-stream ring buffers (e.g. 32–64 KB each; sized in the
  build once we see real refill cadence). 16-bit stereo samples, mixer-native.
- `BLEEP_BANK` — a few short SFX samples preloaded once (the "bleeps and bloops"),
  played as extra mixer channels on keypress.
- Chosen to not collide with the tiny HX421-mode cart ROM image in PSRAM.

## The three seams that need the FPGA side (i.e. 6b work)
1. **Offload → ring, not just dac_buf.** Today `tgt=1` (dac_buf) is what MSU uses;
   we drive `tgt=0` (PSRAM) with the ring write pointer as the MEM address. Mostly
   already there (ROM-load path); needs the wrap handling at ring end.
2. **Mixer reads the ring.** Each stream is a mixer channel whose sample source is
   its PSRAM ring (loop over [base, base+size)). This is 6b — mixer as a PSRAM
   requestor in the base core's `free_slot`/`STATE` machine (`docs/audio-6b-psram.md`).
3. **Drain feedback + FFT capture (status reads, FPGA→M4 SPI DMA):**
   - each channel's current read pointer, so the arbiter knows how much a ring has
     drained and how much to refill;
   - a small rolling capture of the *mixed* output (post-mix, pre-I2S) for the FFT,
     since the M4 no longer sees the mixed samples. `audio_fft` is built to FFT a
     captured window — this is exactly its input. DECISION: capture width/rate.

## STM32 side — the streaming arbiter (this task)
Ported from `engine/audio` (already M4-minded C):
- `audio_wav_read` + `wav.h` — parse the two WAVs (rate/channels/data extent).
- `audio_ring_stream` / `audio_file_stream` — per-stream: file cursor, ring write
  pointer, "how full" vs the FPGA read pointer; produce refill requests.
- `audio_arbiter` — round-robin/priority across the two external (file-stream)
  voices; issue the next SD-DMA offload for whichever ring is emptiest; handle WAV
  loop / EOF; admit/stop on joystick.
- Platform seams to write: SD via FatFs (exists), offload kick (`fpga_sddma`),
  ring-pointer + capture status reads, and the mode's cooperative `hx421_loop()`
  (modeled on `msu1_loop()` — called from the run loop, non-blocking).
- `audio_fft` with the CMSIS-DSP `arm_rfft_q15` kernel (needs CMSIS-DSP vendored).

## Input — joystick → SNES → M4
The HX421-mode cart ROM reads the controller each frame and writes buttons into
the `snescmd` shared area; the M4 polls it (`fpga_set_snescmd_addr(SNESCMD_SNES_CMD)`
/ `fpga_read_snescmd`, the existing channel). Buttons: start/stop stream A, start/
stop stream B, and a few that trigger bleep-bank channels on the FPGA directly.

## FFT display
M4 reads the FPGA capture window, runs `audio_fft` → 16 band levels, writes them to
the shared area; the cart ROM reads them and draws sprite bars (reuse the
`fmv-fft-overlay` sprite scheme). Band update is a background task, not per-sample.

## "Enter HX421 mode"
A menu entry / special cart that, instead of `while(!msu1_loop())`, runs
`while(!hx421_loop())` with the arbiter + FFT service. Keep it behind a config flag
so the normal firmware path is untouched (like every prior HX bring-up).

## Increments
- **A1 — one stream to the FPGA.** M4 offloads music1.wav into RING_A; FPGA mixer
  plays RING_A (one channel, 6b). Hear one SD-streamed track via the mixer. Proves
  offload→ring→mixer + the drain-feedback loop.
- **A2 — two streams + arbiter.** Add RING_B + `audio_arbiter` round-robin; joystick
  start/stop each. Simultaneous FPGA mix of two SD streams.
- **A3 — bleeps.** Preload BLEEP_BANK; joystick keys trigger extra mixer channels.
- **A4 — FFT.** Capture window + `audio_fft` + sprite bars.
- **Measure.** STM32 `.map` + runtime high-water → SRAM headroom for the rest.

## Sequencing note
This "STM32 first" task and 6b are coupled: A1 already needs the mixer reading a
PSRAM ring (6b core) and the drain-pointer status. The STM32 arbiter, WAV parse,
offload orchestration, input, and FFT can be **written and unit-built** now
(host-testable where pure C); the first *audible* milestone (A1) lands when the 6b
FPGA read path + ring/status seams land alongside it.

Related: [[hx421-fpga-mixer]], [[hx421-stm32-pivot]], [[stream-arbiter]], `docs/audio-6b-psram.md`.
