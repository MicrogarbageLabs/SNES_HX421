#!/bin/sh
# build-media-fw.sh — integrate the HX-421 MEDIA firmware into the sd2snes fork
# and build firmware.stm/.img. This is the RPG-scope M4 role (docs/architecture-
# pivot.md): the SNES 65816 runs the game in WRAM and mailboxes the FPGA; the M4
# ONLY streams audio/FMV SD->PSRAM for the FPGA mixer. It does NOT run game logic
# — that is the (shelved, 3D-future) firmware/dev/ path, a SEPARATE build.
#
# Same "source lives outside the submodule, copy+patch in before build" pattern
# as firmware/dev/build-fw.sh. Nothing is committed into the submodule.
#
# Usage:  sh firmware/media/build-media-fw.sh
#   (must be driven from a shell whose TMP/TEMP are a WINDOWS-style path — on this
#    machine run it from PowerShell: $env:TMP=$env:TEMP='C:\mgtmp'; then bash this.)
set -e
export PATH=/c/msys64/mingw64/bin:/c/msys64/usr/bin:$PATH
# POSIX tools want /c/...; native cc1/ld want a Windows path (else they fall back
# to an unwritable C:\WINDOWS\Temp). Set both; the Windows ones must come from the
# parent env on this machine (bash `export` of them does not reach native cc1).
mkdir -p /c/mgtmp
export TMPDIR=/c/mgtmp
: "${TMP:=C:/mgtmp}"; : "${TEMP:=C:/mgtmp}"; export TMP TEMP

here=$(cd "$(dirname "$0")" && pwd)          # firmware/media
repo=$(cd "$here/../.." && pwd)              # repo root
audio="$repo/firmware/audio"
engaud="$repo/engine/audio"
src="$repo/third_party/sd2snes/src"
sub="$repo/third_party/sd2snes"
[ -d "$src" ] || { echo "!! sd2snes submodule not checked out: $src"; exit 1; }

echo "=== copy media sources + headers into the firmware tree ==="
cp "$here/hx421_media.c" "$here/hx421_media_fw.c" \
   "$audio/hx421_stream.c" "$audio/hx421_wav.c" "$src/"
cp "$here/hx421_media.h" "$here/hx421_media_fw.h" \
   "$audio/hx421_stream.h" "$audio/hx421_wav.h" "$src/"
# canonical WAV parser hx421_wav wraps. It includes "audio/wav.h" (subdir path);
# the fork's src/ is flat, so copy the header flat and rewrite that one include.
cp "$engaud/wav.h" "$src/wav.h"
sed 's#"audio/wav.h"#"wav.h"#' "$engaud/audio_wav_read.c" > "$src/audio_wav_read.c"

echo "=== apply the media integration patch (SRC list + carttype 0xE4 + main-loop hook) ==="
# One patch: registers our .c on the Makefile SRC list, detects the HX-421 core
# (map 0x30 / carttype 0xE4 -> has_hx421 + FPGA_HX421), and hooks the media loop
# in main.c gated on has_hx421 (like MSU-1). The media build and the firmware/dev/
# game-runner build are mutually exclusive: both touch Makefile/smc/fpga/main, and
# dev-mode ALSO carves stm32f401.ld's RAM (unguarded — it would wrongly shrink RAM
# in a media build). So reset the WHOLE tracked src/ to a pristine base first —
# robust to any prior build, and it makes the apply idempotent. checkout only
# affects tracked files; the untracked copied sources stay.
patch_file="$here/sd2snes-media.patch"
git -C "$sub" checkout -- src 2>/dev/null || true
if git -C "$sub" apply --check "$patch_file" 2>/dev/null; then
  git -C "$sub" apply "$patch_file"
  echo "  applied sd2snes-media.patch"
else
  echo "  !! patch does not apply against the pinned submodule — the fork may have"
  echo "     drifted. Re-generate firmware/media/sd2snes-media.patch."
  exit 1
fi

echo "=== build firmware.stm ==="
cd "$src"
[ -f utils/genhdr ] || gcc -O2 -o utils/genhdr utils/genhdr.c
# VERSION='*' -> a unique build-timestamp CONFIG_VERSION so the SD updater always
# reflashes and the on-screen version confirms which build is live (see the
# firmware/dev notes; same reflash trap applies).
make CONFIG=config-mk3-stm32 VERSION='*'

# mk3-stm32 emits firmware.stm; the FXPak Pro bootloader reads firmware.img.
cp obj-mk3-stm32/firmware.stm obj-mk3-stm32/firmware.img

echo
echo "=== result — copy firmware.img to the SD card /sd2snes/ and COLD-BOOT ==="
echo "    (needs the HX-421 audio core at /sd2snes/fpga_hx421.bi3 and, for the"
echo "     bring-up default, /sd2snes/hx421/music1.wav)"
ls -la obj-mk3-stm32/firmware.img obj-mk3-stm32/firmware.stm
