/* gen_vectors.c — co-sim golden for hx_dsp.v (mailbox math coprocessor).
 *
 * Models the intended arithmetic of each function and emits a golden 32-bit
 * result per op. The TB drives the same op sequence (byte-write operands, START,
 * wait, read 4 result bytes) and diffs. Ops MUST match hx_dsp_tb.v.
 * FUNC: 0 MUL(signed 16x16), 1 MAC(acc+=A*B), 2 DIV(A/B), 3 MACINIT(acc=A*B). CC0. */

#include <stdint.h>
#include <stdio.h>

static int64_t acc;   /* mirrors the RTL accumulator across ops */

static uint32_t op(int func, uint32_t a, uint32_t b) {
    int32_t as = (int16_t)(a & 0xFFFF);
    int32_t bs = (int16_t)(b & 0xFFFF);
    int32_t prod = as * bs;                       /* signed 16x16 */
    switch (func) {
        case 0: return (uint32_t)prod;                                   /* MUL   */
        case 3: acc = prod;             return (uint32_t)(acc & 0xFFFFFFFF); /* MACINIT */
        case 1: acc += prod;            return (uint32_t)(acc & 0xFFFFFFFF); /* MAC   */
        case 2: return (b & 0xFFFF) ? (a / (b & 0xFFFF)) : 0xFFFFFFFFu;  /* DIV   */
    }
    return 0;
}

int main(void) {
    acc = 0;
    FILE *g = fopen("golden.hex", "w");
    /* op sequence (func, argA, argB) — mirror in the TB */
    /* MUL 300*-7 ; MACINIT 4*5 ; MAC +6*7 ; MAC +2*3 ; DIV 100000/7 ; DIV 12345/0 ; MUL -32768*-32768 */
    fprintf(g, "%08x\n", op(0, 300, (uint32_t)(-7)));
    fprintf(g, "%08x\n", op(3, 4, 5));
    fprintf(g, "%08x\n", op(1, 6, 7));
    fprintf(g, "%08x\n", op(1, 2, 3));
    fprintf(g, "%08x\n", op(2, 100000u, 7));
    fprintf(g, "%08x\n", op(2, 12345u, 0));
    fprintf(g, "%08x\n", op(0, (uint32_t)(-32768), (uint32_t)(-32768)));
    fclose(g);
    printf("gen_vectors: golden.hex written (7 DSP ops)\n");
    return 0;
}
