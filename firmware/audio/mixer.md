# MCU audio processor — design notes

Full rationale in `../../docs/audio.md`; this is the implementation-facing summary.

## Loop shape (Option B, master-locked)

1. FPGA `audio_tick` (master/487 ≈ 44101 Hz, block-rated) → IRQ/DMA request.
2. Per tick, produce exactly one block of N mixed stereo frames.
3. FPGA `fifo_level` telemetry steers block size / production so produce == consume (high/low water
   marks). The M3's own clock never defines the rate — the tick does. Drift = 0.

## Mixer

- 8 voices: ~3 guaranteed streams + up to 5 one-shot SFX (partition, not free-for-all).
- Cubic interpolation per source; per-voice native sample rate.
- Accumulate in a wider intermediate, saturate to `SAMPLE_BITS` on output.

## Voice sourcing

- Streams: head + refill ring in SRAM, refilled from PSRAM over SPI (DMA), stream-priority on the bridge.
- SFX: **short ones resident-whole** in SRAM (zero bridge on trigger); long ones head+refill.
- Ring/head depth = worst-case bridge-stall runway (2–4 ms ≈ ~1 KB/voice).

## SFX palette

- Load the scene's palette (tens of SFX) on scene enter (bulk PSRAM→SRAM burst), free on exit — via
  block-based fragmentation-free sound RAM.
- Trigger latency: 0 for resident, ~0.5–1 ms for a cold (unprimed) SFX.

## Voice allocation / eviction

- v1: FCFS + reject-when-full within the SFX partition.
- Seam for the upgrade: priority admission/eviction (steal oldest/lowest-priority for a critical SFX).
  Keep `voice_alloc()` isolated so this drops in without touching the mixer.

## TODO

- [ ] tick IRQ/DMA handler + block producer
- [ ] fifo_level feedback controller
- [ ] cubic resampler (port from microgarbage mixer)
- [ ] SPI-DMA refill scheduler (reuse microgarbage stream arbiter; stream priority)
- [ ] scene palette load/evict over block sound RAM
- [ ] voice_alloc() seam (FCFS now, priority later)
