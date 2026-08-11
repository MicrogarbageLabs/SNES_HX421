/* gen_vectors.c — co-sim golden for hx_cmdbox.v.
 *
 * Models the command mailbox (mem[256] + pending + doorbell/ack) and replays the
 * SAME op sequence as hx_cmdbox_tb.v, emitting golden.hex: the expected value at
 * each CHECK point (a pending sample -> 00/01, or a readback byte). Ops MUST match
 * the testbench. CC0. */

#include <stdint.h>
#include <stdio.h>

#define DOORBELL 0xFF

static uint8_t mem[256];
static int     pending;
static FILE   *g;

static void wr(int a, int d) {              /* SNES byte write */
    mem[a & 0xFF] = (uint8_t)d;
    if ((a & 0xFF) == DOORBELL) pending = 1;
}
static void ack(void)              { pending = 0; }
static void collide(int a, int d) {          /* doorbell write + ack same cycle */
    mem[a & 0xFF] = (uint8_t)d;              /* doorbell wins -> pending stays set */
    pending = 1;
}
static void chkp(void)      { fprintf(g, "%02x\n", pending ? 1 : 0); }
static void chkr(int a)     { fprintf(g, "%02x\n", mem[a & 0xFF]); }

int main(void) {
    g = fopen("golden.hex", "w");

    /* --- Command A: a full PLAY block at offsets 0..11, then the doorbell --- */
    wr(0, 0x01); wr(1, 0x00); wr(2, 0x0A); wr(3, 0x00);   /* op,slot,asset16   */
    wr(4, 0x01); wr(5, 0x02); wr(6, 0xFF); wr(7, 0x80);   /* flags,prio,gain,pan*/
    wr(8, 0x00); wr(9, 0x00); wr(10, 0x00); wr(11, 0x00); /* arg32             */
    wr(DOORBELL, 0x01);
    chkp();                                   /* 1: pending == 1 */
    for (int a = 0; a <= 11; a++) chkr(a);    /* 2..13: block reads back intact */
    chkr(DOORBELL);                           /* 14: doorbell byte stored (01)  */
    ack(); chkp();                            /* 15: pending cleared            */

    /* --- Command B: overwrite a few fields + re-ring (proves reuse) --- */
    wr(0, 0x04); wr(1, 0x01); wr(6, 0x40); wr(7, 0xC8);   /* GAIN,slot1,g,pan  */
    wr(DOORBELL, 0x02);
    chkp();                                   /* 16: pending == 1 */
    chkr(0); chkr(6); chkr(7); chkr(DOORBELL); /* 17..20: 04 40 C8 02 */

    /* --- collision: doorbell + ack in the same cycle -> command not dropped --- */
    collide(DOORBELL, 0x03); chkp();          /* 21: pending stays 1 */
    ack(); chkp();                            /* 22: pending cleared */

    fclose(g);
    printf("gen_vectors: golden.hex (22 checks) written\n");
    return 0;
}
