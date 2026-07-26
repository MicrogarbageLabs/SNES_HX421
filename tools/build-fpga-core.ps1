<#
  build-fpga-core.ps1 — build an HX-421 variant of the h2_base core with extra
  VERILOG_MACROs and pack it to a named .bi3, WITHOUT disturbing the default qsf.

  Injects the macros into a scratch copy of main.qsf, runs the Quartus flow, RLE-
  packs output_files/main.rbf, and restores the qsf. Examples:

    # two-channel chord test core (reuses h6_wram.sfc; you hear a fifth):
    ./tools/build-fpga-core.ps1 -Macros "HX421_SECOND_CH=1" -OutBi3 fpga_obc1_chord.bi3

    # streaming core (mixer reads the STM32 ring @0x800000, 64 KB mono loop):
    ./tools/build-fpga-core.ps1 -Macros "HX421_MIX_BASE=8388608","HX421_LOOP_LEN=32768" -OutBi3 fpga_obc1_stream.bi3

  Flash by copying the chosen .bi3 to the SD card as /sd2snes/fpga_obc1.bi3.
  Public domain (CC0). No warranty.
#>
param(
  [string[]]$Macros = @(),
  [string]$OutBi3 = "fpga_obc1_variant.bi3"
)
$ErrorActionPreference = "Stop"
$repo = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
$h2   = Join-Path $repo "fpga\build\h2_base"
$qsf  = Join-Path $h2 "main.qsf"
$bak  = Join-Path $h2 "main.qsf.bak"
function Step($m) { Write-Host "build-fpga-core: $m" -ForegroundColor Cyan }

if (-not (Get-Command quartus_sh -ErrorAction SilentlyContinue)) {
  throw "quartus_sh not on PATH. Add e.g. C:\altera_lite\25.1std\quartus\bin64"
}

# pack tool
$rlepack = Join-Path $repo "tools\rlepack.exe"
if (-not (Test-Path $rlepack)) {
  Step "building rlepack"
  & gcc -O2 -o $rlepack (Join-Path $repo "tools\hx421_rlepack.c")
  if ($LASTEXITCODE -ne 0) { throw "rlepack build failed" }
}

Copy-Item $qsf $bak -Force
try {
  if ($Macros.Count -gt 0) {
    $lines = $Macros | ForEach-Object { "set_global_assignment -name VERILOG_MACRO `"$_`"" }
    Add-Content -Path $qsf -Value ("`n# --- injected by build-fpga-core.ps1 ---") -Encoding utf8
    Add-Content -Path $qsf -Value $lines -Encoding utf8
    Step ("injected macros: {0}" -f ($Macros -join ", "))
  }
  Step "quartus_sh --flow compile main (this takes a few minutes)..."
  Push-Location $h2
  & quartus_sh --flow compile main
  $rc = $LASTEXITCODE
  Pop-Location
  if ($rc -ne 0) { throw "Quartus compile failed (rc=$rc)" }

  $rbf = Join-Path $h2 "output_files\main.rbf"
  if (-not (Test-Path $rbf)) { throw "main.rbf not produced" }
  $out = Join-Path $repo $OutBi3
  & $rlepack $rbf $out
  if ($LASTEXITCODE -ne 0) { throw "rlepack failed" }
  Step ("packed {0} ({1} bytes)" -f $OutBi3, (Get-Item $out).Length)
}
finally {
  Copy-Item $bak $qsf -Force
  Remove-Item $bak -Force
  Step "restored main.qsf"
}
Write-Host ""
Write-Host "Copy $OutBi3 to the SD card as /sd2snes/fpga_obc1.bi3 to flash it." -ForegroundColor Green
