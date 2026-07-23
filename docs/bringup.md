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

## The LEDs are NOT observable — the screen is the diagnostic

`fpga_pgm` panics into a blink pattern on three LEDs, and the first version of this plan was built
around reading it. **That was useless in practice: the FXPak Pro is a sealed cartridge and those LEDs
are on the PCB inside the shell.** The blink codes are recorded below only because they are visible
on a bare sd2snes board or with a shell open.

| code | LEDs (bit2=ready, bit1=read, bit0=write) | meaning |
|---|---|---|
| 1 | write | `PROG_B` stuck high — board, not our bitstream |
| 2 | read | no `INIT_B` response |
| 3 | read+write | `DONE` stuck high |
| 4 | ready | failed to configure after 10 tries — bitstream rejected |

So the probe ROM has to carry the signal itself, and `snes/h1_probe.s` produces three states that can
be told apart by eye:

| screen | meaning |
|---|---|
| **black** | the ROM never executed |
| **solid red** | reset and PPU init ran; the main loop is stuck |
| **cycling colour** | the 65816 is executing our loop continuously |

The colour is set during the initial forced blank, so "got here" and "still running" are separate
observations. A static screen cannot be distinguished from a frozen CPU, which is why the loop
animates.

### Measured: mid-frame CGRAM writes are NOT dropped

I predicted that writing CGRAM during active display would be discarded on silicon and leave a black
screen. **Wrong.** The first build of the probe wrote CGRAM in a tight unsynced loop and on hardware
produced drifting horizontal colour bands: the write takes effect at the raster position it lands
on, and the loop period beats against the frame so the band walks up the screen.

That artifact is *positive* evidence — an unsynced writer paints bands, so seeing them means the CPU
is looping. The probe now syncs to v-blank for a clean whole-screen colour, which is easier to read,
but either behaviour is a pass.

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

### Result: PASS (2026-07-21, FXPak Pro)

`h1_probe.sfc` ran, cycling colours with the drifting band described above.

That is better than predicted. The chain it exercises is: MCU reads the ROM header, matches
map/carttype to the OBC1 core, streams `/sd2snes/fpga_obc1.bi3` into the FPGA, waits for `DONE`,
loads the ROM, releases the SNES — and the 65816 then fetches and executes from cart space. If
`fpga_pgm` had failed it would have hit `led_panic`, a `while(1)`, and the ROM would never have
loaded at all. **So the ROM running is itself proof the bitstream configured.**

It also settles a question the plan had left open: **`baseline_mini` maps a ROM.** I expected the
minimal base might not, and had written a black screen down as an acceptable pass. It does.

### The remaining confound: is it OUR bitstream?

"The ROM runs" does not by itself prove our file is what got programmed. If the swap had somehow not
taken effect, the FPGA would still hold `fpga_base` from power-on — which maps LoROM perfectly well
and would run the probe identically.

A **missing** file does not distinguish the cases either: `fpga_pgm` opens the file, fails, and
returns *without reconfiguring*, so `fpga_base` stays loaded and the ROM still runs.

A **truncated** file does. `fpga_pgm` streams it, never sees `DONE`, retries ten times and calls
`led_panic` — a `while(1)`. The cart hangs and the screen stays black.

`build-h1.ps1` emits `fpga_obc1_TRUNCATED.bi3` for exactly this.

**Run 2026-07-21: the control passes.** With the truncated file in place the cart **hangs on the
menu's "Loading ..." message**. That is a sharper signature than the black screen predicted, and it
localises the stall exactly: `load_rom` prints "Loading ...", *then* calls `fpga_pgm`, which streams
the file, never sees `DONE`, retries ten times and enters `led_panic`. The text stays on screen
because nothing after it ever runs.

So both directions are now established:

| bitstream at `/sd2snes/fpga_obc1.bi3` | result | conclusion |
|---|---|---|
| ours, intact | probe runs, colours cycle | configuration succeeded |
| ours, truncated | hangs at "Loading ..." | the file is genuinely read and programmed |

Together those rule out a stale `fpga_base` explaining the pass. **H1 is closed: a bitstream built by
our Quartus flow and packed by our packer configures on real hardware.**

Recovery is a power cycle plus restoring the good file. `led_panic` is a spin loop; nothing is
written to flash.

## Settled by the H1 run

- **The firmware fork is compatible.** The FXPak Pro accepted a `.bi3` at `/sd2snes/fpga_obc1.bi3`
  and selected it from a `map=$30 carttype=$25` header, so the vendored `mrehkopf/sd2snes` logic in
  `smc.c` matches the shipping firmware closely enough for core substitution.
- **Our RLE packer produces a bitstream the MCU accepts.** Round-trip verification in software
  predicted this; hardware confirmed it.
- **`baseline_mini` maps a ROM well enough to execute.**

Still open:

- **Whether a UART is reachable.** `fpga_pgm` prints a running commentary (`P`, `p`, `C`, `c`, byte
  counts) and `led_panic` keeps `cli_entrycheck` alive. Not needed for H1, but it would turn every
  later failure from a black screen into a sentence.

## After H1

- **H2** — a core that does something observable: hold a known value at a cart address the 65816 can
  read, proving our logic runs and the bus decode works.
- **H3** — re-measure the force-blank DMA budget on silicon against the bsnes figure of ~163 B/line
  (`snes/dma_rate_test.s`). Every bandwidth number in `docs/raycaster.md`, `docs/tbdr.md` and
  `docs/fmv-engine.md` rests on that constant, and it is the one most likely to differ on hardware.
- **H4** — the audio path: `fpga/cores/mixer_out` against the C mixer reference in `engine/`.
