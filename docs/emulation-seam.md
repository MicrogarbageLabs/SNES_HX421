# The cartridge seam (PC ↔ hardware parity contract)

The core parity idea: define the **cartridge bus seam as one contract**, implemented on both sides and
consumed by identical coprocessor code. This is the mgapi ABI generalized from a bsnes-plus mapper hook
into a proper bus contract, retargeted to **ares**.

```
                    ┌──────────────── CARTRIDGE SEAM (contract) ────────────────┐
   SNES-bus side ───┤                                                           ├─── coprocessor side
                    │  ROM-window read (addr → data)                            │
   ares (PC)  ──────┤  MCU command / mailbox (joypad, status, queue, release)   ├──  HX-421 firmware
   FPGA (hw)  ──────┤  release / sentinel / NMI-DMA staging handshake           │    as a stable-ABI
                    │  master tick (hw: master/487 · ares: emulated master clk)  │    DLL — IDENTICAL
                    │  audio sample sink (mixed stereo @ master/487)            │    on PC and hw
                    └───────────────────────────────────────────────────────────┘
```

- **ares** implements the SNES-bus side (emulated).
- **FPGA + M3** implement the SNES-bus side (real hardware).
- **The HX-421 firmware/audio engine is a stable-ABI DLL** (like `mgapi.dll` in bsnes-plus) — byte-
  identical logic in both worlds. Only the seam implementation differs.

## Why ares (not bsnes-plus) for this project

The riskiest hardware assumption is the cooperative **release → hot-path → NMI-DMA** timing. bsnes-plus
is loose enough on cycle timing that a bug there could pass in emulation and fail on hardware. Ares is
far more cycle-accurate, so validating the seam against ares de-risks the real cart. Ares is already
the accuracy reference (bsnes-accuracy is broken on Win11) and has an MSU-1 audio path to hang the
coprocessor audio on. Cost: fork ares, add a coprocessor board (`sfc/coprocessor/`, BML manifest) + a
shim that loads the HX-421 DLL — the same pattern as bsnes-plus + mgapi.dll, cleaner codebase.

## Contract surface (what crosses the seam)

| Channel | Hardware | ares (PC) |
|---|---|---|
| ROM-window read | FPGA serves from PSRAM | board read handler → DLL |
| MCU command / mailbox | SPI bridge + FPGA mailbox regs | board mailbox regs → DLL |
| release / sentinel / NMI-DMA | FPGA gating + M3 hot-path | board timing model → DLL |
| master tick | FPGA master/487 (from pin 1) | derived from ares emulated master clock |
| audio sample sink | M3 → FIFO → DAC → cart audio-in | DLL samples → MSU-1/audio mix + PC drift correction |

## Audio + PC-side drift correction (reuse HX-420)

On hardware the FPGA locks audio to the SNES master (master/487). In ares there is no crystal drift,
but the emulator's production rate must reconcile with the host WASAPI output — the **same PC-side
drift correction / dynamic rate control used on HX-420 (via mgapi)**. Lift it over: it does in software
exactly what the FPGA clock lock does in silicon. See `audio.md` for the master/487 rationale.

## Parity guarantee

The DLL is the same compiled logic (PC build vs. M3 cross-compile of the same sources). Develop and
debug against ares with fast iteration + cycle-accurate timing; the identical engine ships on the cart.
The seam is the only thing that is implemented twice.

## ABI reference (extracted from mgapi) + HX-421 decisions

mgapi's seam is **one header, flat C, `extern "C"`, dual-target** (Windows DLL via
`LoadLibrary`/`GetProcAddress` AND a static lib linked into STM32 firmware). Copy that discipline
verbatim — one platform-neutral header is the whole contract. Reference: `microgarbage/include/mgapi/
mgapi.h`, call-order example in `tools/mgapi_host_test.c`.

### ABI surface (grouped) — the template to refine

| Group | mgapi functions | Keep for HX-421 |
|---|---|---|
| lifecycle | `mgapi_init(cfg)→int`, `mgapi_shutdown()`, `mgapi_version()→str` | yes |
| cart bus | `mgapi_cart_read(addr24)→u8` (read-only pull; address-keyed side effects) | yes |
| tick | `mgapi_step(elapsed_ns)` (just signals a worker thread — host never blocks) | yes |
| audio | `mgapi_audio_pull(int16* dst, frames)→frames` (stereo int16 @ 44100) | **yes — implement for real (see below)** |
| input | `mgapi_post_joypads(u16[4])`, `mgapi_post_mouse(dx,dy,btn)` | yes |
| reset | `mgapi_cart_reset_begin/ready/end` (models CIC + window restage) | yes |
| dev-only | `mgapi_dev_*` | **no** — test scaffolding, not contract |

### Config struct

`MgapiConfig`: `cart_window_size (==65536)`, `audio_sample_rate (0=auto/WASAPI query)`, `audio_frames_max`,
`tcp_listen_port`, `pad_count`, `rom_select`, `shell_elf_path`, `autostart_path`, `reset.hold_ms`,
`disable_default_stdio`, `_reserved_pad[7]==0`. **HX-421 change: add an explicit `abi_version` word** —
mgapi versions only via the reserved-pad zero-check, which the audit flagged as the weakest part.

### Cadence (per emulated frame)

`init` once → each frame: `post_joypads`/`post_mouse` then `step(elapsed_ns)` → continuously during CPU
exec: `cart_read(addr)` per bus byte → `audio_pull` on the host audio cadence. Reset:
`begin → loop step while !ready → host reset → end`. The worker-thread decoupling (host thread never
blocks on VM/audio) is worth replicating in the ares shim.

### Mailbox model (decision point)

mgapi has **no separate mailbox region and no host write path** — the "mailbox" is *magic addresses
inside the 64 KB window* read via `cart_read`: `$7000-77FF` joypad latch, `$7800` frame-ready,
`$7808-7847` DMA descriptors, `$7E00` boot→runtime one-shot, `$7F00` status. Guest→window staging is
internal. **DECISION:** keep this proven magic-address scheme for v1 (it already pairs with the 65816
kernel), or design a cleaner dedicated mailbox for HX-421. Recommend: keep for v1, revisit later.

### Audio delivery — DECISION: implement the real pull model

mgapi *declares* a pull model but on Windows **bypasses it** — it opens its own WASAPI sink and pushes,
because it was fighting bsnes-plus's Qt audio pipeline; `audio_pull` returns 0 and only serves as the
SNES-master-clock tick for the drift PLL. **ares has a proper audio sink, so implement `audio_pull` for
real: ares owns the sink and pulls mixed int16 stereo from the DLL.** This also relocates
drift-correction responsibility cleanly to the ares side (below). Do NOT copy the internal-WASAPI hack.

### Drift correction — you already have it, and it's portable

Two loops, both pure fixed-point (no OS deps), both extracted into `engine/`:
- **Mixer sync PLL** (`audio_mixer.c:1028-1149`) — 1st-order low-pass PLL: measures internal (rendered)
  vs external (fed/clock) frame counts, steers per-channel resampler step. Constants: `SYNC_ACC_BITS=16`,
  smoothing `Q15/128` (~1.5 s TC), max ±2%, outlier-reject >6.25%. **Ships free with the mixer copy.**
- **Ring-level P-controller** (FMV A/V) — regulates ring fill-ms to a setpoint captured after a ~3 s
  settle; `ppm = kp·(ema_ms − target)`, `kp=20 ppm/ms`, clamped ±18000 → `mixer_set_drift_ppm`.
  Re-implement in the fresh `service.c`.

**On ares:** drive the PLL exactly like mgapi's FMV loop, relocated across the seam — internal clock =
ares audio-callback frame count, external clock = SNES sample cadence (the `audio_pull` count as the
master-clock tick). `mixer_*_sync` ports with zero changes. This *is* "the HX-420 PC-side drift
correction," now on the ares side.

## Relationship to mgapi (reference only — not a dependency)

mgapi (bsnes-plus cart runtime, stable-ABI DLL) is a **design reference**, not a dependency. HX-421 is a
standalone repo; it borrows the *shape* of the mgapi ABI as a starting template for this contract, then
owns its own definition. The DLL here is HX-421's `engine/`, compiled independently — no link against
microgarbage/mgapi. See `../engine/PROVENANCE.md`.
