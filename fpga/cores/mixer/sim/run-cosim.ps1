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

# 3. one-channel datapath: golden sequence from the REAL mixer, compared to
#    the stateful RTL. Different shape from the combinational primitives — links
#    audio_mixer.c + ring_buffer.c, and the DUT pulls in hx_cubic/hx_lerp.
$engine = Resolve-Path (Join-Path $mixer "..\..\..\engine")
Step "channel datapath: golden sequence from the real mixer"
& gcc -O2 -std=c99 -I"$engine" -o genchan.exe `
    (Join-Path $here "gen_chan_vectors.c") `
    (Join-Path $engine "audio\audio_mixer.c") `
    (Join-Path $engine "containers\ring_buffer.c")
if ($LASTEXITCODE -ne 0) { $anyfail = $true; Write-Host "gen_chan build FAILED" -ForegroundColor Red }
else {
    & .\genchan.exe chan_vectors.txt
    & iverilog -g2012 -o tb_chan.vvp `
        (Join-Path $mixer "hx_cubic.v") (Join-Path $mixer "hx_lerp.v") `
        (Join-Path $mixer "hx_chan.v") (Join-Path $here "tb_chan.v")
    if ($LASTEXITCODE -ne 0) { $anyfail = $true; Write-Host "hx_chan compile FAILED" -ForegroundColor Red }
    else {
        & vvp tb_chan.vvp | Where-Object { $_ -match "co-sim|^RESULT|^MISMATCH" } | ForEach-Object {
            if ($_ -match "^RESULT: PASS") { Write-Host $_ -ForegroundColor Green }
            elseif ($_ -match "^RESULT: FAIL" -or $_ -match "^MISMATCH") { Write-Host $_ -ForegroundColor Red; $anyfail = $true }
            else { Write-Host $_ }
        }
    }
}

# 4. full 8-channel render: golden scene from the real mixer, compared to the
#    time-multiplexed engine (pulls in every mixer submodule).
Step "8-channel render: golden scene from the real mixer"
& gcc -O2 -std=c99 -I"$engine" -o genmix.exe `
    (Join-Path $here "gen_mix_vectors.c") `
    (Join-Path $engine "audio\audio_mixer.c") `
    (Join-Path $engine "containers\ring_buffer.c")
if ($LASTEXITCODE -ne 0) { $anyfail = $true; Write-Host "gen_mix build FAILED" -ForegroundColor Red }
else {
    & .\genmix.exe mix_vectors.txt
    & iverilog -g2012 -o tb_mix.vvp `
        (Join-Path $mixer "hx_cubic.v") (Join-Path $mixer "hx_lerp.v") `
        (Join-Path $mixer "hx_scale.v") (Join-Path $mixer "hx_finalize.v") `
        (Join-Path $mixer "hx_mixer.v") (Join-Path $here "tb_mix.v")
    if ($LASTEXITCODE -ne 0) { $anyfail = $true; Write-Host "hx_mixer compile FAILED" -ForegroundColor Red }
    else {
        & vvp tb_mix.vvp | Where-Object { $_ -match "co-sim|^RESULT|^MISMATCH" } | ForEach-Object {
            if ($_ -match "^RESULT: PASS") { Write-Host $_ -ForegroundColor Green }
            elseif ($_ -match "^RESULT: FAIL" -or $_ -match "^MISMATCH") { Write-Host $_ -ForegroundColor Red; $anyfail = $true }
            else { Write-Host $_ }
        }
    }
}

# 5. latency-tolerant engine: SAME golden scene, but reads answered late by a
#    modelled PSRAM. Output must be identical at every latency (1/7/12 cycles).
Step "latency-tolerant render: same scene under modelled PSRAM latency"
& iverilog -g2012 -o tb_mix_seq.vvp `
    (Join-Path $mixer "hx_cubic.v") (Join-Path $mixer "hx_lerp.v") `
    (Join-Path $mixer "hx_scale.v") (Join-Path $mixer "hx_finalize.v") `
    (Join-Path $mixer "hx_produce.v") (Join-Path $mixer "hx_mixer_seq.v") (Join-Path $here "tb_mix_seq.v")
if ($LASTEXITCODE -ne 0) { $anyfail = $true; Write-Host "hx_mixer_seq compile FAILED" -ForegroundColor Red }
else {
    foreach ($lat in 1, 7, 12) {
        & vvp tb_mix_seq.vvp "+LAT=$lat" | Where-Object { $_ -match "co-sim|^RESULT|^MISMATCH" } | ForEach-Object {
            if ($_ -match "^RESULT: PASS") { Write-Host $_ -ForegroundColor Green }
            elseif ($_ -match "^RESULT: FAIL" -or $_ -match "^MISMATCH") { Write-Host $_ -ForegroundColor Red; $anyfail = $true }
            else { Write-Host $_ }
        }
    }
}

Pop-Location
if ($anyfail) { Write-Host "`nSOME DUTS FAILED" -ForegroundColor Red; exit 1 }
else { Write-Host "`nALL MIXER STAGES BIT-EXACT" -ForegroundColor Green }
