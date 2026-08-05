/* ============================================================
 *  hx421_termcdc.h — a text console over the existing USB CDC endpoint.
 *
 *  In the dev firmware the single CDC-ACM port carries the interactive CLI
 *  (ls / run / game output) instead of the binary usb2snes protocol. The USB
 *  DESCRIPTOR is unchanged — same COM port that already enumerates — only the
 *  data path is rewired: EP2-OUT bytes land in the RX ring, EP2-IN drains the
 *  TX ring. `uart.c`'s console primitives tee/route through this in the dev
 *  build, so all printf/CLI/game output flows over USB with no edits to cli.c.
 *  See docs/dev-mode.md.
 *
 *  Public domain (CC0). No warranty.
 * ============================================================ */

#ifndef HX421_TERMCDC_H
#define HX421_TERMCDC_H

#include <stdint.h>

/* RX (host -> device): CDC_BulkOut (USB ISR) pushes; the CLI pops in main ctx. */
void term_rx_push(const uint8_t *buf, uint32_t n);
int  term_avail(void);   /* nonzero if at least one byte is waiting */
int  term_getc(void);    /* blocking: wait for a byte, return it 0..255 */

/* TX (device -> host): main ctx queues bytes; the USB ISR drains to EP2-IN. */
void term_putc(char c);         /* non-blocking: drops if the ring is full */
void term_on_in_complete(void); /* CDC_BulkIn (USB ISR): EP2-IN transfer done */
void term_reset(void);          /* USB_Configure_Event: clear TX state on (re)config */

#endif /* HX421_TERMCDC_H */
