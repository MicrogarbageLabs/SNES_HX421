# engine/ — portable coprocessor core

HX-421's own portable C core, host-independent. Compiled into **both**:

- the **M3 firmware** (`../firmware/`) on real hardware, and
- the **ares-side stable-ABI DLL** (`../host/`) on PC,

behind the cartridge-seam contract (`../docs/emulation-seam.md`). Same sources, two builds — the parity
spine.

## Scope

- audio mixer (8-voice, cubic resample, per-source rate)
- block-based fragmentation-free sound RAM
- stream arbiter (primed heads, stream-priority refill)
- Q15 fixed-point FFT (spectrum)
- PC-side drift correction / dynamic rate control (host audio sink)

Design notes: `../firmware/audio/mixer.md`, `../firmware/audio/fft.md`, `../docs/audio.md`.

## Independence (important)

**HX-421 does not depend on microgarbage.** No submodule, no shared library, no include path into the
microgarbage tree, no build coupling. A few sources may be **copied in and adapted** from microgarbage
as a starting point (see `PROVENANCE.md`), after which they are HX-421's own code and free to diverge.
Where a piece is small, prefer a clean re-implementation over a copy.

Keep this core free of host specifics: no SD/FAT, no USB, no ares/bsnes types, no STM32 HAL. Those live
in `firmware/` (hardware) and `host/` (ares) and reach the engine only through narrow interfaces
(sample sink, file/stream provider, seam mailbox).
