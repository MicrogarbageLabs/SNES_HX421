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
