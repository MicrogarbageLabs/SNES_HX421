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

# 6. free-running audio subsystem: tick -> render -> sample, no underrun.
Step "free-running audio subsystem (tick-driven, underrun check)"
& iverilog -g2012 -o tb_audio_top.vvp `
    (Join-Path $mixer "hx_cubic.v") (Join-Path $mixer "hx_lerp.v") `
    (Join-Path $mixer "hx_scale.v") (Join-Path $mixer "hx_finalize.v") `
    (Join-Path $mixer "hx_produce.v") (Join-Path $mixer "hx_mixer_seq.v") `
    (Join-Path $mixer "hx_audio_top.v") (Join-Path $here "tb_audio_top.v")
if ($LASTEXITCODE -ne 0) { $anyfail = $true; Write-Host "hx_audio_top compile FAILED" -ForegroundColor Red }
else {
    & vvp tb_audio_top.vvp | Where-Object { $_ -match "co-sim|^RESULT|^MISMATCH" } | ForEach-Object {
        if ($_ -match "^RESULT: PASS") { Write-Host $_ -ForegroundColor Green }
        elseif ($_ -match "^RESULT: FAIL" -or $_ -match "^MISMATCH") { Write-Host $_ -ForegroundColor Red; $anyfail = $true }
        else { Write-Host $_ }
    }
}

# 7. mixer under adversarial PSRAM contention (renderer hammering the bus).
Step "mixer under full PSRAM contention (arbiter, underrun check)"
& iverilog -g2012 -o tb_arb.vvp `
    (Join-Path $mixer "hx_cubic.v") (Join-Path $mixer "hx_lerp.v") `
    (Join-Path $mixer "hx_scale.v") (Join-Path $mixer "hx_finalize.v") `
    (Join-Path $mixer "hx_produce.v") (Join-Path $mixer "hx_mixer_seq.v") `
    (Join-Path $mixer "hx_audio_top.v") (Join-Path $mixer "hx_psram_arb.v") (Join-Path $here "tb_arb.v")
if ($LASTEXITCODE -ne 0) { $anyfail = $true; Write-Host "hx_psram_arb compile FAILED" -ForegroundColor Red }
else {
    & vvp tb_arb.vvp | Where-Object { $_ -match "co-sim|^RESULT|^MISMATCH" } | ForEach-Object {
        if ($_ -match "^RESULT: PASS") { Write-Host $_ -ForegroundColor Green }
        elseif ($_ -match "^RESULT: FAIL" -or $_ -match "^MISMATCH") { Write-Host $_ -ForegroundColor Red; $anyfail = $true }
        else { Write-Host $_ }
    }
}

# 8. audio SEAM (H4a): tone -> dac_mix -> I2S. Not a golden-vector co-sim (there
#    is no C reference for the DAC); a self-checking plumbing test that our sample
#    source reaches the I2S output standalone (no MSU). Guards the seam wiring.
Step "audio seam: tone through dac_mix to I2S (H4a bring-up)"
$fpga = Split-Path -Parent (Split-Path -Parent $mixer)   # ...\fpga
$h2base = Join-Path $fpga "build\h2_base"
& iverilog -g2012 -o tb_tone_dac.vvp `
    (Join-Path $h2base "hx_tone_dac.v") (Join-Path $h2base "dac_mix.v") (Join-Path $here "tb_tone_dac.v")
if ($LASTEXITCODE -ne 0) { $anyfail = $true; Write-Host "tb_tone_dac compile FAILED" -ForegroundColor Red }
else {
    & vvp tb_tone_dac.vvp | Where-Object { $_ -match "seam:|^RESULT|^FAIL" } | ForEach-Object {
        if ($_ -match "^RESULT: PASS") { Write-Host $_ -ForegroundColor Green }
        elseif ($_ -match "^RESULT: FAIL" -or $_ -match "^FAIL") { Write-Host $_ -ForegroundColor Red; $anyfail = $true }
        else { Write-Host $_ }
    }
}

# 9. audio path (H4b): the REAL mixer -> dac_mix -> I2S, playing a baked sine.
#    Self-checking: config FSM completes, mixer produces frames, and the sample
#    handed to the DAC is a full-amplitude sine at the expected pitch.
Step "audio path: real mixer through dac_mix (H4b)"
Copy-Item (Join-Path $h2base "sine128.hex") (Join-Path $work "sine128.hex") -Force
& iverilog -g2012 -o tb_mixer_dac.vvp `
    (Join-Path $h2base "hx_mixer_dac.v") (Join-Path $h2base "dac_mix.v") `
    (Join-Path $mixer "hx_mixer_seq.v") (Join-Path $mixer "hx_produce.v") `
    (Join-Path $mixer "hx_cubic.v") (Join-Path $mixer "hx_lerp.v") `
    (Join-Path $mixer "hx_scale.v") (Join-Path $mixer "hx_finalize.v") (Join-Path $here "tb_mixer_dac.v")
if ($LASTEXITCODE -ne 0) { $anyfail = $true; Write-Host "tb_mixer_dac compile FAILED" -ForegroundColor Red }
else {
    & vvp tb_mixer_dac.vvp | Where-Object { $_ -match "mixer-dac:|^RESULT|^FAIL" } | ForEach-Object {
        if ($_ -match "^RESULT: PASS") { Write-Host $_ -ForegroundColor Green }
        elseif ($_ -match "^RESULT: FAIL" -or $_ -match "^FAIL") { Write-Host $_ -ForegroundColor Red; $anyfail = $true }
        else { Write-Host $_ }
    }
}

Pop-Location
if ($anyfail) { Write-Host "`nSOME DUTS FAILED" -ForegroundColor Red; exit 1 }
else { Write-Host "`nALL MIXER STAGES BIT-EXACT" -ForegroundColor Green }
