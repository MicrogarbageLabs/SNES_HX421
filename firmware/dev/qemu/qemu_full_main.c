/* qemu_full_main.c — the CAPSTONE tier-3 test: the real firmware-side table
 * builder (hx421_sysimpl) drives a loaded game on a Cortex-M4. The loader test
 * used a hand-built mock table; this uses hx421_sys_build() — the exact code the
 * firmware will run — with the loader placing a separate game blob, so the whole
 * dev-mode chain (sys_build -> loader -> game -> arena + sandbox) is proven on
 * real Thumb execution. See docs/dev-mode.md.
 *
 * Self-contained backend (semihosting + in-memory file) so it doesn't touch the
 * other passing QEMU harnesses. */

#include "hx421_loader.h"
#include "hx421_memmap.h"
#include "hx421_sysimpl.h"

void qh_write0(const char *s);   /* startup_m4.c */

extern const uint8_t _binary_game_hxg_start[];
extern const uint8_t _binary_game_hxg_end[];

#define GAME_BASE  ((void *)(uintptr_t)HX421_GAME_BASE)
#define GAME_SIZE  HX421_GAME_SIZE

/* ---- tiny mini-printf (no newlib) for the backend's vprint ---- */
static void put_u(char **d, char *end, uint32_t v, unsigned base, int upper, int width, int zero) {
    char tmp[16]; int n = 0;
    const char *digs = upper ? "0123456789ABCDEF" : "0123456789abcdef";
    if (v == 0) tmp[n++] = '0';
    while (v) { tmp[n++] = digs[v % base]; v /= base; }
    for (int pad = width - n; pad > 0; --pad) if (*d < end) *(*d)++ = zero ? '0' : ' ';
    while (n-- > 0) if (*d < end) *(*d)++ = tmp[n];
}
static int mini_vsn(char *out, int cap, const char *fmt, __builtin_va_list ap) {
    char *d = out, *end = out + cap - 1;
    for (; *fmt; ++fmt) {
        if (*fmt != '%') { if (d < end) *d++ = *fmt; continue; }
        ++fmt; int zero = 0, width = 0;
        if (*fmt == '0') { zero = 1; ++fmt; }
        while (*fmt >= '0' && *fmt <= '9') { width = width*10 + (*fmt-'0'); ++fmt; }
        switch (*fmt) {
            case 's': { const char *s = __builtin_va_arg(ap, const char*); while (*s && d<end) *d++=*s++; } break;
            case 'u': put_u(&d,end,__builtin_va_arg(ap,unsigned),10,0,width,zero); break;
            case 'd': { int v=__builtin_va_arg(ap,int); if(v<0){if(d<end)*d++='-';put_u(&d,end,(unsigned)(-v),10,0,width,zero);} else put_u(&d,end,(unsigned)v,10,0,width,zero);} break;
            case 'x': put_u(&d,end,__builtin_va_arg(ap,unsigned),16,0,width,zero); break;
            case 'X': put_u(&d,end,__builtin_va_arg(ap,unsigned),16,1,width,zero); break;
            case '%': if(d<end)*d++='%'; break;
            default: if(d<end)*d++='%'; if(d<end)*d++=*fmt; break;
        }
    }
    *d = 0; return (int)(d - out);
}

/* ---- capture + backend impls ---- */
static char     g_cap[1024]; static unsigned g_caplen;
static char     g_last_path[HX421_SANDBOX_MAX];
static unsigned g_yields;
static uint8_t  g_file[64]; static uint32_t g_flen, g_fpos; static int g_fopen;
static uint32_t g_pad[4];

static void cap(const char *s) { while (*s && g_caplen < sizeof g_cap - 1) g_cap[g_caplen++] = *s++; g_cap[g_caplen] = 0; }
static int  qstr(const char *h, const char *n) { for (; *h; ++h) { const char *a=h,*b=n; while(*b&&*a==*b){a++;b++;} if(!*b) return 1; } return 0; }

static void bk_puts(void *c, const char *s) { (void)c; cap(s); qh_write0(s); }
static int  bk_vprint(void *c, const char *fmt, __builtin_va_list ap) { (void)c;
    char b[256]; int n = mini_vsn(b, sizeof b, fmt, ap); cap(b); qh_write0(b); return n; }
static void bk_fatal(void *c, uint32_t code) { (void)c; (void)code; cap("[ABORT]"); }
static void bk_yield(void *c) { (void)c; g_yields++; }
static hx421_handle bk_open(void *c, const char *abspath, uint32_t mode) {
    (void)c;
    unsigned i = 0;
    for (; abspath[i] && i < sizeof g_last_path - 1; ++i) g_last_path[i] = abspath[i];
    g_last_path[i] = 0;
    if (g_fopen) return -1;
    g_fopen = 1; g_fpos = 0;
    if (mode & HX421_O_CREATE) g_flen = 0;
    return 7;
}
static uint32_t bk_read(void *c, hx421_handle h, void *d, uint32_t n) {
    (void)c; if (h != 7) return 0;
    if (g_fpos + n > g_flen) n = g_flen - g_fpos;
    for (uint32_t i = 0; i < n; ++i) ((uint8_t *)d)[i] = g_file[g_fpos + i];
    g_fpos += n; return n;
}
static uint32_t bk_write(void *c, hx421_handle h, const void *s, uint32_t n) {
    (void)c; if (h != 7) return 0;
    if (g_fpos + n > sizeof g_file) n = sizeof g_file - g_fpos;
    for (uint32_t i = 0; i < n; ++i) g_file[g_fpos + i] = ((const uint8_t *)s)[i];
    g_fpos += n;
    if (g_fpos > g_flen) g_flen = g_fpos;
    return n;
}
static uint32_t bk_seek(void *c, hx421_handle h, uint32_t o) { (void)c; if (h!=7) return (uint32_t)-1;
    g_fpos = o <= g_flen ? o : g_flen; return g_fpos; }
static void bk_close(void *c, hx421_handle h) { (void)c; if (h==7) g_fopen = 0; }
static uint32_t bk_input(void *c, uint32_t pad) { (void)c; return pad < 4 ? g_pad[pad] : 0; }

static const Hx421FwBackend g_backend = {
    0, bk_puts, bk_vprint, bk_fatal, bk_yield,
    bk_open, bk_read, bk_write, bk_seek, bk_close, bk_input
};

/* cursor over the embedded .hxg, returning short chunks like an SD read */
static const uint8_t *cur_p, *cur_end;
static uint32_t blob_read(void *ctx, void *dst, uint32_t n) {
    (void)ctx;
    uint32_t avail = (uint32_t)(cur_end - cur_p);
    if (n > avail) n = avail;
    if (n > 13) n = 13;                 /* deliberately short, to test the fill loop */
    for (uint32_t i = 0; i < n; ++i) ((uint8_t *)dst)[i] = cur_p[i];
    cur_p += n;
    return n;
}

/* the game heap arena: carved from the top of the game region, below the stack,
 * exactly as the firmware would. 4K here. */
#define ARENA_SIZE (4u * 1024u)
static uint8_t *arena_ptr(void) {
    return (uint8_t *)(uintptr_t)(HX421_GAME_BASE + HX421_GAME_SIZE - HX421_GAME_STACK - ARENA_SIZE);
}

int qemu_main(void) {
    qh_write0("hx421 full chain: sysimpl table + loader (qemu, cortex-m4)\n");
    g_pad[0] = 0xC0DE;

    /* Build the table the REAL firmware way. */
    static Hx421SysImpl impl;
    Hx421Sys table;
    hx421_sys_build(&impl, &g_backend, arena_ptr(), ARENA_SIZE, "/sd2snes/hx421/mygame", &table);

    /* Load and run the game through that table, via the STREAMING path — the
     * firmware's actual method (read straight into the region, never a full-file
     * buffer beside it). A cursor over the embedded blob stands in for f_read. */
    cur_p = _binary_game_hxg_start; cur_end = _binary_game_hxg_end;
    Hx421LoadResult r = hx421_loader_run_stream(GAME_BASE, GAME_SIZE, HX421_GAME_STACK,
                                                blob_read, 0, &table);

    int ok = 1;
    if (r != HX421_LOAD_OK)                        { qh_write0("QEMU FAIL: load != OK\n"); ok = 0; }
    if (!qstr(g_cap, "running from the game region")) { qh_write0("QEMU FAIL: game entry\n"); ok = 0; }
    if (!qstr(g_cap, "pad0=0000C0DE"))             { qh_write0("QEMU FAIL: input via sysimpl\n"); ok = 0; }
    if (!qstr(g_cap, "file=ABCDEFGH"))             { qh_write0("QEMU FAIL: file round-trip via sysimpl\n"); ok = 0; }
    /* the sandbox must have prefixed the game's path before the backend saw it */
    if (!qstr(g_last_path, "/sd2snes/hx421/mygame/save.dat")) { qh_write0("QEMU FAIL: sandbox not applied\n"); ok = 0; }
    if (g_yields != 1)                             { qh_write0("QEMU FAIL: yield\n"); ok = 0; }
    /* the game's malloc must have come from the arena (peak survives the game's
     * free; arena_top is back to 0 because the game frees its one block) */
    if (impl.arena_peak == 0)                      { qh_write0("QEMU FAIL: arena unused (malloc bypassed it)\n"); ok = 0; }
    if (!qstr(g_cap, "LOADED GAME OK"))            { qh_write0("QEMU FAIL: game did not finish\n"); ok = 0; }

    qh_write0(ok ? "QEMU PASS\n" : "QEMU FAIL\n");
    return ok ? 0 : 1;
}
