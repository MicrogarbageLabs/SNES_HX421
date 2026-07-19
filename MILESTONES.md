# HX-421 — Milestones & status

**Where it stands (2026-07-16):** the audio subsystem and the SNES video foundation both run **in
emulation (bsnes-plus)**, with zero hardware. You can hear interactive audio (SFX + music + a live FFT
meter) coming out of an emulated cart, and a 65816 kernel boots from the served window, relocates to
WRAM, and drives the PPU. No VM, no cart release — the RISC-V-does-everything / FPGA-serves-the-bus
model, stood up against the real cartridge seam.

Development host = **bsnes-plus** (proven toolchain). **ares** is banked — its cycle accuracy is worth
it only when we validate the tight bus/DMA timing on the way to hardware; the board is already written
and type-checked (`Source/ares/.../sfc/coprocessor/hx421/`).

---

## Track A — Audio (emulation) ✅ DONE
- [x] `engine/` extracted from microgarbage (rev `258ad24`), standalone, **291 tests green**.
- [x] `engine/service.c` (`hxa_*` owner) — engine renders as a unit (`demo/player.c` → out.wav).
- [x] `include/hx421.h` — the flat-C dual-target cartridge-seam ABI.
- [x] `runtime/hx421_runtime.c` → `hx421.dll`; proven via `tools/hx421_host_test.c`.
- [x] bsnes-plus **hx421 chip** (`host/bsnes-plus/`, opt-in `HX421_ENABLE=1`, coexists with mgapi).
      Fed bsnes's audio `Stream` from the real `hx421_audio_pull`. Fixed the DSP-reset-mute gate.
- [x] **Clean tone → real WAV music** through the mixer in bsnes (`-Wav`, natural level).
- [x] **Command channel** — `hx421_audio_command` + built-in SFX; chip maps buttons → commands
      (A/B/X/Y = SFX, Start = music toggle, Select = stop). Interactive, confirmed.
- [x] **FFT spectrum** — engine FFT enabled + `hx421_fft_bands` ABI + an ASCII meter (-Log). Reacts to
      audio (the "all zeros" was pcm-stream priming latency, not a bug).
- Follow-ups (non-blocking): move the FFT compute off the RT pull path (per `audio_fft.h` rule);
  tune the ~1 s pcm-stream priming latency; per-voice gain trims when many loud voices mix.

## Track B — SNES video kernel (emulation) ◐ IN PROGRESS
- [x] **B1: boot → WRAM → static test pattern.** `snes/` (ca65): reset stub → copy kernel to WRAM →
      run from WRAM → PPU shows "HX-421"/"BOOT OK" + color bands. DLL serves the boot ROM out of its
      window (`-Kernel`). Confirmed on screen alongside live audio.
- [x] **B2 ✅ VISUALLY CONFIRMED (2026-07-17, bsnes-plus): free-running H-IRQ (`$10`) chainer +
      per-line action table → dynamic frames + the H-blank siphon framework.** Stable letterboxed
      frame, FFT bars, AND the scrolling-green-band siphon proven (GP-DMA lands in VRAM mid-active-
      display in bsnes-plus, not just ares). States
      nothing/force-blank/unblank/**siphon**; dynamic force-blank letterbox = dynamic DMA bandwidth
      (no window regs). **Tables read directly from cart BRAM, double-buffered, no WRAM staging** (the
      HX-420 freewheel win). First content = live **FFT bars**.
      - Fixed the "flashing/mostly-blank" bug: the bulk DMA walk is armed at the **`V>=VIS_END`
        crossing** (not the V-wrap), so the 2 KB tilemap DMAs through the ~70-line bottom-LB+vblank+top-LB
        window — resident ~40 lines before the reveal, no straddle. (bsnes-plus DOES honor `$10`
        auto-repeat — verified from its `poll_irq`; H-only stays, it's what the siphon needs.)
      - **NMI EMITTER adopted (max bandwidth):** replaced the descriptor-list walk with the
        microgarbage `mg_nmi`/`copro_r3d` baked-immediate technique. The DLL emits the per-frame 65816
        DMA body into the window; the kernel `jsl`s straight into it in the served BRAM
        (execute-from-window, no WRAM copy, no descriptor read, no runtime dispatch). Removed
        walk_step/calc_bytes_rem; coprocessor owns the fit budget at emit time. Headless test all-pass.
      - **H-blank siphon (`ACT_SIPHON`) — SHELVED for real hardware.** Works in bsnes-plus + ares but
        overruns the true H-blank ceiling on silicon (microgarbage's finding). Code + emulator demo
        (scrolling green band) kept as a proven-in-emulator reference; hardware uses bulk DMA + OAM only.
- [ ] **B3 — FMV demo port (emulator-first, then the real-hardware demo seed):** port the microgarbage
      **FMV player + mouse overlay** ("film critic shooting the screen": mouse cursor + bullethole
      sprites over full-motion video). Big per-frame VRAM pushes via the bulk emitted-body DMA + dynamic
      force-blank letterbox for bandwidth (NO siphon); mouse + bulletholes = OAM sprites in the emitted
      body. Get it running in bsnes-plus, then use as the starting point for on-hardware bring-up.
- [ ] **B4+:** playlist/UI, settle the window/mailbox address map + sync `hx421.h`.

## Track C — Hardware (FPGA + M3) ○ FUTURE
- [ ] FPGA: fork `sd2snes_mini` (Cyclone IV EP4CE22) base via the free Quartus-Lite docker; add the
      mixer output stage, the soft RISC-V, the optional 3D compositor. `fpga/`.
- [ ] M3 firmware: audio mixer (link `engine/` static, `HX421_STATIC`) + FXPak base firmware + the USB
      dev-host shell (RISC-V load + `printf` forwarding — see `docs/dev-mode.md`).
- [ ] Boot ROM in FPGA BRAM; reset stub branches game vs FXPak launcher.
- [ ] Bring-up + on-hardware timing (re-measure the force-blank byte budget vs the bsnes ~160 B/line).

## Track D — ares (banked)
- [ ] Build the ares fork (needs the VS C++ workload) when cycle-accurate bus/DMA-timing validation is
      wanted — especially the release-free kernel's per-line chainer. Board already written.

## Cross-cutting
- Keep `engine/` and `runtime/` **portable** — same sources on PC (DLL) and M3 (`HX421_STATIC`); PC
  parity is the debugging lifeline.
- The recurring theme: every HX-420 constraint (VM, cart release, per-frame table staging) falls away
  because on HX-421 the FPGA serves the bus in hardware while the RISC-V freewheels beside it — the
  kernel keeps getting *simpler*, not harder.
