# PC build (engine, DLL, demo)

How to build and run the host-side pieces on Windows. This is the fast dev loop — the engine and its
ABI are proven here before any FXPak hardware or ares work.

## Toolchain

MSYS2 mingw-w64 gcc (C11 — some sources use `<stdatomic.h>`). Verified with gcc 16.1.0.
- `C:\msys64\mingw64\bin` (gcc) and `C:\msys64\usr\bin` (make, coreutils) on PATH.

## The MSYS2 `TMP` gotcha (important)

The mingw assembler/linker do **not** pick up a `TMP` set via a bash `export` — they fall back to
`C:\WINDOWS\` and fail with "Permission denied". Set the temp dir in the **PowerShell** environment
before invoking bash, and point it at a writable dir:

```powershell
if (-not (Test-Path C:\mgtmp)) { New-Item -ItemType Directory -Force C:\mgtmp | Out-Null }
$env:TMP=$env:TEMP=$env:TMPDIR='C:\mgtmp'
$env:PATH = "C:\msys64\mingw64\bin;C:\msys64\usr\bin;$env:PATH"
```

Then run make from the `engine/` dir via bash (single-quote the path — it may
contain spaces, which bash would otherwise split):

```powershell
bash -c "cd '/c/path/to/SNES_HX_421/engine' && make CC=gcc <target>"
```

## Rebuilding bsnes-plus with the HX-421 chip

Needed whenever `host/bsnes-plus/*` or `include/hx421.h` changes — the chip is compiled INTO
`bsnes.exe`, so a new DLL alone is not enough.

**1. Copy the sources in** (the bsnes tree uses different filenames):

| ours | bsnes-plus tree (`bsnes/snes/chip/hx421/`) |
|---|---|
| `host/bsnes-plus/hx421_chip.cpp` | `hx421.cpp` |
| `host/bsnes-plus/hx421_chip.hpp` | `hx421.hpp` |
| `include/hx421.h` | `hx421.h` |
| `host/bsnes-plus/hx421_chip.serialization.cpp` | (same name) |

Nothing auto-syncs these. Skip the copy and the rebuild silently links the OLD chip against the
new DLL.

**2. Force the affected objects out.** bsnes-plus's `.o` rules use hand-maintained dependency
lists, so a changed `#include`d file can silently not rebuild:

```powershell
cd <bsnes-plus>\bsnes
Remove-Item obj\compatibility\snes-hx421.o, obj\compatibility\snes-cartridge.o -Force
```

**3. Build** (MSYS2 MinGW64 tools on PATH):

```powershell
$env:PATH = "C:\msys64\mingw64\bin;C:\msys64\usr\bin;$env:PATH"
mingw32-make -j4
```

Output is `bsnes/out/bsnes.exe`. `tools/run-hx421.ps1` stages `hx421.dll` beside it on each launch.

### When the chip fails to load, bsnes does NOT fail loudly

It falls back to normal mapping and boots the trigger `.sfc` as an ordinary cart — so the symptom
is "the wrong ROM booted", not "the DLL was rejected". **Read the log first**; the real reason is
on line 1:

```
hx421: ABI mismatch — header 00010000, dll 00010001. refusing to load.
```

That exact case (an additive MINOR bump rejected by a strict-equality check) cost a debugging
round; both sides now compare MAJOR only.

## Make targets (`engine/Makefile`)

| Target | Produces | What it proves |
|---|---|---|
| `make test` | 10 test binaries in `build/` | the component engine (291 checks, 0 failed) |
| `make demo` | `build/out.wav` | `service.c` renders as a unit (WAV + FFT) |
| `make dll` | `build/hx421.dll` | the runtime shim (`runtime/hx421_runtime.c`) links over the engine |
| `make dlltest` | `build/hx421_host_test.exe` → `build/dll_out.wav` | the `hx421.h` ABI works end-to-end via `LoadLibrary` |

`make dll dlltest` chains both. Expected `dlltest` output:

```
loaded: version="hx421-runtime 0.1 (M0)" abi=00010000 (header 00010000)
pulled 88200 frames, rms=431.0, peak=1000
OK: audio flowed through the hx421.dll ABI
```

(peak 1000 == 8000 tone amplitude >> 3 headroom bits — the arithmetic is exact.)

## Notes

- The engine is warning-clean under `-std=c11 -Wall`.
- `hx421.dll` is a mingw/gcc C DLL; hosts (ares, the test harness) load it via `LoadLibraryA` +
  `GetProcAddress` — flat `extern "C"`, so the DLL's compiler need not match the host's.
- Build artifacts live in `engine/build/` (gitignored). `make clean` removes them.
- This is the desktop loop; the same `engine/` sources cross-compile into the M3 firmware
  (`HX421_STATIC`) with an SD/flash reader + DAC sink in place of the stdio reader + `hx421.dll`.
