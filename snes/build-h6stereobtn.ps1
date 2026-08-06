<#
  build-h6stereobtn.ps1 — assemble the execute-from-WRAM test on the 6b.1b core.
  Boots, unmutes, plays the PSRAM sine, then jml's into a WRAM spin loop so the
  cart bus is 100% free for the mixer. Same OBC1 header -> selects fpga_hx421.bi3.
  Public domain (CC0). No warranty.
#>
$ErrorActionPreference = "Stop"
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$out  = Join-Path $here "build"
New-Item -ItemType Directory -Force $out | Out-Null
function Step($m) { Write-Host "build-h6stereobtn: $m" -ForegroundColor Cyan }
if (-not (Get-Command ca65 -ErrorAction SilentlyContinue)) { throw "ca65 not found on PATH." }

$sfc = Join-Path $out "h6_stereo_btn.sfc"
ca65 --cpu 65816 -I $here -o "$out\h6_stereo_btn.o" "$here\h6_stereo_btn.s"
ca65 --cpu 65816 -o "$out\h6_stereo_btn_header.o" "$here\h1_header.s"
ld65 -C "$here\h6_1b_lorom.cfg" -o $sfc "$out\h6_stereo_btn.o" "$out\h6_stereo_btn_header.o"
Step "linked $sfc ($((Get-Item $sfc).Length) bytes)"

$bytes = [System.IO.File]::ReadAllBytes($sfc)

# verify the sine landed at file offset 0x2000 (raw PSRAM MIX_WAVE_BASE).
$w0  = [int]$bytes[0x2000] -bor ([int]$bytes[0x2001] -shl 8)   # L0 = 0
$w64 = [int]$bytes[0x2080] -bor ([int]$bytes[0x2081] -shl 8)   # L32 = peak 0x5000
if ($w0 -ne 0x0000 -or $w64 -ne 0x5000) {
    throw ("STEREO clip not at 0x2000: L0={0:X4} L32={1:X4} (expected 0000 / 5000)" -f $w0, $w64)
}
Step ("stereo clip at file 0x2000 verified (L0={0:X4} L-peak={1:X4})" -f $w0, $w64)

$sum = 0; foreach ($b in $bytes) { $sum += $b }
$sum = $sum -band 0xFFFF; $comp = $sum -bxor 0xFFFF; $hdr = 0x7FC0
$bytes[$hdr + 0x1C] = [byte]($comp -band 0xFF); $bytes[$hdr + 0x1D] = [byte](($comp -shr 8) -band 0xFF)
$bytes[$hdr + 0x1E] = [byte]($sum -band 0xFF);  $bytes[$hdr + 0x1F] = [byte](($sum -shr 8) -band 0xFF)
[System.IO.File]::WriteAllBytes($sfc, $bytes)
Step ("checksum {0:X4}" -f $sum)
if ($bytes[$hdr + 0x16] -ne 0xE4) { throw ("carttype {0:X2}, expected E4" -f $bytes[$hdr+0x16]) }
Step "carttype E4 -> selects /sd2snes/fpga_hx421.bi3 (the 6b.1b core)"
Write-Host ""
Write-Host "Flash the 6b.1b fpga_hx421.bi3, then run h6_stereo_btn.sfc." -ForegroundColor Green
Write-Host "The SNES jumps into WRAM -> mixer gets full bandwidth."
Write-Host "Expect a CLEANER, STEADIER ~344 Hz sine than h6_drain (which ran from ROM)."
