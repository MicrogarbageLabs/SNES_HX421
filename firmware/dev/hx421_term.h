/* ============================================================
 *  hx421_term.h — the dev-mode terminal: non-blocking debug print + a
 *  line-edited command channel, transport-agnostic.
 *
 *  Owns the logic; knows nothing about USB. Bytes leave through a SINK
 *  callback and arrive one at a time via hx421_term_rx(). On the target the
 *  sink is the CDC-ACM IN endpoint and rx is fed from the OUT endpoint; the
 *  host test wires both to buffers. Same discipline as the audio sink and the
 *  stream arbiter's source seam — the logic is portable, only the glue is not.
 *  See docs/dev-mode.md.
 *
 *  Two rules that matter:
 *    - Debug print NEVER blocks. If the ring is full it DROPS and counts the
 *      drop; a game frame must not stall waiting on a debug byte (the WASAPI
 *      lesson). Drops are surfaced, never silent.
 *    - The sink reports how many bytes it accepted, so USB back-pressure (a
 *      busy 64-byte FS endpoint that NAKs) leaves the remainder in the ring
 *      for the next drain instead of being lost.
 *
 *  Public domain (CC0). No warranty.
 * ============================================================ */

#ifndef HX421_TERM_H
#define HX421_TERM_H

#include <stdint.h>
#include <stddef.h>
#include <stdarg.h>

#ifndef HX421_TERM_TX_BITS
#define HX421_TERM_TX_BITS 10            /* 1 KB TX ring (power of two)      */
#endif
#define HX421_TERM_TX_SIZE (1u << HX421_TERM_TX_BITS)
#define HX421_TERM_TX_MASK (HX421_TERM_TX_SIZE - 1u)

#define HX421_TERM_LINE_MAX 128          /* longest command line             */
#define HX421_TERM_MAX_ARGS 8

/* Accept up to n bytes; return how many were actually taken (<= n). On the
 * target this is the CDC IN endpoint write; on the host test it appends to a
 * capture buffer and may deliberately take fewer to model back-pressure. */
typedef uint32_t (*Hx421TermSink)(void *ctx, const uint8_t *buf, uint32_t n);

/* Called once per completed line, already tokenized. argv[0] is the command.
 * The module owns editing/echo; the host owns what the commands MEAN. */
typedef void (*Hx421TermCmd)(void *ctx, int argc, char **argv);

typedef struct {
    uint8_t        tx[HX421_TERM_TX_SIZE];
    volatile uint32_t tx_head;           /* producer (print side)            */
    volatile uint32_t tx_tail;           /* consumer (drain side)            */
    uint32_t       dropped;              /* bytes lost to a full ring        */

    char           line[HX421_TERM_LINE_MAX];
    uint32_t       line_len;

    Hx421TermSink  sink;   void *sink_ctx;
    Hx421TermCmd   on_cmd; void *cmd_ctx;
    uint8_t        echo;                 /* echo typed characters back       */
} Hx421Term;

void hx421_term_init(Hx421Term *t, Hx421TermSink sink, void *sink_ctx,
                     Hx421TermCmd on_cmd, void *cmd_ctx);

/* ---- TX: debug output (non-blocking) ---- */
void     hx421_term_write(Hx421Term *t, const void *buf, uint32_t n);
void     hx421_term_puts(Hx421Term *t, const char *s);
int      hx421_term_printf(Hx421Term *t, const char *fmt, ...);
/* Drain the ring to the sink; call from USB IN-ready / SOF. Returns bytes
 * sent this call (may be < pending if the sink applied back-pressure). */
uint32_t hx421_term_drain(Hx421Term *t);
uint32_t hx421_term_pending(const Hx421Term *t);

/* ---- RX: one received byte at a time (from the CDC OUT endpoint) ---- */
void hx421_term_rx(Hx421Term *t, uint8_t c);

/* ---- ANSI helpers for the PuTTY experience (256-color, cursor, clear) ---- */
void hx421_term_color(Hx421Term *t, uint8_t fg, uint8_t bg);  /* 256-color   */
void hx421_term_reset_attr(Hx421Term *t);
void hx421_term_clear(Hx421Term *t);
void hx421_term_gotoxy(Hx421Term *t, uint16_t col, uint16_t row);
void hx421_term_prompt(Hx421Term *t);                          /* redraw "> " */

#endif /* HX421_TERM_H */
