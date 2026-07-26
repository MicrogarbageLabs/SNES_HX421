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

## PSRAM memory map (above the loaded ROM) — 16 MB available, rings are free
- `RING_MUSIC0`, `RING_MUSIC1` — the two PCM music rings (16-bit stereo,
  mixer-native). Each begins with a primed head. Default ~32–64 KB; with 16 MB we
  can be generous and tune on hardware.
- `RING_FMV_AUDIO` + `RING_FMV_VIDEO` — the FMV stream's two rings (audio channel
  + video frames), fed from one SD file, primed heads, highest priority.
- `BLEEP_BANK` — a few short SFX preloaded once, played as extra mixer channels on
  keypress (FPGA-resident, no streaming).
- All above the HX421-mode cart ROM image in PSRAM.

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

## The STM32 internal SD-fetch arbiter (foundational mechanism)
The M4 runs one internal arbiter that schedules SD file fetches across up to
**3 streams: two PCM music + one FMV** (the FMV cap is because it interpolates
audio *and* video, so it is the heavy one). Each stream's PSRAM buffer is a
**primed head** — loaded up front so playback/seek starts instantly and covers
SD-fetch latency — followed by **requested data**, the on-demand refills the
arbiter issues as the buffer drains. Refill order is **priority-weighted**: the
FMV stream (a starved buffer drops a frame) is serviced ahead of the music
streams under SD contention.

Built + host-tested: `firmware/audio/hx421_stream.{c,h}` — N-stream ring producer
with per-stream `prime_bytes` (head size) and `prio`. Selection is
deficit×(prio+1), emptiest-first within a priority. Ring-wrap and source-loop are
independent split conditions; non-looping stops at EOF. Proven on the host (mock
SD + simulated mixer drain): no underrun, byte-exact across loop+wrap, EOF, and
priority ordering (`tools/hx421_stream_test.c`).

### The FMV stream (A/V) — how it differs from a PCM stream
FMV is one SD file the arbiter fetches, but it fills **two** PSRAM rings: a video
ring (frames for the FMV/renderer path) and an **audio ring that is just another
mixer channel** — so FMV audio mixes with the two music streams and the bleeps in
the same 8-ch FPGA mixer. The fetch demuxes A/V (or the FPGA does). Its audio
channel uses the same `hx421_stream` mechanism; the video ring is a parallel
consumer with its own drain pointer. It carries the highest `prio`. (The demo
here is the 2-PCM subset; the FMV slot is the third, designed-in from the start.)

## STM32 side — porting the rest of the stack
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

## STM32 firmware integration — BUILT (2026-07-25)
The M4-side pieces are written and **compile clean for the Cortex-M4** against the
real mk3 firmware headers (`arm-none-eabi-gcc -mcpu=cortex-m4`):
- `firmware/audio/hx421_stream.{c,h}` — the arbiter (host-tested).
- `firmware/audio/hx421_wav.{c,h}` — streaming WAV header parse (host-tested).
- `firmware/audio/hx421_mode.{c,h}` — the firmware glue. Binds the arbiter to the
  platform seams: SD-DMA offload SD→PSRAM (`set_mcu_addr` + `ff_sd_offload` +
  `sd_offload_tgt=0` + `f_read`), joystick input (`snes_get_snes_cmd`), and a
  drain pointer (time-estimated at 44.1 kHz until the FPGA exposes the mixer's
  real read position). Opens `music1.wav`/`music2.wav`, parses, lays out two 64 KB
  PSRAM rings at 0x800000/0x810000, primes, and services one offload per loop.
- **Build recipe** (apply in the sd2snes *submodule* `src/Makefile` — kept out of
  the submodule so it stays upstream-clean; reaches the repo sources via VPATH):
  ```make
  VPATH        += ../../../firmware/audio ../../../engine/audio
  EXTRAINCDIRS += ../../../firmware/audio ../../../engine ../../../engine/audio
  SRC          += hx421_stream.c hx421_wav.c hx421_mode.c audio_wav_read.c
  ```
  Then `make CONFIG=config-mk3-stm32`. All four objects compile clean for the M4.

**Remaining to run it:**
1. **`main.c` hook** (John's build): in the game-run loop, next to
   `if(romprops.has_msu1){ while(!msu1_loop()); … }`, add
   `if(<hx421 detected>){ if(!hx421_mode_init()) while(!hx421_mode_loop());
   prepare_reset(); continue; }`. Detection is a choice — a magic in the ROM
   header/title, a filename, or a new `romprops` flag.
2. **FPGA pairing (the mixer must actually read the rings):**
   - **Drain pointer — DONE (2026-07-25):** the mixer's channel-0 read position
     (`hx_mixer_seq.pos0` → `hx_mixer_dac.drain_pos` → main.v) is served at the
     diagnostic window `$3F:F00B-F00D` (24-bit), sim-verified to advance.
   - **Ring base + loop length — parameterized DONE (2026-07-26):** `hx_mixer_dac`
     takes a `LOOP_LEN` parameter and `main.v` exposes `HX421_MIX_BASE` +
     `HX421_LOOP_LEN` macros (default 0x2000/128 = the baked sine). A stream core is
     just `build-fpga-core.ps1 -Macros "HX421_MIX_BASE=8388608","HX421_LOOP_LEN=32768"`
     → the mixer reads the 0x800000 ring as a 64 KB mono loop, no other RTL change.
     Sim-verified (`tb_loop_param`). Runtime-set-by-STM32 (an FPGA SPI command in
     `mcu_cmd.v`) is a later refinement over the compile-time macro.
   - **Remaining:** (b) the STM32 reads the drain pointer — either an **SPI status
     read** (add a command in `mcu_cmd.v` + an `fpga_spi.c` getter, replacing the
     time-estimate in `hx421_mode.c`'s `read_ptr` seam), or an **SNES-relay** (the
     WRAM engine reads `$F00B-F00D` and forwards it via `snescmd`, which the STM32
     already polls). Then the streamed samples become sound.

### On-silicon test ladder (each rung standalone-testable before the firmware)
The mixer read path + bandwidth model are validated on the FXPak in rungs that need
no STM32, so a streaming failure is isolated to the ring/firmware side:
1. **6b.1a/6b.1b** — mixer reads a PSRAM wavetable (single 128-sample sine). DONE.
2. **h6_drain** — the drain pointer advances; proves the STM32's feedback readout and
   that a ROM spin starves the mixer (free-slot contention). DONE (18→80).
3. **h6_wram** — the 65816 jml's into WRAM so the cart bus is 100% free → the mixer
   gets full 44.1 kHz bandwidth; a clean sustained sine vs the ROM-starved one. This
   is the bandwidth foundation the stream needs (engine runs from WRAM, PSRAM is the
   mixer's). *Pending John's bench.*
4. **chord core** (`build-fpga-core.ps1 -Macros "HX421_SECOND_CH=1"`, reuses
   `h6_wram.sfc`) — two channels (1.0 + 1.5) summed by the real mixer → an audible
   fifth. First on-silicon multi-channel MIX; sim-verified (`tb_chord`). *Pending.*
5. **stream core** (base=0x800000, loop_len=32768) + the STM32 firmware filling the
   ring + drain feedback → A1: the first SD-streamed track through the mixer.
3. **FFT + input protocol** (A4 / joystick): CMSIS-DSP `arm_rfft_q15`, the post-mix
   capture read-back, and the SNES-ROM→snescmd button map.

## Sequencing note
This "STM32 first" task and 6b are coupled: A1 already needs the mixer reading a
PSRAM ring (6b core) and the drain-pointer status. The STM32 arbiter, WAV parse,
offload orchestration, input, and FFT can be **written and unit-built** now
(host-testable where pure C); the first *audible* milestone (A1) lands when the 6b
FPGA read path + ring/status seams land alongside it.

Related: [[hx421-fpga-mixer]], [[hx421-stm32-pivot]], [[stream-arbiter]], `docs/audio-6b-psram.md`.
