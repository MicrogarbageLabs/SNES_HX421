<#
  build-h2.ps1 — build the H2 signature probe ROM and pack the H2 core.

    .\snes\build-h2.ps1

  Produces, in snes\build\:
    h2_probe.sfc     LoROM, OBC1 header, reads $F000-$F003 and reports
    fpga_hx421.bi3    the h2_sig bitstream, RLE-packed

  The SAME ROM reads $FF filler on the stock core and 'H','X','4','2' on
  ours, so the two cases cannot be confused. See docs\bringup.md.

  Public domain (CC0). No warranty.
#>
$ErrorActionPreference = "Stop"
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$repo = Split-Path -Parent $here
$out  = Join-Path $here "build"
New-Item -ItemType Directory -Force $out | Out-Null

function Step($m) { Write-Host "build-h2: $m" -ForegroundColor Cyan }
function Warn($m) { Write-Host "build-h2: $m" -ForegroundColor Yellow }

if (-not (Get-Command ca65 -ErrorAction SilentlyContinue)) {
    throw "ca65 not found on PATH. Install the cc65 suite (ca65 + ld65)."
}

$sfc = Join-Path $out "h2_probe.sfc"
ca65 --cpu 65816 -I $here -o "$out\h2_probe.o"  "$here\h2_probe.s"
ca65 --cpu 65816 -o "$out\h2_header.o" "$here\h1_header.s"
ld65 -C "$here\h1_lorom.cfg" -o $sfc "$out\h2_probe.o" "$out\h2_header.o"
Step "linked $sfc ($((Get-Item $sfc).Length) bytes)"

$bytes = [System.IO.File]::ReadAllBytes($sfc)
$sum = 0
foreach ($b in $bytes) { $sum += $b }
$sum = $sum -band 0xFFFF
$comp = $sum -bxor 0xFFFF
$hdr = 0x7FC0
$bytes[$hdr + 0x1C] = [byte]($comp -band 0xFF)
$bytes[$hdr + 0x1D] = [byte](($comp -shr 8) -band 0xFF)
$bytes[$hdr + 0x1E] = [byte]($sum -band 0xFF)
$bytes[$hdr + 0x1F] = [byte](($sum -shr 8) -band 0xFF)
[System.IO.File]::WriteAllBytes($sfc, $bytes)
Step ("checksum {0:X4}" -f $sum)

# The signature window must land on ROM FILLER, or the "stock core" case
# would read real code and the comparison would mean nothing.
# Bank $3F of a 128 KB LoROM mirrors to bank $03, so $3F:F000 is file offset
# 3*$8000 + $7000 = $1F000.
$sigoff = 0x1F000
$fill = $bytes[$sigoff..($sigoff+3)]
$asHex = ($fill | ForEach-Object { "{0:X2}" -f $_ }) -join " "
if (($fill | Where-Object { $_ -ne 0xFF }).Count -ne 0) {
    throw "file offset 1F000 is $asHex, not FF filler - the probe would read real ROM data there"
}
Step "file offset 1F000 is FF FF FF FF - the stock-core case reads filler"

$type = $bytes[$hdr + 0x16]
if ($type -ne 0xE4) { throw ("carttype is {0:X2}, expected E4 (HX-421)" -f $type) }
Step "carttype E4 -> selects /sd2snes/fpga_hx421.bi3"

# ---- an emulator-testable twin -------------------------------------------
# bsnes-plus reads the SAME header and instantiates its OBC1 chip, then
# access-violates on a ROM that is not an OBC1 game. The coprocessor header
# is only needed by the FXPak for core SELECTION, so the emulator build is
# byte-identical apart from carttype $00 and its checksum. It exercises the
# read, compare and display paths; only the FPGA half goes untested.
$emu = Join-Path $out "h2_probe_emu.sfc"
$eb = [System.IO.File]::ReadAllBytes($sfc)
$eb[$hdr + 0x16] = 0x00
$eb[$hdr + 0x1C] = 0xFF; $eb[$hdr + 0x1D] = 0xFF
$eb[$hdr + 0x1E] = 0x00; $eb[$hdr + 0x1F] = 0x00
$esum = 0
foreach ($b in $eb) { $esum += $b }
$esum = $esum -band 0xFFFF
$ecomp = $esum -bxor 0xFFFF
$eb[$hdr + 0x1C] = [byte]($ecomp -band 0xFF)
$eb[$hdr + 0x1D] = [byte](($ecomp -shr 8) -band 0xFF)
$eb[$hdr + 0x1E] = [byte]($esum -band 0xFF)
$eb[$hdr + 0x1F] = [byte](($esum -shr 8) -band 0xFF)
[System.IO.File]::WriteAllBytes($emu, $eb)
Step ("wrote h2_probe_emu.sfc (carttype 00, checksum {0:X4}) for bsnes" -f $esum)

$rbf = Join-Path $repo "fpga\build\h2_sig\output_files\main.rbf"
if (-not (Test-Path $rbf)) { Warn "no h2_sig bitstream at $rbf"; exit 0 }

$packer = Join-Path $out "rlepack.exe"
$gcc = Get-Command gcc -ErrorAction SilentlyContinue
if (-not $gcc) { $gcc = "C:\msys64\mingw64\bin\gcc.exe" }
& $gcc -O2 -std=c99 -o $packer (Join-Path $repo "tools\hx421_rlepack.c")
& $packer $rbf (Join-Path $out "fpga_hx421.bi3")

Write-Host ""
Write-Host "On the SD card:" -ForegroundColor Green
Write-Host "  1. copy snes\build\fpga_hx421.bi3 -> /sd2snes/  (back up the old one)"
Write-Host "  2. copy snes\build\h2_probe.sfc  -> anywhere browsable"
Write-Host "  3. run it"
Write-Host ""
Write-Host "  READ 72 88 52 50  + OUR LOGIC IS RUNNING   = H2 pass"
Write-Host "  READ 255 255 255 255 + STOCK CORE          = our core is not loaded"
