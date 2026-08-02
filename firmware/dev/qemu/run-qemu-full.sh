#!/bin/sh
# run-qemu-full.sh — the capstone: the real sysimpl table builder + the loader,
# driving a loaded game on cortex-m4. Same two-stage build as the loader test
# (game linked separately, packed, embedded), but the firmware uses
# hx421_sys_build() instead of a mock table. PASS if the loaded game runs
# through the sysimpl-built table with the arena + sandbox exercised.
set -e
export PATH=/c/msys64/mingw64/bin:/c/msys64/usr/bin:$PATH
here="$(cd "$(dirname "$0")" && pwd)"
dev="$here/.."
repo="$dev/../.."
out="${OUT:-$here/build}"
mkdir -p "$out"

CC=arm-none-eabi-gcc
OBJCOPY=arm-none-eabi-objcopy
CFLAGS="-Os -Wall -Wextra -std=c11 -mcpu=cortex-m4 -mthumb -ffreestanding -nostdlib -nostartfiles"

# stage 1: game -> .bin (linked at the frozen region base)
"$CC" $CFLAGS -I"$dev" -T "$here/link_game.ld" \
  "$here/game_blob.c" "$dev/hx421_gamert.c" -o "$out/game.elf"
"$OBJCOPY" -O binary "$out/game.elf" "$out/game.bin"

WANT_EXPR=$("$CC" -I"$dev" -E -P -x c - <<'EOF' | tr -d ' \r' | tr -d 'uU'
#include "hx421_memmap.h"
HX421_GAME_BASE
EOF
)
WANT_HEX=$(printf '%x' "$(( WANT_EXPR ))")
ENTRY=$(arm-none-eabi-nm "$out/game.elf" | awk '/ game_main$/ {print $1}')
[ "$ENTRY" = "$WANT_HEX" ] || { echo "FAIL: game at $ENTRY, frozen base $WANT_HEX"; exit 1; }

# pack + embed
gcc -O2 -std=c11 -o "$out/mkhxg" "$repo/tools/hx421_mkhxg.c"
"$out/mkhxg" "$out/game.bin" "$out/game.hxg" 0 0
( cd "$out" && "$OBJCOPY" -I binary -O elf32-littlearm -B arm game.hxg game_hxg.o )

# stage 2: firmware = startup + full main + sysimpl + loader + embedded game
ELF="$out/qemu_full.elf"
"$CC" $CFLAGS -I"$dev" -I"$here" -T "$here/link_m4_split.ld" \
  "$here/startup_m4.c" "$here/qemu_full_main.c" \
  "$dev/hx421_sysimpl.c" "$dev/hx421_loader.c" "$out/game_hxg.o" \
  -o "$ELF"

log="$out/qemu_full.log"
qemu-system-arm -machine netduinoplus2 -nographic -semihosting -kernel "$ELF" > "$log" 2>&1 || true
cat "$log"
if grep -q "QEMU PASS" "$log"; then
  echo "== TIER 3 FULL CHAIN (QEMU cortex-m4) PASS =="; exit 0
else
  echo "== TIER 3 FULL CHAIN (QEMU cortex-m4) FAIL =="; exit 1
fi
