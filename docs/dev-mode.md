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

### Later (not yet): more of the runtime moves into firmware

The same size argument that bakes in libc applies to everything a game repeatedly needs. As the
platform settles, the firmware absorbs — each as a new append-only table group, so old binaries are
unaffected:

- **Game API** — the locked `mg_*` cart-side surface ([[game-api-decisions]], `docs/game-api.md`):
  frame staging, sprites, palettes, input. The game calls it; the firmware and PSRAM implement it.
- **Asset loading** — higher-level than raw `open/read`: load-and-decode of packed assets (the FMV
  container, tile/CHR packs, the raycaster strip bake) straight into PSRAM, so a game hands over an
  asset id, not a decode loop.
- **FPGA HAL** — the register/DMA/mailbox choreography for the FPGA (staging chains, mixer channel
  setup, collision registry, the BRAM-vector window) behind named calls, so a game never touches a
  raw FPGA register and firmware can retune the hardware path without recompiling games.

Each move shrinks the game `.bin` and widens the frozen ABI. **Design each group before it ships** —
adding a slot is free, changing one after games bind to it is not. Deliberately deferred: get the
loader, terminal and a trivial game working against the minimal table first, then grow it.

## Frozen map (firmware/dev/hx421_memmap.h) — F401xC 64 KB

```
  0x20000000  +------------------------------+
              |  FIRMWARE working set  32 KB |  USB, terminal, FatFs, streaming
              |                              |  CONTROL (data is in PSRAM), baked-
              |                              |  libc state, stack.
  0x20008000  +------------------------------+
              |  GAME REGION           32 KB |  code+rodata+data+bss+heap+stack,
              |  (load == run, in place)     |  loaded from SD.  HX421_GAME_BASE.
  0x2000FFFF  +------------------------------+
```

`hx421_memmap.h` is the single source of truth (`HX421_GAME_BASE`/`_SIZE`/`_STACK`); the loader, the
firmware link and the game link all derive from it, and `run-qemu-loader.sh` asserts the game linked
at `HX421_GAME_BASE` — so the three cannot drift, and the QEMU loader test validates the *production*
map, not an arbitrary one.

**Why 32/32 is comfortable, not tight.** With rich services (libc, and later the actor library, FMV
routines, input, asset loading, 2D/3D hooks) resident ONCE in firmware and reached through the
syscall table, the game binary stays tiny — measured ~100 bytes of overhead where a self-contained
build is 7–31 KB. A game with little code fits a 16 KB/16 KB code/data split easily. So the game gets
a roomy 32 KB and the firmware the other 32 KB, on both sides of a budget that stream data (in PSRAM)
never touches.

**A later F411 (128 KB) just extends the region** — raise `HX421_GAME_SIZE`. Growing it is backward
compatible: a game built for 32 KB runs unchanged in a larger region; only shrinking below what a
game was built against would break it.

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

## Reality check: most of the transport is ALREADY in the stock firmware

Building the stock `firmware.stm` (`config-mk3-stm32`) and reading its USB code settles what actually
has to be written vs. what's already there:

- **CDC-ACM virtual serial: present.** `usbdesc.c` declares `bDeviceClass = COMMUNICATIONS`, two
  interfaces (control + data), bulk IN/OUT + a notification endpoint. It's the transport we designed,
  already working — usb2snes runs over it.
- **File transfer over USB: present.** The usb2snes protocol (`usbinterface.c`) has `GET`/`PUT`/`VGET`/
  `VPUT`/`LS`/`BOOT`, and `PUT` does `f_write()` straight to the SD. So **pushing a `game.hxg` to the
  cart's SD already works** through existing PC tools (usb2snes / QUsb2Snes / SNI) — no MSC, no new
  firmware.
- **libc: present.** The firmware links newlib; `printf`/stdio/`malloc` are in the image already.
- **FatFs, SD driver, boot trigger: present** (`ff.c`, `sdnative.c`, the `BOOT` opcode).

So the **MSC drag-drop drive and the PuTTY terminal are UX polish, not requirements.** The functional
dev loop needs only three *additions* to the stock firmware — the syscall TABLE (its implementations
already exist), the RAM loader (QEMU-proven), and the game region (linker) — plus a trigger, which can
reuse the existing `BOOT` opcode.

```
  stock firmware  +  syscall table  +  loader  +  game region
                                              |
     usb2snes PUT (existing PC tool) pushes game.hxg to SD
                                              |
     trigger (BOOT opcode / small custom op) -> loader copies to region, jumps
```

Debug output rides the SNES screen (`snes/textmode.inc`) or the CDC. MSC and a dedicated PuTTY
terminal are **phase 2**, added once the core loop runs.

## Populating the table from the firmware's existing services

The syscall table is the one genuinely new integration, and it is almost all *wiring* — the
implementations behind each slot already exist in the stock firmware. Sketch of where each group binds:

```
  slot          firmware backing (stock sd2snes)                    new code
  ----------------------------------------------------------------------------------------
  abi_version   a constant                                          ~0
  yield         return to the firmware main loop / USB+SD service   thin
  sys_abort     show error on SNES screen (textmode.inc) + halt     thin
  print/printf  newlib vsnprintf -> a sink (SNES screen or CDC)     thin adapter
  term_ctrl     ANSI emit over the same sink                        thin
  open/read/    FatFs f_open/f_read/f_write/f_lseek/f_close on a    handle table +
   write/seek/    game-scoped directory                             path sandboxing
   close
  malloc/free   a game-region allocator (NOT firmware's newlib      a small arena
                 heap — see the arena rule below)                    allocator
  input_read    the firmware's existing joypad read path            thin
  copro_*       the mg_* coprocessor calls (staging, mixer, ...)    later table group
```

Two rules the wiring must hold to:

- **`malloc` hands out the GAME region's heap, never firmware's.** The game .bin runs in the frozen
  region; its heap must be an arena carved from that region, so a game leak or overrun can't corrupt
  the resident firmware's newlib heap. This means the table's `mem_alloc` is a *small game-scoped
  allocator*, not a forward to firmware `malloc`.
- **File paths are sandboxed.** `open` prefixes a game directory (e.g. `/sd2snes/hx421/<game>/`) so a
  loaded game can't read or clobber firmware/menu/core files on the same card. The firmware owns the
  policy; the game sees only its own tree.

Everything else is a one-line adapter from the table slot to the function the firmware already has.
That is the whole point of baking libc in: the code exists once, resident, and the table is the seam.

**Whole chain proven on Thumb** (`firmware/dev/qemu/run-qemu-full.sh`): the capstone QEMU test builds
the table with the *real* `hx421_sys_build()` (not a mock), the loader places a separately-linked game
at the frozen region base, and the loaded game runs through that table — printing its input
(`pad0=0000C0DE`), round-tripping a file whose path the sandbox prefixed (`/sd2snes/hx421/mygame/
save.dat`), and allocating from the game-region arena (verified by the arena's high-water mark). So
`sys_build -> loader -> game -> arena + sandbox` composes correctly on real target execution — the
exact firmware path minus the peripheral backends, which are the thin adapters above.

## USB: composite CDC + MSC (phase 2)

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

**Built** (`firmware/dev/hx421_term.{c,h}`, host-tested in `tools/hx421_term_test.c`, 21 checks). The
terminal is **transport-agnostic**: it owns the TX ring, the RX line editor, command tokenizing, and
the ANSI helpers, and knows nothing about USB. Bytes leave through a **sink callback** and arrive via
`hx421_term_rx(byte)`. Two properties the host test pins down:

- **Non-blocking drop.** A full ring drops and counts (`dropped`), never blocks the caller. The bytes
  it keeps are the first ones; the overflow is surfaced, not silent.
- **Back-pressure.** The sink returns how many bytes it accepted, so a busy 64-byte FS endpoint that
  NAKs leaves the remainder queued for the next drain rather than losing it.

The only target-only piece is the **CDC glue shim**: a `sink` that writes the CDC-ACM IN endpoint,
and an interrupt/poll that feeds the OUT endpoint's bytes to `hx421_term_rx`. That shim needs the
STM32 USB device stack and is the one part not host-testable — everything with logic is above it,
tested on the host exactly like `hx421_stream`.

## Testing without the cart: three tiers

There is no bespoke emulator for this. (An earlier draft of this doc named "armulator" as if it
existed — it was only a brainstorm, never begun.) Three real tiers cover progressively more, and the
first two need no emulator at all:

1. **Host (x86-64) tests — algorithm validation, instant.** The `make runtimetest` suites
   (`hx421_syscall_test`, `hx421_term_test`, …). Fast, but pointers are 8 bytes here, so struct
   layout and the ABI offset asserts are only proven for the *host*, not the target.
2. **`arm-none-eabi-gcc` cross-compile — the REAL 32-bit ARM ABI, still no emulator.** Compiling the
   firmware/dev modules for `cortex-m4` makes the append-only `_Static_assert`s evaluate against
   `sizeof(void*) == 4`: `Hx421Sys` is exactly 56 bytes (14 slots), the layout the cart actually
   sees. This is the tier the host test cannot give, and it costs one compile:
   ```
   arm-none-eabi-gcc -c -Os -std=c11 -mcpu=cortex-m4 -mthumb -ffreestanding \
     -Ifirmware/dev firmware/dev/hx421_gamert.c firmware/dev/hx421_term.c
   ```
   (On this machine the toolchain is `C:\msys64\mingw64\bin\arm-none-eabi-gcc`; put `mingw64\bin` on
   PATH or `cc1` fails silently.)
3. **QEMU — execution of real Thumb code. LIVE.** `qemu-system-arm 11.0.0` on `netduinoplus2` (an
   F405, Cortex-M4 — no F401 model exists, but the CPU/ABI match) runs the boundary as actual target
   code. `firmware/dev/qemu/` is a self-contained bare-metal harness (own vector table, startup,
   semihosting for console + exit, a mini `printf` — no newlib): a game binds the table and runs
   through the firmware's indirect calls at the real 4-byte-pointer ABI, and prints `QEMU PASS`.
   Build + run with `firmware/dev/qemu/run-qemu.sh`. This is the tier that will validate the loader's
   *jump into the game region* before hardware.
   **Scope it honestly:** QEMU models the CPU, NVIC and memory — it does **not** emulate the STM32
   OTG_FS USB device, the SD peripheral, the FPGA or any real timing. USB CDC/MSC, the SD-ownership
   interlock, and everything on the FPGA/PSRAM side are **hardware-only** (the FXPak, screen/ear as
   the diagnostic).

So: edit -> host test (logic) -> arm cross-compile (target ABI) -> QEMU (execution) -> FXPak (USB,
SD, FPGA). The first three run on this machine with no cart; only the last needs hardware.

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
   `abort()`. **Built** (`firmware/dev/hx421_loader.{c,h}`, packer `tools/hx421_mkhxg.c`). A game file
   is `[Hx421GameHeader][image]`, the image linked to run AT the region base (load == run, so only
   bss needs clearing, no relocation). Split so the logic is host-tested (`hx421_loader_test.c`,
   16 checks: validation, byte-exact placement, bss zero, and every reject — bad magic, wrong ABI,
   short file, entry out of range, over-region, and an overflow-safe bounds check) and the
   **load-and-jump is proven under QEMU** (`run-qemu-loader.sh`): a game linked separately at the
   region base, packed by `mkhxg`, embedded in the firmware, copied into the region and jumped to —
   the loaded code runs through the table and prints `QEMU PASS`. That is the whole dev loop in
   miniature.
4. **Firmware integration + build proof** — **Done.** The portable modules (`hx421_loader.c`,
   `hx421_sysimpl.c`) plus the firmware glue (`hx421_fw.c`: backend adapters over FatFs +
   `uart_putc`, and `hx421_fw_run_game()` tying `sys_build` → streaming loader together) build
   *inside the sd2snes fork* and link into `firmware.stm`. `firmware/dev/build-fw.sh` is the
   integration step: it copies our sources into the submodule's `src/`, registers them on the
   Makefile `SRC` list idempotently, and builds `config-mk3-stm32`. This is the source-of-truth
   pattern used for bsnes/Mgapi — nothing is committed into the submodule. Verified: all three
   objects compile under the fork's strict `-Werror` (incl. `-Werror=misleading-indentation`)
   against the real `ff.h`/uart headers, and every firmware symbol they need
   (`f_open`/`f_read`/`f_write`/`f_lseek`/`f_close`, `uart_putc`, `strlen`) resolves in the
   firmware ELF.
5. **Load trigger + region carve** — **Done.** The CLI gains a `run <file.hxg>` command
   (`cmd_run` → `hx421_fw_run_game`) so a game loads over the serial console, and the linker RAM is
   capped so the game region is genuinely free (see the map below). With the trigger referencing the
   chain, `--gc-sections` keeps it: `cmd_run`, `hx421_fw_run_game`, `hx421_loader_run_stream`,
   `hx421_sys_build` are all `T` in the final ELF. The trigger (`cli.c`), the carve
   (`stm32f401.ld`), and the Makefile `SRC` registration all live in
   `firmware/dev/sd2snes-devmode.patch`; `build-fw.sh` copies our sources into the submodule and
   applies the patch idempotently, then builds `config-mk3-stm32`. Source-of-truth pattern (as for
   bsnes/Mgapi) — nothing is committed into the submodule. **This firmware is flashable.** Open
   caveat: `fw_yield` is still a stub, so USB/CDC is not serviced *during* a game's run — use games
   that return for now; a cooperative yield to the USB service loop is the next refinement.
6. **MSC drive + the SD-ownership interlock** — drag-drop replaces manual SD swaps.
7. **Stream arbiter integration** — the resident audio/FMV path running underneath a loaded game.

## Memory map (measured against the real firmware, not the paper map)

The frozen map (`hx421_memmap.h`) puts the game region at `0x20008000–0x2000FFFF` — the top 32K of
the F401's 64K SRAM. Reading the *built* ELF (`arm-none-eabi-objdump -h`, not the linker script)
shows this is clean; an earlier worry that `.ahbram` sat inside the region was a **misremembered
address**. The real layout:

| section | range | note |
|---|---|---|
| `.data`   | `0x20000070–0x20000474` | |
| `.ahbram` | `0x20000474–0x20002794` | USB OTG buffers — **low**, well below the region |
| `.bss`    | `0x20002794–0x2000577C` | grows to ~`0x2000577C` once `hx421_fw`'s FIL pool links in |
| heap      | grows **up** from `__bss_end__` | only transient FatFs LFN buffers (~512 B), freed at once |
| stack     | grows **down** from `__stack` | |

All static data ends ~`0x2000577C`, so the region is clear of `.data`/`.bss`/`.ahbram`. The only
things that would grow into it are the firmware heap and stack — so the carve is one line:
`stm32f401.ld` RAM `LENGTH` `0x0ff90 → 0x07f90`, putting `__stack` (and the heap ceiling) at
`0x20008000`. That leaves the firmware ~10.4K of heap+stack (static-end → `0x20008000`), ample given
its ~512 B transient heap and few-KB stack. During a game's run the firmware isn't allocating (it
jumped to the game, which uses its own arena), so nothing contends for the region.

## Running a game on hardware

The `run` command lives on the **interactive CLI**, which on the mk3 STM32 build is the physical
debug UART — **USART2, PA2 (TX) / PA3 (RX), 8N1, 921600 baud** (`stm32f4xx/uart.c`,
`config-mk3-stm32`). It is *not* the USB port: USB runs the binary usb2snes protocol (file transfer),
while the text CLI (`ls`, `put`, `run`) is UART-only. So you need a 3.3 V TTL USB-serial adapter on
PA2/PA3/GND. From the SNES/menu side the dev firmware looks exactly like a stock FXPak — boots to the
menu, runs ROMs — because the dev-mode addition is entirely on the serial console. (Verified on
hardware: the carved firmware boots to menu and runs an SNES ROM normally, so capping RAM to 32K did
not disturb stock operation.)

Build a game and run it:

1. `sh firmware/dev/build-game.sh` → `firmware/dev/game/build/hello.hxg`. It links the game at the
   frozen region base, **asserts `game_main` lands at `HX421_GAME_BASE`** (a memmap/link drift
   check), and packs it with the real `.bss` size (`_ebss - _sbss`) so the loader zeros bss.
2. Copy `hello.hxg` to the SD card.
3. On the serial console: `run hello.hxg`. It loads into the region, jumps, and prints a banner,
   the arena-malloc result, `pad0` (0 until the input mailbox lands), and a guarded file test — all
   through the syscall table, so seeing it proves the whole chain end-to-end.

`hello.c` is a template: it uses only `sys_*` (no libc), and `game_main` is forced first via
`__attribute__((section(".text.game_main")))`. Point `build-game.sh` at another `.c` to build your
own. Caveat (until `fw_yield` is real): a game that never returns will block the CLI — start with
games that return.

## Relationship to the audio player

Dev mode and the audio player share the plumbing: USB loads files, the mixer plays them, the FFT feeds
the SNES display. The player is dev-mode plus the spectrum/playlist UI.
