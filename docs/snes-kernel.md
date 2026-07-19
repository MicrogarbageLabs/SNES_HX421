# SNES-side management kernel (HX-421)

> **Milestone status.** M1 (static boot → WRAM → PPU) and **M2 (dynamic
> per-frame renderer)** are implemented in `snes/{boot.s, kernel.s}`. M2 adopts
> the free-running-H-IRQ + per-line **action-table** kernel described as the
> "cleaner evolution" in §3 below (not the fixed descriptor-walk state machine).
> The live window layout — action-table double buffer, frame-ready/frame-done
> handshake, descriptor format — is specified in **`docs/window-contract.md`**;
> the prose below is the original survey/rationale.

Design note. Maps microgarbage's proven 65816 cart-side kernel onto the HX-421
(FXPak Pro / SD2SNES Pro) coprocessor cart, accounting for HX-421's different
compute model. Prose + loop structure only — not code.

Source of truth surveyed: `microgarbage/snes/{boot.s, kernel.s, copro.inc,
README.md}` and `microgarbage/docs/emitter-kernel.md`. Cited inline. **Nothing in
microgarbage was modified.**

---

## 0. What the microgarbage SNES model actually does

The coprocessor **is the cartridge ROM**. On microgarbage the STM32 presents its
64 KB M7 DTCM as the cart window (16-bit A0–A15 decode, mirrored across every
bank, mapped HiROM so the flat window is reachable through bank `$C0`)
(`copro.inc:1-33`, README "Hardware model"). The SNES half is a thin 65816 kernel
— **a generic transfer substrate, not frame logic** (README:7-9).

### Boot → run-from-WRAM

`boot.s` lives in the reset window at `$8000–$FFFF`. At reset it goes native,
sets stack/direct-page, forces blank (`INIDISP=$8F`), disables NMI + auto-joypad
(`stz NMITIMEN`), then (`boot.s:24-72`):

1. **Handshake** — spin until `COPRO_STATUS & ST_KERNEL_RDY` ($80) — the copro
   signals the kernel blob is staged in the window (`boot.s:48-53`).
2. **Copy the kernel blob** from the ROM window (`__KERNEL_LOAD__`) into low WRAM
   (`__KERNEL_RUN__`, which the linker puts at `$0400`) byte-by-byte
   (`boot.s:55-63`; `snes.cfg` gives KERNEL a LOAD-in-ROM / RUN-in-WRAM split).
3. **Strobe `STROBE_BOOTED`** — a single read whose *address* tells the copro
   "I'm running from RAM, switch the window to runtime serving" (`boot.s:65-68`).
4. **`jmp` into the WRAM kernel** (`boot.s:71`).

The hardware NMI/IRQ/BRK vectors point at 3-byte **trampolines** in the window's
top page that `jmp (RAMVEC_NMI/IRQ)` through a WRAM pointer, so the RAM kernel
installs its own handlers (`boot.s:78-89`, `copro.inc` `RAMVEC_* = $0200/$0202`).
Contract: the copro **must keep `$FF00–$FFFF` static** after boot (vectors +
trampolines); everything below is the per-frame data channel (`copro.inc:18-23`).

The copro **holds the SNES in reset** until the boot window is primed, so the very
first fetch already sees a valid image (`copro.inc:12-16`, README:45-47).

### The per-frame loop (virtual-NMI kernel)

The runtime kernel (`kernel.s`) runs **NMI-off**. A single self-chaining **H+V
timer IRQ** (`NMITIMEN=$30`, `HTIME=22`, set once at boot — `kernel.s:155-170`)
drives everything as a two-state machine that also owns a **force-blank
letterbox** (`kernel.s` proc `irq:927`, README:63-95):

```
state A  (fires at V = VIS_END, the bottom-letterbox line):
    force-blank (INIDISP.7 = 1)                       ; opens the DMA window
    if COPRO_FRAME_RDY != 0:
        run frame_dma:
            apply PPU register batch (32 B @ COPRO_PPU_BATCH)   ; BGMODE/SC/NBA/TM/TS/scrolls...
            apply Mode-7 batch (only if BGMODE==7)
            arm HDMA channels 1..6 from COPRO_HDMA_CONFIG
            cycle-budgeted DMA-list walk (below)
    schedule unblank at V = top_lb ; -> state B

state B  (fires at V = top_lb, the top-letterbox line):
    (arm the per-scanline VRAM siphon if configured, while still blanked)
    unblank (INIDISP = brightness)                    ; reveal visible region
    schedule next blank+burst at V = VIS_END ; -> state A
```

The **cycle-budgeted DMA-list chainer** (`frame_dma:363`, walk at `:638-817`) is
the heart of it. Each "frame_ready" burst walks the 8-slot descriptor list; before
each slot it reads the **live beam position** (`calc_bytes_rem:833` via
SLHV/OPVCT) and fires the slot only if the bytes still fit the remaining blank
window (~170 B/line, measured safe). If not, it **defers** the rest to the next
frame (leaves `K_SLOT_CURSOR` where it stopped). When the last slot is walked it
resets the cursor and **strobes `COPRO_FRAME_DONE`**, on which the copro bumps
`frame_consumed` + clears `frame_ready`. A payload too big for one window (a full
240×208 frame) thus streams over ~4 frames; the copro double-buffers depth-2 so
the kernel never idles (README:138-150). Channel 0 is reused across slots (SNES
DMA channels never run in parallel). Reads are idempotent — if the copro doesn't
restage, the last picture persists.

Force-blank around the DMA is mandatory: VRAM writes during active display are
silently dropped by the PPU (`kernel.s:624-636`), so the letterbox exists both to
give a clean top edge and to guarantee the burst lands.

### The cart-window / DMA-descriptor staging contract

Verified against `copro.inc` (current layout — see the note on drift below):

| Region | Address (bank `$C0`) | Meaning |
|---|---|---|
| `COPRO_DATA` | `$0000` | per-frame PPU payload the SNES DMAs (copro-laid) |
| `COPRO_FRAME_RDY` | `$7800` | non-zero = a frame is staged; 0 = skip this vblank (`copro.inc:43`) |
| `COPRO_DMA_LIST` | `$7808–$7847` | 8 slots × 8 bytes, the descriptor list (`copro.inc:44`, `:136-162`) |
| `COPRO_PPU_BATCH` | `$7848` | 32-byte PPU register batch (`copro.inc:45`, `:188-211`) |
| `COPRO_INIDISP_HDMA` | `$7868–$7967` | (legacy) per-scanline INIDISP HDMA table |
| `COPRO_HDMA_CONFIG` | `$7968` | HDMA ch1-6 config, 8-byte stride (`copro.inc:52`) |
| `COPRO_STATUS` | `$79B1` | boot status; `ST_KERNEL_RDY=$80` (`copro.inc:60`, `:243`) |
| `STROBE_BOOTED` | `$79B0` | boot→runtime one-shot read (`copro.inc:180`) |
| `COPRO_FRAME_DONE` | `$79C1` | read-strobe: last slot walked (`copro.inc:110`) |
| `JOYPORT_P0..P3_LO/HI` | `$7000,$7100,…,$7700` | 8 page-aligned joypad post ports (`copro.inc:171-178`) |

Each DMA slot is 8 bytes (`copro.inc:143-156`): `+0` bbus (`$21xx` low byte:
`$22`=CGDATA, `$18`=VMDATAL, `$04`=OAMDATA, `0`=empty/end), `+1` DMAP, `+2..3`
source offset within `$C0`, `+4..5` byte count, `+6..7` prep value written to the
dest register (CGADD / VMADD / OAMADDR) before firing.

> **Address-drift note.** The HX-421 `docs/emulation-seam.md` and `include/hx421.h`
> quote the *original* microgarbage map (`$7E00` boot strobe, `$7F00` status). The
> live `copro.inc` has since moved status to `$79B1` and the boot strobe to `$79B0`
> (to escape the per-frame INIDISP-HDMA region that gets rewritten every frame —
> `copro.inc:59,179-180`). `$7800`/`$7808-7847` frame-ready + descriptors are
> unchanged. HX-421 is defining its own contract, so treat the exact offsets as
> ours to pick — but inherit the *structure*, and don't copy the stale numbers.

### Joypad mailboxing (the address-readback) — how and why

microgarbage runs with **auto-joypad OFF**. Each frame, in active display (right
after state B unblanks), the kernel **bit-bangs `$4016`/`$4017`** itself to read up
to four pads in parallel via the multitap protocol (`read_joypads:1353`), then
**forwards each pad byte to the copro by reading a magic address**
(`joypad_mailbox:1329`): `lda f:JOYPORT_P0_LO_L,x` where `x` is the pad byte. The
copro watches the address bus, sees `$7000 + N`, and latches `N` as that pad
byte — **the access itself is the message; the returned data byte is don't-care**
(`copro.inc:164-178`). Eight page-aligned 256-byte ports carry 4 pads × 2 bytes;
page-aligning them means the 8-bit indexed read can never cross a page (no 65816
indexed dummy-read), so the copro latches exactly one clean address per port.

**Why it exists:** the microgarbage cart bus is **strictly read-only** — the STM32
presents DTCM-as-ROM and decodes only *which address is read*; there are no write
pins (`copro.inc:6-10`). So the only way the SNES can *send* anything back to the
copro is to encode the data into a read address. Joypad is the main thing it needs
to send, so joypad delivery is an **address-as-data write emulated over a read-only
bus**. The post doubles as the frame handshake: the port-7 read is what tells the
copro "previous frame consumed, stage the next" (`kernel.s:225-233`).

Note the round-trip in the PC/emulator world: the host injects pad state into the
*emulated controller ports* (`mgapi_post_joypads`), the emulated SNES bit-bangs
`$4016/$4017` to read it, then strobes it *back* to the copro over the address bus.
The relay exists because the copro (whether STM32 or a PC DLL) sits on the **cart
bus, not the controller bus** — it never sees the pads directly.

---

## 1. HX-421 differences that reshape this

Three things differ from microgarbage:

1. **The compute core is a real, non-virtualized soft RISC-V** on the FPGA (plus
   the M3), running game logic + processing directly — **not** microgarbage's
   interpreted RISC-V VM with its runtime-emitted 65816 (`emitter-kernel.md`). The
   RISC-V/compositor **natively produces the staged frame data**.
2. **The bus is a real cartridge bus**, not a read-only DTCM window. The FXPak /
   sd2snes base already decodes **writable regions** (it maps battery SRAM today).
   So the SNES can genuinely *write* to the cart, not just read.
3. **The SNES reads its own controllers natively.** On real hardware there is no
   host injecting pads; the SNES's own auto-joypad ($4218/$4219) or a bit-bang
   already *has* the pad state. What it lacks is a way to hand that state to the
   coprocessor — the reverse of the emulator problem.

---

## 2. What carries over from microgarbage largely unchanged

The **whole per-frame display engine** ports as-is. None of it depends on the VM
or on the bus being read-only:

- **Boot → copy-kernel-to-WRAM → hand off.** Identical shape (`boot.s`): reset
  stub in the window, spin on a "kernel ready" status bit, byte/`MVN`-copy the
  blob into low WRAM, strobe "booted", `jmp` to WRAM. The FPGA holds the SNES in
  reset until the window is primed — the sd2snes base already gates the bus at
  power-on, so this is free.
- **Vector trampolines + `$FF00–$FFFF` static contract.** Unchanged (`boot.s:78-89`).
  The RAM kernel owns interrupts via `RAMVEC_*`.
- **The virtual-NMI two-state loop** (state A blank+burst at `VIS_END`, state B
  unblank at `top_lb`) with the **force-blank dynamic letterbox** — carries over
  verbatim. It is the owner's target loop almost line-for-line:

  > boot → unload kernel to WRAM → joypad → wait vblank/early-forced-blank → do
  > whatever DMA → enable rendering at the top letterbox line → repeat.

- **The DMA-descriptor staging contract** (`frame_ready` flag + 8-slot list +
  `frame_done` strobe) — keep it. It is compute-model-agnostic: something fills
  the list, the kernel walks it. `frame_dma`'s PPU batch + HDMA ch1-6 arm + slot
  walk (`kernel.s:363-817`) is proven and should be lifted intact.
- **The cycle-budgeted chainer + defer/resume + multi-sub-frame delivery**
  (`calc_bytes_rem`, live-beam budgeting, `K_SLOT_CURSOR` persistence) — keep it
  if HX-421 wants to stream payloads bigger than one blank window (FMV, full 3D
  frames). This is independent of who stages the data.
- **The force-blank-around-DMA rule** and the PPU baseline zeroing at boot
  (`kernel.s:68-97`, window/color-math regs) — keep both; they prevent whole
  classes of latent PPU bugs.
- **The per-scanline H-blank VRAM siphon** (`siphon_isr:1208`) — carry over only if
  a specific mode needs it (the 240×200 60-colour 3D path). Optional.

The single most important carry-over: **the SNES kernel stays a dumb transfer
substrate.** It should have *no* game knowledge. Everything game-specific lives on
the coprocessor side and arrives as staged data.

---

## 3. What changes for the non-virtualized RISC-V

microgarbage grew a large apparatus to let an **interpreted VM guest emit its own
per-frame 65816 at runtime** — the "NMI builder" / full-emitter kernel: a
version-polled code-install path (`COPRO_NMI_VERSION` → copy `COPRO_NMI_CODE` into
`K_NMI_CODE_BASE=$0E00` → repoint `RAMVEC_IRQ`), an ABI jump-table so guest code
can call kernel routines at fixed addresses, and `ISR_MODE`/`FB_MODE` vector-swap
seams (`kernel.s:266-337`, `copro.inc:62-127`, `emitter-kernel.md`). The rationale
was *generality*: any VM app, any frame shape, minimum DMA overhead, without
touching the 65816 kernel.

**HX-421 does not need any of that.** A real soft core that natively produces the
frame data has no reason to ship 65816 code across the bus per app. So:

- **Drop the runtime-code-emission machinery.** No `COPRO_NMI_VERSION` poll, no
  `K_NMI_CODE_BASE` copy, no ABI jump-table, no `ISR_MODE`/`FB_MODE` guest-image
  swaps. The kernel becomes a **single fixed** ISR: the proven state-A/state-B
  loop calling the built-in `frame_dma`. This is a large simplification —
  it removes most of `copro.inc:62-353` and the `@loop` install-poll block.
- **The RISC-V (or M3 compositor) writes the descriptor list + PPU batch + payload
  directly** into the cart-window staging region each frame, in native C on the
  soft core — exactly the job microgarbage's host-side `mg_state_build_frame` did,
  but now on real coprocessor silicon. The kernel just consumes it.
- **Pick one transport and hard-code it.** microgarbage kept three live paths
  (descriptor walk, framebuffer vector-swap, full-emitter ISR) to serve every VM
  demo simultaneously. HX-421 picks the **fixed descriptor-walk** as the v1
  baseline (it already carries FMV + 3D). If the tear-free 240×200 path is
  wanted, add the siphon variant as a second *compile-time* mode, not a runtime
  guest-installed one.
- The **cleaner evolution** the microgarbage notes point at (a free-running
  per-line H-IRQ that reads `OPVCT` and dispatches off a **WRAM action table** the
  copro refills each frame — `emitter-kernel.md:170-179`) is a good HX-421 target:
  it gives dynamic letterbox + siphon + per-scanline effects with no counter
  marching and no guest code emission. Worth adopting once the fixed baseline is
  proven on ares. It fits HX-421 better than microgarbage because the copro can
  cheaply DMA a fresh action table into the window every frame.

Net: HX-421's kernel is the microgarbage display loop **minus the VM trampoline
layer** — smaller, static, and easier to validate on ares.

---

## 4. Joypad delivery — recommendation

**Recommendation: a WRITE mailbox.** The SNES *writes* its joypad state to a small
cart-mapped address range that the FPGA captures into a register / PSRAM slot the
M3 and RISC-V read. Drop the microgarbage read-address-strobe scheme.

### Why HX-421 differs from microgarbage here

The microgarbage address-readback exists for exactly one reason: **its bus has no
write path** (`copro.inc:6-10`). Encoding pad bytes into read addresses is a
workaround for a read-only DTCM window. HX-421 does not have that constraint — the
sd2snes/FXPak base **already decodes writable regions** (it maps SRAM the SNES
writes to, snooped by the FPGA). Once a write path exists, the read-strobe hack is
strictly worse: 8 indexed reads with page-alignment gymnastics to move 8 bytes,
versus a handful of plain stores.

The direction of the problem is also reversed. In bsnes/ares the *host injects*
pads and the SNES relays them back — a round-trip. On **real** HX-421 hardware the
SNES reads its **own** controllers (auto-joypad `$4218/$421A` or a bit-bang) and
already *has* the pad state; the only missing link is handing it to the
coprocessor. A write mailbox is the direct expression of "SNES → coprocessor":
the SNES writes the four pad words to `$xxxx`, the FPGA latches them, done.

### The options considered

1. **Read-address strobe (microgarbage `$7000-$77FF`).** Works on any bus,
   including a pure read-only ROM window. But it's a write emulated as 8 reads,
   needs the page-alignment trick, and only makes sense when you *can't* write.
   **Keep only if** the FPGA core is built to serve a strictly read-only window
   with no write decode (unlikely — sd2snes already has SRAM write). It is the
   safe fallback and the natural **ares-parity** default, since the seam DLL's
   `hx421_cart_read` already models it (`include/hx421.h:66-70`).
2. **Write mailbox (recommended).** Reserve a small writable window (e.g. 8–16
   bytes) in the cart map. The kernel stores the 4 pad words there each frame;
   the FPGA routes those writes to a mailbox register (or a fixed PSRAM address)
   the M3/RISC-V polls. Natural on sd2snes (it's just a tiny SRAM-like region with
   a snoop), it's the cheapest 65816 path, and it cleanly separates "SNES→copro
   control" from "copro→SNES data (the ROM window)".
3. **Auto-joypad + write mailbox (the simplest kernel).** Additionally *enable*
   auto-joypad (`NMITIMEN` bit 0) so the PPU auto-reads pads into `$4218-$421F`
   during early vblank; the kernel just copies those to the write mailbox — no
   bit-bang routine at all. Trade-off: auto-joypad steals the first ~4.5 scanlines
   of vblank, competing with the DMA burst. microgarbage disabled auto-joypad
   *specifically* to reclaim that window and bit-bangs in active display instead
   (`kernel.s:155-169`, README:37-43). **Recommendation:** default to the
   microgarbage discipline (auto-joypad off, bit-bang in active display) so the
   full blank window stays available for DMA, but *write* the result to the
   mailbox instead of strobing it. Revisit auto-joypad only if the bit-bang cost
   ever matters (it's ~150 cycles — it won't).

### Fits the read-only-ROM-window model?

Strictly, the ROM *window* stays read-only — the copro→SNES data channel is
unchanged. The write mailbox is a **separate, tiny writable region** the FPGA
decodes alongside the ROM window (exactly how sd2snes coexists ROM + SRAM). So
"read-only ROM window for data, small write region for control" is the clean
split. If a future core truly wants zero write decode, option 1 remains the
fallback and keeps ares parity for free.

### ares / seam parity

Keep the seam able to model **both**: `hx421_post_joypads` already takes the 4
words host-side; on ares the board can satisfy them either by servicing the
read-strobe addresses (option 1, byte-identical to mgapi today) or by capturing
writes to the mailbox region (option 2). Implement the write-mailbox capture in
the ares board shim so the timing-critical path you validate matches hardware.

---

## 5. Open questions

- **Write-region mechanics on sd2snes_mini.** Confirm the base can decode a small
  writable mailbox distinct from battery SRAM, and whether the FPGA can snoop the
  write into a mailbox register the M3/RISC-V polls without a full SRAM round-trip
  through PSRAM (latency: a pad write the copro reads next frame is fine; same
  frame may need care). Verify at the first `sd2snes_mini` build.
- **Where does game logic run — RISC-V, M3, or both?** The architecture doc lists
  the compositor/RISC-V as the frame-data producer but also gives the M3 "game
  logic." Whichever fills the descriptor list must own the cart-window staging
  region. Pin this down before fixing the mailbox reader.
- **Descriptor walk vs. action-table kernel for v1.** Recommend shipping the fixed
  descriptor-walk first (proven), then evaluating the free-running-H-IRQ +
  WRAM-action-table kernel (`emitter-kernel.md:170-179`) as the cleaner HX-421
  target once ares validation is in place.
- **Real-hardware H-blank siphon ceiling.** microgarbage found the per-line siphon
  overruns the *real* H-blank window where emulators were clean
  (`emitter-kernel.md:212-233`); the master per-line byte ceiling is still owed a
  silicon measurement (`snes/dma_rate_test.s`). If HX-421 wants the tear-free
  240×200 60-colour path, that measurement gates it — but the FXPak's faster PSRAM
  window may change the number. Re-measure on the FXPak.
- **`frame_done` / handshake timing under the FPGA's release model.** microgarbage
  couples the frame handshake to the joypad post (port-7 read gates sub-frame
  advance — `kernel.s:225-233`). With a write mailbox that coupling changes; define
  the HX-421 handshake explicitly (e.g. a dedicated `frame_done` write-strobe or a
  status word), and validate it against ares' cycle-accurate release→NMI-DMA
  timing (the whole reason the project chose ares — `emulation-seam.md:24-31`).
- **Auto-joypad vs. bit-bang final call.** Defaulting to bit-bang (per §4 option 3)
  keeps the DMA window; confirm nothing in the FXPak audio/stream path needs
  auto-joypad's `$4218` latches for its own timing.

---

## Reference — files drawn from

- `microgarbage/snes/boot.s` — reset, handshake, copy-to-WRAM, vectors + trampolines.
- `microgarbage/snes/kernel.s` — virtual-NMI IRQ (`irq:927`), `frame_dma:363`,
  `calc_bytes_rem:833`, `read_joypads:1353`, `joypad_mailbox:1329`,
  `siphon_isr:1208`, boot init `kmain:21`.
- `microgarbage/snes/copro.inc` — the full SNES↔copro window/port/status/descriptor map.
- `microgarbage/snes/README.md` — hardware model, boot lifecycle, per-frame loop, status.
- `microgarbage/docs/emitter-kernel.md` — the VM full-emitter kernel HX-421 drops,
  plus the action-table direction and the real-hardware siphon findings.
- HX-421: `docs/architecture.md`, `docs/emulation-seam.md`, `docs/memory-budget.md`,
  `include/hx421.h`, `README.md`.
