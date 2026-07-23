<#
  build-dma.ps1 — build the H3 DMA rate test ROM.

    .\snes\build-dma.ps1

  Produces snes\build\dma_rate.sfc — a plain LoROM (carttype $00) that runs
  on the STOCK FXPak core. No bitstream swap, nothing on the card to back
  up: it measures a property of the console, not of the cartridge.

  On screen it prints, for 8 KB / 16 KB / 32 KB transfers, the scanlines
  each took and the resulting bytes per line. See docs\bringup.md.

  Public domain (CC0). No warranty.
#>
$ErrorActionPreference = "Stop"
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$out  = Join-Path $here "build"
New-Item -ItemType Directory -Force $out | Out-Null

function Step($m) { Write-Host "build-dma: $m" -ForegroundColor Cyan }

if (-not (Get-Command ca65 -ErrorAction SilentlyContinue)) {
    throw "ca65 not found on PATH. Install the cc65 suite (ca65 + ld65)."
}

$sfc = Join-Path $out "dma_rate.sfc"
ca65 --cpu 65816 -o "$out\dma_rate.o"     "$here\dma_rate.s"
ca65 --cpu 65816 -o "$out\dma_rate_hdr.o" "$here\dma_rate_header.s"
ld65 -C "$here\h1_lorom.cfg" -o $sfc "$out\dma_rate.o" "$out\dma_rate_hdr.o"
Step "linked $sfc ($((Get-Item $sfc).Length) bytes)"

# Checksum: with the complement at $FFFF and the checksum at $0000 the pair
# already contributes 0x1FE, exactly what a real checksum and complement
# contribute, so the plain byte sum IS the checksum. No iteration needed.
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

# Guard: this ROM must NOT declare a coprocessor, or the FXPak would try to
# load a core for it and the test would stop being risk-free.
$type = $bytes[$hdr + 0x16]
if ($type -ne 0x00) { throw ("carttype is {0:X2}, expected 00 (ROM only)" -f $type) }
Step "carttype 00 - runs on the stock core, no bitstream swap"

$reset = [int]$bytes[0x7FFC] + ([int]$bytes[0x7FFD] * 256)
if ($reset -lt 0x8000) { throw ("reset vector is {0:X4}, expected >= 8000" -f $reset) }
Step ("reset vector {0:X4}" -f $reset)

Write-Host ""
Write-Host "Copy snes\build\dma_rate.sfc to the SD card and run it." -ForegroundColor Green
Write-Host ""
Write-Host "  Expect three rows: bytes, scanlines, bytes-per-line."
Write-Host "  bsnes measures ~163 B/line; theory is (1364 master cycles"
Write-Host "  - 40 for DRAM refresh) / 8 = ~165."
Write-Host ""
Write-Host "  If the three PER LINE figures agree, there is no meaningful"
Write-Host "  fixed cost per DMA and the engine can use many small transfers."
Write-Host "  If they climb with size, larger transfers are worth batching."
