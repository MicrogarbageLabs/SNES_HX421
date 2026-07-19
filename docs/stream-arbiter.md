# HX-421 Stream Arbiter — design

## Why

The FMV pipeline grew buffering in five uncoordinated places — the unit read-ahead
ring, the pcm push ring, the mixer voice, the output layer (WASAPI/bsnes), and the
VRAM double-buffer — with **no single owner of the A/V timeline**. Result: a
1–2 s audio offset that can't be found by inspection, because no stage has a
known depth. Every point fix fights an invisible buffer.

The arbiter replaces that with **one streaming layer that owns every buffer as a
designed number** and **one A/V clock**. It is also microgarbage's proven shape
(`src/mgapi/stream_arbiter.c`) and, crucially, **the M3's firmware** — portable C
behind two seams (source: SD read / `fread`; buffer memory: PSRAM / host RAM). So
building it here *is* building the M3 streaming firmware.

## Architecture

```
 source (SD / .fmv file)                     [SEAM: stream_read(ctx, dst, n)]
        │  round-robin fill on the worker thread
        ▼
 ┌─────────────── Stream Arbiter (worker thread) ───────────────┐
 │  for each registered stream, if its buffer < high-water:     │
 │     read one unit from source → decode → push to its buffer  │
 └───────────────────────────────────────────────────────────────┘
        │                                   │
        ▼ (per FMV frame)                   ▼ (per FMV frame)
 ┌───────────────────────┐         ┌───────────────────────┐
 │ VIDEO FRAME FIFO       │         │ AUDIO RING (per stream)│
 │ N complete frames      │         │ N frames of PCM        │
 │ (cgram+tilemap+chr)    │         │ (mixer drains it)      │
 │ depth = FIFO_DEPTH     │         │ depth = AUDIO_LEAD     │
 └───────────────────────┘         └───────────────────────┘
        │ pop on FRAME_DONE (SNES clock)     │ mixer render (audio clock)
        ▼                                    ▼
   stage bands → VRAM                    bsnes / WASAPI out
```

Clients register with the arbiter: **FMV video** (fills the frame FIFO), **FMV
audio / WAV music / SFX** (fill audio rings). The arbiter round-robins, keeping
each buffer topped to its high-water mark. Every depth is a named constant.

## The A/V sync mechanism (the part that's been failing)

One rule: **the video FIFO is the master clock; audio is paced to it.**

1. The arbiter decodes frame `k` and, atomically, pushes frame `k`'s audio into
   the audio ring. So the FIFO and the audio ring advance **together, one frame
   at a time** — they can never diverge by more than rounding.
2. **Preroll = the ring depth.** Video is held (black) until the audio ring holds
   `AUDIO_LEAD` frames. Then frame 0 is released. Now the audio being *heard* is
   exactly `AUDIO_LEAD` frames behind what's *buffered* — i.e. frame 0 is shown as
   frame 0's audio reaches the output. The latency is `AUDIO_LEAD` frames, a
   **designed number** (e.g. 4 frames ≈ 267 ms), not a mystery.
3. Video is **released on FRAME_DONE** (SNES clock); the FIFO depth absorbs bsnes
   jitter. Audio is drained by the mixer (audio clock).
4. A **setpoint drift PLL** (the one we already have working — ppm locked near 0)
   trims the mixer rate so the audio-ring fill holds at its setpoint, correcting
   only the slow bsnes-vs-audio clock drift.

Because #1 keeps A and V frame-locked at the source and #2 fixes the start offset
to a known depth, there is nowhere for a 1–2 s offset to hide.

## Explicit buffer depths (tunable, but named)

| buffer | depth | ~time @ 15 fps |
|---|---|---|
| video frame FIFO | `FIFO_DEPTH = 4` | 267 ms lookahead |
| FMV audio ring | `AUDIO_LEAD = 4` frames | 267 ms |
| preroll | `= AUDIO_LEAD` | 267 ms |
| VRAM double-buffer | 2 (A/B) | — |

Total designed A/V latency ≈ `AUDIO_LEAD` frames. No implicit buffering remains:
the old unit read-ahead ring and the ad-hoc pcm-ring fill are subsumed.

## Primed heads (instant playback — the Dragon's Lair goal)

Each stream can pin its **head** resident (first `FIFO_DEPTH` frames + audio) so a
`play(stream)` starts with the FIFO already full — zero buffering stall. Load
several FMVs' heads and a branching scene-jump plays instantly with no gap. The
arbiter tops the active stream off from the source behind the head.

## M3 seams

- **`stream_read(ctx, dst, n)`** — SD read on the M3, `fread` in the DLL.
- **Buffer memory** — PSRAM on the M3, host RAM in the DLL. The FIFO/ring live
  behind an allocator seam.
- Everything else (arbiter round-robin, FIFO/ring logic, the A/V sync loop) is
  portable C, identical on both. The M3 core *is* the arbiter.

## Port plan (from microgarbage `src/mgapi/stream_arbiter.c`)

1. Arbiter core: stream registry + round-robin `arbiter_tick()` + SPSC ring per
   client. (Port `stream_arbiter.c`; swap the source seam.)
2. Video frame FIFO of complete `FmvFrame { cgram, tilemap, chr }`; the worker
   decodes .fmv units into it (moves today's per-band decode off the hot path).
3. Consumer: FRAME_DONE pops the current FIFO frame, stages its bands (the
   FRAME_DONE-driven pacing we already proved). Preroll gate + primed-head hold.
4. Audio: FMV audio pushed per frame into an audio ring; preroll = ring depth;
   the setpoint PLL (already working) locks the rate.
5. A/V instrumentation: log the FIFO frame index vs the audio-ring drain index so
   the offset is a measured number, not a guess.

Build incrementally; keep the existing FRAME_DONE pacing (proven) and setpoint
PLL (proven) — the arbiter reorganizes *where the buffers live and who owns the
clock*, it doesn't throw away the two things that already work.
