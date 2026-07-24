<#
  run-cosim.ps1 — generate vectors from the golden C and check the RTL.

    .\fpga\cores\mixer\sim\run-cosim.ps1

  Compiles gen_cubic_vectors.c, runs it to emit cubic_vectors.txt from the
  shipping interp_cubic_q15(), then compiles hx_cubic.v + tb_cubic.v with
  Icarus Verilog and replays the vectors. Bit-exact = PASS.

  Uses Icarus (license-free) rather than Questa: the Questa FSE bundled with
  Quartus Lite needs a separately-provisioned Starter license, and this sim is
  small combinational co-sim that iverilog runs identically.

  Public domain (CC0). No warranty.
#>
$ErrorActionPreference = "Stop"
$here  = Split-Path -Parent $MyInvocation.MyCommand.Path
$mixer = Split-Path -Parent $here
$env:PATH = "C:\msys64\mingw64\bin;" + $env:PATH
$work  = Join-Path $here "work_sim"
New-Item -ItemType Directory -Force $work | Out-Null
Push-Location $work

function Step($m) { Write-Host "cosim: $m" -ForegroundColor Cyan }

# 1. vectors from the golden C
Step "generating vectors from interp_cubic_q15()"
& gcc -O2 -std=c99 -o gen.exe (Join-Path $here "gen_cubic_vectors.c")
& .\gen.exe cubic_vectors.txt
$lines = (Get-Content cubic_vectors.txt | Measure-Object -Line).Lines
Step "$lines vectors written"

# 2. Icarus: compile RTL + testbench, run headless
Step "compiling + simulating (Icarus Verilog)"
& iverilog -g2012 -o cubic_tb.vvp (Join-Path $mixer "hx_cubic.v") (Join-Path $here "tb_cubic.v")
if ($LASTEXITCODE -ne 0) { Pop-Location; throw "iverilog compile failed" }
& vvp cubic_tb.vvp | Where-Object { $_ -match "co-sim|RESULT|MISMATCH" }

Pop-Location
