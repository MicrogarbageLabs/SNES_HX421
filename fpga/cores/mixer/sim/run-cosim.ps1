<#
  run-cosim.ps1 — co-simulate the mixer primitives against the C golden model.

    .\fpga\cores\mixer\sim\run-cosim.ps1

  Compiles gen_vectors.c (golden functions verbatim from the mixer), runs it to
  emit one vector file per primitive, then compiles each DUT + its testbench
  with Icarus Verilog and replays the vectors. Bit-exact on every DUT = PASS.

  Icarus rather than Questa: the Questa FSE bundled with Quartus Lite needs a
  separately-provisioned Starter license, and these combinational co-sims run
  identically on iverilog.

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

# 1. one generator, all vector files
Step "generating vectors from the golden C"
& gcc -O2 -std=c99 -o gen.exe (Join-Path $here "gen_vectors.c")
& .\gen.exe

# 2. each DUT: { rtl module, testbench, top }
$duts = @(
    @{ rtl = "hx_cubic.v";    tb = "tb_cubic.v";    top = "tb_cubic"    },
    @{ rtl = "hx_lerp.v";     tb = "tb_lerp.v";     top = "tb_lerp"     },
    @{ rtl = "hx_scale.v";    tb = "tb_scale.v";    top = "tb_scale"    },
    @{ rtl = "hx_finalize.v"; tb = "tb_finalize.v"; top = "tb_finalize" }
)

$anyfail = $false
foreach ($d in $duts) {
    & iverilog -g2012 -o "$($d.top).vvp" (Join-Path $mixer $d.rtl) (Join-Path $here $d.tb)
    if ($LASTEXITCODE -ne 0) { $anyfail = $true; Write-Host "compile FAILED: $($d.rtl)" -ForegroundColor Red; continue }
    $out = & vvp "$($d.top).vvp" | Where-Object { $_ -match "co-sim|^RESULT|^MISMATCH" }
    $out | ForEach-Object {
        # Match only the verdict/detail lines, not the "N mismatches" summary
        # (whose text would otherwise trip a naive MISMATCH match).
        if ($_ -match "^RESULT: PASS") { Write-Host $_ -ForegroundColor Green }
        elseif ($_ -match "^RESULT: FAIL" -or $_ -match "^MISMATCH") { Write-Host $_ -ForegroundColor Red; $anyfail = $true }
        else { Write-Host $_ }
    }
}

Pop-Location
if ($anyfail) { Write-Host "`nSOME DUTS FAILED" -ForegroundColor Red; exit 1 }
else { Write-Host "`nALL MIXER PRIMITIVES BIT-EXACT" -ForegroundColor Green }
