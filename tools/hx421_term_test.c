/* hx421_term_test.c — host test for the dev-mode terminal.
 *
 * The USB endpoint is modelled by a capture-buffer sink that can apply
 * back-pressure (take fewer bytes than offered), and RX is fed byte-by-byte as
 * the CDC OUT endpoint would. Proves: non-blocking drop on overflow, ordered
 * drain across the ring wrap, back-pressure leaves the remainder queued, line
 * editing + tokenizing, echo, and ANSI emission.
 */

#include <stdio.h>
#include <string.h>
#include "../firmware/dev/hx421_term.h"

static int fails = 0, checks = 0;
static void ck(int cond, const char *what) {
    checks++;
    if (!cond) { printf("  FAIL: %s\n", what); fails++; }
}

/* ---- the "USB endpoint": a capture buffer with optional per-call cap ---- */
static uint8_t  g_cap[8192];
static uint32_t g_cap_len;
static uint32_t g_take_limit;   /* 0 = take all; else max bytes per drain call */

static uint32_t sink(void *ctx, const uint8_t *buf, uint32_t n) {
    (void)ctx;
    if (g_take_limit && n > g_take_limit) n = g_take_limit;
    if (g_cap_len + n > sizeof g_cap) n = (uint32_t)sizeof g_cap - g_cap_len;
    memcpy(g_cap + g_cap_len, buf, n);
    g_cap_len += n;
    return n;
}
static void cap_reset(void) { g_cap_len = 0; g_take_limit = 0; }

/* ---- the command recorder ---- */
static int  g_argc;
static char g_argv[HX421_TERM_MAX_ARGS][64];
static int  g_cmd_calls;
static void on_cmd(void *ctx, int argc, char **argv) {
    (void)ctx;
    g_cmd_calls++;
    g_argc = argc;
    for (int i = 0; i < argc && i < HX421_TERM_MAX_ARGS; ++i) {
        strncpy(g_argv[i], argv[i], sizeof g_argv[i] - 1);
        g_argv[i][sizeof g_argv[i] - 1] = 0;
    }
}

static void feed(Hx421Term *t, const char *s) {
    for (const char *p = s; *p; ++p) hx421_term_rx(t, (uint8_t)*p);
}

int main(void) {
    printf("hx421 terminal tests\n");
    Hx421Term t;

    /* ---- basic TX + drain ---- */
    hx421_term_init(&t, sink, 0, on_cmd, 0);
    cap_reset();
    hx421_term_puts(&t, "hello");
    ck(hx421_term_pending(&t) == 5, "puts queues bytes without sending");
    ck(g_cap_len == 0, "nothing reaches the sink until drained");
    hx421_term_drain(&t);
    ck(g_cap_len == 5 && memcmp(g_cap, "hello", 5) == 0, "drain delivers in order");
    ck(hx421_term_pending(&t) == 0, "ring empty after a full drain");

    hx421_term_printf(&t, "n=%d s=%s", 42, "ok");
    hx421_term_drain(&t);
    ck(strstr((char *)g_cap, "n=42 s=ok") != 0, "printf formats into the ring");

    /* ---- non-blocking DROP on overflow ---- */
    hx421_term_init(&t, sink, 0, on_cmd, 0);
    cap_reset();
    static char big[2000];
    memset(big, 'X', sizeof big);
    hx421_term_write(&t, big, sizeof big);
    ck(hx421_term_pending(&t) == HX421_TERM_TX_SIZE, "ring fills to capacity, no more");
    ck(t.dropped == sizeof big - HX421_TERM_TX_SIZE, "the overflow is counted, not silent");
    /* it never blocked or corrupted — the bytes it DID keep are the first ones */
    hx421_term_drain(&t);
    ck(g_cap_len == HX421_TERM_TX_SIZE, "exactly the capacity survived");

    /* ---- back-pressure: a busy endpoint takes a little at a time ---- */
    hx421_term_init(&t, sink, 0, on_cmd, 0);
    cap_reset();
    g_take_limit = 30;                       /* endpoint accepts 30/call */
    hx421_term_puts(&t, "0123456789ABCDEFGHIJ0123456789ABCDEFGHIJ0123456789"); /* 50 */
    uint32_t d1 = hx421_term_drain(&t);
    ck(d1 == 30, "a back-pressured drain sends only what the endpoint took");
    ck(hx421_term_pending(&t) == 20, "the remainder stays queued");
    g_take_limit = 0;
    hx421_term_drain(&t);
    ck(g_cap_len == 50 && memcmp(g_cap, "0123456789ABCDEFGHIJ0123456789ABCDEFGHIJ0123456789", 50) == 0,
       "the queued remainder finishes intact and in order");

    /* ---- ring WRAP: drain correctly across the buffer boundary ---- */
    hx421_term_init(&t, sink, 0, on_cmd, 0);
    cap_reset();
    /* push the tail near the end, drain, then write past the wrap */
    for (int i = 0; i < 3; ++i) {
        char blk[400];
        memset(blk, 'a' + i, sizeof blk);
        hx421_term_write(&t, blk, sizeof blk);
        hx421_term_drain(&t);                /* keeps head/tail advancing past 1024 */
    }
    ck(g_cap_len == 1200, "1200 bytes across the wrap all delivered");
    ck(g_cap[0] == 'a' && g_cap[400] == 'b' && g_cap[800] == 'c', "and in write order across the wrap");

    /* ---- RX: line editing + tokenizing ---- */
    hx421_term_init(&t, sink, 0, on_cmd, 0);
    cap_reset();
    g_cmd_calls = 0;
    feed(&t, "help\r");
    ck(g_cmd_calls == 1 && g_argc == 1 && strcmp(g_argv[0], "help") == 0, "single-word command dispatches");

    feed(&t, "run  game.bin\r");             /* extra space tolerated */
    ck(g_argc == 2 && strcmp(g_argv[0], "run") == 0 && strcmp(g_argv[1], "game.bin") == 0,
       "arguments tokenize, runs of spaces collapse");

    /* backspace edits before dispatch: "runX"<bs>" foo" -> "run foo" */
    g_cmd_calls = 0;
    feed(&t, "runX");
    hx421_term_rx(&t, 0x08);                 /* backspace removes the X */
    feed(&t, " foo\r");
    ck(g_argc == 2 && strcmp(g_argv[0], "run") == 0 && strcmp(g_argv[1], "foo") == 0,
       "backspace edits the line before dispatch");

    /* Ctrl-U kills the whole line, so the next line stands alone */
    feed(&t, "garbage");
    hx421_term_rx(&t, 0x15);                 /* Ctrl-U */
    g_cmd_calls = 0;
    feed(&t, "reset\r");
    ck(g_cmd_calls == 1 && strcmp(g_argv[0], "reset") == 0, "Ctrl-U kills the line");

    /* a blank line dispatches nothing */
    g_cmd_calls = 0;
    feed(&t, "\r");
    ck(g_cmd_calls == 0, "an empty line runs no command");

    /* echo: what you type comes back out (drain first — echo lands in the ring) */
    cap_reset();
    feed(&t, "hi\r");
    hx421_term_drain(&t);
    g_cap[g_cap_len] = 0;
    ck(strstr((char *)g_cap, "hi") != 0, "typed characters are echoed");

    /* ---- ANSI emission ---- */
    hx421_term_init(&t, sink, 0, on_cmd, 0);
    cap_reset();
    hx421_term_color(&t, 208, 0);            /* orange fg */
    hx421_term_drain(&t);
    g_cap[g_cap_len] = 0;
    ck(strcmp((char *)g_cap, "\x1b[38;5;208m\x1b[48;5;0m") == 0, "256-color escape is exact");

    cap_reset();
    hx421_term_clear(&t);
    hx421_term_drain(&t);
    g_cap[g_cap_len] = 0;
    ck(strcmp((char *)g_cap, "\x1b[2J\x1b[H") == 0, "clear-screen escape is exact");

    printf("%d checks, %d failures\n", checks, fails);
    return fails ? 1 : 0;
}
