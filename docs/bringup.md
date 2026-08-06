# Hardware bring-up on the FXPak Pro

First contact between our build and real silicon. The organising principle: **the first experiment
must not be able to brick the device**, and it must produce a legible answer even if everything
about our bitstream is wrong.

> **UPDATE (2026-08-05): HX-421 now has its OWN registered core — no more OBC1 borrowing.**
> Once firmware flashing was working, `FPGA_HX421` was added to the firmware (`fpga.h`/`smc.h`/`smc.c`,
> in `firmware/dev/sd2snes-devmode.patch`): a ROM with **`map $30` + `carttype $E4`** now selects
> **`/sd2snes/fpga_hx421.bi3`** — its own slot, leaving stock `fpga_obc1.bi3` intact. The shared
> `snes/h1_header.s` and the `build-*.ps1` scripts were updated accordingly ($25→$E4, pack
> `fpga_hx421.bi3`). The OBC1-borrow method below is kept as the historical record of how H1–H6 ran;
> the mechanism is identical, only the registered `(map, carttype) → file` triple changed.

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

### Result: MISREAD as a pass (2026-07-21), later shown genuine at H2

`h1_probe.sfc` ran, cycling colours. At the time this was recorded as proof our bitstream configured.
**It was not** — see H2 below. Our `mini`-derived core never configured usably; the colours came from
the stock core still resident from power-on. The reasoning below is preserved because it is the
correct logic applied to a wrong premise, and the premise (that our file was the one loaded) is
exactly what went unchecked.

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

This table WAS read as establishing both directions. It does not:

| bitstream at `/sd2snes/fpga_obc1.bi3` | result | what it ACTUALLY shows |
|---|---|---|
| ours (`mini`), intact | probe runs, colours cycle | **stale stock core still resident** — not proof ours loaded |
| ours, truncated | hangs at "Loading ..." | a bad file hangs; says nothing about a good one |

The error: "intact -> runs" and "truncated -> hangs" are consistent with our file loading, but also
consistent with our file NEVER loading and the stock core running the intact case. A control that
proves a *bad* file fails is not a control that proves a *good* file succeeds. The missing test was a
POSITIVE control — a known-good core producing a DISTINGUISHABLE result — which is exactly what the H2
signature finally is. See the H2 section.

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

## H3 — the DMA rate (`snes/dma_rate.s`)

Every bandwidth figure in `docs/{raycaster,tbdr,fmv-engine}.md` rests on one constant: bytes moved
into VRAM per scanline of blanking. This measures it, and separately measures what a transfer
*costs* to start.

Plain LoROM, carttype `$00` — **no coprocessor, no core swap, nothing on the card to back up.** What
it measures is a property of the console.

### Two instruments

**Rate**, by counter: force-blank the whole frame, read V, run a DMA of known size, read V again.
The CPU is halted for the duration of a DMA so it cannot count lines itself; reading the counter
either side is exact.

**Fit**, by binary search: with the display running at brightness 0 (**not** forced blank, which
would grant VRAM access all frame and destroy the very window being measured), trigger at the top of
vblank and ask whether the transfer was still inside vblank when it ended. Walk the size to the
largest that fits.

> A ROM→WRAM copy cannot be used for this. A GP-DMA always runs to completion and halts the CPU — it
> is not truncated when the window closes — and WRAM accepts writes at any time, so a verify would
> pass at every size and the walk would climb to its ceiling. **VRAM is the only target where "did it
> land" means "did it fit"**, because writes outside blanking are dropped.

### Measured — and hardware agrees with bsnes-plus TO THE BYTE

The FXPak Pro returned **6064 / 6048 / 5872**, identical to bsnes-plus across all three structures.
Exact agreement on three independent figures is not coincidence: for GP-DMA into VRAM and for the
size of the vblank window, **bsnes-plus is trustworthy and DMA-budget work can be iterated in the
emulator**. That is a methodology result worth more than the constants.

It does not generalise to everything. This session already found one place bsnes and silicon differ
in kind — mid-frame CGRAM writes — so the claim is specifically about GP-DMA timing.

```
bytes    V start   V end   lines   B/line
 8192      0        50      50      163
16384      0        99      99      165
32768      0       198     198      165
```

The rate converges on **165.5 B/line**, exactly theory: (1364 master cycles − 40 for DRAM refresh) / 8.
The 163 at 8 KB is not lower efficiency — 8192/165.5 = 49.5 lines, which can only be reported as 50.
Line quantisation, not overhead.

```
bytes that fit in one vblank      vs one DMA
one DMA                6064          —
eight chained          6048        -16   (one search step: free)
eight triggers         5872       -192   (~27 B per extra trigger)
```

### The engine result: chain, do not re-trigger

**Eight channels fired by a single `$420B` write move the same bytes as one large DMA.** They all
target `$2118` and VMADD keeps incrementing across them, so a chain writes one contiguous VRAM run
for one trigger. Chaining is free.

A separate trigger costs **~27 bytes (~0.17 scanlines)** — and that is the floor, measuring only the
trigger itself with the channels pre-armed. The engine's emitted body additionally rewrites VMADD and
the source registers per slot, so its real per-slot cost is higher.

So: **contiguous VRAM regions should use up to 8 chained channels per trigger.** Only scattered
destinations need CPU between transfers to move VMADD, and only those pay the per-slot cost. At a
dozen-plus slots per frame the trigger cost alone is ~6% of the window.

### What this does and does not change in the budgets

The **~163 B/line** the other docs use is right, and is the number to keep. 6064 bytes over a 37-line
vblank is 163.9 B/line *effective* — the raw rate is 165.5, and the difference is the trigger
latency, which real transfers always pay. Budgeting on 163 is correct and marginally conservative.

The **~6.2 KB vblank figure is optimistic** and should read **6064 B (5.9 KB)**. 6.2 KB is 6349,
about 5% over, which is enough to turn a frame that "just fits" on paper into one that overruns.

The raycaster's 54-line window at 240x208 gives 54 x 163.9 = **~8850 B**, against the 8802 B the doc
claims. Unchanged in substance.

## H2 — our logic runs on the FPGA (PASSED 2026-07-24)

The `h2_base` core intercepts four reads at `$3F:F000-$F003` and serves 'H','X','4','2' from the
fabric. `h2_probe.sfc` read exactly that and printed **OUR LOGIC IS RUNNING** — bytes at addresses
that hold `$FF` filler in the ROM, so they can only have come from our Verilog.

This also makes **H1 genuine in retrospect**, which it was not before. The original H1 pass was
misread: our packed bitstream had never actually configured, and the cycling colours came from the
stock core still resident from power-on. Two things hid it — the truncated-file control only proved a
*bad* file hangs, never that a *good* one loads, and every one of our own packed cores hung at
"Loading ...". H2 is the first unambiguous configuration of a core we built.

### Why every earlier attempt hung: we forked the wrong project

The scaffold forked **`sd2snes_mini`**. That is the minimal boot core embedded in MCU flash to paint
a power-on message; its `.qsf` sets `ENABLE_CONFIGURATION_PINS OFF` and it has **no MCU ROM-load
path**. A `mini`-derived core configures fine and then cannot service the ROM load — the menu hangs
on "Loading ..." indistinguishably from a bitstream that never configured, which is what sent the
diagnosis wrong three times (LEDs, bank gating, bitstream compression — all real issues, none the
cause).

**`sd2snes_base` is the core the FXPak loads for a game**: it carries `set_mcu_addr`, `sd_offload`,
DMA and the full read path. `fpga/build/h2_base/` is `base` with SignalTap stripped (it needed a
`tap_pll.qip` we do not have, and it is debug-only) and the signature spliced in at the TOP of the
read priority chain. 6359 LE (41% of the EP4CE15) vs `mini`'s 410 — the size gap IS the ROM-load
machinery `mini` was missing. The packed `.bi3` is 153 KB, the same order as the stock 97 KB OBC1
core and nothing like `mini`'s 55 KB.

### The signature detail

- Gated on **bank `$3F`** (`SNES_ADDR[23:16] == 8'h3F`), which the menu — a low-bank LoROM — never
  addresses, so the interception is invisible to the loader. An earlier version matched `$F000` in
  every bank and corrupted the menu's own reads.
- The probe uses long addressing `lda f:$3FF000,x`. In a 128 KB LoROM, `$3F` mirrors to bank `$03`,
  file offset `$1F000`, which is `$FF` filler — so the stock-core case would read `255 255 255 255`
  and the two outcomes cannot be confused. `build-h2.ps1` asserts that offset is filler before
  building.
- **`carttype $25` crashes bsnes-plus**: it instantiates its OBC1 chip and access-violates on a
  non-OBC1 ROM. `build-h2.ps1` emits `h2_probe_emu.sfc` (carttype `$00`) for emulator testing, which
  necessarily shows the STOCK-core result since bsnes has no FPGA.

## Build order from here

- **H2 base is a springboard, not the product.** It is the stock game core plus a four-byte hack; the
  real HX-421 core replaces the whole read path. But it proves the toolchain end to end and gives a
  known-good `base` fork to grow from.
- **H4** — the audio path: `fpga/cores/mixer_out` against the C mixer reference in `engine/`.

## Milestone H4a — "the audio seam carries a signal" (tone core)

The 8-channel mixer is proven bit-exact and timing-closed entirely in sim (`docs/audio-fpga-mixer.md`).
What is NOT yet proven is the *seam*: does a sample our logic produces actually reach the cart's audio
output on real silicon? The FXPak's MSU-1 DAC back half (`dac.v` — a CIC interpolator + I2S serializer
+ a master-locked 44.1 kHz phase accumulator) is known-good, so the smallest first step swaps only the
DAC's *sample source*, nothing else:

- `dac_mix.v` — `dac.v` verbatim except `dac_data` comes from a direct `sample_in` latched on the
  44.1 kHz tick (`sample_req`), instead of from the `dac_buf` ring the MSU fills.
- `hx_tone_dac.v` — a power-up reset + a loud 441 Hz square wave into `dac_mix`, `play=1`, full volume.
- `main.v` — under `` `ifdef HX421_AUDIO_TONE `` (set in `main.qsf`), the MSU `dac` instance is
  replaced by `hx_tone_dac`. The H2 signature stays, so **one flash tests both**: signature present ⇒
  core loaded; tone audible ⇒ seam works.

Verified in sim first (`fpga/cores/mixer/sim/tb_tone_dac.v`, Icarus): standalone `sample_req` ticks at
44102.6 Hz (want 44100), the tone toggles at 441.0 Hz, and `sdout`/`lrck` carry live I2S activity — so
the phase accumulator runs without the MSU and our sample reaches I2S. A tone through a sealed cart is
un-scopable, so sim is the only pre-hardware check; the ear is the hardware check.

Build + deploy: `cd fpga\build\h2_base; quartus_sh --flow compile main` then `.\snes\build-h4a.ps1`
(packs `output_files/main.rbf` → `fpga_obc1.bi3`). Run the existing `h2_probe.sfc`. Expect the H2
signature on screen AND a steady tone. **Comment out the `HX421_AUDIO_TONE` macro in `main.qsf` to
rebuild the plain base / H2 core.**

- **H4b** (next) — feed `dac_mix.sample_in` from `hx_audio_top` (the real mixer) instead of the square
  generator, with a hardcoded one-channel wavetable scene. Then the mixer itself is heard on hardware.

## Gotcha: mgapi.dll hijacks any ROM in bsnes-plus

`mgapi.dll` takes over the whole HiROM cart region **purely by existing next to `bsnes.exe`** —
`cartridge.cpp` calls `mgapi.try_load()` with no opt-out environment variable, unlike `HX421_ENABLE`.
Loading a plain test ROM shows the microgarbage boot banner instead. To test an ordinary ROM, rename
`mgapi.dll` (and `hx421.dll`) out of the way first, and remember to put them back.
