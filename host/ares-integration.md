# ares integration — concrete board plan

Confirmed from `ares-emulator/ares` (`master`) source. MSU-1 is the template: it's a coprocessor that
streams PCM audio and exposes bus ports — our exact shape. (Items marked ⚠ are inferred from usage and
must be verified against a local clone; see bottom.)

## How ares SFC coprocessors work

- Live in `ares/sfc/coprocessor/<name>/<name>.{hpp,cpp}`. **No base class** — each is a free `struct`
  inheriting `Thread` (the cooperative libco scheduler thread). One global instance (`extern MSU1 msu1;`).
- **Clock/step:** `power()` does `Thread::create(44100, &main)` + `cpu.coprocessors.push_back(this)`.
  `main()` runs once per tick: do one unit of work → `Thread::step(1)` → `Thread::synchronize(cpu)`.
  Bus handlers call `cpu.synchronize(*this)` first, so ordering vs. the CPU is cycle-correct for free.
  Running at 44100 makes **one `main()` iteration == one audio frame** — pairs naturally with audio pull.
- **Bus map (in code, not manifest):** in `cartridge/load.cpp`,
  `bus.map(bind_front(&MSU1::readIO,&msu1), bind_front(&MSU1::writeIO,&msu1), "00-3f,80-bf:2000-2007");`
  Handlers are `n8(n24,n8)` / `void(n24,n8)`. Address ranges are C++ strings `"banks:offsets"`.
- **Attach:** MSU-1 keys off file presence — `if(pak->read("msu1.data.rom")) loadMSU1();` sets
  `has.MSU1` (a bool in `Cartridge::has`), and `cartridge.cpp` gates `load/unload/power/serialize` on it.
- **Audio:** `stream = parent->append<Node::Audio::Stream>("MSU1"); stream->setChannels(2);
  setFrequency(44100);` in `load()`; per frame `stream->frame(left, right)` with `f64` in ±1.0. The
  **stream node owns resampling to host rate** — feed native 44100 and drift-correct upstream.
- **Build:** CMake unity build. `sfc.cpp` includes each subsystem `.cpp`; `coprocessor.cpp` includes each
  coprocessor `.cpp`. **Adding a coprocessor = no new compile target** — just extend the `#include` chain.

## The `hx421` coprocessor board

New files `ares/sfc/coprocessor/hx421/hx421.{hpp,cpp}` (skeleton — adapt to verified ares APIs):

```cpp
struct HX421 : Thread {
  Node::Audio::Stream stream;
  // small int16 ring for hx421_audio_pull()

  auto load(Node::Object parent) -> void {           // stream: 2ch @ 44100
    stream = parent->append<Node::Audio::Stream>("HX421");
    stream->setChannels(2); stream->setFrequency(HX421_AUDIO_RATE_HZ);
  }
  auto unload(Node::Object) -> void;                 // hx421_shutdown(); detach from cpu.coprocessors
  auto power() -> void {                              // hx421_init(&cfg) validated;
    hx421_init(&cfg);                                 // then create the thread + register
    Thread::create(HX421_AUDIO_RATE_HZ, {&HX421::main, this});
    cpu.coprocessors.push_back(this);
  }
  auto main() -> void {                               // once per audio frame
    hx421_step(NS_PER_TICK);                          // NS_PER_TICK = 1e9/44100 ≈ 22675
    if(ring_empty) hx421_audio_pull(ringbuf, N);      // refill in blocks
    int16 l, r = ring_pop();
    stream->frame(l/32768.0, r/32768.0);
    Thread::step(1); Thread::synchronize(cpu);
  }
  auto readIO(n24 addr, n8) -> n8 {                   // read-only pull
    cpu.synchronize(*this);
    return hx421_cart_read(addr);
  }
  auto writeIO(n24 addr, n8 data) -> void { cpu.synchronize(*this); /* control writes if any */ }
  auto serialize(serializer&) -> void;
};
extern HX421 hx421;
```

## Files to touch (mirror MSU-1)

1. `coprocessor/hx421/hx421.{hpp,cpp}` — new.
2. `coprocessor.hpp`: `#include "hx421/hx421.hpp"`; `coprocessor.cpp`: `#include "hx421/hx421.cpp"`.
3. `cartridge.hpp`: add `bool HX421` to `has`; declare `auto loadHX421() -> void;`.
4. `cartridge/load.cpp`: trigger (`if(pak->read("hx421.rom")) loadHX421();` or a manifest board node) and
   `bus.map(...)` your register window + optionally a wide range to `readIO` for the ROM/data window.
5. `cartridge.cpp`: `if(has.HX421) hx421.{load,unload,power,serialize}(...)` in each lifecycle path.

## API → model mapping

| hx421.h | ares hook |
|---|---|
| `hx421_init` | `power()`, before `Thread::create` |
| `hx421_cart_read(addr)` | `readIO` body (after `cpu.synchronize`); map wide ranges here too |
| `hx421_step(ns)` | once per `main()`, `ns ≈ 22675` |
| `hx421_audio_pull` | refill ring in `main()`, emit via `stream->frame()` — the drift seam |
| `hx421_post_joypads` | controller-poll path (`sfc/controller/`) or snapshot in `main()` |
| `hx421_cart_reset_*` | ⚠ no direct analog — hook `Cartridge::power()` + system reset |

## Verify against a local clone — CONFIRMED (local clone at `Source/ares`, 2026-07-15)

All five gap items are now confirmed against the local source. The board was written and
implemented; see `Source/ares/ares/sfc/coprocessor/hx421/{hx421.h,hx421.hpp,hx421.cpp}` and the
five wiring edits (each tagged `// HX421`).

### 1. `Thread` base (`ares/ares/ares/scheduler/thread.hpp`)
These are **non-static member functions** (called `Thread::create(...)` etc. from within the derived
struct, MSU1-style):
```cpp
auto create(double frequency, std::function<void ()> entryPoint) -> void;
auto step(u32 clocks) -> void;
auto synchronize() -> void;
template<typename... P> auto synchronize(Thread&, P&&...) -> void;   // e.g. Thread::synchronize(cpu)
auto destroy() -> void;
auto serialize(serializer& s) -> void;
```
`cpu.synchronize(*this)` in a bus handler is `CPU`'s inherited `Thread::synchronize(Thread&)`.
MSU1 registers via `cpu.coprocessors.push_back(this)` (`cpu.coprocessors` is `std::vector<Thread*>`,
in `sfc/cpu/cpu.hpp`). `cpu.power()` calls `coprocessors.clear()` **before** `cartridge.power()`, so
every coprocessor `power()` re-`push_back`s itself each power/reset — no double-registration.

### 2. `Bus::map` (`ares/ares/sfc/memory/memory.{hpp,cpp}`)
```cpp
auto Bus::map(
  const std::function<n8   (n24, n8)>& read,
  const std::function<void (n24, n8)>& write,
  const string& addr, u32 size = 0, u32 base = 0, u32 mask = 0) -> u32;
```
Grammar: `"banks:offsets"`, split on `:`; each side is a comma list of `lo-hi` hex ranges. A wide ROM
window is just a bigger range (with optional `size`/`base`/`mask` for mirroring). We use MSU1's
`"00-3f,80-bf:2000-2007"`. Handlers receive the **full 24-bit** bus address (MSU1 masks with `&7`);
our `readIO` forwards it whole to `hx421_cart_read`, which is ABI-correct regardless of window.

### 3. `Cartridge::has` + lifecycle (`ares/ares/sfc/cartridge/{cartridge.hpp,cartridge.cpp,load.cpp}`)
`has` is `struct Has { boolean ICD; ... boolean MSU1; ... }` — add `boolean HX421;`. Call sites:
- `load.cpp` `loadCartridge()`: file-presence trigger `if(auto fp = pak->read("hx421.rom")) loadHX421();`
  and `loadHX421()` sets `has.HX421 = true; bus.map(...)`.
- `cartridge.cpp` `Cartridge::connect()`   → `if(has.HX421) hx421.load(node);`
- `cartridge.cpp` `Cartridge::disconnect()`→ `if(has.HX421) hx421.unload(node);`
- `cartridge.cpp` `Cartridge::power(bool reset)` → `if(has.HX421) hx421.power(reset);`
- **serialize is NOT in cartridge.cpp** (deviation from the earlier plan): it lives in
  `ares/ares/sfc/system/serialization.cpp` `System::serialize(serializer&, bool)`:
  `if(cartridge.has.HX421) s(hx421);` (right after the `MSU1` line).

### 4. Reset lifecycle (`ares/ares/sfc/system/system.cpp`)
`System::power(bool reset)` → `cpu.power(reset); ...; cartridge.power(reset)`. The `reset` bool is
`false` on hard power-on, `true` on soft reset (`System::run()` calls `power(true)` on the reset
control edge). We give `HX421::power(bool reset)` the reset flag and, when `reset==true`, drive the
`hx421_cart_reset_begin() → loop hx421_step() while !hx421_cart_reset_ready() → hx421_cart_reset_end()`
handshake synchronously (bounded to 65536 iterations as a safety cap).

### 5. Runtime DLL binding — use `nall::library` (`ares/nall/nall/dl.{hpp,cpp}`)
ares already ships a cross-platform loader, pulled in by `ares/ares/ares.hpp` (`#include <nall/dl.hpp>`)
and compiled into the nall unity (`nall/nall/nall.cpp` includes `dl.cpp`). No hand-rolled Win32:
```cpp
nall::library lib;
lib.open("hx421");        // Windows: LoadLibraryW("hx421.dll"); search order includes the exe dir
lib.sym("hx421_init");    // -> void*  (GetProcAddress)
lib.close();              // FreeLibrary
```
We `#define HX421_STATIC` before including the vendored `hx421.h` so `HX421_API` is empty (no
`dllimport`); the ABI prototypes are unreferenced (we only call through resolved function pointers),
so there is zero link-time dependency on the DLL. Confirmed: the DLL at
`engine/build/hx421.dll` exports all 12 `hx421_*` symbols the board binds.

## Build/verify status (2026-07-15)

- **Board written + wired** (5 edits, all `// HX421`-tagged). Files: `hx421.{h,hpp,cpp}`.
- **DLL seam verified end-to-end:** `engine/build/hx421_host_test.exe` (same LoadLibrary+GetProcAddress
  pattern the board uses) runs green — `rms=431, peak=1000, "OK: audio flowed through the hx421.dll ABI"`.
  `objdump -p hx421.dll` shows all 12 exports.
- **ares-side C++ type-checked:** `g++ 16.1.0 -std=c++2b -fsyntax-only` against the real
  `ares/`, `nall/`, `thirdparty/`, `libco/` include roots — `hx421.hpp`+`hx421.cpp`, plus the edited
  `coprocessor.cpp`, `cartridge.cpp`, and `system/system.cpp` unity TUs — **all exit 0**, no errors
  (only pre-existing nall `-Wdeprecated-literal-operator` warnings).
- **Full ares binary build: BLOCKED** in this environment. ares' Windows CMake presets need MSVC or
  ClangCL (the VS 2026 Community install here has an empty `VC\Tools\MSVC` — no compiler), or the
  MinGW/Ninja CI preset which wants Clang + Ninja on PATH; only MinGW **GCC** 16.1.0 is present, and
  ares is not GCC-on-Windows clean. A configure also fetches `ares-deps` (prebuilt windows-x64) from
  GitHub (network). To finish: install the VS "Desktop development with C++" workload (MSVC + Windows
  SDK), then `cmake --preset windows-msvc && cmake --build --preset windows-msvc`, drop
  `engine/build/hx421.dll` next to the built `ares.exe`, and load a pak containing an `hx421.rom`
  member to trigger the board (SMOKE mode emits the diagnostic tone through the `HX421` audio stream).
