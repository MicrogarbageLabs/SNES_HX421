# FMV engine — design

Generalizes the working film-critic path (240x208, 15 fps, 4 bands) into an engine the game
drives with start/pause/stop/reset, fed from a packed multi-clip container on SD.

## VRAM: 1 3/4 buffers via CHR-base overlap

Two full CHR buffers cost 8 KB more than necessary, purely because `BG12NBA` addresses in
**4096-word (8 KB) granules**. Placing the back buffer one granule EARLIER overlaps the two by
exactly that granule and reclaims it:

```
frame = 781 tiles = 12496 words

no overlap   A base 0      0..12495
             B base 16384  16384..28879      span 28880 words
overlap      A base 0      0..12495
             B base 12288  12288..24783      span 24784 words   -> 8 KB saved
```

**The shared region is words 12288-12495: 208 words, 13 tiles** — A's tiles 768-780 against B's
tiles 0-12. That is the whole cost, and it is manageable because of WHEN it is written:

- bands 0-2 fill B's tiles 13..780 — no collision, A still displaying
- **band 3 fills B's tiles 0-12 (416 B)** — safe, because A has been displayed for the last time
  and the flip follows immediately

The rule generalizes: **the contested tiles must land in the final band before the flip.** With a
781-tile frame that is 13 tiles; the packer keeps it that way (below).

Reclaiming 8 KB is what buys room for the tilemap pair, OBJ CHR, and a second layer.

## Packing: keep the contested set small

The overlap size is fixed by alignment, but WHICH tiles land in it is the packer's choice. Two
rules:

1. **Order tiles so the contested indices are the least valuable.** Frame-static tiles (borders,
   letterbox filler, repeated background) are cheap to rewrite every frame; put them at the ends.
2. **Waste tiles rather than split a band.** If a frame needs 790 tiles and the clean fit is 781,
   pad to the next boundary. Wasted VRAM is far cheaper than a band that overruns the blank —
   the kernel has no cycle-budgeted chainer, so an overrun writes into active display.

## Frame delivery

Unchanged in shape from the working path, generalized in size:

- **15 fps** = 4 sub-frames per video frame, one per SNES frame. Each sub-frame carries a quarter
  of the CHR and a quarter of the tilemap (the tilemap split landed already — 512 B/band instead
  of 2 KB on band 0).
- **20 fps** = 3 sub-frames, pending the H-blank siphon. The engine takes sub-frame count as a
  parameter so this is a config change, not a rewrite.
- Sub-frames may be triggered **individually** or **as a burst**; the engine preps DMA for
  whichever is asked.

## Container format

One packed file per directory of clips, so playback is an `fseek` rather than a file open:

```
header    magic, version, clip_count, flags
clip[]    name, fps, frame_count, w/h, tile_count, audio_bytes/frame,
          offset of its frame index
frame[]   per clip: absolute offset + length      <- enables seek to any frame
payload   the frames themselves
```

Per-frame **offset and length** is the load-bearing part: it makes any frame a seek target, which
is what branching playback (Dragon's Lair-style) and resume-after-pause need. `tools/` gets a
packer that walks a directory and emits this.

## Runtime model

```
SD ──(seek by frame index)──> FIFO of decoded frames ──> sub-frame staging ──> SNES
        │
        └── preroll head resident in PSRAM: playback starts instantly,
            covering SD seek + arbiter wait (~45-60 ms worst case)
```

The engine pulls automatically — from the FIFO in steady state, from the PSRAM preroll head at
start. The game never names a frame or a sub-frame.

### Pack load primes EVERY head

On loading an FMV pack the loader primes the head of **every clip in it** into PSRAM and builds a
small reference table; the ARM then selects a head by index and arbitrates the tail off SD. The
arithmetic is comfortable:

```
head length      1 frame covers 67 ms at 15 fps, against a worst-case
                 45-60 ms SD seek + arbiter wait  ->  2 frames is ample margin
head size        2 x 38536 B  =  ~77 KB per clip
32 clips primed  ~2.5 MB of 16 MB PSRAM
reference table  ~16 B per clip (psram offset, frames, clip id)  ->  512 B
```

So "prime everything" costs a couple of MB of a resource we have in abundance, and the selection
table stays a few hundred bytes — small enough to hand across SPI whole. Storage on the card is
not a consideration; only PSRAM residency and seek latency are.

## API

Game-facing, deliberately small:

```
fmv_prime(clip)    load the preroll head into PSRAM; no playback
fmv_start(clip)    begin (instant if primed)
fmv_pause()        hold; FIFO retained
fmv_stop()         end; FIFO released
fmv_reset()        rewind to frame 0
```

**Stream objects live on the ARM side.** The ARM owns SD seeking, issues the head command, and
clears the FIFO of stale frames on a seek or stop — the FPGA/coprocessor side only consumes what
it is handed. That split matches the architecture pivot: the STM32 runs logic and streaming, the
fabric does fixed-function staging.

## Build order

1. **Container + packer** (`tools/`) — asset pipeline first; everything downstream needs it.
2. **Overlap buffer layout** — the 8 KB reclaim, with the 13-tile contested set on band 3.
3. **Engine state machine** — FIFO, preroll, the five API calls.
4. **Multi-clip seek** — exercise the frame index with mid-clip jumps.

Sub-frame count stays a parameter throughout, so 20 fps is a config change once the siphon works.
