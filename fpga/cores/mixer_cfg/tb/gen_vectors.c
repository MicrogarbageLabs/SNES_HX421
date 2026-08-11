/* gen_vectors.c — co-sim golden for hx_mixer_cfg.v.
 *
 * For each STM32 register write {addr,data}, computes the expected mixer cfg
 * strobe (we, ch, field, data) and packs it into golden.hex as one 40-bit word
 * per write: {we[38], ch[37:35], field[34:32], data[31:0]}. Writes MUST match
 * hx_mixer_cfg_tb.v. CC0. */

#include <stdint.h>
#include <stdio.h>

typedef struct { uint8_t addr; uint32_t data; } W;

/* mixer_ctl-shaped traffic: enable+vol+pan_l+pan_r for ch1, ch0; a ch7 vol; and
 * a reserved field-7 write that must NOT emit a cfg strobe. */
static const W W_LIST[] = {
    { 0x0A, 0x00000002 },   /* ch1 field2 flags: active bit set (enable)   */
    { 0x0B, 0x00004000 },   /* ch1 field3 vol                              */
    { 0x0C, 0x00007FFF },   /* ch1 field4 pan_l (unity)                    */
    { 0x0D, 0x00003000 },   /* ch1 field5 pan_r                            */
    { 0x02, 0x00000002 },   /* ch0 field2 flags active                     */
    { 0x03, 0x00007FFF },   /* ch0 field3 vol unity                        */
    { 0x3B, 0x00001234 },   /* ch7 field3 vol                              */
    { 0x07, 0xDEADBEEF },   /* ch0 field7 RESERVED -> we must be 0          */
};
#define N (int)(sizeof(W_LIST)/sizeof(W_LIST[0]))

int main(void) {
    FILE *g = fopen("golden.hex", "w");
    for (int i = 0; i < N; i++) {
        int field = W_LIST[i].addr & 0x7;
        int ch    = (W_LIST[i].addr >> 3) & 0x7;
        int we    = (field <= 6) ? 1 : 0;
        unsigned long long word =
              ((unsigned long long)we    << 38)
            | ((unsigned long long)ch    << 35)
            | ((unsigned long long)field << 32)
            |  (unsigned long long)W_LIST[i].data;
        fprintf(g, "%010llx\n", word);
    }
    fclose(g);
    printf("gen_vectors: golden.hex (%d cfg writes) written\n", N);
    return 0;
}
