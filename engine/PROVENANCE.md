# Provenance

Tracks the origin of every file in `engine/`. HX-421 has **no dependency** on microgarbage; everything
below is a **one-time copy/adaptation** from `microgarbage` (all CC0 — license-clean), thereafter owned
here and free to diverge. Do not try to keep it in sync with microgarbage.

Source revision: `258ad24` (microgarbage `git rev-parse --short HEAD`, copied 2026-07-15)

## Copy manifest (extraction audit, 2026-07-15)

Ordered so each file's deps precede it. Target path is under `engine/`.

| Target | Origin | Mark | Notes |
|---|---|---|---|
| `math/bits.h` | `math/bits.h` | adapted | clz/ctz/popcount, header-only. Or re-implement (trivial). |
| `math/fixed_point.h` | `math/fixed_point.h` | adapted | Q15/Q31/Q16.16/Q32.32 ops. The numeric substrate. Trim to Q15+Q32.32 if desired. |
| `containers/ring_buffer.{h,c}` | `containers/ring_buffer.{h,c}` | adapted | mixer channel rings + music_player buffer. Or re-implement (~5 fns). |
| `containers/bitset.{h,c}` | `containers/bitset.{h,c}` | adapted | pool free-block map. Or inline a `uint32_t used[]` in the pool and drop it. |
| `audio/audio_mixer.{h,c}` | `audio/audio_mixer.{h,c}` | adapted | **8-voice mixer, per-source cubic resample, AND the drift-sync PLL** (`mixer_observe_sync`/`mixer_set_drift_ppm`, mixer.c:1028-1149). Core. |
| `audio/music_player.{h,c}` | `audio/music_player.{h,c}` | adapted | **primed-head + lookahead** streaming (intro/loop pinned heads). This is the "primed head" feature. |
| `audio/audio_pool.{h,c}` | `audio/audio_pool.{h,c}` | adapted | block-based fragmentation-free sound RAM (FAT-style chain, refcounted handles). |
| `audio/audio_arbiter.{h,c}` | `audio/audio_arbiter.{h,c}` | adapted | object→voice arbitration, FCFS reject-on-full (voice_alloc seam for future priority). |
| `audio/audio_pool_stream.{h,c}` | `audio/audio_pool_stream.{h,c}` | adapted | pool→player glue (one-shot SFX / resident samples). |
| `audio/audio_ring_stream.{h,c}` | `audio/audio_ring_stream.{h,c}` | adapted | SPSC stereo ring source (FMV/PCM push). Needs C11 `<stdatomic.h>`. |
| `audio/audio_fft.{h}` + `audio_fft.c` | `audio/audio_fft.*` | adapted | band-meter bucketing (Hann, log bands) over mixed output. |
| `audio/audio_fft_kernel.c` | `audio/audio_fft_kernel.c` | adapted | Q15 radix-2 FFT, no libm. Swap for CMSIS `arm_rfft_q15` on MCU. |
| `audio/wav.h` **(NEW)** + `audio/audio_wav_read.c` | decls split out of `audio_sink.h`; `audio_wav_read.c` | adapted | WAV parse + mono/stereo/resample. Fix the mis-colocation: WAV decls move to `wav.h`. |
| `audio/audio_file_stream.{h,c}` | `audio/audio_file_stream.*` | adapted | WAV file-backed stream; I/O behind `AudioFileReader` vtable. Point include at new `wav.h`. |
| `audio/sink.h` **(NEW)** | vtable half of `audio_sink.h` | adapted | just `AudioSinkBackend`/`AudioSink` {open/write/close}, int16 interleaved stereo. No WAV decls. |
| `audio/audio_sink_wav.c` *(optional)* | `audio/audio_sink_wav.c` | adapted | RIFF-writer debug sink for headless test verification. |
| `service.{h,c}` | — | **own (done)** | thin owner: builds pool+arbiter+mixer+players+fft, exposes `hxa_render(int16*,frames)` + command entry points. Re-implemented FRESH (2026-07-15); NOT a copy of `audio_service.c` (no VM `service_channel`/`REQ_AUDIO_*` IPC, no host/OS deps). |
| `demo/player.c` | — | **own (new)** | M0 end-to-end proof: creates the service, loads procedural + WAV SFX, triggers them, optionally streams a WAV, renders ~2 s to `out.wav` via the WAV sink, prints the FFT meter. Built + run by `make demo`. |
| `tests/` (harness + 10 suites) | `include/test_runner.h`, `include/test_portable.h`, `src/audio/tests/test_audio_{mixer,fft,pool,arbiter,pool_stream,pool_stream_integration,wav_read,file_stream,sink_wav}.c`, `test_music_player.c` | adapted | device-free validation (feed sines, assert FFT band energy, mixer output, pool refcounts). Lift as the M0 test harness. |

## Left behind (do NOT copy)

| File | Why |
|---|---|
| `src/audio/audio_service.c` | welded to VM `service_channel.h` + `REQ_AUDIO_*` IPC. Re-implement `service.c` fresh. |
| `src/io/stream_arbiter.*` | host-coupled to `vm_host_fs`; a generic SD round-robin *file* scheduler, not needed for audio (primed-head lookahead = `music_player` + `audio_ring_stream`). Copy-adapt only if you later want multi-file SD scheduling (+`spsc_ring`). |
| `audio_sink_wasapi.c`, `audio_sink_waveout.c` | Win32/COM. Host-side; re-implement per DAC (hardware) / per ares audio device (PC). |
| `test_audio_service.c` | pulls VM channel + pthread. |

## Deviations applied during the copy (2026-07-15)

The extraction matched the manifest closely. Concrete deviations/notes:

- **`math/fixed_point.c` not copied.** `fixed_point.h` is header-only (every op is `static inline`;
  the source `.c` has no non-inline helpers). Manifest listed only the `.h`, so this is consistent —
  the engine needs no `fixed_point.c`.
- **`audio_wav_read.c` gained `#include <stdbool.h>`.** It used `bool`/`true`/`false` but got them
  transitively from `audio_sink.h` before the split. `wav.h` intentionally carries no `<stdbool.h>`
  (its decls don't use bool), so the `.c` now includes it directly.
- **`audio_sink_wav.c` dispatcher trimmed to the WAV backend only.** The original
  `audio_sink_open()` had `#if defined(_WIN32)` branches selecting `audio_sink_waveout`/`audio_sink_wasapi`.
  Those backends are "Left behind"; on mingw `_WIN32` *is* defined, so keeping the branches would have
  produced link errors against symbols we don't ship. The dispatcher now recognizes only `"wav"`; live
  backends are to be registered by `firmware/` and `host/`. `sink.h` likewise drops the
  `audio_sink_waveout`/`audio_sink_wasapi` externs (keeps only `audio_sink_wav`).
- **`service.{h,c}` written fresh (2026-07-15).** The owner (`hxa_*` API) builds and wires
  pool + sync-enabled 16-bit-stereo mixer (8 tracks, cubic interp) + arbiter (via an
  `AudioArbiterSink`) + per-track `music_player` staging + `audio_fft`. It re-implements the
  reference `microgarbage/src/audio/audio_service.c` wiring pattern (the `svc_sink_start/stop/is_done`
  sink, the SFX pool→mixer feed, the external file/ring music voices, and the pump→reap→render loop)
  WITHOUT the VM `service_channel`/`REQ_AUDIO_*` IPC and without any host/OS deps.
  Notable deviations from the reference:
  - **I/O-free TU.** All file I/O goes through the existing `AudioFileReader` vtable, supplied by the
    caller (the demo binds a stdio `fopen` reader). `hxa_load_sfx_wav` slurps via the reader; the
    streamed-WAV voice uses `audio_file_stream`. So `service.c` reaches only the engine + libc
    alloc/mem — no `fopen` of its own. (Preferred option per the task.)
  - **Direct C command API, not IPC.** `hxa_load_sfx_{wav,pcm}` / `hxa_trigger_sfx` /
    `hxa_play_stream_wav` / `hxa_open_pcm_stream`+`hxa_feed_pcm` / `hxa_render` /
    `hxa_observe_sync`+`hxa_set_drift_ppm` / `hxa_fft_bands`. The `pend_*` single-context handoff to the
    sink is kept (file vs ring source), but the tagged music-object table, PCM-FEED drift diagnostics,
    FMV ring/PLL controller, refcounted per-VM FFT enable, and VM-sweep paths are dropped (owner scope).
  - **`hxa_render` folds pump+reap+render.** The reference split request-processing (`_process`) from
    `_render`; the owner's single `hxa_render` runs `music_update` on active slots, `audio_arbiter_reap`,
    then `mixer_render` + `audio_fft_capture` — matching the eventual `hx421_audio_pull` shape.
  - **SFX pool objects stored at native WAV rate** (`audio_pool_alloc_with_rate(..., info.sample_rate)`),
    exercising the mixer's per-channel resampler at play time (no pre-resample on load).
  - **`fft_bands` returns `uint32_t`** (widened from the `uint8_t` band levels) and calls
    `audio_fft_update` itself (non-RT recompute) before copying, so a caller with no separate service
    loop still gets fresh bands.
- **Build (2026-07-15, MSYS2 gcc 16.1.0, `-std=c11 -Wall`):** `make test` still green — **291 checks,
  0 failed** — with `service.o` added to the engine objects. `make demo` builds `build/player.exe` and
  runs it: `out.wav` = 352844 bytes (88200 frames stereo s16), RMS ~2429 (peak 10988, no clip), FFT
  meter shows the 440 Hz sine energy in the low bands rolling off to the highs. Both new files compile
  warning-clean. (Windows build must run from PowerShell with `$env:TMP=$env:TEMP=$env:TMPDIR='C:\mgtmp'`
  and `C:\msys64\mingw64\bin` + `C:\msys64\usr\bin` on PATH — the MSYS2 native-toolchain TMP quirk; a
  bash `export` of TMP does not propagate to the mingw linker/assembler.)
- **Include paths needed almost no rewriting.** microgarbage already used subpath includes
  (`audio/...`, `math/...`, `containers/...`), so the engine is self-contained under `-I engine`
  (`-I.` from the Makefile) with `-Itests/include` for the two harness headers. Only the `audio_sink.h`
  references were repointed at the split headers (`wav.h` for the WAV reader; `sink.h` for the vtable).

### Layout / build

- Headers and sources are co-located per subdir (`engine/audio/`, `engine/containers/`, `engine/math/`)
  rather than split include/src. Tests live in `engine/tests/` with the harness in `engine/tests/include/`.
- `engine/Makefile` (portable C11, gcc; `-Wall`) compiles the 13 engine TUs to objects and links each of
  the 10 portable suites against them. `make test` builds + runs all suites.
- **Verified 2026-07-15** with MSYS2 gcc 16.1.0: all 10 suites green, **291 checks, 0 failed**
  (mixer 48, fft 21, pool 44, arbiter 62, pool_stream 24, pool_stream_integration 8, wav_read 28,
  file_stream 16, sink_wav 18, music_player 22). No microgarbage/host/OS/VM header is reachable from the
  engine tree.

## Shims needed by the copy set

Only four internal headers are transitively required, all header-or-tiny: `math/fixed_point.h`,
`math/bits.h`, `containers/ring_buffer`, `containers/bitset`. **No assert/log macro deps** — core files
use return codes. That's the entire non-audio surface to bring over.
