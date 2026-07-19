<#
  build.ps1 — assemble + link the HX-421 milestone-1 boot ROM (cc65).

  Produces a 64 KB window image the hx421 DLL serves at the cart bus:
    .\snes\build.ps1     ->  snes\build\hx421boot.bin

  Needs ca65 + ld65 on PATH (the cc65 suite; here: C:\cc65\bin).

  The DLL loads the $8000-$FFFF upper half of this image into its cart
  window; the SNES fetches the reset vector at $FFFC and runs boot.s,
  which copies kernel.s into WRAM and displays a static test pattern.
  See docs\snes-kernel.md and tools\run-hx421.ps1 (-Kernel).

  Public domain (CC0). No warranty.
#>
$ErrorActionPreference = "Stop"
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$out  = Join-Path $here "build"
New-Item -ItemType Directory -Force $out | Out-Null

if (-not (Get-Command ca65 -ErrorAction SilentlyContinue)) {
    throw "ca65 not found on PATH. Install the cc65 suite (ca65 + ld65)."
}

$outName = "hx421boot.bin"

ca65 --cpu 65816 -o "$out\boot.o"   "$here\boot.s"
ca65 --cpu 65816 -o "$out\kernel.o" "$here\kernel.s"
ld65 -C "$here\hx421.cfg" -o "$out\$outName" "$out\boot.o" "$out\kernel.o"

Write-Host "wrote $out\$outName ($((Get-Item "$out\$outName").Length) bytes)"
