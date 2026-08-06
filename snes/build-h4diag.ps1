<#
  build-h4diag.ps1 — assemble the H4a audio-seam diagnostic ROM.

    .\snes\build-h4diag.ps1

  Produces snes\build\h4_diag.sfc — reads the tone core's diagnostic window
  ($3F:F000-F007) twice and displays the signature + per-stage counters/flags so
  we can see which stage of the DAC seam is dead on real hardware. Needs the tone
  core (HX421_AUDIO_TONE) flashed; run it like h2_probe.sfc.

  Same OBC1 header ($30/$25) as h2_probe so the FXPak selects /sd2snes/fpga_hx421.bi3.

  Public domain (CC0). No warranty.
#>
$ErrorActionPreference = "Stop"
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$out  = Join-Path $here "build"
New-Item -ItemType Directory -Force $out | Out-Null
function Step($m) { Write-Host "build-h4diag: $m" -ForegroundColor Cyan }

if (-not (Get-Command ca65 -ErrorAction SilentlyContinue)) {
    throw "ca65 not found on PATH. Install the cc65 suite (ca65 + ld65)."
}

$sfc = Join-Path $out "h4_diag.sfc"
ca65 --cpu 65816 -I $here -o "$out\h4_diag.o"   "$here\h4_diag.s"
ca65 --cpu 65816 -o "$out\h4_header.o" "$here\h1_header.s"
ld65 -C "$here\h1_lorom.cfg" -o $sfc "$out\h4_diag.o" "$out\h4_header.o"
Step "linked $sfc ($((Get-Item $sfc).Length) bytes)"

# checksum fixup (LoROM, header at $7FC0)
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
Write-Host "Copy snes\build\h4_diag.sfc to the SD card and run it (tone core flashed)." -ForegroundColor Green
Write-Host "Report the two diagnostic rows (R1 / R2: TICK TONE SDO STAT)."
