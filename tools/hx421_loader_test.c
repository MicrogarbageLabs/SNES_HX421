/* hx421_loader_test.c — host test for the RAM loader's logic: header
 * validation, image placement (copy + bss zero), and every reject path. The
 * jump-and-run is proven under QEMU (it executes real target code); here we
 * prove the data handling and that bad images are refused, not run.
 */

#include <stdio.h>
#include <string.h>
#include "../firmware/dev/hx421_loader.h"

static int fails = 0, checks = 0;
static void ck(int cond, const char *what) {
    checks++;
    if (!cond) { printf("  FAIL: %s\n", what); fails++; }
}

/* Build a game file: header + `load` image bytes (a ramp) into `out`. */
static uint32_t make_file(uint8_t *out, uint32_t magic, uint32_t abi,
                          uint32_t entry_off, uint32_t load_size, uint32_t bss_size,
                          uint32_t image_bytes /* bytes actually appended */) {
    Hx421GameHeader h = {0};
    h.magic = magic; h.abi_major = abi;
    h.entry_off = entry_off; h.load_size = load_size; h.bss_size = bss_size;
    memcpy(out, &h, sizeof h);
    for (uint32_t i = 0; i < image_bytes; ++i) out[sizeof h + i] = (uint8_t)(i + 1);
    return (uint32_t)sizeof h + image_bytes;
}

int main(void) {
    printf("hx421 loader tests\n");
    static uint8_t file[512];
    static uint8_t region[256];

    const uint32_t REGION = sizeof region;
    const uint32_t STACK  = 32;

    /* ---- a good load ---- */
    uint32_t flen = make_file(file, HX421_GAME_MAGIC, HX421_ABI_MAJOR,
                              /*entry*/ 4, /*load*/ 64, /*bss*/ 16, /*image*/ 64);
    const Hx421GameHeader *h = 0;
    ck(hx421_loader_check(file, flen, REGION, STACK, &h) == HX421_LOAD_OK, "a valid image checks OK");
    ck(h && h->entry_off == 4, "check hands back the header");

    memset(region, 0xEE, sizeof region);
    uint32_t entry = 0xFFFF;
    ck(hx421_loader_place(region, REGION, STACK, file, flen, &entry) == HX421_LOAD_OK, "valid image places");
    ck(entry == 4, "entry offset is reported");
    /* image copied byte-exact */
    int copy_ok = 1;
    for (uint32_t i = 0; i < 64; ++i) if (region[i] != (uint8_t)(i + 1)) { copy_ok = 0; break; }
    ck(copy_ok, "image copied into the region byte-exact");
    /* bss zeroed after the image */
    int bss_ok = 1;
    for (uint32_t i = 64; i < 64 + 16; ++i) if (region[i] != 0) { bss_ok = 0; break; }
    ck(bss_ok, "bss zeroed after the image");
    /* the loader must NOT touch beyond load+bss */
    ck(region[64 + 16] == 0xEE, "nothing written past load+bss");

    /* ---- every reject path ---- */
    uint8_t tiny[8];
    ck(hx421_loader_check(tiny, sizeof tiny, REGION, STACK, 0) == HX421_LOAD_SHORT,
       "a file smaller than the header is SHORT");

    flen = make_file(file, 0xDEADBEEF, HX421_ABI_MAJOR, 4, 64, 0, 64);
    ck(hx421_loader_check(file, flen, REGION, STACK, 0) == HX421_LOAD_BAD_MAGIC, "wrong magic refused");

    flen = make_file(file, HX421_GAME_MAGIC, HX421_ABI_MAJOR + 1, 4, 64, 0, 64);
    ck(hx421_loader_check(file, flen, REGION, STACK, 0) == HX421_LOAD_ABI, "wrong ABI major refused");

    /* header says load_size 64 but only 32 image bytes present */
    flen = make_file(file, HX421_GAME_MAGIC, HX421_ABI_MAJOR, 4, 64, 0, /*image*/ 32);
    ck(hx421_loader_check(file, flen, REGION, STACK, 0) == HX421_LOAD_SHORT,
       "load_size larger than the file is SHORT");

    /* entry at or past the image end */
    flen = make_file(file, HX421_GAME_MAGIC, HX421_ABI_MAJOR, /*entry*/ 64, 64, 0, 64);
    ck(hx421_loader_check(file, flen, REGION, STACK, 0) == HX421_LOAD_BAD_ENTRY, "entry at image end refused");
    flen = make_file(file, HX421_GAME_MAGIC, HX421_ABI_MAJOR, 4, /*load*/ 0, 0, 0);
    ck(hx421_loader_check(file, flen, REGION, STACK, 0) == HX421_LOAD_BAD_ENTRY, "zero-length image refused");

    /* load + bss + stack must fit the 256-byte region */
    flen = make_file(file, HX421_GAME_MAGIC, HX421_ABI_MAJOR, 4, /*load*/ 200, /*bss*/ 40, 200);
    ck(hx421_loader_check(file, flen, REGION, /*stack*/ 32, 0) == HX421_LOAD_TOO_BIG,
       "load+bss+stack over the region is TOO_BIG");
    /* and the exact-fit boundary is accepted: 200 + 40 + 16 == 256 */
    ck(hx421_loader_check(file, flen, REGION, /*stack*/ 16, 0) == HX421_LOAD_OK,
       "an exact fit is accepted");

    /* a bss_size so large it would overflow load+bss must be caught, not wrap */
    flen = make_file(file, HX421_GAME_MAGIC, HX421_ABI_MAJOR, 4, /*load*/ 64, /*bss*/ 0xFFFFFF00u, 64);
    ck(hx421_loader_check(file, flen, REGION, STACK, 0) == HX421_LOAD_TOO_BIG,
       "a huge bss is rejected without integer overflow");

    printf("%d checks, %d failures\n", checks, fails);
    return fails ? 1 : 0;
}
