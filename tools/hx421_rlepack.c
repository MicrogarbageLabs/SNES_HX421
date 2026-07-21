/* ============================================================
 *  hx421_rlepack.c — pack a Quartus .rbf into the sd2snes .bi3 format
 *
 *  The FXPak's MCU streams FPGA bitstreams to the device through rle_file_getc()
 *  (third_party/sd2snes/src/rle.c), so a `.bi3` is an RLE-compressed `.rbf` —
 *  renaming one to the other produces a file that silently fails to configure.
 *
 *  Format (decoder in src/rle.c, encoder in utils/rle.c):
 *     0x5B <data> <len8>            run, len 1..255   (emits len bytes)
 *     0x77 <data> <lenlo> <lenhi>   run, len 256..65535
 *     0x9B <data>                   escaped literal (data is one of 5B/77/9B)
 *     <byte>                        literal
 *
 *  This reimplements it rather than depending on sd2snes' utils building under
 *  Windows, and — the point of writing our own — it DECODES ITS OWN OUTPUT and
 *  compares against the input before writing anything. A malformed bitstream on
 *  hardware surfaces as led_panic(LED_PANIC_FPGA_NOCONF) with no further
 *  diagnostic, which is a miserable thing to debug from a blinking LED. Verify
 *  in software where the failure is legible.
 *
 *  Public domain (CC0). No warranty.
 * ============================================================ */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#define RLE_RUN      0x5Bu
#define RLE_RUNLONG  0x77u
#define RLE_ESC      0x9Bu
#define LEN_THRESH   3u      /* runs longer than this are worth encoding */

static uint8_t *slurp(const char *path, size_t *len) {
    FILE *f = fopen(path, "rb");
    if (!f) { perror(path); return 0; }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n <= 0) { fclose(f); fprintf(stderr, "%s: empty\n", path); return 0; }
    uint8_t *b = malloc((size_t)n);
    if (!b || fread(b, 1, (size_t)n, f) != (size_t)n) {
        fclose(f); free(b); fprintf(stderr, "%s: short read\n", path); return 0;
    }
    fclose(f);
    *len = (size_t)n;
    return b;
}

/* ---- encode -------------------------------------------------------------- */

static size_t encode(const uint8_t *in, size_t n, uint8_t *out) {
    size_t o = 0, i = 0;
    while (i < n) {
        const uint8_t d = in[i];
        size_t run = 1;
        while (i + run < n && in[i + run] == d && run < 65535u) run++;

        if (run > LEN_THRESH) {
            if (run < 256u) {
                out[o++] = RLE_RUN;  out[o++] = d;  out[o++] = (uint8_t)run;
            } else {
                out[o++] = RLE_RUNLONG; out[o++] = d;
                out[o++] = (uint8_t)(run & 0xFFu);
                out[o++] = (uint8_t)(run >> 8);
            }
            i += run;
        } else {
            /* A literal that collides with a control byte must be escaped, or
             * the decoder reads the next bytes as a run header. */
            if (d == RLE_RUN || d == RLE_RUNLONG || d == RLE_ESC) out[o++] = RLE_ESC;
            out[o++] = d;
            i++;
        }
    }
    return o;
}

/* ---- decode: mirrors src/rle.c EXACTLY, including the -1 on run length ---- */

static size_t decode(const uint8_t *in, size_t n, uint8_t *out, size_t cap) {
    size_t o = 0, i = 0;
    while (i < n) {
        uint8_t data = in[i++];
        uint32_t filled = 0;
        if (data == RLE_RUN) {
            if (i + 1 >= n + 1) break;
            data = in[i++];
            filled = (uint32_t)in[i++] - 1u;      /* decoder: getc() - 1 */
        } else if (data == RLE_RUNLONG) {
            data = in[i++];
            filled = (uint32_t)in[i++];
            filled |= (uint32_t)in[i++] << 8;
            filled--;
        } else if (data == RLE_ESC) {
            data = in[i++];
        }
        /* one byte, then `filled` repeats */
        for (uint32_t k = 0; k <= filled; ++k) {
            if (o >= cap) return o;
            out[o++] = data;
        }
    }
    return o;
}

int main(int argc, char **argv) {
    if (argc != 3) {
        fprintf(stderr,
            "usage: %s <in.rbf> <out.bi3>\n\n"
            "Packs a Quartus raw binary file into the sd2snes RLE container the\n"
            "FXPak MCU expects. Verifies by decoding its own output before writing.\n",
            argv[0]);
        return 2;
    }

    size_t n = 0;
    uint8_t *in = slurp(argv[1], &n);
    if (!in) return 1;

    /* Worst case is 2 bytes per input byte (every byte an escaped literal). */
    uint8_t *enc = malloc(n * 2 + 16);
    uint8_t *dec = malloc(n + 16);
    if (!enc || !dec) { fprintf(stderr, "out of memory\n"); return 1; }

    const size_t elen = encode(in, n, enc);

    /* ---- the check that matters ---- */
    const size_t dlen = decode(enc, elen, dec, n + 16);
    if (dlen != n || memcmp(dec, in, n) != 0) {
        fprintf(stderr,
            "REFUSING TO WRITE: round-trip mismatch (%zu bytes in, %zu out)\n"
            "The packed bitstream would not configure the FPGA. This is a bug in\n"
            "the packer, not in your build.\n", n, dlen);
        for (size_t k = 0; k < (dlen < n ? dlen : n); ++k)
            if (dec[k] != in[k]) {
                fprintf(stderr, "first difference at byte %zu: got %02X want %02X\n",
                        k, dec[k], in[k]);
                break;
            }
        return 1;
    }

    FILE *f = fopen(argv[2], "wb");
    if (!f) { perror(argv[2]); return 1; }
    if (fwrite(enc, 1, elen, f) != elen) { perror("write"); fclose(f); return 1; }
    fclose(f);

    printf("%s -> %s\n", argv[1], argv[2]);
    printf("  %zu B raw -> %zu B packed (%.1f%%), round-trip verified\n",
           n, elen, 100.0 * elen / n);
    if (elen > n)
        printf("  NOTE: packed is LARGER than raw. Legal, but unusual for a\n"
               "  bitstream — check the .rbf is a real Quartus raw binary file.\n");
    return 0;
}
