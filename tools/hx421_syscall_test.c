/* hx421_syscall_test.c — exercise the firmware<->game syscall boundary on the
 * host, the same way hx421_stream is host-tested.
 *
 * Proves a "game" that calls ONLY sys_*() runs correctly against a
 * firmware-populated table: console output goes through the firmware's
 * formatter, file I/O round-trips, heap and input route through the table, and
 * a MAJOR ABI mismatch is refused. The point is that the game code below
 * references no libc directly — only the table.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <setjmp.h>
#include "../firmware/dev/hx421_gamert.h"

static int fails = 0, checks = 0;
static void ck(int cond, const char *what) {
    checks++;
    if (!cond) { printf("  FAIL: %s\n", what); fails++; }
}

/* ---- firmware side: host-backed implementations of every table slot ---- */

static char     g_console[8192];   /* captures everything the game prints */
static unsigned g_console_len;
static uint32_t g_yields;
static uint32_t g_abort_code;
static int      g_aborted;
static jmp_buf  g_abort_jmp;
static uint32_t g_term_last[3];    /* last term_ctrl op,a,b */

static void host_out(const char *s, unsigned n) {
    if (g_console_len + n >= sizeof g_console) n = sizeof g_console - g_console_len - 1;
    memcpy(g_console + g_console_len, s, n);
    g_console_len += n;
    g_console[g_console_len] = 0;
}
static void host_print(const char *s) { host_out(s, (unsigned)strlen(s)); }
static int  host_printf(const char *fmt, ...) {
    char buf[512];
    va_list ap; va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    if (n > 0) host_out(buf, (unsigned)(n < (int)sizeof buf ? n : (int)sizeof buf - 1));
    return n;
}
static void host_term(uint32_t op, uint32_t a, uint32_t b) {
    g_term_last[0] = op; g_term_last[1] = a; g_term_last[2] = b;
}
static void host_yield(void) { g_yields++; }
static void host_abort(uint32_t code) {
    g_abort_code = code; g_aborted = 1;
    longjmp(g_abort_jmp, 1);   /* unwind back to the harness, like never returning */
}

/* a one-file in-memory "SD card" */
static uint8_t g_file[256];
static uint32_t g_file_len;
static int      g_file_open;
static uint32_t g_file_pos;

static hx421_handle host_open(const char *path, uint32_t mode) {
    (void)path;
    if (g_file_open) return -1;
    g_file_open = 1; g_file_pos = 0;
    if (mode & HX421_O_CREATE) g_file_len = 0;
    return 3;   /* any non-negative handle */
}
static uint32_t host_write(hx421_handle h, const void *src, uint32_t n) {
    if (h != 3 || !g_file_open) return 0;
    if (g_file_pos + n > sizeof g_file) n = sizeof g_file - g_file_pos;
    memcpy(g_file + g_file_pos, src, n);
    g_file_pos += n;
    if (g_file_pos > g_file_len) g_file_len = g_file_pos;
    return n;
}
static uint32_t host_read(hx421_handle h, void *dst, uint32_t n) {
    if (h != 3 || !g_file_open) return 0;
    if (g_file_pos + n > g_file_len) n = g_file_len - g_file_pos;
    memcpy(dst, g_file + g_file_pos, n);
    g_file_pos += n;
    return n;
}
static uint32_t host_seek(hx421_handle h, uint32_t off) {
    if (h != 3 || !g_file_open) return (uint32_t)-1;
    g_file_pos = off <= g_file_len ? off : g_file_len;
    return g_file_pos;
}
static void host_close(hx421_handle h) { if (h == 3) g_file_open = 0; }

static void *host_malloc(uint32_t n) { return malloc(n); }
static void  host_free(void *p)      { free(p); }

static uint32_t g_pad[4];
static uint32_t host_input(uint32_t pad) { return pad < 4 ? g_pad[pad] : 0; }

static Hx421Sys make_table(uint32_t version) {
    Hx421Sys s;
    s.abi_version = version;
    s.yield = host_yield;   s.sys_abort = host_abort;
    s.print = host_print;   s.print_f = host_printf;  s.term_ctrl = host_term;
    s.open = host_open;     s.read = host_read;       s.write = host_write;
    s.seek = host_seek;     s.close = host_close;
    s.mem_alloc = host_malloc; s.mem_free = host_free;
    s.input_read = host_input;
    return s;
}

/* ================= the "game" — references ONLY sys_* ================= */

static void demo_game(const Hx421Sys *sys) {
    if (hx421_game_bind(sys) != 0) { sys_abort(0xBAD); return; }

    sys_print("HX421 GAME UP\n");
    sys_printf("abi %u.%u, pad0=%08X\n",
               (unsigned)(sys->abi_version >> 16),
               (unsigned)(sys->abi_version & 0xFFFF),
               (unsigned)sys_input(0));

    /* heap through the table */
    char *buf = (char *)sys_malloc(32);
    for (int i = 0; i < 8; ++i) buf[i] = (char)('A' + i);

    /* file round-trip through the table */
    hx421_handle f = sys_open("save.bin", HX421_O_WRITE | HX421_O_CREATE);
    sys_write(f, buf, 8);
    sys_seek(f, 0);
    char back[8] = {0};
    uint32_t got = sys_read(f, back, 8);
    sys_close(f);
    sys_printf("wrote/read %u: %.8s\n", (unsigned)got, back);
    sys_free(buf);

    sys_term(1, 4, 2);   /* e.g. set-color(4,2) — recorded, not interpreted */
    sys_yield();
}

/* ===================================================================== */

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);   /* so a crash cannot swallow progress */
    printf("hx421 syscall boundary tests\n");

    /* --- a game running against a matched table --- */
    g_console_len = 0; g_console[0] = 0; g_yields = 0; g_aborted = 0;
    g_pad[0] = 0x0055;
    Hx421Sys t = make_table(HX421_ABI_VERSION);
    if (setjmp(g_abort_jmp) == 0) demo_game(&t);

    ck(!g_aborted, "a matched-ABI game does not abort");
    ck(strstr(g_console, "HX421 GAME UP") != 0, "sys_print reached the firmware console");
    ck(strstr(g_console, "abi 1.0") != 0, "sys_printf formatted via the firmware");
    ck(strstr(g_console, "pad0=00000055") != 0, "sys_input value crossed the boundary");
    ck(strstr(g_console, "wrote/read 8: ABCDEFGH") != 0, "file round-trip through the table");
    ck(g_yields == 1, "sys_yield reached the firmware");
    ck(g_term_last[0] == 1 && g_term_last[1] == 4 && g_term_last[2] == 2,
       "term_ctrl args crossed intact");

    /* --- the ABI gate: a game refuses a wrong-MAJOR firmware --- */
    g_aborted = 0; g_console_len = 0; g_console[0] = 0;
    Hx421Sys wrong = make_table(((uint32_t)(HX421_ABI_MAJOR + 1) << 16) | 0u);
    if (setjmp(g_abort_jmp) == 0) demo_game(&wrong);
    ck(g_aborted && g_abort_code == 0xBAD, "a MAJOR-mismatch firmware is refused, not run");
    ck(strstr(g_console, "GAME UP") == 0, "and nothing after the version check ran");

    /* --- a NEWER-MINOR firmware still runs an old game (append-only) --- */
    g_aborted = 0; g_console_len = 0; g_console[0] = 0;
    Hx421Sys newer = make_table(((uint32_t)HX421_ABI_MAJOR << 16) | (HX421_ABI_MINOR + 5));
    if (setjmp(g_abort_jmp) == 0) demo_game(&newer);
    ck(!g_aborted && strstr(g_console, "GAME UP") != 0,
       "a newer-MINOR firmware still runs the game (append-only compat)");

    printf("%d checks, %d failures\n", checks, fails);
    return fails ? 1 : 0;
}
