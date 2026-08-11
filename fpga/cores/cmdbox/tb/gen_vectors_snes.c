/* gen_vectors_snes.c — co-sim golden for hx_cmdbox_snes.v (SNES-bus wrapper).
 *
 * Models the window decode + mailbox: a SNES write in the window stores at its
 * offset (0xFF = doorbell -> pending; 0xFE = ack -> clear); a read returns pending
 * at 0xFD else the stored byte. Replays the SAME sequence as the testbench and
 * emits golden.hex (expected value at each read CHECK). CC0. */

#include <stdint.h>
#include <stdio.h>

#define DOORBELL 0xFF
#define ACK_OFF  0xFE
#define PEND_OFF 0xFD

static uint8_t mem[256];
static int     pending;
static FILE   *g;

/* in-window SNES write */
static void wr(int off, int d) {
    mem[off & 0xFF] = (uint8_t)d;
    if      ((off & 0xFF) == DOORBELL) pending = 1;
    else if ((off & 0xFF) == ACK_OFF)  pending = 0;
}
static uint8_t rd(int off) {
    return ((off & 0xFF) == PEND_OFF) ? (uint8_t)(pending ? 1 : 0) : mem[off & 0xFF];
}
static void chk(int off) { fprintf(g, "%02x\n", rd(off)); }

int main(void) {
    g = fopen("golden.hex", "w");

    /* Command A: block at 0..3, then doorbell */
    wr(0x00, 0x01); wr(0x01, 0x00); wr(0x02, 0x0A); wr(0x03, 0x80);
    wr(DOORBELL, 0x01);
    chk(PEND_OFF);                       /* 1: pending == 1 */
    chk(0x00); chk(0x01); chk(0x02); chk(0x03);   /* 2..5: block reads back */

    /* self-ack (bring-up), pending clears */
    wr(ACK_OFF, 0x00);
    chk(PEND_OFF);                       /* 6: pending == 0 */

    /* an out-of-window write must NOT disturb the mailbox (modelled by simply
     * not calling wr()); offset 0 still holds Command A's op */
    chk(0x00);                           /* 7: still 0x01 */

    fclose(g);
    printf("gen_vectors_snes: golden.hex (7 checks) written\n");
    return 0;
}
