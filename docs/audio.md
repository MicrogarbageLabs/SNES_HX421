# Audio subsystem (the flagship feature)

Goal: an 8-channel mixer with **structurally drift-free**, SNES-master-locked output — the primary
reason this project exists.

## Clock: drift is *relative*, so lock to the source

Two independent crystals always drift relative to each other (~40 ppm ≈ ~12 ms A/V slip over a 5-min
FMV). No MCU clock precision fixes this — only a **shared timebase** does. The SNES master clock is on
pin 1, wired to the FPGA, so the FPGA is the only device that can lock to it.

**Option B (chosen):** the FPGA divides master by **487 → 44101 Hz** (the MSU-1 native rate) and hands
that tick to the M3. The M3 produces exactly one block per tick.

- **One** interpolation stage total (the M3's per-source cubic resample), at the output rate.
- FPGA does **no** interpolation — just FIFO + DAC clocked by the same master/487.
- Drift is **zero by construction**; the M3's own crystal frequency is irrelevant to audio rate.

The base already ships `pll.v`/`pll.qip`/`dcm.v` — the master/487 divider grafts onto existing clock
plumbing. See `fpga/cores/mixer_out/mixer_out.v`.

## Voices & mixing (M3)

- 8 voices: ~3 **guaranteed streams** (music/FMV) + up to **5 one-shot SFX**.
- Cubic per-source interpolation; per-source native rates (SFX 11–22 kHz/8-bit; streams higher).
- Refill scheduling = reuse microgarbage's stream arbiter; **streams get bridge priority** (a stream
  dropout is continuous/audible; an SFX is short/low-exposure).
- Drive SPI refills by **DMA**, not CPU, so refills don't steal mixing cycles.

## Primed-head SFX palette (heads ≠ voices)

- **Voices** = simultaneous playback (8). **Primed heads** = what can start instantly = the whole
  scene's SFX palette (tens).
- Per-scene, bulk-load the SFX palette PSRAM→MCU-SRAM (one-time burst at scene transition; maps onto
  microgarbage block-based fragmentation-free sound RAM — alloc on enter, free on exit).
- **Short SFX: resident whole** → zero bridge access on trigger, not even a refill. Streams stay
  head+refill. So during play the bridge carries streams only; SFX drop off it entirely.
- **Head depth** = worst-case bridge-stall runway. 2–4 ms → ~1 KB/voice → ~8 KB for the active rings;
  palette size bounded by MCU SRAM (tens of ~1 KB heads / ~10–30 SFX per scene is plenty).
- Eviction: FCFS + reject-when-full is a fine v1. Upgrade path = microgarbage priority
  admission/eviction so a gameplay-critical SFX *steals* a voice instead of being dropped. Leave the
  voice-allocation seam where that policy drops in.

## Bridge budget (sanity)

At 44.1 kHz output + period-appropriate source rates: mixed output ~176 KB/s + source reads
~175–350 KB/s ≈ **~20–30% of ~2 MB/s**. Comfortable. Keep source rates modest to stay there.

## Storage vs. polyphony (don't conflate)

- Stored in PSRAM: hundreds of SFX (a few MB / 5–22 KB each). Not the limit.
- Simultaneous: 8 voices. The real ceiling.
- Instantly triggerable: all of them (cold trigger = first-chunk fetch, ~0.5–1 ms, imperceptible).
