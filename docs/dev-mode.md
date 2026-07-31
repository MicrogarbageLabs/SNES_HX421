# Dev mode — RAM-loaded game binaries, USB drive + terminal

The development loop for **STM32-side game logic**: compile a `.bin` on the PC, drag it onto the cart
(which Windows sees as a USB drive), load it into STM32 RAM from a terminal or the TV, run it, and
print debug back over the same USB — **without ever reflashing the STM32**.

> This supersedes the earlier version of this doc, which targeted a RISC-V soft core in the FPGA and
> reused the usb2snes Put/Get protocol. After the [STM32-runs-game-logic pivot](hx421-stm32-pivot),
> the executable loads into **STM32 RAM** and the transport is a real **MSC drive + CDC terminal**,
> not a file-push protocol.

## The goal: reflash firmware rarely, load games endlessly

| what | where it lives | how often written |
|---|---|---|
| firmware (libc, USB, FatFs, syscall table, arbiter) | internal flash | rarely (firmware updates only) |
| game `.bin` | **STM32 RAM** | every iteration — **zero flash wear** |
| game saves | SD card | as the game chooses |

Internal flash is never touched during the edit-build-run loop. The only flash writes are firmware
updates. That is the entire point.

## The keystone: a frozen, append-only syscall table

Baking libc into the firmware is what keeps a game `.bin` tiny — it carries its own logic plus thin
trampolines, and calls `printf`, `malloc`, `open/read/write/seek/close` **through the firmware**.
Both firmware and game build with the same `arm-none-eabi` toolchain under AAPCS, so the boundary can
carry varargs directly: firmware owns the real `printf`, the game calls through a table slot.

The firmware exports a **table of function pointers at a fixed, published address** (or passes the
table pointer to the game's entry in a known register). The game's `crt0` captures it; its libc shims
are one indirect call each.

**The frozen contract is the table LAYOUT — index -> signature — not the implementations.**

- Fix a bug in `printf`: every game benefits, no rebuild.
- Add a function: **append at the end**. Old binaries keep working.
- Reorder a slot or change a signature: every compiled game silently breaks.

This is the microgarbage `SYS_*` ecall discipline. The [[guest-host-enum-abi]] lesson is mandatory
here: **every struct crossing the boundary uses sized types (`uint32_t`, not bare `enum`/`int`)** --
the mode7 black-screen saga was exactly this class of bug, and a game binary compiled against a
drifted struct layout is far harder to debug than a guest ELF.

First-cut table (append-only; reserve generous room per group so additions never force a renumber):

```
  index  group        signature
  ----------------------------------------------------------------------
  0x00   abi_version() -> u32           /* {major<<16|minor}; game aborts on major mismatch */
  0x01   yield()                        /* cooperative: hand a slice back to firmware (audio, USB) */
  0x02   abort(u32 code)                /* fatal; firmware shows the error screen */

  0x10   print(const char*)             /* raw, already-formatted */
  0x11   printf(const char* fmt, ...)   /* varargs across AAPCS; firmware owns the formatter */
  0x12   term_ctrl(...)                 /* cursor, color, clear — thin wrappers over ANSI emit */

  0x20   open(const char* path, int mode) -> handle
  0x21   read(handle, void* dst, u32 n) -> u32
  0x22   write(handle, const void* src, u32 n) -> u32
  0x23   seek(handle, u32 off) -> u32
  0x24   close(handle)
  0x25   readdir(...)                   /* SD browse from the game */

  0x30   malloc(u32) / free(void*)      /* backed by the game region's heap, NOT firmware's */
  0x40   copro_* ...                    /* the mg_* coprocessor API: stage frame, mixer, collision */
  0x50   input_read(u32 pad) -> u32     /* joypads, mouse */
```

Groups are spaced (0x10 apart) so a new file op can land at 0x26 without disturbing 0x30. `malloc`
hands out the **game region's** heap, never firmware's — the two allocators must not share an arena,
or a game leak corrupts the resident firmware.

## Memory map: identical across dev and shipping firmware

The dev firmware is fatter than a shipping one (it carries USB + MSC + FatFs + libc + terminal), so
its private RAM working set is larger and the game region is *smaller* during dev. Resolve this by
**freezing the game-visible map and the syscall table identically in both builds** — a `.bin` that
fits and runs on dev runs unchanged on ship. Only firmware internals differ.

```
  0x20000000  +---------------------------+
              |  GAME REGION              |  code + rodata + data + bss + heap + stack,
              |  (frozen size, e.g. 44K   |  loaded from SD, executed in place. Cortex-M4
              |   on a 64K part / 64K on   |  runs from SRAM at 0-wait — as fast as flash.
              |   a 96K part)             |
              +---------------------------+
              |  FIRMWARE WORKING SET     |  arbiter control, SD sector buffer, USB/FatFs
              |  (private)                |  state, IPC with FPGA, firmware stack.
  end-of-RAM  +---------------------------+  Stream DATA is in PSRAM, not here.
```

The stream arbiter's bulk data (FMV frames, audio PCM rings) lives in **PSRAM** with FPGA SD->PSRAM
DMA offload (see [[hardware-budget]] / `docs/stream-arbiter.md`), so the firmware's *concurrent* SRAM
cost is control structures and one SD sector buffer — ~8-16K, not the streams themselves. That is
what leaves room for a large game region even on the 64K part. Pin the game size to fit the **dev**
firmware and freeze it; then it is portable to any firmware build. (See the STM32F401 64K-vs-96K
question in [[hardware-budget]] — the RAM part only sets how generous the frozen region can be.)

## USB: composite CDC + MSC

One composite device gives both the drag-drop drive and the live terminal.

- **MSC** — Windows mounts the SD as a removable drive. Drag a `.bin` (or updated FPGA cores, assets)
  straight on.
- **CDC-ACM** — a virtual serial port for the debug terminal and command channel.

**F401 endpoint budget is the thing to verify.** CDC needs 3 endpoints (interrupt-IN, bulk-IN,
bulk-OUT); MSC needs 2 (bulk-IN, bulk-OUT). The F401's USB is **Full-Speed only (~1 MB/s)** with a
limited OTG_FS endpoint count and a shared FIFO. A game `.bin` is tens of KB — instant. Bulk asset
copies are slow but fine for dev. Confirm CDC+MSC fits the F401's IN/OUT endpoint count before
committing; it is common on an F407, which does not prove it on an F401.

## SD ownership: host XOR firmware, never both

Exposing the raw SD to Windows over MSC *and* letting FatFs write it is the classic FAT-corruption
trap — Windows caches directory/FAT sectors and the firmware writes underneath. The interlock:

```
  +-- PLAY --------------------------+        +-- XFER --------------------------+
  | game runs in RAM                 |  enter | play suspended                   |
  | firmware owns SD (FatFs)         | -----> | MSC presents the SD to Windows   |
  | MSC presents NO media            |        | FatFs unmounted                  |
  | CDC terminal live (debug)        | <----- | CDC terminal live (for the cmd)  |
  +----------------------------------+  exit  +----------------------------------+
     ^                                            |
     |  on exit: firmware RE-MOUNTS FatFs from    |  auto-lock on unplug / host unmount
     +-- scratch (re-reads the FAT) --------------+
```

- MSC presents **media-present only in XFER**, so Windows cannot touch the card during play.
- On exit, firmware **re-mounts FatFs fresh** and treats the card as dirty — Windows write-back cache
  means "unmounted" is not "flushed," so any pre-XFER in-RAM FAT state is void.
- Residual risk: a yank mid-write. Mitigation is to eject cleanly before resuming; the firmware
  validates the FAT on re-acquire and shows an error rather than trusting it.

Alternative if the eject dance proves annoying: a **firmware-mediated MSC volume** (firmware owns a
small FAT the host reads/writes, then copies accepted files to SD). Corruption-proof, but more
firmware and RAM-limited. Start with passthrough; it gives Windows full access to cores and assets,
not just a staging area.

## The terminal is nearly free

PuTTY with UTF-8 box-drawing and 256-color (`\e[38;5;Nm`) is pure escape sequences the firmware emits
over the CDC data endpoint — no USB work beyond the byte pipe. The microgarbage OS TUI is the
precedent and that renderer is portable. Commands (`load <file>`, `ls`, `xfer`, `run`, `reset`) come
back on CDC-OUT; the same actions are reachable on the TV with the controller, using
`snes/textmode.inc` for the on-screen menu and error screens.

## Debug print must never stall the game

`printf` -> a TX ring drained on USB IN / SOF. If the host is not reading (PuTTY closed), **drop, do
not block** — a game frame must never hitch waiting on a debug byte. Same rule as the WASAPI audio
path: the producer never waits on the consumer.

## PC parity: run the game in the emulator first

[[armulator-project]] is a Cortex-M emulator. A game `.bin` can run in armulator against a **mocked
syscall table** on the PC, with `printf` to stdout and the coprocessor calls stubbed or bridged to
the existing `hx421.dll` — the same "PC parity is the debugging lifeline" pattern the DLL/bsnes loop
gives the SNES side. Edit -> build -> run in armulator -> *then* drag to hardware. Most iteration
never touches the cart.

## Mode state machine (summary)

```
  BOOT -> stub ROM loads the resident kernel, firmware waits for a game
   |
   +- (TV: pick .bin, or CDC: `run <file>`) -> load into GAME REGION -> jump -> PLAY
   |                                                                             |
   PLAY - (TV button / CDC `xfer`) -> XFER - (CDC `run` / eject+resume) ---------+
   |
   +- fatal (bad .bin, SD error, abort()) -> ERROR SCREEN (textmode.inc) + CDC message
```

## Build order

1. **Syscall table contract** — freeze the layout on paper (this doc), pick the fixed export address,
   write the game-side `crt0` + trampoline stubs and a firmware-side dispatch stub. Nothing else can
   be built stably until this is fixed.
2. **CDC terminal + non-blocking debug print** — the smallest useful loop: firmware boots, prints a
   banner to PuTTY, echoes commands. No game loading yet.
3. **RAM loader** — `run <file>`: read a `.bin` from SD into the game region, jump to it, catch its
   `abort()`. Prove it with a trivial game that just `printf`s and reads the pad.
4. **MSC drive + the SD-ownership interlock** — drag-drop replaces manual SD swaps.
5. **Freeze the memory map** against the dev firmware; document the region base/size games link to.
6. **Stream arbiter integration** — the resident audio/FMV path running underneath a loaded game.

## Relationship to the audio player

Dev mode and the audio player share the plumbing: USB loads files, the mixer plays them, the FFT feeds
the SNES display. The player is dev-mode plus the spectrum/playlist UI.
