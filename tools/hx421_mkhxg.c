/* hx421_mkhxg.c — pack a raw game .bin into the .hxg the RAM loader expects:
 * an Hx421GameHeader followed by the image bytes. The .bin must be linked to
 * run at the game region base (load == run), so its bytes are copied verbatim.
 *
 *   hx421_mkhxg <in.bin> <out.hxg> [entry_off] [bss_size]
 *
 * entry_off defaults to 0 (link the entry first); bss_size to 0. load_size is
 * the input size. Header layout matches firmware/dev/hx421_loader.h exactly.
 *
 * Public domain (CC0). No warranty.
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>

#define HX421_GAME_MAGIC 0x31475848u   /* "HXG1" */
#define HX421_ABI_MAJOR  1u            /* must track hx421_syscall.h */

static void put32(FILE *f, uint32_t v) {   /* little-endian, host-independent */
    fputc((int)(v & 0xFF), f);
    fputc((int)((v >> 8) & 0xFF), f);
    fputc((int)((v >> 16) & 0xFF), f);
    fputc((int)((v >> 24) & 0xFF), f);
}

int main(int argc, char **argv) {
    if (argc < 3) {
        fprintf(stderr, "usage: %s <in.bin> <out.hxg> [entry_off] [bss_size]\n", argv[0]);
        return 2;
    }
    const uint32_t entry_off = argc > 3 ? (uint32_t)strtoul(argv[3], 0, 0) : 0u;
    const uint32_t bss_size  = argc > 4 ? (uint32_t)strtoul(argv[4], 0, 0) : 0u;

    FILE *in = fopen(argv[1], "rb");
    if (!in) { perror(argv[1]); return 1; }
    fseek(in, 0, SEEK_END);
    long n = ftell(in);
    fseek(in, 0, SEEK_SET);
    if (n <= 0) { fprintf(stderr, "%s: empty\n", argv[1]); fclose(in); return 1; }

    if (entry_off >= (uint32_t)n) {
        fprintf(stderr, "entry_off %u is outside the %ld-byte image\n", entry_off, n);
        fclose(in); return 1;
    }

    FILE *out = fopen(argv[2], "wb");
    if (!out) { perror(argv[2]); fclose(in); return 1; }

    /* Hx421GameHeader: magic, abi_major, entry_off, load_size, bss_size, [3] */
    put32(out, HX421_GAME_MAGIC);
    put32(out, HX421_ABI_MAJOR);
    put32(out, entry_off);
    put32(out, (uint32_t)n);          /* load_size */
    put32(out, bss_size);
    put32(out, 0); put32(out, 0); put32(out, 0);

    int c;
    while ((c = fgetc(in)) != EOF) fputc(c, out);

    fclose(in); fclose(out);
    printf("%s -> %s: %ld image bytes, entry_off %u, bss %u\n",
           argv[1], argv[2], n, entry_off, bss_size);
    return 0;
}
