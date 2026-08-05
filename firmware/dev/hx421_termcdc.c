/* ============================================================
 *  hx421_termcdc.c — text console over the existing USB CDC endpoint (EP2).
 *
 *  Two rings. RX is filled from the USB ISR (CDC_BulkOut) and drained by the
 *  CLI in main context. TX is filled by main context (uart_putc tee) and
 *  drained to EP2-IN from the ISR (CDC_BulkIn -> term_on_in_complete). The
 *  descriptor is untouched; this only changes where the CDC data goes.
 *
 *  Concurrency: main context guards its "start a transfer" critical section by
 *  masking interrupts (save/restore PRIMASK); the ISR path is already
 *  non-preemptible w.r.t. itself, so it starts transfers directly. tx_busy is
 *  the single owner-flag of the in-flight EP2-IN transfer.
 *
 *  Public domain (CC0). No warranty.
 * ============================================================ */

#include <stdint.h>
#include "config.h"
#include "usbhw.h"      /* USB_WriteEP */
#include "usbcfg.h"     /* USB_CDC_BUFINSIZE */
#include "cdcuser.h"    /* CDC_DEP_IN */
#include CONFIG_MCU_H   /* __get_PRIMASK / __disable_irq / __set_PRIMASK */
#include "hx421_termcdc.h"

#define TERM_RX_SZ 256u    /* host -> device: keystrokes (power of two) */
#define TERM_TX_SZ 2048u   /* device -> host: console + game output    */

static volatile uint8_t  rx_buf[TERM_RX_SZ];
static volatile uint32_t rx_wr, rx_rd;
static volatile uint8_t  tx_buf[TERM_TX_SZ];
static volatile uint32_t tx_wr, tx_rd;
static volatile int      tx_busy;                 /* an EP2-IN xfer is in flight */
static uint8_t           tx_stage[USB_CDC_BUFINSIZE];

/* ---- RX: ISR pushes, main pops ---- */
void term_rx_push(const uint8_t *buf, uint32_t n) {
    for (uint32_t i = 0; i < n; ++i) {
        uint32_t nxt = (rx_wr + 1u) & (TERM_RX_SZ - 1u);
        if (nxt == rx_rd) break;                  /* full: drop the rest */
        rx_buf[rx_wr] = buf[i];
        rx_wr = nxt;
    }
}

int term_avail(void) { return rx_wr != rx_rd; }

int term_getc(void) {
    while (rx_wr == rx_rd) { /* spin: the USB ISR fills the ring */ }
    uint8_t c = rx_buf[rx_rd];
    rx_rd = (rx_rd + 1u) & (TERM_RX_SZ - 1u);
    return (int)c;
}

/* ---- TX core: if idle and data waiting, stage a chunk and start an EP2-IN
 *      transfer. Caller must hold off the USB ISR (or already be in it). ---- */
static void tx_start_locked(void) {
    if (tx_busy) return;
    uint32_t n = 0;
    while (n < USB_CDC_BUFINSIZE && tx_rd != tx_wr) {
        tx_stage[n++] = tx_buf[tx_rd];
        tx_rd = (tx_rd + 1u) & (TERM_TX_SZ - 1u);
    }
    if (n) {
        tx_busy = 1;
        USB_WriteEP(CDC_DEP_IN, tx_stage, n);
    }
}

void term_putc(char c) {
    uint32_t nxt = (tx_wr + 1u) & (TERM_TX_SZ - 1u);
    if (nxt == tx_rd) return;                     /* ring full (host not reading): drop */
    tx_buf[tx_wr] = (uint8_t)c;
    tx_wr = nxt;

    uint32_t pri = __get_PRIMASK();
    __disable_irq();
    tx_start_locked();
    __set_PRIMASK(pri);
}

/* EP2-IN transfer completed (called from the USB ISR via CDC_BulkIn) */
void term_on_in_complete(void) {
    tx_busy = 0;
    tx_start_locked();                            /* already in ISR context */
}
