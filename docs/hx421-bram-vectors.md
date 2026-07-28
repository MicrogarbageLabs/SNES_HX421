# HX-421 BRAM-served 65816 vectors

The FPGA serves the 65816 vector region from an on-fabric 32-byte register file
(`vec_mem`, `$FFE0-$FFFF` offsets 0-31) instead of PSRAM. This lets the WRAM engine
install its own NMI/IRQ handlers (point vectors at WRAM) and takes the boot path off
the PSRAM bus (no mixer contention → no boot-time tilemap garbling).

Gated by `HX421_BRAM_VECTORS` in `fpga/build/h2_base/main.v`.

## Register map / contract (FPGA **and** bsnes/mgapi must implement identically)

`vec_mem[0..31]` maps to CPU vector bytes `$FFE0..$FFFF` (offset = addr - `$FFE0`).
Power-up defaults: all `0` except the reset vector `$FFFC/$FFFD` = `$00/$80` (→ `$8000`).

| Access | SNES address | Effect |
|---|---|---|
| **Serve** (read) | `$00:FFFC-$FFFD` | returns `vec_mem[0x1C..0x1D]` (reset) — always |
| **Serve** (read) | `$00:FFE0-$FFFF` | returns `vec_mem[addr-$FFE0]` — **only when widen=1** (NMI/IRQ/etc.) |
| **Write** | `$3F:F020 + n` (n=0..31) | `vec_mem[n] <= data byte` |
| **Readback** (read) | `$3F:F040 + n` (n=0..31) | returns `vec_mem[n]` (verify writes) |
| **Widen enable** (write) | `$3F:F060` | `widen <= data bit0` (0 = reset-only serve; 1 = whole region) |

Notes:
- **Reset-only serve, for now.** Only `$FFFC-$FFFD` is served from `vec_mem`; NMI/IRQ
  (`$FFEA/$FFEE` native, `$FFFA/$FFFE` emu) still come from the ROM. Serving 0 for
  NMI/IRQ crashed the sd2snes loader (it runs with interrupts on). Widening the serve
  to NMI/IRQ is safe only once `vec_mem` holds valid handlers (write-path / MCU-init).
- All windows are in the `$3F:F0xx` fabric region, decoded like the `HX_SIG`/diagnostic
  reads (`~ROMSEL & bank & addr-match`), served ahead of the normal ROM path.
- Writes are captured on `SNES_WR_end` from `BUS_DATA` (the latched write byte).

## bsnes / mgapi side (the "easy change")
Mirror the table above in the custom mapper's read/write handlers:
- read `$00:FFFC/$FFFD` → the vec_mem[reset] bytes (so the CPU fetches the served reset)
- write `$3F:F020+n` → store into a 32-byte vec_mem array
- read `$3F:F040+n` → return vec_mem[n]
Same power-up defaults. That makes `h6_vecwrite.sfc` show GREEN in bsnes just like on
hardware, and keeps the PC-dev path bit-for-bit with the FXPak.

## Status
- **Step 1 — reset serve: HARDWARE-CONFIRMED** (`h6_bramvec.sfc` → solid green; the SNES
  booted to `$8000` from the fabric, not the ROM's `$FFFC`).
- **Step 2a — write window: HARDWARE-CONFIRMED** (`h6_vecwrite.sfc` → green: the SNES
  wrote `vec_mem[0]=$AB` via `$3F:F020` and read it back via `$3F:F040`).
- **Step 2b — widen + custom NMI handler: built** (`h6_vecnmi.sfc` installs an NMI
  handler into `vec_mem[$FFEA]`, sets widen, enables NMI → the handler cycles the
  backdrop color each vblank, fetched from the fabric vector). *Pending bench.*
- **Step 3** — MCU copies each ROM's real vectors into `vec_mem` at load (firmware), so
  the reset default is per-ROM and the whole thing is transparent to any ROM.
