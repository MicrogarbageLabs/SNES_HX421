/* hx421_sysimpl_test.c — host test for the syscall-table builder's new logic:
 * the game arena allocator, the path sandbox, and the slot/handle wiring. The
 * firmware backend is mocked, so this proves the parts we WRITE without the
 * firmware. */

#include <stdio.h>
#include <string.h>
#include <stdarg.h>
#include "../firmware/dev/hx421_sysimpl.h"

static int fails = 0, checks = 0;
static void ck(int cond, const char *what) {
    checks++;
    if (!cond) { printf("  FAIL: %s\n", what); fails++; }
}

/* ---- arena ---- */
static void test_arena(void) {
    Hx421SysImpl s;
    static uint8_t mem[128];
    hx421_arena_init(&s, mem, sizeof mem);

    void *a = hx421_arena_alloc(&s, 1);
    ck(a == mem, "first alloc is at the base");
    void *b = hx421_arena_alloc(&s, 1);
    ck((uint8_t *)b - (uint8_t *)a == 4, "allocations are 4-byte aligned");

    /* fill the rest; the next alloc must fail cleanly, not wrap */
    ck(hx421_arena_alloc(&s, 128) == 0, "an over-capacity alloc returns NULL");
    ck(hx421_arena_alloc(&s, 0xFFFFFFF0u) == 0, "a huge alloc cannot integer-overflow the bound");

    /* LIFO free: freeing the most recent block reclaims it */
    hx421_arena_init(&s, mem, sizeof mem);
    void *x = hx421_arena_alloc(&s, 16);
    void *y = hx421_arena_alloc(&s, 16);
    hx421_arena_free(&s, y);
    void *z = hx421_arena_alloc(&s, 16);
    ck(z == y, "LIFO free of the top block lets the next alloc reuse it");
    hx421_arena_free(&s, x);           /* not the top -> no-op */
    void *w = hx421_arena_alloc(&s, 16);
    ck(w != x, "freeing a non-top block does not reclaim it (documented LIFO)");
}

/* ---- sandbox ---- */
static void test_sandbox(void) {
    char out[HX421_SANDBOX_MAX];
    const char *pfx = "/sd2snes/hx421/mygame";

    ck(hx421_sandbox_path(out, sizeof out, pfx, "save.dat") == 0 &&
       strcmp(out, "/sd2snes/hx421/mygame/save.dat") == 0, "a normal path is prefixed");

    ck(hx421_sandbox_path(out, sizeof out, pfx, "levels/1.map") == 0 &&
       strcmp(out, "/sd2snes/hx421/mygame/levels/1.map") == 0, "subdirectories pass through");

    /* a prefix that already ends in / must not double it */
    ck(hx421_sandbox_path(out, sizeof out, "/root/", "f") == 0 &&
       strcmp(out, "/root/f") == 0, "trailing slash on the prefix is not doubled");

    /* escapes */
    ck(hx421_sandbox_path(out, sizeof out, pfx, "/etc/passwd") < 0, "absolute path rejected");
    ck(hx421_sandbox_path(out, sizeof out, pfx, "..") < 0, "bare .. rejected");
    ck(hx421_sandbox_path(out, sizeof out, pfx, "../secret") < 0, "leading ../ rejected");
    ck(hx421_sandbox_path(out, sizeof out, pfx, "a/../../b") < 0, "middle .. rejected");
    ck(hx421_sandbox_path(out, sizeof out, pfx, "dir/..") < 0, "trailing /.. rejected");

    /* ..foo is NOT an escape — .. must be a whole path component */
    ck(hx421_sandbox_path(out, sizeof out, pfx, "..foo") == 0, "..foo is a normal name, not an escape");
    ck(hx421_sandbox_path(out, sizeof out, pfx, "a..b") == 0, "a..b is a normal name");

    /* too long */
    char big[HX421_SANDBOX_MAX + 8];
    memset(big, 'x', sizeof big - 1); big[sizeof big - 1] = 0;
    ck(hx421_sandbox_path(out, sizeof out, pfx, big) < 0, "an over-long path is rejected, not truncated");
}

/* ---- a mock firmware backend + the full table wiring ---- */
static char     g_out[512]; static uint32_t g_outlen;
static int      g_fatal;
static uint32_t g_yields;
static char     g_last_abspath[HX421_SANDBOX_MAX];
static uint8_t  g_file[64]; static uint32_t g_flen, g_fpos; static int g_fopen_calls;
static uint32_t g_pad[4];

static void m_puts(void *c, const char *s) { (void)c; uint32_t n = (uint32_t)strlen(s);
    if (g_outlen + n < sizeof g_out) { memcpy(g_out + g_outlen, s, n); g_outlen += n; g_out[g_outlen]=0; } }
static int  m_vprint(void *c, const char *fmt, va_list ap) { (void)c;
    char b[256]; int n = vsnprintf(b, sizeof b, fmt, ap); m_puts(0, b); return n; }
static void m_fatal(void *c, uint32_t code) { (void)c; (void)code; g_fatal = 1; }
static void m_yield(void *c) { (void)c; g_yields++; }
static hx421_handle m_fopen(void *c, const char *abspath, uint32_t mode) { (void)c;
    strncpy(g_last_abspath, abspath, sizeof g_last_abspath - 1);
    g_fopen_calls++;
    g_fpos = 0; if (mode & HX421_O_CREATE) g_flen = 0;
    return 42; }
static uint32_t m_fread(void *c, hx421_handle h, void *d, uint32_t n) {
    (void)c; if (h != 42) return 0;
    if (g_fpos + n > g_flen) n = g_flen - g_fpos;
    memcpy(d, g_file + g_fpos, n); g_fpos += n; return n;
}
static uint32_t m_fwrite(void *c, hx421_handle h, const void *s, uint32_t n) {
    (void)c; if (h != 42) return 0;
    if (g_fpos + n > sizeof g_file) n = sizeof g_file - g_fpos;
    memcpy(g_file + g_fpos, s, n); g_fpos += n;
    if (g_fpos > g_flen) g_flen = g_fpos;
    return n;
}
static uint32_t m_fseek(void *c, hx421_handle h, uint32_t o) { (void)c; if (h!=42) return (uint32_t)-1;
    g_fpos = o <= g_flen ? o : g_flen; return g_fpos; }
static void m_fclose(void *c, hx421_handle h) { (void)c; (void)h; }
static uint32_t m_input(void *c, uint32_t pad) { (void)c; return pad < 4 ? g_pad[pad] : 0; }

static const Hx421FwBackend g_backend = {
    0, m_puts, m_vprint, m_fatal, m_yield,
    m_fopen, m_fread, m_fwrite, m_fseek, m_fclose, m_input
};

static void test_wiring(void) {
    Hx421SysImpl impl;
    static uint8_t arena[128];
    Hx421Sys t;
    hx421_sys_build(&impl, &g_backend, arena, sizeof arena, "/sd2snes/hx421/mygame", &t);

    ck(t.abi_version == HX421_ABI_VERSION, "table carries the ABI version");

    /* console goes through the backend */
    g_outlen = 0; g_out[0] = 0;
    t.print("hi ");
    t.print_f("n=%d", 7);
    ck(strcmp(g_out, "hi n=7") == 0, "print/printf route through the firmware backend");

    /* input */
    g_pad[0] = 0xAB;
    ck(t.input_read(0) == 0xAB, "input routes through the backend");

    /* malloc uses the arena, not the firmware */
    void *p = t.mem_alloc(16);
    ck((uint8_t *)p >= arena && (uint8_t *)p < arena + sizeof arena, "malloc comes from the game arena");

    /* open sandboxes the path before the backend sees it */
    hx421_handle f = t.open("save.dat", HX421_O_WRITE | HX421_O_CREATE);
    ck(f >= 0, "open returns a game handle");
    ck(strcmp(g_last_abspath, "/sd2snes/hx421/mygame/save.dat") == 0,
       "the backend receives the SANDBOXED absolute path");
    t.write(f, "ABCD", 4);
    t.seek(f, 0);
    char back[5] = {0};
    ck(t.read(f, back, 4) == 4 && memcmp(back, "ABCD", 4) == 0, "file round-trips through the slot table");
    t.close(f);

    /* a game trying to escape the sandbox is refused at open() */
    ck(t.open("../../menu.bin", HX421_O_READ) < 0, "open refuses a sandbox escape");

    /* the open-file cap: only HX421_MAX_OPEN handles at once */
    hx421_handle hs[HX421_MAX_OPEN + 2];
    int opened = 0;
    for (unsigned i = 0; i < HX421_MAX_OPEN + 2; ++i) {
        hs[i] = t.open("f", HX421_O_READ);
        if (hs[i] >= 0) opened++;
    }
    ck(opened == (int)HX421_MAX_OPEN, "open is capped at HX421_MAX_OPEN handles");

    t.yield();
    ck(g_yields == 1 && !g_fatal, "yield reaches the backend; no spurious abort");
}

int main(void) {
    printf("hx421 sysimpl tests\n");
    test_arena();
    test_sandbox();
    test_wiring();
    printf("%d checks, %d failures\n", checks, fails);
    return fails ? 1 : 0;
}
