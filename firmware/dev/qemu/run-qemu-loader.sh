#!/bin/sh
# run-qemu-loader.sh — tier-3 execution test of the RAM LOADER (docs/dev-mode.md
# step 3). Two-stage build: link a game SEPARATELY at the game region base, pack
# it into an .hxg, embed it in the firmware, then let the loader copy it into
# the region and JUMP. PASS if the loaded code prints "QEMU PASS".
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

# --- stage 1: the game, linked at the game region base, -> raw binary ---
"$CC" $CFLAGS -I"$dev" -T "$here/link_game.ld" \
  "$here/game_blob.c" "$dev/hx421_gamert.c" -o "$out/game.elf"
"$OBJCOPY" -O binary "$out/game.elf" "$out/game.bin"

# game_main is forced first by the link script, so entry_off is 0. Verify.
ENTRY=$(arm-none-eabi-nm "$out/game.elf" | awk '/ game_main$/ {print $1}')
if [ "$ENTRY" != "20010000" ]; then
  echo "WARN: game_main at $ENTRY, expected 20010000 (entry not first)"; exit 1
fi

# --- pack into .hxg (entry_off 0, bss 0: the game binds g_sys before reading) ---
gcc -O2 -std=c11 -o "$out/mkhxg" "$repo/tools/hx421_mkhxg.c"
"$out/mkhxg" "$out/game.bin" "$out/game.hxg" 0 0

# --- embed the .hxg as a linkable blob (symbols _binary_game_hxg_start/_end) ---
( cd "$out" && "$OBJCOPY" -I binary -O elf32-littlearm -B arm game.hxg game_hxg.o )

# --- stage 2: firmware with the loader + the embedded game ---
ELF="$out/qemu_loader.elf"
"$CC" $CFLAGS -I"$dev" -I"$here" -T "$here/link_m4_split.ld" \
  "$here/startup_m4.c" "$here/qemu_loader_main.c" "$here/qemu_hostsys.c" \
  "$dev/hx421_loader.c" "$out/game_hxg.o" \
  -o "$ELF"

# --- run ---
log="$out/qemu_loader.log"
qemu-system-arm -machine netduinoplus2 -nographic -semihosting -kernel "$ELF" > "$log" 2>&1 || true
cat "$log"
if grep -q "QEMU PASS" "$log"; then
  echo "== TIER 3 LOADER (QEMU cortex-m4) PASS =="
  exit 0
else
  echo "== TIER 3 LOADER (QEMU cortex-m4) FAIL =="
  exit 1
fi
