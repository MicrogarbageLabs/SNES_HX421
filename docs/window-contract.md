# HX-421 window contract — milestone 2 (dynamic renderer)

The coprocessor presents a 64 KB window as the cart ROM (bank `$C0-$FF` flat
`$0000-$FFFF`, plus `$8000-$FFFF` aliased into the low banks). On M2 the DLL
(playing the RISC-V) freewheels frame data into the low half of that window; the
SNES kernel (`snes/kernel.s`) reads it and drives the PPU. This file is the
authoritative layout. It is mirrored in `snes/hx421.inc` (SNES side) and
`runtime/hx421_runtime.c` (DLL side) — keep all three in sync.

## The rendering model

A **free-running H-timer IRQ** (`NMITIMEN=$10`, HTIME=22) fires **every
scanline**. Each fire the kernel:

1. ACKs the timer (read `$4211`) — **unconditionally, every line** (gating this
   on frame-ready black-screens the display: microgarbage's v2.30.14 bug),
2. latches the live **V counter** (SLHV → OPVCT),
3. reads `action_table[V]` straight from the **front buffer in the window** (no
   WRAM staging), and dispatches the per-line action.

Per-line **action codes** (one byte per V line, V = 0..261):

| code | name | effect |
|---|---|---|
| 0 | `ACT_NONE` | leave INIDISP as-is |
| 1 | `ACT_BLANK` | `INIDISP=$8F` (force-blank — opens VRAM DMA); on the frame's **first** blank line, `jsl` into the coprocessor-emitted DMA body if a frame is pending |
| 2 | `ACT_UNBLANK` | `INIDISP=$0F` (reveal) — the top-letterbox line |
| 3 | `ACT_SIPHON` | per-scanline **H-blank siphon** — a small GP-DMA to VMDATA fired inside this visible line's right-edge H-blank (see below) |

The letterbox is **force-blank (INIDISP), not the PPU window registers**. The
DMA-able window = vblank + top letterbox + bottom letterbox, so a bigger
letterbox = more DMA bandwidth (dynamic letterbox = dynamic bandwidth). Frame
VRAM data (tilemap/CHR/CGRAM) is moved by **general DMA during force-blank lines
only** — never during rendered lines (the PPU drops active-display VRAM writes).

The DMA push uses the **NMI-emitter technique** (microgarbage `mg_nmi` /
`copro_r3d`): the coprocessor bakes the per-frame VRAM push as a run of
**immediate-loaded** 65816 DMA slots into the window's DMA-body region, and the
kernel `jsl`s straight into it (**execute-from-window** — the freewheel win). No
descriptor list to long-load per slot, no runtime dest dispatch — the lowest
per-transfer overhead, maximizing blank-window bandwidth. The coprocessor owns
the **fit budget** at emit time (total DMA must land in the blank window), so the
kernel carries no runtime cycle-budget/defer machinery.

The body is armed at the **`V >= VIS_END` crossing** (once/frame, gated by
`K_FRAME_ARMED`): `frame_prep` self-patches the `jsl` operand (`fire_dma_body+1`)
to the front buffer's body and sets `K_BODY_PENDING`. It fires on the frame's
first blank line, so the DMA runs through the **big** blank window (bottom
letterbox + vblank + top letterbox, ~70 lines) and is resident long before the
reveal at `V=TOP_LB`. (Arming at the V-wrap instead — DMA on only the ~16 top
letterbox lines — leaves a 2 KB burst racing a ~3-line margin to the unblank;
straddle it and the whole frame stays force-blanked = flashing. VIS_END is the fix.)

## Double buffering

Two self-contained buffers live in the low window. The SNES reads the **FRONT**
buffer; the DLL writes the **BACK** buffer. The front is latched **once per
frame** (at the `V >= VIS_END` crossing, when the visible region has just ended)
and held for the whole frame, so a per-line action read can never see a
half-written table.

```
buffer 0 base = $0000     buffer 1 base = $3000     stride = $3000
```

Offsets inside a buffer:

| offset | size | field |
|---|---|---|
| `$0000` | 512 B | **action table** — `action[V]`, V = 0..261, indexed by the live 9-bit V |
| `$0200` | 1 B | header: **TOP_LB** (top-letterbox line count = the unblank line) |
| `$0201` | 1 B | header: **VIS_END** (first bottom-letterbox line = 224 − BOT_LB) |
| `$0202` | 2 B | siphon: **SIP_BPL** — bytes/line (`0` = siphon off) |
| `$0204` | 2 B | siphon: **SIP_VRAM** — VRAM word dst, set once on the first siphon line, auto-increments after |
| `$0206` | 2 B | siphon: **SIP_SRC** — source base offset in bank `$C0` (running cursor advances by SIP_BPL/line) |
| `$0208` | ≤504 B | **emitted DMA body** — baked-immediate 65816 the kernel `jsl`s (RTL) |
| `$0400` | 2048 B | **tilemap** — 32×32 BG1 map words (the staged frame) |
| `$0C00` | — | **siphon payload** (clear of the tilemap; e.g. the streamed CHR/rows) |

**Emitted DMA body** — the coprocessor writes 65816 machine code here each frame
(see `runtime/hx421_runtime.c` `hx_emit_dma_body` / `e_dma_vram_slot`). It sets
the A-bus bank (`$4304`=$C0) **once**, then one or more baked slots —

```
lda #bbus : sta BBAD0 ; lda #dmap : sta DMAP0        ; B-bus + transfer pattern
rep #$20  ; lda #src : sta A1T0L ; lda #size : sta DAS0L ; sep #$20   ; A-bus + count
(dest prep: VRAM -> VMAIN=$80 + VMADDL word ; CGRAM -> CGADD ; OAM -> OAMADDL)
lda #$01  : sta MDMAEN                                ; fire channel 0
```

— and ends in `rtl`. The src is the **absolute window offset within bank `$C0`**
(buf0 tilemap `$0400`, buf1 `$3400`), baked per buffer. M2 emits exactly one slot
(tilemap → VRAM word 0, 2048 B); CGRAM/OAM/CHR slots append the same way. The
`prep` dispatch is resolved at **emit time**, so the body contains only the one
destination path — no runtime `cmp`/branch ladder.

## Control mailbox (single-instance; not double-buffered)

Kept clear of the two frame buffers (`$0000-$5FFF`) and the M1 boot strobe
(`$7FE0`). Addresses carry read-side effects (the read is the signal — the cart
bus is read-only), matching the mgapi scheme.

| addr | name | meaning |
|---|---|---|
| `$7800` | `HX_FRAME_READY` | DLL writes: `0`=none, else `(back_index + 1)`. The kernel reads it at the `V >= VIS_END` crossing and latches that buffer as the new front. |
| `$79C1` | `HX_FRAME_DONE` | SNES **read-strobe**: emitted right after the DMA body returns (RTL). On this read the DLL flips buffers (the published buffer is now on-screen; it starts writing the other) and clears `HX_FRAME_READY`. |
| `$7E00` | boot→runtime | M1: one-shot status boot → runtime |
| `$7F00` | status | M1 status byte |
| `$7FE0` | boot strobe | M1: boot.s handoff read-strobe |

### The handshake (depth-2 ping-pong)

```
DLL:    fill BACK buffer (action table + tilemap + siphon) ; HX_FRAME_READY = back+1
SNES:   at V>=VIS_END -> read HX_FRAME_READY; if new, latch it FRONT; read all frame
        bottom-LB+vblank+top-LB blank window -> jsl emitted DMA body -> DMA tilemap to VRAM
        body returns (RTL) -> read HX_FRAME_DONE (strobe)
DLL:    on the HX_FRAME_DONE read -> flip BACK^=1 ; HX_FRAME_READY = 0
```

Because the DLL always writes the buffer that is **not** the current front, and
the front only advances at the SNES's own frame boundary, per-line reads are
always of a stable, fully-written buffer. VRAM itself is single-buffered and
updated entirely inside force-blank, so there is no tearing.

## M2 line plan (as the DLL stages it)

`TOP_LB=16`, `BOT_LB=16`, `VIS_END=208`:

```
V = 0..15    ACT_BLANK    (top letterbox — force-blank only; body already ran)
V = 16       ACT_UNBLANK  (reveal the visible band)
V = 17..18   ACT_SIPHON   (H-blank siphon: stream tile-2 CHR into VRAM live) [SHELVED]
V = 19..207  ACT_NONE     (visible band — FFT bars + the siphon tile row)
V = 208..261 ACT_BLANK    (bottom letterbox + vblank — body is armed + fires here)
```

The DMA body is armed + fired at the `V=208` crossing, so the whole tilemap DMA
(2048 B ≈ 13 lines) runs during the bottom-letterbox + vblank window (~54 lines)
and is resident well before the reveal at `V=16` — a ~40-line margin.

> **H-blank siphon (`ACT_SIPHON`) is SHELVED for real hardware.** It works in
> bsnes-plus (and ares), but the per-line GP-DMA overruns the true H-blank
> ceiling on silicon (microgarbage's `emitter-kernel.md` finding). The code and
> the M2 emulator demo stay as a proven-in-emulator reference; hardware content
> uses only the bulk emitted-body DMA (sprites/OAM for overlays), not the siphon.

## H-blank siphon (per-line live VRAM push)

The siphon is the community trick for updating VRAM **during active display**:
it is **not HDMA**, but a plain **channel-0 GP-DMA to `$2118` (VMDATA)** executed
inside a line's right-edge H-blank (`$4212` bit 6, ~dot 274+), which the SNES
lets land in VRAM even while the screen is live. `siphon_line` (in `kernel.s`),
per `ACT_SIPHON` line:

1. sets ch0 source = running cursor (`SIP_SRC`, advances by `SIP_BPL`/line),
   count = `SIP_BPL`, B-bus = VMDATAL, A-incrementing;
2. on the **first** siphon line of the frame, programs `VMAIN=$80` + `VMADDL`
   from `SIP_VRAM` **once** (writing VMADD mid-active-display corrupts VRAM; it
   only auto-increments after);
3. **spins on the H-blank flag**, then `INIDISP=$8F` → `MDMAEN` → `INIDISP=$0F`
   entirely inside the off-screen H-blank;
4. advances the source cursor.

With the **free-running H-IRQ** the next line's IRQ re-fires on its own — so,
unlike microgarbage's H+V siphon, there is **no per-line `VTIME` re-arm** (that
re-arm off the live V was the jitter source that would wreck the tight per-line
timing). `SIP_BPL` must fit the H-blank (~16 B is the safe ceiling; ~272 master
cycles). The per-frame runtime (`K_SIPHON_STARTED`, source cursor) is reset at
the `VIS_END` crossing **every** frame, so a frame the handshake left un-latched
can't siphon with a stale VMADD/cursor.

The M2 demo siphons an **animated 32-byte CHR** for tile 2 (2 lines × 16 B) into
VRAM, and the bulk tilemap plants that tile across a row at screen line ~64 —
fetched long after the siphon writes it. If the siphon lands you see a scrolling
green band; if not, that row stays backdrop. This is the primitive the rolling
3D-band and FMV paths will build on.

## First content: FFT spectrum bars

The DLL calls `hxa_fft_bands(16)` each frame and paints 16 vertical bars (2 tile
columns each) into the tilemap: tile 1 = solid bar (CGRAM colour 1, green),
tile 0 = empty (colour 0, blue backdrop). Bars grow up from BG tile row 25
(bottom of the visible band), height ∝ band level. They react to whatever audio
plays (the SMOKE 440 Hz tone by default, or a `-Wav` music stream).

## Timing details to verify on hardware / ares (flagged)

- **Top-edge unblank.** The unblank writes `INIDISP=$0F` at (V=TOP_LB, H=22).
  H=22 is early in the line but not in H-blank, so the first visible line may
  show a few blanked pixels at the far left / top edge (microgarbage's "dogcat
  late-unblank" class). If eyeballing shows a top-edge artifact, move
  `ACT_UNBLANK` to `V=TOP_LB-1` in the DLL (`hx_produce_frame`) — a DLL-only
  tweak, no kernel rebuild. Symmetric caveat at the `VIS_END` blank line's left
  edge.
- **HTIME value.** 22 is inherited from microgarbage; the exact dot may want a
  nudge once the top edge is eyeballed on ares.
- **DMA-fits-window constraint.** The emitted body fires all its slots in one
  go (no runtime defer), so the coprocessor must size the total DMA to fit the
  bottom-LB + vblank + top-LB window at **emit time**. Arming at `VIS_END` gives
  ~70 lines, so a 2 KB tilemap has a ~40-line margin; bigger payloads need a
  taller letterbox (dynamic force-blank = dynamic bandwidth) or fewer bytes.
- **H-blank siphon on bsnes-plus.** `siphon_line` relies on the `$4212` bit-6
  H-blank flag and on a GP-DMA landing in VRAM mid-active-display. This is proven
  on ares (`microgarbage/snes/siphon_hblank_test.s`); confirm bsnes-plus honors
  it (the M2 scrolling-green-band demo is the eyeball check). `SIP_BPL` may need
  a nudge down if the band shows tearing/speckle (DMA overran the H-blank).
