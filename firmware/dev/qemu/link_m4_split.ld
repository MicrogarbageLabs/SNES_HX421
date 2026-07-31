/* link_m4_split.ld — firmware RAM capped at the low 32K (HX421_FW_SIZE) so the
 * game region at 0x20008000 (HX421_GAME_BASE, see hx421_memmap.h) stays clear.
 * This mirrors the real F401xC 64K map: firmware low 32K, game high 32K. The
 * embedded game .hxg rides in FLASH. */
MEMORY {
  FLASH (rx) : ORIGIN = 0x08000000, LENGTH = 1024K
  RAM  (rwx) : ORIGIN = 0x20000000, LENGTH = 32K     /* firmware working set */
}
_estack = ORIGIN(RAM) + LENGTH(RAM);

SECTIONS {
  .isr_vector : { KEEP(*(.isr_vector)) } > FLASH
  .text : { *(.text*) *(.rodata*) *(.rodata.*) } > FLASH
  . = ALIGN(4);
  _sidata = LOADADDR(.data);
  .data : { _sdata = .; *(.data*) . = ALIGN(4); _edata = .; } > RAM AT> FLASH
  .bss  : { _sbss = .; *(.bss*) *(COMMON) . = ALIGN(4); _ebss = .; } > RAM
}
