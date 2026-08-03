#!/bin/sh
# build-game.sh — build a dev-mode game .hxg for HARDWARE. Links the game source
# SEPARATELY at the frozen game region base, verifies game_main really lands
# there (a link/memmap drift check), and packs it into an .hxg with mkhxg. The
# result is dropped on the SD card and loaded with the firmware's `run <file.hxg>`
# CLI command. Same two-stage recipe as the QEMU tests, minus the emulator.
#
# Usage:  sh firmware/dev/build-game.sh [game.c]   (default: game/hello.c)
set -e
export PATH=/c/msys64/mingw64/bin:/c/msys64/usr/bin:$PATH
mkdir -p /c/mgtmp
export TMPDIR=/c/mgtmp TMP='C:/mgtmp' TEMP='C:/mgtmp'

here=$(cd "$(dirname "$0")" && pwd)        # firmware/dev
repo=$(cd "$here/../.." && pwd)
gsrc=${1:-"$here/game/hello.c"}
name=$(basename "$gsrc" .c)
out="$here/game/build"
mkdir -p "$out"

CC=arm-none-eabi-gcc
OBJCOPY=arm-none-eabi-objcopy
# Match the QEMU-proven build: freestanding, no libc/crt0 (link_game.ld has
# ENTRY(game_main), the game references only the syscall table). The game passes
# only ints/pointers through the table, so its float ABI is irrelevant and need
# not match the firmware's hard-float — soft-float here keeps it self-contained.
CFLAGS="-Os -Wall -Wextra -std=c11 -mcpu=cortex-m4 -mthumb -ffreestanding -nostdlib -nostartfiles"

echo "=== stage 1: link the game at the frozen region base ==="
"$CC" $CFLAGS -I"$here" -T "$here/qemu/link_game.ld" \
  "$gsrc" "$here/hx421_gamert.c" -o "$out/$name.elf"
"$OBJCOPY" -O binary "$out/$name.elf" "$out/$name.bin"

# game_main must be first (entry_off 0) AND at HX421_GAME_BASE.
WANT=$(grep -E "define +HX421_GAME_BASE" "$here/hx421_memmap.h" | grep -oE "0x[0-9A-Fa-f]+")
WANT_HEX=$(printf "%08x" "$WANT")
ENTRY=$(arm-none-eabi-nm "$out/$name.elf" | awk '/ game_main$/ {print $1}')
if [ "$ENTRY" != "$WANT_HEX" ]; then
  echo "!! game_main at 0x$ENTRY, but HX421_GAME_BASE is 0x$WANT_HEX (link/memmap drift)"; exit 1
fi
echo "  game_main linked at frozen HX421_GAME_BASE = 0x$WANT_HEX"

echo "=== stage 2: pack into .hxg (entry_off 0, bss from _ebss-_sbss) ==="
# .bss is NOBITS so objcopy -O binary excludes it; the loader zeros bss_size bytes
# right after the image. link_game.ld places .bss immediately after .data and
# brackets it with _sbss/_ebss, so that region is exactly base+load_size onward.
SBSS=$(arm-none-eabi-nm "$out/$name.elf" | awk '/ _sbss$/{print $1}')
EBSS=$(arm-none-eabi-nm "$out/$name.elf" | awk '/ _ebss$/{print $1}')
BSS=$(( 0x$EBSS - 0x$SBSS ))
echo "  bss_size = 0x$EBSS - 0x$SBSS = $BSS bytes"
[ -x "$out/mkhxg" ] || gcc -O2 -std=c11 -o "$out/mkhxg" "$repo/tools/hx421_mkhxg.c"
"$out/mkhxg" "$out/$name.bin" "$out/$name.hxg" 0 "$BSS"

echo
echo "=== result: $out/$name.hxg ==="
ls -la "$out/$name.hxg" "$out/$name.bin"
echo "copy $name.hxg to the SD card and, over the 921600 8N1 serial console, type:  run $name.hxg"
