/* link_game.ld — link the loadable game to RUN AT the game region base. Load
 * address == run address (the unified-execution model): objcopy -O binary gives
 * exactly the bytes the loader copies to 0x20010000. game_main is forced first,
 * so entry_off is 0. .bss is emitted after .data; this game binds g_sys before
 * reading it, so it needs no bss zeroing (the header sets bss_size 0). */
MEMORY { GAME (rwx) : ORIGIN = 0x20010000, LENGTH = 64K }
ENTRY(game_main)
SECTIONS {
  . = 0x20010000;
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
