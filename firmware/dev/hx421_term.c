/* ============================================================
 *  hx421_term.c — dev-mode terminal implementation. See hx421_term.h.
 *  Portable C: no USB, no libc beyond vsnprintf/str basics.
 * ============================================================ */

#include "hx421_term.h"
#include <stdio.h>
#include <string.h>

void hx421_term_init(Hx421Term *t, Hx421TermSink sink, void *sink_ctx,
                     Hx421TermCmd on_cmd, void *cmd_ctx) {
    memset(t, 0, sizeof *t);
    t->sink = sink; t->sink_ctx = sink_ctx;
    t->on_cmd = on_cmd; t->cmd_ctx = cmd_ctx;
    t->echo = 1;
}

/* ---- TX ring ---- */

static uint32_t tx_used(const Hx421Term *t) { return t->tx_head - t->tx_tail; }

void hx421_term_write(Hx421Term *t, const void *buf, uint32_t n) {
    const uint8_t *p = (const uint8_t *)buf;
    for (uint32_t i = 0; i < n; ++i) {
        /* full ring: DROP, count it, never block. Losing debug bytes is
         * always better than stalling the caller (a game frame). */
        if (tx_used(t) >= HX421_TERM_TX_SIZE) { t->dropped += n - i; return; }
        t->tx[t->tx_head & HX421_TERM_TX_MASK] = p[i];
        t->tx_head++;
    }
}

void hx421_term_puts(Hx421Term *t, const char *s) {
    hx421_term_write(t, s, (uint32_t)strlen(s));
}

int hx421_term_printf(Hx421Term *t, const char *fmt, ...) {
    char buf[256];
    va_list ap; va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    if (n < 0) return n;
    uint32_t w = (uint32_t)n < sizeof buf ? (uint32_t)n : sizeof buf - 1;
    hx421_term_write(t, buf, w);
    return n;
}

uint32_t hx421_term_pending(const Hx421Term *t) { return tx_used(t); }

uint32_t hx421_term_drain(Hx421Term *t) {
    uint32_t sent = 0;
    while (tx_used(t)) {
        /* Contiguous run up to the ring wrap, so the sink gets one flat span. */
        uint32_t tail = t->tx_tail & HX421_TERM_TX_MASK;
        uint32_t run  = HX421_TERM_TX_SIZE - tail;      /* to end of buffer */
        uint32_t have = tx_used(t);
        if (run > have) run = have;
        uint32_t took = t->sink(t->sink_ctx, &t->tx[tail], run);
        t->tx_tail += took;
        sent += took;
        if (took < run) break;                          /* back-pressure    */
    }
    return sent;
}

/* ---- RX line editor ---- */

static void dispatch(Hx421Term *t) {
    /* tokenize t->line in place */
    char *argv[HX421_TERM_MAX_ARGS];
    int argc = 0;
    char *p = t->line;
    while (*p && argc < HX421_TERM_MAX_ARGS) {
        while (*p == ' ' || *p == '\t') *p++ = 0;
        if (!*p) break;
        argv[argc++] = p;
        while (*p && *p != ' ' && *p != '\t') p++;
    }
    if (argc && t->on_cmd) t->on_cmd(t->cmd_ctx, argc, argv);
}

void hx421_term_rx(Hx421Term *t, uint8_t c) {
    switch (c) {
        case '\r':
        case '\n':
            if (t->echo) hx421_term_write(t, "\r\n", 2);
            t->line[t->line_len] = 0;
            if (t->line_len) dispatch(t);
            t->line_len = 0;
            hx421_term_prompt(t);
            return;
        case 0x08:   /* backspace */
        case 0x7F:   /* DEL */
            if (t->line_len) {
                t->line_len--;
                if (t->echo) hx421_term_write(t, "\b \b", 3);  /* erase on screen */
            }
            return;
        case 0x15:   /* Ctrl-U: kill line */
            while (t->line_len) {
                t->line_len--;
                if (t->echo) hx421_term_write(t, "\b \b", 3);
            }
            return;
        default:
            if (c >= 0x20 && t->line_len < HX421_TERM_LINE_MAX - 1) {
                t->line[t->line_len++] = (char)c;
                if (t->echo) hx421_term_write(t, &c, 1);
            }
            return;
    }
}

/* ---- ANSI helpers ---- */

void hx421_term_color(Hx421Term *t, uint8_t fg, uint8_t bg) {
    hx421_term_printf(t, "\x1b[38;5;%um\x1b[48;5;%um", (unsigned)fg, (unsigned)bg);
}
void hx421_term_reset_attr(Hx421Term *t) { hx421_term_puts(t, "\x1b[0m"); }
void hx421_term_clear(Hx421Term *t)      { hx421_term_puts(t, "\x1b[2J\x1b[H"); }
void hx421_term_gotoxy(Hx421Term *t, uint16_t col, uint16_t row) {
    hx421_term_printf(t, "\x1b[%u;%uH", (unsigned)row, (unsigned)col);
}
void hx421_term_prompt(Hx421Term *t) { hx421_term_puts(t, "hx421> "); }
