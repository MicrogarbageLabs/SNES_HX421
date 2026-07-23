<#
  build-h1.ps1 — build the H1 bring-up probe ROM and pack the FPGA core.

    .\snes\build-h1.ps1

  Produces, in snes\build\:
    h1_probe.sfc     128 KB LoROM, header declares OBC1 (map $30 / type $25)
    fpga_obc1.bi3    the baseline bitstream, RLE-packed for the FXPak MCU

  Copy BOTH to the SD card:
    h1_probe.sfc     anywhere you can browse to
    fpga_obc1.bi3    /sd2snes/  -- BACK UP THE EXISTING FILE FIRST

  See docs\bringup.md. Needs ca65 + ld65 on PATH, and gcc for the packer.

  Public domain (CC0). No warranty.
#>
$ErrorActionPreference = "Stop"
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$repo = Split-Path -Parent $here
$out  = Join-Path $here "build"
New-Item -ItemType Directory -Force $out | Out-Null

function Step($m) { Write-Host "build-h1: $m" -ForegroundColor Cyan }
function Warn($m) { Write-Host "build-h1: $m" -ForegroundColor Yellow }

# ---- 1. the probe ROM ----------------------------------------------------
if (-not (Get-Command ca65 -ErrorAction SilentlyContinue)) {
    throw "ca65 not found on PATH. Install the cc65 suite (ca65 + ld65)."
}

$sfc = Join-Path $out "h1_probe.sfc"
ca65 --cpu 65816 -o "$out\h1_probe.o"  "$here\h1_probe.s"
ca65 --cpu 65816 -o "$out\h1_header.o" "$here\h1_header.s"
ld65 -C "$here\h1_lorom.cfg" -o $sfc "$out\h1_probe.o" "$out\h1_header.o"
Step "linked $sfc ($((Get-Item $sfc).Length) bytes)"

# ---- 2. patch the checksum ----------------------------------------------
# With the complement at $FFFF and the checksum at $0000, the two fields
# already contribute 0x1FE to the sum -- exactly what a real checksum and its
# complement contribute. So the plain byte sum IS the checksum, and writing it
# back does not disturb the total. No iteration needed.
$bytes = [System.IO.File]::ReadAllBytes($sfc)
$sum = 0
foreach ($b in $bytes) { $sum += $b }
$sum = $sum -band 0xFFFF
$comp = $sum -bxor 0xFFFF

$hdr = 0x7FC0                      # LoROM header at file offset $7FC0
$bytes[$hdr + 0x1C] = [byte]($comp -band 0xFF)
$bytes[$hdr + 0x1D] = [byte](($comp -shr 8) -band 0xFF)
$bytes[$hdr + 0x1E] = [byte]($sum -band 0xFF)
$bytes[$hdr + 0x1F] = [byte](($sum -shr 8) -band 0xFF)
[System.IO.File]::WriteAllBytes($sfc, $bytes)
Step ("checksum {0:X4}, complement {1:X4}" -f $sum, $comp)

# sanity: the declared map/type must be what smc.c looks for, or the FXPak
# loads fpga_base and the whole experiment silently tests nothing.
$map  = $bytes[$hdr + 0x15]
$type = $bytes[$hdr + 0x16]
if ($map -eq 0x30 -and $type -eq 0x25) {
    Step ("header map={0:X2} carttype={1:X2} -> selects /sd2snes/fpga_obc1.bi3" -f $map, $type)
} else {
    throw ("header map={0:X2} carttype={1:X2} does NOT select the OBC1 core" -f $map, $type)
}

# ---- 3. pack the bitstream ----------------------------------------------
$rbf = Join-Path $repo "fpga\build\baseline_mini\output_files\main.rbf"
if (-not (Test-Path $rbf)) {
    Warn "no bitstream at $rbf -- build the FPGA baseline first; ROM is still usable."
    exit 0
}

$packer = Join-Path $out "rlepack.exe"
$gcc = Get-Command gcc -ErrorAction SilentlyContinue
if (-not $gcc) { $gcc = "C:\msys64\mingw64\bin\gcc.exe" }
if (-not (Test-Path $gcc) -and -not (Get-Command gcc -ErrorAction SilentlyContinue)) {
    Warn "gcc not found -- cannot build the packer. ROM is built; pack manually."
    exit 0
}
& $gcc -O2 -std=c99 -o $packer (Join-Path $repo "tools\hx421_rlepack.c")
& $packer $rbf (Join-Path $out "fpga_obc1.bi3")

# ---- 4. the negative control --------------------------------------------
# "The ROM runs" does NOT prove OUR bitstream is loaded: if the file were
# being ignored, the FPGA would still hold fpga_base from power-on, which
# maps LoROM perfectly well and would run the probe identically.
#
# A MISSING file does not distinguish either: fpga_pgm returns early
# without reconfiguring, so fpga_base stays and the ROM still runs.
#
# A TRUNCATED file does. fpga_pgm streams it, never sees DONE, retries ten
# times and calls led_panic, which is a while(1). The cart hangs and the
# screen stays black. So a black screen with this file is PROOF that the
# file is being read and programmed, and therefore that the working result
# came from our bitstream rather than a stale fpga_base.
$good = Join-Path $out "fpga_obc1.bi3"
$bad  = Join-Path $out "fpga_obc1_TRUNCATED.bi3"
$gb = [System.IO.File]::ReadAllBytes($good)
$half = [int]($gb.Length / 2)
[System.IO.File]::WriteAllBytes($bad, $gb[0..($half - 1)])
Step ("wrote fpga_obc1_TRUNCATED.bi3 ({0} B) - negative control, expect a HANG" -f $half)

Write-Host ""
Write-Host "Ready. On the SD card:" -ForegroundColor Green
Write-Host "  1. BACK UP  /sd2snes/fpga_obc1.bi3  off the card"
Write-Host "  2. copy     snes\build\fpga_obc1.bi3  ->  /sd2snes/"
Write-Host "  3. copy     snes\build\h1_probe.sfc   ->  anywhere browsable"
Write-Host "  4. load h1_probe.sfc and WATCH THE SCREEN"
Write-Host ""
Write-Host "  cycling colour = the 65816 is running our code (H1 PASS)"
Write-Host "  solid red      = init ran, main loop stuck"
Write-Host "  black          = the ROM never executed"
Write-Host ""
Write-Host "  The FXPak Pro is a sealed cart: its status LEDs are inside the"
Write-Host "  shell, so the screen is the only diagnostic. See docs\bringup.md."
