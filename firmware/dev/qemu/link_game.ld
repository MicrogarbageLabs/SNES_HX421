/* link_game.ld — link the loadable game to RUN AT the frozen game region base
 * (HX421_GAME_BASE = 0x20008000, see firmware/dev/hx421_memmap.h). Load address
 * == run address (unified execution): objcopy -O binary gives exactly the bytes
 * the loader copies there. game_main is forced first, so entry_off is 0. The
 * build (run-qemu-loader.sh) asserts game_main lands at this address, so a map
 * change in the header without a change here fails the build. */
MEMORY { GAME (rwx) : ORIGIN = 0x20008000, LENGTH = 32K }
ENTRY(game_main)
SECTIONS {
  . = 0x20008000;
  .text : {
    KEEP(*(.text.game_main))   /* entry first -> entry_off 0 */
    *(.text*)
    *(.rodata*)
  } > GAME
  .data : { *(.data*) } > GAME
  _sbss = .;
  .bss  : { *(.bss*) *(COMMON) } > GAME
  _ebss = .;
}
