#!/bin/sh
# build-media-test.sh — host-build + run the media service test (no hardware).
#
# The media service (hx421_media) + arbiter (hx421_stream) are pure logic with
# function-pointer platform seams, so the whole play/prime/offload/priority/
# seek/gain/stop path is verifiable on the host against mock SD/PSRAM/mixer.
# The sd2snes bindings (hx421_media_fw.c: fpga_sddma, FatFs, the mailbox) are the
# only hardware-gated part and are excluded here.
#
# Usage:  sh firmware/media/build-media-test.sh
#   (drive from PowerShell with $env:TMP=$env:TEMP='C:\mgtmp' on this machine, or
#    any shell where the native gcc can write its temp files.)
set -e
export PATH=/c/msys64/mingw64/bin:/c/msys64/usr/bin:$PATH
mkdir -p /c/mgtmp
export TMPDIR=/c/mgtmp
: "${TMP:=C:/mgtmp}"; : "${TEMP:=C:/mgtmp}"; export TMP TEMP

here=$(cd "$(dirname "$0")" && pwd)          # firmware/media
repo=$(cd "$here/../.." && pwd)              # repo root
out="$here/build"
mkdir -p "$out"

CC=${CC:-gcc}
"$CC" -std=c11 -Wall -Wextra \
  -I"$repo/firmware/media" -I"$repo/firmware/audio" \
  "$repo/tools/hx421_media_test.c" \
  "$repo/firmware/media/hx421_media.c" \
  "$repo/firmware/audio/hx421_stream.c" \
  -o "$out/hx421_media_test" -lm

"$out/hx421_media_test"
