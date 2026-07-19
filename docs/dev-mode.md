# Dev mode (USB terminal + file transfer)

A firmware mode for development and as the audio-player/workstation: push SFX/WAV over USB, watch a
debug terminal, and show status on the SNES. Not for running a game (SD/FAT is busy during transfer).

## USB — mostly reuse

The FXPak USB is MCU-connected and already speaks the **usb2snes** protocol with FAT file ops
(List/Get/Put) — that's how host tools push files to the SD today. So:

- **Debug terminal:** CDC-ACM virtual serial. STM32 USB device supports CDC. Check whether the mk3
  firmware exposes CDC or only a vendor class; add a CDC interface (composite device if keeping
  usb2snes) for `printf`-style debug.
- **File transfer:** reuse the existing FAT write path (usb2snes Put/Get) — "FAT32 client" = the MCU's
  existing FAT driver exposed over USB. Queue incoming files; write to the SD's SFX/WAV folders.
- **Verify:** the Pro's USB port is wired to the MCU USB *data* lines (strong prior yes — usb2snes
  supports FXPak Pro). Confirm on the mk3 firmware/schematic.

## SNES-side status (during SD-busy transfer)

- No game/stream runs while the SD is busy, but the FPGA keeps a small **status window** (mailbox
  region in the ROM-window space) that the 65816 polls.
- A **WRAM-resident 65816 status program** (loaded once, like the boot-banner / dead-simple-kernel
  pattern) renders: transfer progress bar, current file, queued files, byte counts.
- Status fields the MCU writes → FPGA window → 65816 reads: `{state, cur_name, cur_pct, queue_len,
  queue_names[], last_error}`.

## M3 as the RISC-V dev host (feasibility: ample headroom)

The USB is on the M3. Beyond file transfer, the M3 can host a **serial debug shell** that (a) loads
new executables into the FPGA's RISC-V soft core, (b) gives FAT file access (no ramdisk, but useful),
and (c) forwards RISC-V debug prints (a `string_push` on the soft core → shared FIFO → the M3 drains it
to USB CDC). **Horsepower is not the constraint:** the only *steady* M3 load is the audio mixer
(~20-25% at 100 MHz); the shell/FAT/exec-load/print are all event-driven and I/O-bound (USB IRQ/DMA, SD
controller, the M3↔FPGA link), costing CPU only in short bursts. An exec load is a one-shot burst with
the RISC-V halted (no contention); debug print is a memcpy + USB write.

The care is priority + shared resources, not cycles:
- **Audio stays real-time** — the DAC fill is IRQ/DMA-driven off the mixer's lookahead rings, so a
  blocking shell/SD op never starves it; run the shell at lower priority.
- **Share SD with streaming** — shell file ops + music/FMV streams both hit FAT; route through the
  stream arbiter with streams prioritized; chunk shell transfers.
- **Share the M3↔FPGA link** — exec-load (RISC-V halted) + tiny debug strings; only load while
  paused/in the shell so it never hitches gameplay frames.

Payoff: a **JTAG-free dev loop for the RISC-V game logic** — edit on PC → cross-compile → to SD →
shell `load` into the soft core → runs → `printf` back over the same USB terminal. The realized
rvcc/dynamic-loading direction on real silicon.

## Relationship to the audio player

Dev mode and the audio player share the plumbing: USB loads files, the mixer plays them, the FFT feeds
the SNES display. The player is dev-mode + the spectrum/playlist UI (M4).
