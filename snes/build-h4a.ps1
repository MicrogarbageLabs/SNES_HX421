<#
  build-h4a.ps1 — pack the H4a audio-seam TONE core for the FXPak Pro.

    .\snes\build-h4a.ps1

  The tone core is h2_base built with -DHX421_AUDIO_TONE (main.qsf). It is the
  base core PLUS the H2 signature ($3F:F000 -> 'H','X','4','2') PLUS a standalone
  441 Hz square wave routed through the proven MSU-1 CIC/I2S DAC back half
  (hx_tone_dac -> dac_mix). So ONE flash tests both halves at once:

     h2_probe.sfc shows READ 72 88 52 50  -> our core is loaded
     a tone comes out of the SNES audio    -> the audio seam works

  This script only PACKS the bitstream Quartus produced. Build it first with:
     cd fpga\build\h2_base
     quartus_sh --flow compile main         (HX421_AUDIO_TONE is set in main.qsf)

  Produces snes\build\fpga_obc1.bi3 (RLE-packed, ready for /sd2snes/).

  Reuses the existing h2_probe.sfc (unchanged — same signature read).

  Public domain (CC0). No warranty.
#>
$ErrorActionPreference = "Stop"
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$repo = Split-Path -Parent $here
$out  = Join-Path $here "build"
New-Item -ItemType Directory -Force $out | Out-Null

function Step($m) { Write-Host "build-h4a: $m" -ForegroundColor Cyan }
function Warn($m) { Write-Host "build-h4a: $m" -ForegroundColor Yellow }

$rbf = Join-Path $repo "fpga\build\h2_base\output_files\main.rbf"
if (-not (Test-Path $rbf)) {
    throw "no tone bitstream at $rbf`n  build it: cd fpga\build\h2_base; quartus_sh --flow compile main"
}
$rbfAge = (Get-Date) - (Get-Item $rbf).LastWriteTime
Step ("using main.rbf ({0:N0} bytes, built {1:N0} min ago)" -f (Get-Item $rbf).Length, $rbfAge.TotalMinutes)

# Confirm an audio macro is actually enabled in the project that produced it.
$qsf = Get-Content (Join-Path $repo "fpga\build\h2_base\main.qsf") -Raw
if ($qsf -match 'VERILOG_MACRO\s+"HX421_AUDIO_MIXER') {
    Step "audio build: HX421_AUDIO_MIXER (real 8-ch mixer -> sine, H4b)"
} elseif ($qsf -match 'VERILOG_MACRO\s+"HX421_AUDIO_TONE') {
    Step "audio build: HX421_AUDIO_TONE (square wave, H4a)"
} else {
    Warn "no HX421_AUDIO_* macro set in main.qsf - this rbf may be the plain base core (no audio test)."
}

$packer = Join-Path $out "rlepack.exe"
$gcc = Get-Command gcc -ErrorAction SilentlyContinue
if (-not $gcc) { $gcc = "C:\msys64\mingw64\bin\gcc.exe" }
& $gcc -O2 -std=c99 -o $packer (Join-Path $repo "tools\hx421_rlepack.c")
$bi3 = Join-Path $out "fpga_obc1.bi3"
& $packer $rbf $bi3
Step ("packed -> {0} ({1:N0} bytes)" -f $bi3, (Get-Item $bi3).Length)

# Make sure the signature probe ROM exists (build-h2 produces it).
$probe = Join-Path $out "h2_probe.sfc"
if (-not (Test-Path $probe)) { Warn "h2_probe.sfc missing - run .\snes\build-h2.ps1 to make it" }

Write-Host ""
Write-Host "On the SD card:" -ForegroundColor Green
Write-Host "  1. BACK UP /sd2snes/fpga_obc1.bi3 first (your H2 core / stock OBC1)"
Write-Host "  2. copy snes\build\fpga_obc1.bi3 -> /sd2snes/"
Write-Host "  3. copy snes\build\h2_probe.sfc  -> anywhere browsable, run it"
Write-Host ""
Write-Host "  Expect BOTH:" -ForegroundColor Green
Write-Host "    screen: READ 72 88 52 50 + OUR LOGIC IS RUNNING   (core loaded)"
Write-Host "    audio : a steady ~441 Hz tone                     (seam works)"
Write-Host ""
Write-Host "  tone but no signature  -> unexpected (different core loaded)"
Write-Host "  signature but no tone  -> seam/wiring issue, isolated to the DAC path"
Write-Host "  neither                -> core did not load (see docs\bringup.md)"
