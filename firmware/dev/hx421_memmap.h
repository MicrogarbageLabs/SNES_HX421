/* ============================================================
 *  hx421_memmap.h — the FROZEN RAM map for dev-mode game loading.
 *
 *  Single source of truth: the loader, the firmware link, and the game link
 *  all derive the region from here, and the build asserts the game linked at
 *  HX421_GAME_BASE (run-qemu-loader.sh), so the three can't drift.
 *
 *  STM32F401xC (64 KB SRAM — the FXPak's part):
 *
 *     0x20000000  +------------------------------+
 *                 |  FIRMWARE working set  32 KB |  USB, terminal, FatFs,
 *                 |                              |  streaming CONTROL (data is
 *                 |                              |  in PSRAM), baked-libc state,
 *     0x20008000  +------------------------------+  stack. Comfortable at 32 KB.
 *                 |  GAME REGION           32 KB |  code + rodata + data + bss +
 *                 |  (load == run, in place)     |  heap + stack, loaded from SD.
 *     0x2000FFFF  +------------------------------+
 *
 *  The game region is FROZEN. Games link at HX421_GAME_BASE and are built to fit
 *  HX421_GAME_SIZE, so a .bin is portable across firmware builds. A game with
 *  little code fits a 16K/16K code/data split easily; the region caps the total
 *  at 32 KB. Because rich services (libc, actor lib, FMV, input, asset loading,
 *  2D/3D hooks) live ONCE in firmware and are reached through the syscall table,
 *  the game binary stays tiny and 32 KB is generous.
 *
 *  A later F411 (128 KB) swap EXTENDS the region: raise HX421_GAME_SIZE. Growing
 *  it is backward compatible — a game built for 32 KB runs unchanged in a larger
 *  region. Only shrinking below what a game was built for would break it.
 *
 *  Public domain (CC0). No warranty.
 * ============================================================ */

#ifndef HX421_MEMMAP_H
#define HX421_MEMMAP_H

#define HX421_RAM_BASE    0x20000000u
#define HX421_FW_SIZE     (32u * 1024u)                     /* firmware working set  */
#define HX421_GAME_BASE   (HX421_RAM_BASE + HX421_FW_SIZE)  /* 0x20008000            */
#define HX421_GAME_SIZE   (32u * 1024u)                     /* frozen; F411 extends  */
#define HX421_GAME_STACK  (4u * 1024u)                      /* reserved in the region */

#endif /* HX421_MEMMAP_H */
