<#
  encode-fmv.ps1 — encode a video into a self-contained .fmv clip (audio muxed)
  for the HX-421 FMV engine, and optionally pack a whole directory of clips
  into one seekable container.

    .\tools\encode-fmv.ps1 movie.mp4                    # whole video, 15 fps
    .\tools\encode-fmv.ps1 movie.mp4 -Seconds 6         # first 6 seconds
    .\tools\encode-fmv.ps1 movie.mp4 -Fps 20            # 20 fps (needs the siphon)
    .\tools\encode-fmv.ps1 movie.mp4 -Out intro         # -> intro.fmv
    .\tools\encode-fmv.ps1 -PackDir clips -Pack all.hxfp   # pack, no encode

  DEFAULT IS 15 fps, not 20: 15 is the proven path (4 sub-frames, one per SNES
  frame, no siphon). 20 fps needs 3 sub-frames plus the H-blank siphon, which is
  shelved — encode at 20 only when testing that.

  ffmpeg comes from PATH or from ffmpeg.exe next to this script. gcc (MSYS2
  mingw64) is added on demand and only needed to build the tools from source.
  W/H must match hx421_fmv_encode.c and the player's FMV_* constants.
#>
param(
  [Parameter(Position = 0)][string]$Video,
  [int]$Seconds = 0,
  [int]$Fps = 15,
  [string]$Out = "",
  [string]$PackDir = "",      # pack every .fmv in this directory ...
  [string]$Pack = ""          # ... into this container
)
$ErrorActionPreference = "Stop"
$W = 240; $H = 208; $RATE = 44100

$here  = Split-Path -Parent $MyInvocation.MyCommand.Path      # tools\
$repo  = Split-Path -Parent $here
$build = Join-Path $repo "engine\build"
New-Item -ItemType Directory -Force $build | Out-Null

function Need-Gcc {
  if (Get-Command gcc -ErrorAction SilentlyContinue) { return $true }
  $mingw = "C:\msys64\mingw64\bin"
  if (Test-Path (Join-Path $mingw "gcc.exe")) { $env:PATH = "$mingw;C:\msys64\usr\bin;$env:PATH"; return $true }
  return $false
}
# mingw gcc needs a space-free TMP: the default %TEMP% contains the username,
# which has a space here and breaks gcc's intermediates.
function Use-Tmp { $t = Join-Path $env:SystemDrive "mgtmp"; New-Item -ItemType Directory -Force $t | Out-Null; $env:TMP=$t; $env:TEMP=$t; $env:TMPDIR=$t }
function Stale($out, $srcfile) { return -not (Test-Path $out) -or (Get-Item $srcfile).LastWriteTime -gt (Get-Item $out).LastWriteTime }

function Build-Tool($name, $src) {
  $exe = Join-Path $build "$name.exe"
  if ((Test-Path $src) -and (Stale $exe $src)) {
    if (-not (Need-Gcc)) { throw "$name.exe missing and no gcc to build it" }
    Use-Tmp; Write-Host "building $name ..."
    gcc -Wall -O2 -o $exe $src
  }
  if (-not (Test-Path $exe)) { throw "$name.exe not found and could not be built" }
  return $exe
}

# ---- pack-only mode -------------------------------------------------------
if ($PackDir -ne "") {
  if ($Pack -eq "") { throw "-PackDir needs -Pack <out.hxfp>" }
  $packer = Build-Tool "fmvpack" (Join-Path $here "hx421_fmvpack.c")
  & $packer $PackDir $Pack
  & $packer --list $Pack
  return
}

if ($Video -eq "") { throw "give a video, or use -PackDir/-Pack" }
if (-not (Test-Path $Video)) { throw "no such file: $Video" }
if ($Out -eq "") { $Out = [IO.Path]::GetFileNameWithoutExtension($Video) }
if ($Fps -ne 15 -and $Fps -ne 20) { throw "-Fps must be 15 or 20 (the player's two sub-frame layouts)" }
if ($RATE % $Fps -ne 0) { throw "audio rate $RATE must divide evenly by $Fps for an exact A/V interleave" }

if (-not (Get-Command ffmpeg -ErrorAction SilentlyContinue)) {
  if (Test-Path (Join-Path $here "ffmpeg.exe")) { $env:PATH = "$here;$env:PATH" }
  else { throw "ffmpeg not found on PATH or next to this script ($here)" }
}

$enc = Build-Tool "fmv_encode" (Join-Path $here "hx421_fmv_encode.c")
$t   = if ($Seconds -gt 0) { "-t $Seconds " } else { "" }

# cmd.exe is required for the binary ffmpeg|encoder pipe — PowerShell's pipeline
# corrupts binary streams. Invoke by full path: a bare 'cmd' that PATH cannot
# resolve pops Windows' "select an app to open cmd" dialog.
$comspec = if ($env:ComSpec) { $env:ComSpec } else { Join-Path $env:SystemRoot "system32\cmd.exe" }

# 1) audio to a temp raw-PCM file; muxed into the .fmv below, then deleted, so
#    the clip stays a single self-contained file.
$tmpPcm = "$Out.tmp.pcm"
Write-Host "audio -> (temp)  ($RATE Hz s16 stereo)"
& $comspec /c "ffmpeg -hide_banner -loglevel error -y -i `"$Video`" $t-vn -ar $RATE -ac 2 -f s16le `"$tmpPcm`""
$audioArg = if (Test-Path $tmpPcm) { $tmpPcm } else { Write-Host "  (no audio track - encoding silent)"; "none" }

# 2) pipe video frames into the encoder; it interleaves one audio chunk per
#    frame (audio FIRST, so a streamed read serves the mixer before the PPU).
Write-Host "video+audio -> $Out.fmv  (${W}x${H}, $Fps fps, muxed)"
& $comspec /c "ffmpeg -hide_banner -loglevel error -i `"$Video`" $t-vf scale=${W}:${H},fps=$Fps -f rawvideo -pix_fmt rgb24 - | `"$enc`" - `"$Out.fmv`" `"$audioArg`" --fps $Fps"

if (Test-Path $tmpPcm) { Remove-Item $tmpPcm }

Write-Host ""
Write-Host "done: $Out.fmv"
Write-Host "play: .\tools\run-hx421.ps1 -Kernel snes\build\hx421boot.bin -FmvFile $Out.fmv"
Write-Host "pack: .\tools\encode-fmv.ps1 -PackDir <dir> -Pack clips.hxfp"
