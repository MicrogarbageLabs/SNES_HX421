/* gen_vectors_snes.c — co-sim golden for hx_cmdbox_snes.v (SNES-bus wrapper).
 *
 * Models the window decode + mailbox with BOTH read views (the SNES loopback and
 * the STM32/MCU port read the same mem + pending) and both ack paths (SNES self-
 * ack via a 0xFE write, and host_ack from the MCU bridge). Replays the SAME
 * sequence as the testbench and emits golden.hex (expected value at each read
 * CHECK). CC0. */

#include <stdint.h>
#include <stdio.h>

#define DOORBELL 0xFF
#define ACK_OFF  0xFE
#define PEND_OFF 0xFD

static uint8_t mem[256];
static int     pending;
static FILE   *g;

static void wr(int off, int d) {                 /* in-window SNES write */
    mem[off & 0xFF] = (uint8_t)d;
    if      ((off & 0xFF) == DOORBELL) pending = 1;
    else if ((off & 0xFF) == ACK_OFF)  pending = 0;
}
static void host_ack(void)  { pending = 0; }      /* STM32 consumes over the MCU bridge */
static uint8_t rd(int off) {                       /* both read views are identical */
    return ((off & 0xFF) == PEND_OFF) ? (uint8_t)(pending ? 1 : 0) : mem[off & 0xFF];
}
static void chk(int off) { fprintf(g, "%02x\n", rd(off)); }

int main(void) {
    g = fopen("golden.hex", "w");

    /* Command A: block at 0..3, then doorbell */
    wr(0x00, 0x01); wr(0x01, 0x00); wr(0x02, 0x0A); wr(0x03, 0x80);
    wr(DOORBELL, 0x01);

    /* the SNES loopback view */
    chk(PEND_OFF);                        /* 1: pending == 1 */
    chk(0x00); chk(0x01); chk(0x02); chk(0x03);   /* 2..5: block */

    /* the STM32/MCU-bridge view sees the SAME mailbox */
    chk(PEND_OFF);                        /* 6: pending == 1 (MCU) */
    chk(0x00); chk(0x01); chk(0x02); chk(0x03);   /* 7..10: block (MCU) */

    /* STM32 consumes it -> pending clears */
    host_ack();
    chk(PEND_OFF);                        /* 11: pending == 0 (MCU) */

    /* out-of-window write must not disturb the mailbox */
    chk(0x00);                            /* 12: still 0x01 */

    fclose(g);
    printf("gen_vectors_snes: golden.hex (12 checks) written\n");
    return 0;
}
