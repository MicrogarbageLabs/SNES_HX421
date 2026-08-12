<#
  build-hmbox.ps1 — build the HX-421 mailbox test ROM (registered core).

    .\snes\build-hmbox.ps1

  Produces snes\build\hmbox.sfc: a LoROM image with an HX-421 header (carttype
  0xE4) so OUR firmware programs /sd2snes/fpga_hx421.bi3 (its own slot, OBC1
  untouched). It writes a command block + doorbell to $3F:F1xx and reads it back.
  Requires the media firmware flashed (registers 0xE4 + runs the stream arbiter).
  See docs\bringup.md.

  Public domain (CC0). No warranty.
#>
$ErrorActionPreference = "Stop"
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$out  = Join-Path $here "build"
New-Item -ItemType Directory -Force $out | Out-Null
function Step($m) { Write-Host "build-hmbox: $m" -ForegroundColor Cyan }
function Warn($m) { Write-Host "build-hmbox: $m" -ForegroundColor Yellow }

if (-not (Get-Command ca65 -ErrorAction SilentlyContinue)) {
    throw "ca65 not found on PATH. Install the cc65 suite (ca65 + ld65)."
}

$sfc = Join-Path $out "hmbox.sfc"
ca65 --cpu 65816 -I $here -o "$out\hmbox.o"        "$here\hmbox.s"
ca65 --cpu 65816 -o "$out\hmbox_header.o" "$here\hmbox_header.s"
ld65 -C "$here\h1_lorom.cfg" -o $sfc "$out\hmbox.o" "$out\hmbox_header.o"
Step "linked $sfc ($((Get-Item $sfc).Length) bytes)"

# ---- patch the SNES checksum (helps the LoROM header win the score) ----
$bytes = [System.IO.File]::ReadAllBytes($sfc)
$sum = 0; foreach ($b in $bytes) { $sum += $b }
$sum = $sum -band 0xFFFF
$comp = $sum -bxor 0xFFFF
$hdr = 0x7FC0
$bytes[$hdr + 0x1C] = [byte]($comp -band 0xFF)
$bytes[$hdr + 0x1D] = [byte](($comp -shr 8) -band 0xFF)
$bytes[$hdr + 0x1E] = [byte]($sum -band 0xFF)
$bytes[$hdr + 0x1F] = [byte](($sum -shr 8) -band 0xFF)
[System.IO.File]::WriteAllBytes($sfc, $bytes)
Step ("checksum {0:X4}" -f $sum)

$type = $bytes[$hdr + 0x16]
if ($type -ne 0xE4) { throw ("carttype is {0:X2}, expected E4 (HX-421 registered core)" -f $type) }
Step "carttype E4 -> our firmware selects /sd2snes/fpga_hx421.bi3"

# The mailbox window must land on ROM FILLER, or the "core not loaded" case would
# read real ROM bytes instead of $FF. Bank $3F of a 128 KB LoROM mirrors to bank
# $03, so $3F:F100 is file offset 3*$8000 + $7100 = $1F100.
$mboff = 0x1F100
$fill = $bytes[$mboff..($mboff+3)]
if (($fill | Where-Object { $_ -ne 0xFF }).Count -ne 0) {
    $asHex = ($fill | ForEach-Object { "{0:X2}" -f $_ }) -join " "
    Warn "file offset 1F100 is $asHex, not FF filler - the 'core not loaded' case may not read 255"
} else {
    Step "file offset 1F100 is FF filler - the 'core not loaded' case reads 255 255 255 255"
}

Write-Host ""
Write-Host "On the SD card (our media firmware flashed):" -ForegroundColor Green
Write-Host "  1. flash firmware.img (registers 0xE4 + runs the stream arbiter) - OBC1 untouched"
Write-Host "  2. copy snes\build\fpga_hx421.bi3 -> /sd2snes/  (its own slot)"
Write-Host "  3. copy snes\build\hmbox.sfc -> anywhere browsable, run it"
Write-Host ""
Write-Host "  READ 17 34 51 68  + MAILBOX OK  = registered core loaded, SNES write reached mailbox"
Write-Host "     PEND 0 = M4 arbiter polled+consumed it; PEND 1 = no consumer"
Write-Host "  READ 0 0 0 0                    = core loaded, SNES writes not reaching it"
Write-Host "  READ 255 255 255 255            = our core is not loaded"
