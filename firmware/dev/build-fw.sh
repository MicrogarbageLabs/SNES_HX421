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
export TMPDIR=/c/mgtmp TMP=/c/mgtmp TEMP=/c/mgtmp

here=$(cd "$(dirname "$0")" && pwd)          # firmware/dev
repo=$(cd "$here/../.." && pwd)              # repo root
src="$repo/third_party/sd2snes/src"
[ -d "$src" ] || { echo "!! sd2snes submodule not checked out: $src"; exit 1; }

echo "=== copy dev-mode sources + headers into the firmware tree ==="
cp "$here/hx421_loader.c" "$here/hx421_sysimpl.c" "$here/hx421_fw.c" "$src/"
cp "$here/hx421_syscall.h" "$here/hx421_loader.h" "$here/hx421_sysimpl.h" \
   "$here/hx421_memmap.h" "$here/hx421_fw.h" "$src/"

echo "=== register with the fork's SRC list (idempotent) ==="
if ! grep -q "hx421_fw.c" "$src/Makefile"; then
  sed -i '/^SRC += usbdesc.c/a SRC += hx421_loader.c hx421_sysimpl.c hx421_fw.c' "$src/Makefile"
  echo "  appended SRC += hx421_loader.c hx421_sysimpl.c hx421_fw.c"
else
  echo "  already present"
fi

echo "=== build firmware.stm ==="
cd "$src"
[ -f utils/genhdr ] || gcc -O2 -o utils/genhdr utils/genhdr.c
make CONFIG=config-mk3-stm32

echo
echo "=== result ==="
ls -la obj-mk3-stm32/firmware.stm obj-mk3-stm32/hx421_*.o
