#!/bin/sh
# build-fw.sh — integrate the dev-mode modules into the sd2snes firmware fork and
# build firmware.stm. The sd2snes tree is a git SUBMODULE, so the source of truth
# for our code is firmware/dev/ (this dir); this script copies it in + registers
# it with the fork's Makefile idempotently, the same way the bsnes-plus build
# pulls Mgapi.* from microgarbage. Nothing here is committed into the submodule.
#
# Proves the integration compiles under the firmware's strict -Werror and links
# into firmware.stm. It does NOT yet cap the linker RAM to carve the game region:
# the firmware's USB buffers (.ahbram, an orphan section) land at ~0x2000C000,
# INSIDE the naive game region (0x20008000-0x2000FFFF), so the region placement is
# a separate memory-layout task (see docs/dev-mode.md). Region addresses are only
# constants here, so the compile+link proof stands regardless.
#
# Usage:  sh firmware/dev/build-fw.sh
set -e
export PATH=/c/msys64/mingw64/bin:/c/msys64/usr/bin:$PATH
# POSIX tools want a /c/... path; native Windows cc1/ld want a Windows path (else
# they fall back to C:\WINDOWS\Temp, which is not writable). Set both.
mkdir -p /c/mgtmp
export TMPDIR=/c/mgtmp
export TMP='C:/mgtmp' TEMP='C:/mgtmp'

here=$(cd "$(dirname "$0")" && pwd)          # firmware/dev
repo=$(cd "$here/../.." && pwd)              # repo root
src="$repo/third_party/sd2snes/src"
[ -d "$src" ] || { echo "!! sd2snes submodule not checked out: $src"; exit 1; }

echo "=== copy dev-mode sources + headers into the firmware tree ==="
cp "$here/hx421_loader.c" "$here/hx421_sysimpl.c" "$here/hx421_fw.c" \
   "$here/hx421_termcdc.c" "$here/hx421_msc.c" "$src/"
cp "$here/hx421_syscall.h" "$here/hx421_loader.h" "$here/hx421_sysimpl.h" \
   "$here/hx421_memmap.h" "$here/hx421_fw.h" "$here/hx421_termcdc.h" \
   "$here/hx421_msc.h" "$src/"

echo "=== apply the firmware-tree patch (SRC list + cli 'run' trigger + linker carve) ==="
# One patch does three things in the fork: registers our .c on the Makefile SRC
# list, adds the `run <file.hxg>` CLI command (the load trigger), and caps the
# linker RAM at 0x20008000 so the top 32K is the game region. Idempotent: skip if
# already applied. The .h files were copied above, so cli.c's #include resolves.
sub=$(cd "$src/.." && pwd)
patch_file="$here/sd2snes-devmode.patch"
if git -C "$sub" apply --check "$patch_file" 2>/dev/null; then
  git -C "$sub" apply "$patch_file"
  echo "  applied sd2snes-devmode.patch"
elif git -C "$sub" apply --reverse --check "$patch_file" 2>/dev/null; then
  echo "  already applied"
else
  echo "  !! patch neither applies nor is already applied — the submodule may have"
  echo "     drifted from the pinned commit. Re-generate firmware/dev/sd2snes-devmode.patch."
  exit 1
fi

echo "=== build firmware.stm ==="
cd "$src"
[ -f utils/genhdr ] || gcc -O2 -o utils/genhdr utils/genhdr.c
# VERSION='*' stamps a build timestamp as CONFIG_VERSION (version.mk) so every dev
# build has a UNIQUE, higher version. That guarantees the SD firmware updater
# actually reflashes (a static 1.11.2 looks identical to a prior custom build and
# can be skipped) and lets the on-screen version confirm which build is running.
make CONFIG=config-mk3-stm32 VERSION='*'

# The mk3-stm32 build names its output firmware.stm, but the FXPak Pro bootloader
# looks for firmware.img on the SD card. The .stm IS the correct STM32 firmware
# (right CPU + valid STM3 header) — only the filename differs — so emit an
# identical firmware.img copy that the bootloader will actually pick up.
cp obj-mk3-stm32/firmware.stm obj-mk3-stm32/firmware.img

echo
echo "=== result — copy firmware.img to the SD card /sd2snes/ and COLD-BOOT ==="
ls -la obj-mk3-stm32/firmware.img obj-mk3-stm32/firmware.stm
