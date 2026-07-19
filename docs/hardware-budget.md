# HX-421 hardware budget — measured facts and the audio architecture they force

Everything here was read out of the vendored `third_party/sd2snes` tree or confirmed against the
physical board. Numbers marked **assumed** are not yet verified and are the ones to measure first.

## Confirmed parts

| | part | why it matters |
|---|---|---|
| FPGA | **EP4CE15F17C8N** | 15.4K LE, **504 Kb (63 KB) M9K** |
| MCU | **STM32F401xC** (`config-mk3-stm32`) | Cortex-**M4F**, **64 KB SRAM**, 208 KB firmware flash |
| PSRAM | ~16 MB | ours alone — see below |

`docs/build.md` and the README previously assumed **EP4CE22**. That was wrong: all ten upstream
Quartus projects target `EP4CE15F17C8`, and the board confirms it. Plan against 63 KB of M9K.

The MCU is the STM32 variant, not the LPC1756 (`config-mk3`). That is the better outcome — 64 KB
SRAM instead of 32 KB, and an M4 with DSP instructions. The FPU is mostly irrelevant (the mixer is
Q15 fixed-point throughout) but `SMULxy`/`SMLAxy` and saturating adds matter for fitting 8 channels
at 44.1 kHz alongside cart emulation.

Firmware memory, from `src/stm32f401.ld`:

```
fwhdr  0x0800c000  0x00200
flash  0x0800c200  0x33e00   ~208 KB for firmware
ram    0x20000070  0x0ff90   ~64 KB SRAM total
```

## The SNES never reads PSRAM

This design departs from stock sd2snes. The console sees only BRAM:

- a small **vector window** at the top of cart space, always resident, never reused;
- a **boot payload** that the 65816 copies into SNES WRAM and executes from there;
- a **12–16 KB per-frame DMA staging area** (per-frame DMA while rendering never exceeds this).

After boot the SNES executes from its own WRAM and touches the cart only for staging DMA. So
**PSRAM bandwidth is entirely ours** — there is no strict-priority cart arbiter to design, which a
stock-sd2snes reading of the hardware would wrongly demand.

On reset (soft or hard) the payload area must be repopulated before the 65816 fetches. Make the
vector region a **hard partition**, not a convention: a payload write straying into it is a hang
with no diagnostic.

### BRAM is the scarce resource

63 KB of M9K must hold vectors + payload + staging (**×2 if ping-ponged**, as the host build does
today with two 12 KB buffers) + RISC-V application space + whatever the sd2snes base already
consumes for cart decode, SPI and FIFOs. That last figure is unknown until the baseline builds and
is the most important number still missing.

## The SPI link (MCU ↔ FPGA)

| | value |
|---|---|
| FPGA timing constraint (`main.sdc`) | `SPI_SCK` period 20.833 ns = **48 MHz** |
| STM32F401 ceiling | PCLK2/2 = 42 MHz |
| **actually configured** | **18 MHz = 2.25 MB/s raw**, ~1.7–1.9 MB/s sustained **assumed** |

The FPGA tolerates far more than 18 MHz, so the setting is likely conservative rather than a board
limit. If it can be raised, every margin below improves; worth reading the real `SPI_CR1` divisor
out of the built firmware.

### SD → PSRAM bypasses SPI entirely

`fpga_sddma(tgt, partial)` + `fpga_set_sddma_range()` (`src/fpga_spi.c`) hand the SD clock to the
FPGA (`GPIO_MODE_OUT(SD_CLKREG, SD_CLKBIT)`) and DMA straight into PSRAM. **Bulk video loading
never crosses the SPI link.** This is what makes the budget comfortable.

The cost: SD is **muxed**, not shared. FPGA-DMA and MCU stream reads take turns, which is precisely
what the round-robin stream arbiter exists to schedule — and why primed heads matter, since a
stream cold-starting while the FPGA holds SD must wait for the handover.

### Bulk PSRAM read: MISSING, and needed

There is no `fpga_read_block` in the stock firmware — reads are byte-at-a-time, which does not fit:

```
per-byte txn: cmd(1) + addr(3) + data(1) = 5 B = 2.2 us @18 MHz, ~3-5 us realistic
256 B quantum, one voice  ->  570-1280 us   (20-44% of a 2.9 ms quantum)
8 voices                  ->  4.5-10 ms     vs a 2.9 ms deadline   FAILS
```

The **FPGA hardware already supports it**: `mcu_cmd.v` decodes `0x88`–`0x8F` as read-with-
auto-increment (`cmd_data[7:5]==3'h4 && cmd_data[3]`), latching `mcu_data_in` and bumping
`ADDR_OUT_BUF` per byte. So this is likely **firmware-only work** — a bulk-read routine on the M3 —
rather than an FPGA change.

**Risk: the protocol has no flow control.** SPI keeps clocking whether or not PSRAM serviced the
byte within its 444 ns window; miss it and you read a stale byte *silently*. If the video path can
hold the PSRAM controller longer than that, a small prefetch FIFO in the read path is required, and
that IS an FPGA change. Failure mode is corrupt audio with no error — design it deliberately.

## Audio architecture

| tier | holds | why |
|---|---|---|
| SD | FMV units, music/voice tracks | bulk |
| **PSRAM** | sample pool, **primed heads**, FMV lookahead | ours alone, ~16 MB |
| SRAM (64 KB) | mixer rings, stream buffers | only what must be instant |

Decisions:

- **SFX: 22.05 kHz mono from PSRAM.** Halves the largest SPI term (705 → 353 KB/s) at no perceptual
  cost for one-shots, and the mixer already resamples per channel (`mixer_set_source_rate`).
- **Music/voice: 44.1 kHz streamed from SD.** CD quality is free here — SD-streamed audio never
  crosses SPI. Make **voice mono**: dialogue is mono anyway, halving both SD rate and head size.
- **Primed heads live in PSRAM, ~200 ms each.** SRAM cannot hold even one (a 0.5 s 44.1 stereo head
  is 88 KB > the whole 64 KB). Head length is set by worst-case arbiter wait — `(clients-1) ×
  service + SD seek` ≈ 45–60 ms — so 200 ms gives ~3× margin at ~35 KB of PSRAM per stereo stream.
  Heads are permanently allocated and never evicted, so restart is always instant.
- **No microheads.** Considered and rejected: a PSRAM SFX fetch is ~0.14 ms against a 2.9 ms
  deadline, so there is no latency gap to bridge. See the prefetch rule below, which subsumes it.

### The rule that actually matters

**Never block the render path on a bus transaction.**

- prefetch one quantum ahead, asynchronously;
- if data is not ready, render silence for *that voice* and start it next quantum — one voice 2.9 ms
  late is inaudible;
- never spin on SPI inside the render. That turns a private, inaudible slip into a missed DAC
  deadline that clicks *every* voice.

No amount of head buffering substitutes for this, because the failure is not about where data lives.

### Budget

Trigger-to-heard, dominated by the DAC buffer, not the memory:

```
PSRAM fetch          < 0.1 ms   (SPI dominates by ~1000x; PSRAM access ~70-100 ns)
render quantum       1.5-2.9 ms
DAC double-buffer    5.8-11.6 ms  (256-512 frame halves)
                     ------------
                     ~7-15 ms     -- inside the <20 ms target
```

SPI utilisation, with video on FPGA-DMA:

| | rate | of ~1.8 MB/s |
|---|---|---|
| SFX ← PSRAM (8 × 22 kHz mono) | 353 KB/s | |
| heads ← PSRAM (3 streams cold-starting) | 529 KB/s | transient |
| commands | 30 KB/s | |
| **steady state** (~3 SFX, no cold start) | **~380 KB/s** | **~21%** |
| **worst case** | **~910 KB/s** | **~50%** |

Concurrency: **8 sounding voices** (mixer channels) — typically 1 FMV + 1–2 music/voice + 5–6 SFX.
**2 concurrent SD streams** as the design point, 3 as a burst provided they do not all cold-start on
the same frame.

### Engine resizing required

The desktop config allocates ~5.4 MB and will not fit 64 KB:

| | desktop | M3 |
|---|---|---|
| 8 channel rings | 1024 KB | 8 × 256 frames = 8 KB |
| FMV audio ring | 128 KB | 4096 frames = 16 KB |
| music slots | 320 KB | 2 × 4 KB = 8 KB |
| audio pool | 4096 KB | **PSRAM** |
| **total** | **~5.4 MB** | **~34 KB** (+2 KB scratch) |

Good news: this is sizing, not architecture. `pool_bytes`, `track_count` and per-channel
`buffer_samples` are already runtime parameters, and music staging buffers are caller-supplied. Only
`AUDIO_RING_STREAM_FRAMES` is a hard `#define` sizing a fixed array — a one-line build knob. The
mixer, arbiter, resampler and drift PLL are unchanged.

## Open items, in priority order

1. **Build the stock baseline** (needs Docker). Yields: M9K the base consumes, SRAM the firmware
   leaves, and the real SPI divisor. Everything above is sized around these three unknowns.
2. **Bulk PSRAM read** on the M3, plus a prefetch FIFO in the FPGA read path if PSRAM blocking can
   exceed 444 ns.
3. **Chunk the video PSRAM→BRAM transfer** so audio reads get a slot every few microseconds rather
   than waiting out a per-frame burst (~160 µs if monolithic).
4. Decide **single vs ping-pong staging** in BRAM — it is a third to a half of all M9K.
