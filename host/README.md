# host/ — ares seam (PC development host)

The PC side of the cartridge seam: a **forked ares** with a custom **HX-421 coprocessor board** that
bridges the emulated SNES bus to the HX-421 runtime, and loads that runtime as a **stable-ABI DLL**
(`../include/hx421.h`). Chosen over bsnes-plus because ares is far more cycle-accurate — it validates
the timing-critical release/NMI-DMA handshake that the real cart depends on.

**Template: MSU-1.** ares already has a coprocessor (MSU1) that streams PCM audio and exposes data/status
ports over the bus — exactly our shape. Model the HX-421 board on it. (Exact ares class/API surface is
being confirmed by a research pass; see "Open items".)

## Call mapping (ares board → hx421.h)

| ares board hook | HX-421 call | Notes |
|---|---|---|
| load / power-on | `hx421_init(&cfg)` | cfg from board defaults or manifest; validate `abi_version` |
| bus read in our range | `hx421_cart_read(addr24)` | read-only pull; the mailbox magic addresses live here |
| per emulated frame | `hx421_post_joypads(pads)` / `hx421_post_mouse(...)` then `hx421_step(elapsed_ns)` | post host input, then advance |
| audio callback | `hx421_audio_pull(dst, frames)` → feed ares audio stream | **real pull; ares owns the sink** |
| reset | `hx421_cart_reset_begin()` → poll `hx421_cart_reset_ready()` → `hx421_cart_reset_end()` | models CIC + window restage |
| unload / power-off | `hx421_shutdown()` | re-init allowed after |

`hx421_step` just signals the runtime's worker thread, so the ares thread never blocks on the VM/audio.

## Audio + drift correction (the flagship, PC side)

Route `hx421_audio_pull` output (int16 stereo @ 44100) into ares's audio mix (the MSU-1 audio injection
point). Run the drift loop **on the ares side**, exactly like mgapi's FMV loop relocated across the seam:
- internal clock = ares audio-callback frame count
- external clock = SNES sample cadence (the cumulative `hx421_audio_pull` count = master-clock tick)
- steer via the mixer's `mixer_set_drift_ppm` path (the PLL ships inside the engine mixer).

This reproduces the HX-420 PC-side drift correction; on real hardware the FPGA master/487 does the same
job in silicon. See `../docs/emulation-seam.md`.

## DLL loading

Two options, both fine:
- **Dynamic:** `LoadLibrary` + `GetProcAddress` per `hx421_*` symbol (mirrors mgapi's
  `tools/mgapi_host_test.c` resolver) — hot-swap the runtime without rebuilding ares.
- **Static:** link the runtime as a lib into the ares fork (`HX421_STATIC`) for a single binary.
Start dynamic for iteration.

## Build

Fork `ares-emulator/ares`; add an `hx421` coprocessor + board under `sfc/`; register it in the board
table / manifest system; build with ares's nall/GNUmake toolchain. Keep the fork's HX-421 additions
isolated (a single `sfc/coprocessor/hx421/` dir + a board entry) so upstream ares merges stay clean.

## Integration guide

The concrete board plan (confirmed from ares source) is in **`ares-integration.md`**: coprocessor =
`struct : Thread`, MSU-1 template, `bus.map(read,write,"banks:offsets")`, file-presence attach in
`load.cpp`, audio via `Node::Audio::Stream->frame()`, unity `#include` build. Includes the `HX421`
struct skeleton and the exact files to touch.

Remaining gaps (small; close by cloning ares — which is the M0 build prerequisite anyway): the exact
`Thread`/`cpu.synchronize` signatures, the `memory.cpp` map grammar for wide windows, and the reset
lifecycle hook points (no direct `cart_reset_*` analog).
