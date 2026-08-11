/* gen_vectors.c — co-sim golden for hx_mixer_cfg.v (byte-assembling decoder).
 *
 * Models the shared accumulator + field-width commit: each FA register write
 * merges one byte into acc at its lane, and a cfg strobe is emitted on the field's
 * last byte. Emits golden.hex, one 40-bit word per write: {we[38], ch[37:35],
 * field[34:32], data[31:0]} — data is the FULL acc (leftover high bytes match the
 * RTL by construction; the mixer only reads each field's width). Writes MUST match
 * hx_mixer_cfg_tb.v. CC0. */

#include <stdint.h>
#include <stdio.h>

typedef struct { uint8_t index; uint8_t value; } W;

/* vol/pan (2 B) for ch1, flags (1 B) for ch0, step_lo (4 B) for ch0, loop_len
 * (3 B) for ch2 — one field of each width, written low-to-high byte order. */
static const W W_LIST[] = {
    { 0x2C, 0x00 }, { 0x2D, 0x40 },                 /* ch1 field3 vol  = 0x4000  */
    { 0x30, 0xFF }, { 0x31, 0x7F },                 /* ch1 field4 panL = 0x7FFF  */
    { 0x08, 0x02 },                                 /* ch0 field2 flags= active  */
    { 0x00, 0x44 }, { 0x01, 0x33 }, { 0x02, 0x22 }, { 0x03, 0x11 },  /* ch0 f0 step_lo */
    { 0x58, 0x00 }, { 0x59, 0x02 }, { 0x5A, 0x00 }, /* ch2 field6 loop_len 0x200  */
};
#define N (int)(sizeof(W_LIST)/sizeof(W_LIST[0]))

static int last_b(int f) {
    if (f == 0 || f == 1) return 3;
    if (f == 6)           return 2;
    if (f == 3 || f == 4 || f == 5) return 1;
    return 0;
}

int main(void) {
    FILE *g = fopen("golden.hex", "w");
    uint32_t acc = 0;
    for (int i = 0; i < N; i++) {
        int ch    = (W_LIST[i].index >> 5) & 0x7;
        int field = (W_LIST[i].index >> 2) & 0x7;
        int bsel  =  W_LIST[i].index       & 0x3;
        acc = (acc & ~((uint32_t)0xFF << (bsel * 8)))
            | ((uint32_t)W_LIST[i].value << (bsel * 8));
        int we = (bsel == last_b(field)) ? 1 : 0;
        unsigned long long word =
              ((unsigned long long)we    << 38)
            | ((unsigned long long)ch    << 35)
            | ((unsigned long long)field << 32)
            |  (unsigned long long)acc;
        fprintf(g, "%010llx\n", word);
    }
    fclose(g);
    printf("gen_vectors: golden.hex (%d byte writes) written\n", N);
    return 0;
}
