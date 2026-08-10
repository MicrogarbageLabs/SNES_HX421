/* gen_vectors.c — co-sim golden for hx_dsp.v + generator of the sine ROM.
 *
 * Emits sintab.vh (the quarter-wave Q1.15 table the RTL `include`s) AND golden.hex
 * (expected 32-bit result per op), using the SAME table + folding, so trig matches
 * bit-exact by construction. Ops MUST match hx_dsp_tb.v.
 * FUNC: 0 MUL 1 MAC 2 DIV 3 MACINIT 4 SIN 5 COS. CC0. */

#include <stdint.h>
#include <stdio.h>
#include <math.h>

static int16_t T[257];       /* T[k] = sin(k/1024 * 2pi) * 32767, k=0..256 (0..90deg) */
static int64_t acc;

static void build_table(void) {
    for (int k = 0; k <= 256; k++) {
        double s = sin((double)k / 1024.0 * 2.0 * M_PI) * 32767.0;
        T[k] = (int16_t)lround(s);
    }
}

/* mirror of hx_dsp's SIN/COS folding */
static uint32_t trig(int func, uint32_t a) {
    int ang = a & 0x3FF;
    if (func == 5) ang = (ang + 256) & 0x3FF;      /* COS = sin(+90deg) */
    int i   = ang & 0xFF;
    int idx = (ang & 0x100) ? (256 - i) : i;
    int neg = (ang & 0x200) != 0;
    int val = neg ? -(int)T[idx] : (int)T[idx];
    return (uint32_t)val;                           /* sign-extended 32-bit */
}

static uint32_t op(int func, uint32_t a, uint32_t b) {
    int32_t as = (int16_t)(a & 0xFFFF), bs = (int16_t)(b & 0xFFFF);
    int32_t prod = as * bs;
    switch (func) {
        case 0: return (uint32_t)prod;
        case 3: acc = prod;  return (uint32_t)(acc & 0xFFFFFFFF);
        case 1: acc += prod; return (uint32_t)(acc & 0xFFFFFFFF);
        case 2: return (b & 0xFFFF) ? (a / (b & 0xFFFF)) : 0xFFFFFFFFu;
        case 4: case 5: return trig(func, a);
    }
    return 0;
}

int main(void) {
    build_table();

    /* sintab.vh next to hx_dsp.v (both sim + synth `include` it) */
    FILE *v = fopen("../sintab.vh", "w");
    for (int k = 0; k <= 256; k++) fprintf(v, "T[%d] = 16'sd%d;\n", k, T[k]);
    fclose(v);

    acc = 0;
    FILE *g = fopen("golden.hex", "w");
    fprintf(g, "%08x\n", op(0, 300, (uint32_t)(-7)));       /* MUL   */
    fprintf(g, "%08x\n", op(3, 4, 5));                      /* MACINIT 20 */
    fprintf(g, "%08x\n", op(1, 6, 7));                      /* MAC 62 */
    fprintf(g, "%08x\n", op(1, 2, 3));                      /* MAC 68 */
    fprintf(g, "%08x\n", op(2, 100000u, 7));               /* DIV   */
    fprintf(g, "%08x\n", op(2, 12345u, 0));                /* DIV/0 */
    fprintf(g, "%08x\n", op(0, (uint32_t)(-32768), (uint32_t)(-32768)));
    fprintf(g, "%08x\n", op(4, 0, 0));                      /* SIN 0deg   = 0 */
    fprintf(g, "%08x\n", op(5, 0, 0));                      /* COS 0deg   = 1 */
    fprintf(g, "%08x\n", op(4, 256, 0));                    /* SIN 90deg  = 1 */
    fprintf(g, "%08x\n", op(4, 512, 0));                    /* SIN 180deg = 0 */
    fprintf(g, "%08x\n", op(4, 768, 0));                    /* SIN 270deg = -1 */
    fprintf(g, "%08x\n", op(4, 128, 0));                    /* SIN 45deg  */
    fprintf(g, "%08x\n", op(5, 128, 0));                    /* COS 45deg  */
    fclose(g);
    printf("gen_vectors: sintab.vh + golden.hex (14 ops) written\n");
    return 0;
}
