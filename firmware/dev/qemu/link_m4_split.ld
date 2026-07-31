/* link_m4_split.ld — firmware RAM capped at the low 64K so the high 64K
 * (0x20010000..0x2001FFFF) is free for the loaded game region. Same as
 * link_m4.ld otherwise. The embedded game .hxg rides in FLASH. */
MEMORY {
  FLASH (rx) : ORIGIN = 0x08000000, LENGTH = 1024K
  RAM  (rwx) : ORIGIN = 0x20000000, LENGTH = 64K     /* firmware only */
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
