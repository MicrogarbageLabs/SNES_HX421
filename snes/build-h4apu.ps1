<#
  build-h4apu.ps1 — assemble the H4a "tone + APU unmute" ROM.

    .\snes\build-h4apu.ps1

  Produces snes\build\h4_tone_apu.sfc. Run it on the tone core (fpga_hx421.bi3
  = the HX421_AUDIO_TONE build). It unmutes the SNES DSP so the tone the FPGA is
  already generating becomes audible. No FPGA rebuild needed.

  Same OBC1 header ($30/$25) as the other probes -> selects the tone core.

  Public domain (CC0). No warranty.
#>
$ErrorActionPreference = "Stop"
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$out  = Join-Path $here "build"
New-Item -ItemType Directory -Force $out | Out-Null
function Step($m) { Write-Host "build-h4apu: $m" -ForegroundColor Cyan }

if (-not (Get-Command ca65 -ErrorAction SilentlyContinue)) {
    throw "ca65 not found on PATH. Install the cc65 suite (ca65 + ld65)."
}

$sfc = Join-Path $out "h4_tone_apu.sfc"
ca65 --cpu 65816 -I $here -o "$out\h4_tone_apu.o" "$here\h4_tone_apu.s"
ca65 --cpu 65816 -o "$out\h4apu_header.o" "$here\h1_header.s"
ld65 -C "$here\h1_lorom.cfg" -o $sfc "$out\h4_tone_apu.o" "$out\h4apu_header.o"
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

$type = $bytes[$hdr + 0x16]
if ($type -ne 0xE4) { throw ("carttype is {0:X2}, expected E4 (HX-421)" -f $type) }
Step "carttype E4 -> selects /sd2snes/fpga_hx421.bi3 (the tone core)"

Write-Host ""
Write-Host "Make sure /sd2snes/fpga_hx421.bi3 is the TONE core (147,775 B), then run" -ForegroundColor Green
Write-Host "snes\build\h4_tone_apu.sfc. You should now HEAR the ~441 Hz tone."
