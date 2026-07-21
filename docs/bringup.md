# Hardware bring-up on the FXPak Pro

First contact between our build and real silicon. The organising principle: **the first experiment
must not be able to brick the device**, and it must produce a legible answer even if everything
about our bitstream is wrong.

## The safe path: borrow a core slot, flash nothing

The FXPak already loads a **per-game FPGA core from the SD card**. That is how it does GSU, SA-1,
CX4 and the rest: `smc.c` inspects the ROM header, sets `romprops.fpga_conf` to a filename, and
`memory.c` calls `fpga_pgm()` on it before loading the ROM.

```c
/* third_party/sd2snes/src/smc.c — OBC1 LoROM */
else if (header->map == 0x30 && header->carttype == 0x25) {
    props->has_obc1 = 1;
    props->fpga_conf = FPGA_OBC1;     /* "/sd2snes/fpga_obc1.bi3" */
}
```

So a custom core needs **no firmware modification and no firmware flashing**. Put our bitstream at
`/sd2snes/fpga_obc1.bi3` and load a ROM whose header declares OBC1, and the MCU programs our
bitstream into the FPGA.

**Recovery is restoring one file.** The MCU firmware is never written, the FPGA is volatile and
reconfigured from SD on every boot, and the stock `fpga_base` is untouched. Worst case is a black
screen and a power cycle.

OBC1 is the slot to borrow because Metal Combat is its only game. CX4 (Mega Man X2/X3) and GSU
(Star Fox, Yoshi's Island) are worth keeping intact.

## `.bi3` is RLE-compressed — renaming an `.rbf` will not work

The MCU streams the bitstream through `rle_file_getc()`, so a `.bi3` is an RLE container, not a raw
`.rbf`. `tools/hx421_rlepack.c` packs one:

```
gcc -O2 -o rlepack tools/hx421_rlepack.c
./rlepack fpga/build/baseline_mini/output_files/main.rbf fpga_obc1.bi3
```

It **decodes its own output and compares against the input before writing**, because the failure
mode on hardware is `led_panic(LED_PANIC_FPGA_NOCONF)` — a blinking LED and nothing else. Verify
where the failure is legible.

Cross-validated against upstream: decoded with sd2snes' own `utils/derle`, our output reproduces the
input byte-for-byte (153544 B, matching SHA-256). Upstream's `utils/rle` encoder emits **one spurious
trailing byte** (an EOF off-by-one in `getrunlength`); harmless in practice, since bytes after `DONE`
are ignored, but it means ours is the more faithful of the two and a byte-identical comparison
against upstream is the wrong check.

## What failure looks like

`fpga_pgm` panics into a 5 Hz blink of the three LEDs, the pattern encoding the stage that failed
(`src/stm32f4xx/led.c`, bit 2 = ready, bit 1 = read, bit 0 = write):

| code | LEDs | meaning |
|---|---|---|
| 1 | write | `PROG_B` stuck high — wiring/board, not our bitstream |
| 2 | read | no `INIT_B` response — FPGA not entering configuration |
| 3 | read+write | `DONE` stuck high |
| 4 | ready | **failed to configure after 10 tries — our bitstream is wrong** |

**No blink pattern means the FPGA accepted our bitstream.** That is the milestone, and it is
observable without a UART.

## Milestone H1 — "the device accepts our build"

Deliberately does NOT require our core to do anything useful. `baseline_mini` is the minimal base
(410 LE, 3% of the EP4CE15), and it very likely will not map a ROM correctly — the SNES may show
nothing at all. That is fine. The question H1 answers is only:

> does a bitstream built by our Quartus flow, packed by our packer, configure on real hardware?

Everything downstream depends on that and nothing else can be trusted until it is settled.

1. Back up `/sd2snes/fpga_obc1.bi3` off the card.
2. Copy our packed bitstream in its place.
3. Build a ROM with `map = 0x30`, `carttype = 0x25` in its header.
4. Load it. Watch the LEDs.

Result: no panic blink = configured. Panic code 4 = our bitstream is rejected. Any other code points
at the board or the load sequence rather than at us.

## Unknowns to settle before touching hardware

These are questions about the specific device, not the design, and guessing at them is how a
low-risk experiment becomes a bricked cart:

- **Which firmware fork.** This tree vendors `mrehkopf/sd2snes`. The FXPak Pro ships a different
  distribution; filenames, the `.bi3` extension and the core-selection logic may all differ. Confirm
  by listing `/sd2snes/` on the actual SD card.
- **Whether the mk3 STM32 config matches.** `config-mk3-stm32` says STM32F401 + `.bi3`. A board
  running the LPC1756 (`config-mk3`) uses the same extension but different firmware entirely.
- **Whether a UART is reachable.** `fpga_pgm` `printf`s a running commentary (`P`, `p`, `C`, `c`,
  byte counts). If the debug UART is accessible, H1 becomes far more diagnostic than LED blinks.

## After H1

- **H2** — a core that does something observable: hold a known value at a cart address the 65816 can
  read, proving our logic runs and the bus decode works.
- **H3** — re-measure the force-blank DMA budget on silicon against the bsnes figure of ~163 B/line
  (`snes/dma_rate_test.s`). Every bandwidth number in `docs/raycaster.md`, `docs/tbdr.md` and
  `docs/fmv-engine.md` rests on that constant, and it is the one most likely to differ on hardware.
- **H4** — the audio path: `fpga/cores/mixer_out` against the C mixer reference in `engine/`.
