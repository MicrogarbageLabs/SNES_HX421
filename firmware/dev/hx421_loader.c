/* ============================================================
 *  hx421_loader.c — see hx421_loader.h. Portable; the jump is the only
 *  target-specific line and is isolated at the bottom.
 * ============================================================ */

#include "hx421_loader.h"

Hx421LoadResult hx421_loader_check(const void *file, uint32_t file_len,
                                   uint32_t region_size, uint32_t reserve_stack,
                                   const Hx421GameHeader **hdr_out) {
    if (file_len < sizeof(Hx421GameHeader)) return HX421_LOAD_SHORT;
    const Hx421GameHeader *h = (const Hx421GameHeader *)file;

    if (h->magic != HX421_GAME_MAGIC) return HX421_LOAD_BAD_MAGIC;
    if (h->abi_major != HX421_ABI_MAJOR) return HX421_LOAD_ABI;

    /* the file must actually contain the header + load_size image bytes */
    if (file_len - (uint32_t)sizeof(Hx421GameHeader) < h->load_size) return HX421_LOAD_SHORT;

    /* entry must land inside the loaded image (STRICTLY inside, and clear of a
     * zero-length image) */
    if (h->load_size == 0 || h->entry_off >= h->load_size) return HX421_LOAD_BAD_ENTRY;

    /* load + bss + a reserved stack must fit the region. Written to avoid
     * uint32 overflow: check each addition against the remaining headroom
     * rather than summing first. */
    if (h->load_size > region_size) return HX421_LOAD_TOO_BIG;
    if (h->bss_size  > region_size - h->load_size) return HX421_LOAD_TOO_BIG;
    if (reserve_stack > region_size - h->load_size - h->bss_size) return HX421_LOAD_TOO_BIG;

    if (hdr_out) *hdr_out = h;
    return HX421_LOAD_OK;
}

Hx421LoadResult hx421_loader_place(void *region, uint32_t region_size, uint32_t reserve_stack,
                                   const void *file, uint32_t file_len,
                                   uint32_t *entry_off_out) {
    const Hx421GameHeader *h;
    Hx421LoadResult r = hx421_loader_check(file, file_len, region_size, reserve_stack, &h);
    if (r != HX421_LOAD_OK) return r;

    const uint8_t *img = (const uint8_t *)file + sizeof(Hx421GameHeader);
    uint8_t *dst = (uint8_t *)region;

    for (uint32_t i = 0; i < h->load_size; ++i) dst[i] = img[i];               /* copy image */
    for (uint32_t i = 0; i < h->bss_size; ++i) dst[h->load_size + i] = 0;       /* zero bss   */

    if (entry_off_out) *entry_off_out = h->entry_off;
    return HX421_LOAD_OK;
}

/* Jump to a placed game's entry with the syscall table. On ARM Thumb the entry
 * address needs bit 0 set (the Thumb bit); a host build leaves it clear. */
static void jump_entry(void *region, uint32_t entry_off, const Hx421Sys *sys) {
    uintptr_t addr = (uintptr_t)region + entry_off;
#if defined(__arm__) || defined(__thumb__)
    addr |= 1u;
#endif
    Hx421GameEntry entry = (Hx421GameEntry)addr;
    entry(sys);
}

Hx421LoadResult hx421_loader_run(void *region, uint32_t region_size, uint32_t reserve_stack,
                                 const void *file, uint32_t file_len, const Hx421Sys *sys) {
    uint32_t entry_off;
    Hx421LoadResult r = hx421_loader_place(region, region_size, reserve_stack, file, file_len, &entry_off);
    if (r != HX421_LOAD_OK) return r;
    jump_entry(region, entry_off, sys);
    return HX421_LOAD_OK;
}

/* Loop the read_fn until `n` bytes arrive or it returns 0 (EOF). A single
 * f_read / SD read may come back short; the loader must fill the span itself
 * rather than assume one call delivers it. */
static uint32_t read_full(Hx421ReadFn read, void *ctx, void *dst, uint32_t n) {
    uint8_t *d = (uint8_t *)dst;
    uint32_t got = 0;
    while (got < n) {
        uint32_t r = read(ctx, d + got, n - got);
        if (r == 0) break;                 /* EOF / error */
        got += r;
    }
    return got;
}

Hx421LoadResult hx421_loader_place_stream(void *region, uint32_t region_size, uint32_t reserve_stack,
                                          Hx421ReadFn read, void *read_ctx, uint32_t *entry_off_out) {
    Hx421GameHeader h;
    /* header first — .hxg fields are little-endian, matching the ARM/host struct */
    if (read_full(read, read_ctx, &h, sizeof h) != sizeof h) return HX421_LOAD_SHORT;

    if (h.magic != HX421_GAME_MAGIC) return HX421_LOAD_BAD_MAGIC;
    if (h.abi_major != HX421_ABI_MAJOR) return HX421_LOAD_ABI;
    if (h.load_size == 0 || h.entry_off >= h.load_size) return HX421_LOAD_BAD_ENTRY;
    /* same overflow-safe fit checks as hx421_loader_check */
    if (h.load_size > region_size) return HX421_LOAD_TOO_BIG;
    if (h.bss_size  > region_size - h.load_size) return HX421_LOAD_TOO_BIG;
    if (reserve_stack > region_size - h.load_size - h.bss_size) return HX421_LOAD_TOO_BIG;

    /* image STRAIGHT into the region — never buffered beside it */
    if (read_full(read, read_ctx, region, h.load_size) != h.load_size) return HX421_LOAD_SHORT;

    uint8_t *dst = (uint8_t *)region;
    for (uint32_t i = 0; i < h.bss_size; ++i) dst[h.load_size + i] = 0;

    if (entry_off_out) *entry_off_out = h.entry_off;
    return HX421_LOAD_OK;
}

Hx421LoadResult hx421_loader_run_stream(void *region, uint32_t region_size, uint32_t reserve_stack,
                                        Hx421ReadFn read, void *read_ctx, const Hx421Sys *sys) {
    uint32_t entry_off;
    Hx421LoadResult r = hx421_loader_place_stream(region, region_size, reserve_stack, read, read_ctx, &entry_off);
    if (r != HX421_LOAD_OK) return r;
    jump_entry(region, entry_off, sys);
    return HX421_LOAD_OK;
}
