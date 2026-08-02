/* ============================================================
 *  hx421_loader.h — load a game .bin into the RAM game region and run it.
 *
 *  A game FILE is [Hx421GameHeader][image bytes]. The image is linked to run
 *  AT the game region base (load address == run address — the "unified ROM/RAM
 *  execution" model), so once copied there it just runs; there is no .data
 *  relocation, only a .bss zero. The header tells the loader how much to copy,
 *  how much bss to clear, and where the entry is. See docs/dev-mode.md step 3.
 *
 *  Split so the LOGIC is host-testable and only the jump is target-only:
 *    - hx421_loader_place()  validates + copies + zeroes bss. Pure data;
 *                            host-tested exhaustively including every reject.
 *    - hx421_loader_run()    place() then JUMP to the entry with the syscall
 *                            table. The jump executes real code, so it is
 *                            proven under QEMU, not on the x86 host.
 *
 *  Public domain (CC0). No warranty.
 * ============================================================ */

#ifndef HX421_LOADER_H
#define HX421_LOADER_H

#include <stdint.h>
#include "hx421_syscall.h"

#define HX421_GAME_MAGIC 0x31475848u   /* "HXG1" little-endian */

/* Prepended to a game image. Sized fields only (the enum-ABI lesson): this
 * struct is written by the packer on the PC and read by the loader on the MCU,
 * so its layout is a cross-tool contract. */
typedef struct {
    uint32_t magic;        /* HX421_GAME_MAGIC                                */
    uint32_t abi_major;    /* ABI major the game was built against            */
    uint32_t entry_off;    /* entry offset from the IMAGE start (bytes)       */
    uint32_t load_size;    /* image bytes to copy into the region             */
    uint32_t bss_size;     /* zeroed bytes appended after load_size           */
    uint32_t reserved[3];
} Hx421GameHeader;

typedef enum {
    HX421_LOAD_OK = 0,
    HX421_LOAD_SHORT,      /* file smaller than the header, or than load_size */
    HX421_LOAD_BAD_MAGIC,
    HX421_LOAD_ABI,        /* header abi_major != firmware's                  */
    HX421_LOAD_BAD_ENTRY,  /* entry_off outside the loaded image              */
    HX421_LOAD_TOO_BIG     /* load + bss + reserved stack exceeds the region  */
} Hx421LoadResult;

/* Validate only. On OK, *hdr_out points at the header inside `file`. */
Hx421LoadResult hx421_loader_check(const void *file, uint32_t file_len,
                                   uint32_t region_size, uint32_t reserve_stack,
                                   const Hx421GameHeader **hdr_out);

/* Validate, copy the image into `region`, zero its bss. Does NOT jump. On OK,
 * *entry_off_out is the entry's byte offset within `region`. */
Hx421LoadResult hx421_loader_place(void *region, uint32_t region_size, uint32_t reserve_stack,
                                   const void *file, uint32_t file_len,
                                   uint32_t *entry_off_out);

/* place() then jump to the entry with the syscall table (r0). Returns whatever
 * the game returns via its entry, or a load error if placement failed. The jump
 * runs real target code — proven under QEMU. */
Hx421LoadResult hx421_loader_run(void *region, uint32_t region_size, uint32_t reserve_stack,
                                 const void *file, uint32_t file_len, const Hx421Sys *sys);

/* Read up to n bytes into dst; return the count actually read (< n at EOF). */
typedef uint32_t (*Hx421ReadFn)(void *ctx, void *dst, uint32_t n);

/* STREAMING place: read the header, validate, read the image STRAIGHT INTO the
 * region, zero bss. Does NOT jump. The whole .hxg is never held in RAM beside
 * the region — required on the 64 KB part, where a 32 KB file buffer and the
 * 32 KB game region cannot coexist. This is the data path the firmware uses
 * (read = a FatFs wrapper); host-tested. A short read at any stage is SHORT. */
Hx421LoadResult hx421_loader_place_stream(void *region, uint32_t region_size, uint32_t reserve_stack,
                                          Hx421ReadFn read, void *read_ctx, uint32_t *entry_off_out);

/* place_stream() then jump — the firmware convenience. Jump runs real code, so
 * this is QEMU-proven, not host-tested. */
Hx421LoadResult hx421_loader_run_stream(void *region, uint32_t region_size, uint32_t reserve_stack,
                                        Hx421ReadFn read, void *read_ctx, const Hx421Sys *sys);

#endif /* HX421_LOADER_H */
