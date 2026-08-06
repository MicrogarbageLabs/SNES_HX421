<#
  build-h6probe.ps1 — assemble the 6b PSRAM-probe ROM.
  Run on the 6b core (HX421_AUDIO_MIXER build = mixer + MIX_RD probe).
  Produces snes\build\h6_probe.sfc. Same OBC1 header -> selects fpga_hx421.bi3.
  Public domain (CC0). No warranty.
#>
$ErrorActionPreference = "Stop"
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$out  = Join-Path $here "build"
New-Item -ItemType Directory -Force $out | Out-Null
function Step($m) { Write-Host "build-h6probe: $m" -ForegroundColor Cyan }
if (-not (Get-Command ca65 -ErrorAction SilentlyContinue)) { throw "ca65 not found on PATH." }

$sfc = Join-Path $out "h6_probe.sfc"
ca65 --cpu 65816 -I $here -o "$out\h6_probe.o" "$here\h6_probe.s"
ca65 --cpu 65816 -o "$out\h6_header.o" "$here\h1_header.s"
ld65 -C "$here\h1_lorom.cfg" -o $sfc "$out\h6_probe.o" "$out\h6_header.o"
Step "linked $sfc ($((Get-Item $sfc).Length) bytes)"

$bytes = [System.IO.File]::ReadAllBytes($sfc)
$sum = 0; foreach ($b in $bytes) { $sum += $b }
$sum = $sum -band 0xFFFF; $comp = $sum -bxor 0xFFFF; $hdr = 0x7FC0
$bytes[$hdr + 0x1C] = [byte]($comp -band 0xFF); $bytes[$hdr + 0x1D] = [byte](($comp -shr 8) -band 0xFF)
$bytes[$hdr + 0x1E] = [byte]($sum -band 0xFF);  $bytes[$hdr + 0x1F] = [byte](($sum -shr 8) -band 0xFF)
[System.IO.File]::WriteAllBytes($sfc, $bytes)
Step ("checksum {0:X4}" -f $sum)
if ($bytes[$hdr + 0x16] -ne 0xE4) { throw ("carttype {0:X2}, expected E4" -f $bytes[$hdr+0x16]) }
Step "carttype E4 -> selects /sd2snes/fpga_hx421.bi3 (the 6b core)"
Write-Host ""
Write-Host "Flash the 6b fpga_hx421.bi3, then run h6_probe.sfc." -ForegroundColor Green
Write-Host "Expect: BRAM sine still audible; screen shows MIX/SNES bytes + READS LIVE + BYTESWAP MATCH."
