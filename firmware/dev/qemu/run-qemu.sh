#!/bin/sh
# run-qemu.sh — tier-3 execution test (docs/dev-mode.md): build the dev-mode
# syscall boundary as a bare-metal Cortex-M4 image and run it under QEMU.
# Semihosting output goes to stderr; SYS_EXIT stops QEMU. PASS if the image
# prints "QEMU PASS".
#
# Needs arm-none-eabi-gcc and qemu-system-arm (msys mingw64/bin on PATH so cc1
# and the qemu DLLs load).
set -e
export PATH=/c/msys64/mingw64/bin:/c/msys64/usr/bin:$PATH
here="$(cd "$(dirname "$0")" && pwd)"
dev="$here/.."
out="${OUT:-$here/build}"
mkdir -p "$out"

ELF="$out/qemu_syscall.elf"
arm-none-eabi-gcc -Os -Wall -Wextra -std=c11 \
  -mcpu=cortex-m4 -mthumb -ffreestanding -nostdlib -nostartfiles \
  -T "$here/link_m4.ld" -I"$dev" -I"$here" \
  "$here/startup_m4.c" "$here/qemu_syscall_main.c" "$here/qemu_hostsys.c" "$dev/hx421_gamert.c" \
  -o "$ELF"

# Semihosting lands on stderr; capture both and look for the verdict.
log="$out/qemu.log"
qemu-system-arm -machine netduinoplus2 -nographic -semihosting \
  -kernel "$ELF" > "$log" 2>&1 || true

cat "$log"
if grep -q "QEMU PASS" "$log"; then
  echo "== TIER 3 (QEMU cortex-m4) PASS =="
  exit 0
else
  echo "== TIER 3 (QEMU cortex-m4) FAIL =="
  exit 1
fi
