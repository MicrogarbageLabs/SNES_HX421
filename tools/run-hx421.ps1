# ============================================================
#  run-hx421.ps1 -- launch bsnes-plus with the HX-421 coprocessor chip.
#
#  Sets $HX421_ENABLE=1 (the opt-in the bsnes-plus cartridge-detection
#  path checks), refreshes hx421.dll next to bsnes.exe, and launches
#  bsnes on a trigger .sfc. The cart bus is served by hx421.dll's 64 KB
#  window -- with rom_select = HX421_ROM_SMOKE the DLL self-generates a
#  440 Hz diagnostic tone, so the file passed here is just a trigger for
#  bsnes to instantiate a cart. You should HEAR the tone.
#
#  Usage:
#    .\tools\run-hx421.ps1                        # default: microgarbage snes_smoke.sfc
#    .\tools\run-hx421.ps1 -BsnesDir C:\bsnes     # explicit bsnes location
#    .\tools\run-hx421.ps1 -Rom path\to\rom.sfc   # arbitrary trigger ROM
#    .\tools\run-hx421.ps1 -Log hx421.log         # capture stderr (version + RMS)
#    .\tools\run-hx421.ps1 -Interactive           # buttons -> audio commands
#         A/B/X/Y = SFX beeps  Start = music toggle  Select = stop all
#         (composes with -Wav: Start plays the WAV as music, else a built-in tone)
#
#  Resolution order for bsnes:
#    1. -BsnesDir argument         2. $env:BSNES_HOME
#    3. common paths (..\bsnes-plus\bsnes\out, etc.)   4. bsnes.exe on PATH
#
#  Public domain (CC0). No warranty.
# ============================================================

param(
    [string]$BsnesDir,
    [string]$Rom,          # TRIGGER .sfc bsnes loads to instantiate the cart
    [string]$Kernel,       # HX-421 standalone BOOT ROM blob the DLL SERVES as the
                           # cart window bytes (sets $HX421_ROM). Distinct from -Rom:
                           # -Rom is just what bsnes opens; -Kernel is what the SNES
                           # actually executes off the cart bus (snes\build\hx421boot.bin).
    [string]$Log,
    [string]$Wav,          # stream this WAV (44100 Hz) instead of the SMOKE tone
    [switch]$Interactive,  # set $HX421_CMD=1: DLL preloads SFX slots and the
                           # chip issues audio commands on SNES button presses
    [switch]$Map,          # set $HX421_MAP=1: scrolling metatile map (needs -Kernel).
                           # Proves the edge-seam win: after the seed frames the
                           # tilemap costs <=128 B/frame instead of 2048.
    [switch]$Fmv,          # set $HX421_FMV=1: run the FMV band pipeline (needs -Kernel)
    [string]$FmvFile       # a real .fmv (FMV2) to stream; omit for the synthetic
                           # scrolling source. Implies -Fmv. Audio+video interleaved.
)

$ErrorActionPreference = "Stop"
function Write-Step($msg) { Write-Host "run-hx421: $msg" -ForegroundColor Cyan }
function Die($msg) { Write-Host "run-hx421: ERROR -- $msg" -ForegroundColor Red; exit 1 }

# The opt-in flag the bsnes-plus cartridge detection path checks. Without
# this, bsnes uses the normal mapping / mgapi and the hx421 chip is inert.
$env:HX421_ENABLE = "1"
Write-Step "HX421_ENABLE=1"

# Interactive mode: $HX421_CMD tells the DLL to preload its SFX slots and the
# bsnes-plus hx421 chip to issue Hx421AudioCmd's on SNES button presses.
#   A/B/X/Y = SFX beeps (slots 0..3)   Start = music toggle   Select = stop all
# Composes with -Wav: Start plays the -Wav file as music if given, else a tone.
if ($Interactive) {
    $env:HX421_CMD = "1"
    Write-Step "HX421_CMD=1 (interactive: A/B/X/Y=SFX, Start=music toggle, Select=stop all)"
} else {
    Remove-Item Env:\HX421_CMD -ErrorAction SilentlyContinue
}

# Optional: stream a real WAV (looped, through the mixer) instead of the SMOKE
# tone. The DLL reads HX421_WAV when no cfg autostart path is set. Use an
# absolute path to a 44100 Hz WAV (the file streamer doesn't resample yet).
if ($Wav) {
    if (-not (Test-Path $Wav)) { Die "WAV not found: $Wav" }
    $env:HX421_WAV = (Resolve-Path $Wav).Path
    Write-Step "HX421_WAV=$($env:HX421_WAV)"
} else {
    Remove-Item Env:\HX421_WAV -ErrorAction SilentlyContinue
}

# Optional: serve a standalone HX-421 boot ROM as the cart window. The DLL
# reads $HX421_ROM and loads its $8000-$FFFF half into the 64 KB window, so the
# SNES fetches the reset vector from it and runs our 65816 boot->WRAM kernel
# (a static test pattern for milestone 1). Without -Kernel the window stays the
# flat SMOKE/audio buffer. Build the blob: .\snes\build.ps1
if ($Kernel) {
    if (-not (Test-Path $Kernel)) { Die "kernel/boot ROM not found: $Kernel" }
    $env:HX421_ROM = (Resolve-Path $Kernel).Path
    Write-Step "HX421_ROM=$($env:HX421_ROM) (served as the cart window)"
} else {
    Remove-Item Env:\HX421_ROM -ErrorAction SilentlyContinue
}

# Optional: FMV static-frame band demo. The DLL builds a synthetic 240x208 4bpp
# frame in host RAM (the PSRAM analog) and delivers it in 4 subframe bands, one
# per NMI, through the emitted DMA body. Needs -Kernel (the M2 video kernel).
if ($Map) {
    if (-not $Kernel) { Die "-Map needs -Kernel (the video kernel serves the frames)" }
    if ($Fmv -or $FmvFile) { Die "-Map and -Fmv are mutually exclusive" }
    $env:HX421_MAP = "1"
    Write-Step "HX421_MAP=1, scrolling metatile map (seam-only tilemap updates)"
}

if ($Fmv -or $FmvFile) {
    if (-not $Kernel) { Die "-Fmv needs -Kernel (the video kernel serves the bands)" }
    $env:HX421_FMV = "1"
    if ($FmvFile) {
        if (-not (Test-Path $FmvFile)) { Die "FMV file not found: $FmvFile" }
        $env:HX421_FMV_FILE = (Resolve-Path $FmvFile).Path
        Write-Step "HX421_FMV=1, streaming $($env:HX421_FMV_FILE)"
    } else {
        Remove-Item Env:\HX421_FMV_FILE -ErrorAction SilentlyContinue
        Write-Step "HX421_FMV=1 (synthetic scrolling source)"
    }
} else {
    Remove-Item Env:\HX421_FMV -ErrorAction SilentlyContinue
    Remove-Item Env:\HX421_FMV_FILE -ErrorAction SilentlyContinue
}

# Repo root = parent of the dir holding this script (SNES_HX_421).
$RepoRoot = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
Write-Step "repo: $RepoRoot"

# ---- 1. Locate bsnes ---------------------------------------
# Probe the given dir AND the from-source layout (<checkout>/bsnes/out/)
# so pointing at a checkout root Just Works.
function Find-Bsnes($dir) {
    if (-not $dir) { return $null }
    foreach ($sub in @("", "bsnes\out", "out")) {
        $d = if ($sub) { Join-Path $dir $sub } else { $dir }
        foreach ($exe in @("bsnes.exe", "bsnes-plus.exe")) {
            $p = Join-Path $d $exe
            if (Test-Path $p) { return $p }
        }
    }
    return $null
}

$BsnesExe = $null
if ($BsnesDir) {
    $BsnesExe = Find-Bsnes $BsnesDir
    if (-not $BsnesExe) { Die "no bsnes(-plus).exe in $BsnesDir (also checked bsnes\out, out)" }
}
elseif ($env:BSNES_HOME) {
    $BsnesExe = Find-Bsnes $env:BSNES_HOME
    if (-not $BsnesExe) { Die "BSNES_HOME=$($env:BSNES_HOME) has no bsnes(-plus).exe" }
}
else {
    $sourceRoot = Split-Path -Parent $RepoRoot   # ...\Source
    $candidates = @(
        (Join-Path $sourceRoot "bsnes-plus\bsnes\out"),
        (Join-Path $sourceRoot "bsnes-plus"),
        "$env:ProgramFiles\bsnes-plus",
        "${env:ProgramFiles(x86)}\bsnes-plus"
    )
    foreach ($d in $candidates) { $p = Find-Bsnes $d; if ($p) { $BsnesExe = $p; break } }
    if (-not $BsnesExe) {
        $onPath = Get-Command "bsnes.exe","bsnes-plus.exe" -ErrorAction SilentlyContinue | Select-Object -First 1
        if ($onPath) { $BsnesExe = $onPath.Source }
    }
}
if (-not $BsnesExe) {
    Die "couldn't find bsnes(-plus).exe. Pass -BsnesDir <path> or set BSNES_HOME."
}
$BsnesHome = Split-Path -Parent $BsnesExe
Write-Step "bsnes: $BsnesExe"

# ---- 2. Refresh hx421.dll next to bsnes.exe ----------------
# The cart class LoadLibrary's hx421.dll from alongside bsnes.exe.
# Without this refresh, every rebuild needs a manual copy or bsnes runs
# against the previous DLL.
$srcDll = Join-Path $RepoRoot "engine\build\hx421.dll"
$dstDll = Join-Path $BsnesHome "hx421.dll"
if (-not (Test-Path $srcDll)) {
    Write-Step "WARNING: $srcDll not found -- build it: cd engine; make dll (see docs\pc-build.md)"
} else {
    $needCopy = -not (Test-Path $dstDll)
    if (-not $needCopy) { $needCopy = (Get-Item $srcDll).LastWriteTime -gt (Get-Item $dstDll).LastWriteTime }
    if ($needCopy) {
        try {
            Copy-Item -Force $srcDll $dstDll -ErrorAction Stop
            Write-Step "refreshed $dstDll"
        } catch {
            Die "cannot refresh $dstDll -- another bsnes instance still has it loaded? close it and re-run."
        }
    } else {
        Write-Step "hx421.dll already current"
    }
    # hx421.dll needs the mingw libgcc runtime alongside it.
    $gccSrc = "C:\msys64\mingw64\bin\libgcc_s_seh-1.dll"
    $gccDst = Join-Path $BsnesHome "libgcc_s_seh-1.dll"
    if ((-not (Test-Path $gccDst)) -and (Test-Path $gccSrc)) {
        Copy-Item -Force $gccSrc $gccDst; Write-Step "staged libgcc_s_seh-1.dll"
    }
}

# ---- 3. Pick the trigger ROM -------------------------------
# bsnes just needs a valid .sfc to instantiate a cart; the bytes the SNES
# bus sees come from hx421.dll's window. Reuse microgarbage's snes_smoke.sfc
# (what bsnes reliably recognizes), else require -Rom.
if (-not $Rom) {
    $Rom = Join-Path (Split-Path -Parent $RepoRoot) "microgarbage\snes\build\snes_smoke.sfc"
}
if (-not (Test-Path $Rom)) {
    Die "trigger ROM not found: $Rom  (pass -Rom <path\to\any.sfc>)"
}
$RomAbs = (Resolve-Path $Rom).Path
Write-Step "ROM (trigger): $RomAbs"

# ---- 4. Launch ---------------------------------------------
# Prepend an MSYS2 Qt5 bin to PATH if present (from-source bsnes links
# Qt5Widgets.dll dynamically).
$qtCandidates = @("C:\msys64\mingw64\bin","C:\msys64\clang64\bin","$env:MSYS2_HOME\mingw64\bin") |
    Where-Object { $_ -and (Test-Path (Join-Path $_ "Qt5Widgets.dll")) }
if ($qtCandidates) { $env:PATH = "$($qtCandidates[0]);$env:PATH"; Write-Step "Qt5 bin: $($qtCandidates[0])" }

Write-Step "launching... (close the bsnes window to exit -- you should HEAR a 440 Hz tone)"
if ($Log) {
    $LogAbs = if ([System.IO.Path]::IsPathRooted($Log)) { $Log } else { Join-Path (Get-Location).Path $Log }
    Write-Step "stderr (hx421 version + periodic RMS) -> $LogAbs"
    Start-Process -FilePath $BsnesExe -ArgumentList @("`"$RomAbs`"") -WorkingDirectory $BsnesHome -RedirectStandardError $LogAbs
} else {
    Start-Process -FilePath $BsnesExe -ArgumentList @("`"$RomAbs`"") -WorkingDirectory $BsnesHome
}
Write-Step "done."
