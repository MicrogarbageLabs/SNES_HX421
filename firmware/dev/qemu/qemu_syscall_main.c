/* qemu_syscall_main.c — tier-3 execution proof: run the firmware<->game syscall
 * boundary as real Thumb code on a Cortex-M4 in QEMU. Validates what the host
 * test and the cross-compile cannot: actual EXECUTION on the target ABI (4-byte
 * pointers, real AAPCS, the indirect calls through the table), a game jumping
 * through the firmware. Output + exit via semihosting. See docs/dev-mode.md.
 *
 * Self-contained: no newlib, no libc. A mini formatter and tiny str/mem helpers
 * keep it dependency-free so the only thing under test is the boundary. */

#include "hx421_gamert.h"   /* pulls hx421_syscall.h */

/* from startup_m4.c */
void qh_write0(const char *s);
void qh_exit(int code);

/* ---- tiny freestanding helpers ---- */
static uint32_t qslen(const char *s) { uint32_t n = 0; while (s[n]) n++; return n; }
static void     qcpy(void *d, const void *s, uint32_t n) { uint8_t *a=d; const uint8_t *b=s; while (n--) *a++=*b++; }
static int      qstr(const char *hay, const char *needle) {
    for (; *hay; ++hay) {
        const char *h=hay, *n=needle;
        while (*n && *h==*n) { h++; n++; }
        if (!*n) return 1;
    }
    return 0;
}

/* ---- mini vsnprintf: %s %c %u %d %x %X with 0-pad + width (enough for the
 * game's formats, e.g. %08X). No newlib. ---- */
static void put_u(char **d, char *end, uint32_t v, unsigned base, int upper, int width, int zero) {
    char tmp[16]; int n = 0;
    const char *digs = upper ? "0123456789ABCDEF" : "0123456789abcdef";
    if (v == 0) tmp[n++] = '0';
    while (v) { tmp[n++] = digs[v % base]; v /= base; }
    for (int pad = width - n; pad > 0; --pad) if (*d < end) *(*d)++ = zero ? '0' : ' ';
    while (n-- > 0) if (*d < end) *(*d)++ = tmp[n];
}
static int qvsnprintf(char *out, int cap, const char *fmt, __builtin_va_list ap) {
    char *d = out, *end = out + cap - 1;
    for (; *fmt; ++fmt) {
        if (*fmt != '%') { if (d < end) *d++ = *fmt; continue; }
        ++fmt;
        int zero = 0, width = 0;
        if (*fmt == '0') { zero = 1; ++fmt; }
        while (*fmt >= '0' && *fmt <= '9') { width = width*10 + (*fmt - '0'); ++fmt; }
        switch (*fmt) {
            case 's': { const char *s = __builtin_va_arg(ap, const char *); while (*s && d < end) *d++ = *s++; } break;
            case 'c': { char c = (char)__builtin_va_arg(ap, int); if (d < end) *d++ = c; } break;
            case 'u': put_u(&d, end, __builtin_va_arg(ap, unsigned), 10, 0, width, zero); break;
            case 'd': { int v = __builtin_va_arg(ap, int);
                        if (v < 0) { if (d<end)*d++='-'; put_u(&d,end,(unsigned)(-v),10,0,width,zero); }
                        else put_u(&d,end,(unsigned)v,10,0,width,zero); } break;
            case 'x': put_u(&d, end, __builtin_va_arg(ap, unsigned), 16, 0, width, zero); break;
            case 'X': put_u(&d, end, __builtin_va_arg(ap, unsigned), 16, 1, width, zero); break;
            case '%': if (d < end) *d++ = '%'; break;
            default:  if (d < end) *d++ = '%'; if (d < end) *d++ = *fmt; break;
        }
    }
    *d = 0;
    return (int)(d - out);
}

/* ---- firmware-side table impls (host of the boundary), routed to semihosting
 * AND a capture buffer so qemu_main can assert on what the game produced. ---- */
static char     g_cap[2048];
static uint32_t g_cap_len;
static uint32_t g_yields;
static uint32_t g_abort_code;
static int      g_aborted;

static void cap(const char *s, uint32_t n) {
    if (g_cap_len + n >= sizeof g_cap) n = sizeof g_cap - g_cap_len - 1;
    qcpy(g_cap + g_cap_len, s, n); g_cap_len += n; g_cap[g_cap_len] = 0;
}
static void host_print(const char *s) { cap(s, qslen(s)); qh_write0(s); }
static int  host_printf(const char *fmt, ...) {
    char buf[256];
    __builtin_va_list ap; __builtin_va_start(ap, fmt);
    int n = qvsnprintf(buf, sizeof buf, fmt, ap);
    __builtin_va_end(ap);
    cap(buf, (uint32_t)n); qh_write0(buf);
    return n;
}
static void host_term(uint32_t op, uint32_t a, uint32_t b) { (void)op;(void)a;(void)b; }
static void host_yield(void) { g_yields++; }
static void host_abort(uint32_t code) { g_abort_code = code; g_aborted = 1; }

/* in-memory file + bump heap, so open/read/write/seek/malloc all cross the table */
static uint8_t  g_file[64]; static uint32_t g_flen, g_fpos; static int g_fopen;
static hx421_handle host_open(const char *p, uint32_t m) {
    (void)p;
    if (g_fopen) return -1;
    g_fopen = 1; g_fpos = 0;
    if (m & HX421_O_CREATE) g_flen = 0;
    return 3;
}
static uint32_t host_write(hx421_handle h, const void *s, uint32_t n) {
    if (h != 3) return 0;
    if (g_fpos + n > sizeof g_file) n = sizeof g_file - g_fpos;
    qcpy(g_file + g_fpos, s, n);
    g_fpos += n;
    if (g_fpos > g_flen) g_flen = g_fpos;
    return n;
}
static uint32_t host_read(hx421_handle h, void *d, uint32_t n) {
    if (h != 3) return 0;
    if (g_fpos + n > g_flen) n = g_flen - g_fpos;
    qcpy(d, g_file + g_fpos, n);
    g_fpos += n;
    return n;
}
static uint32_t host_seek(hx421_handle h, uint32_t o) {
    if (h != 3) return (uint32_t)-1;
    g_fpos = o <= g_flen ? o : g_flen;
    return g_fpos;
}
static void host_close(hx421_handle h) { if (h == 3) g_fopen = 0; }

static uint8_t  g_heap[256]; static uint32_t g_hp;
static void *host_malloc(uint32_t n) { n=(n+3)&~3u; if (g_hp+n>sizeof g_heap) return 0;
    void *p=&g_heap[g_hp]; g_hp+=n; return p; }
static void host_free(void *p) { (void)p; }   /* bump allocator: free is a no-op */

static uint32_t g_pad[4];
static uint32_t host_input(uint32_t pad) { return pad < 4 ? g_pad[pad] : 0; }

static Hx421Sys make_table(uint32_t version) {
    Hx421Sys s;
    s.abi_version = version;
    s.yield=host_yield; s.sys_abort=host_abort;
    s.print=host_print; s.print_f=host_printf; s.term_ctrl=host_term;
    s.open=host_open; s.read=host_read; s.write=host_write; s.seek=host_seek; s.close=host_close;
    s.mem_alloc=host_malloc; s.mem_free=host_free; s.input_read=host_input;
    return s;
}

/* ---- the game: references ONLY sys_* ---- */
static void demo_game(const Hx421Sys *sys) {
    if (hx421_game_bind(sys) != 0) { sys_abort(0xBAD); return; }
    sys_print("HX421 GAME UP\n");
    sys_printf("abi %u.%u pad0=%08X\n",
               (unsigned)(sys->abi_version >> 16), (unsigned)(sys->abi_version & 0xFFFF),
               (unsigned)sys_input(0));
    char *buf = (char *)sys_malloc(16);
    for (int i = 0; i < 8; ++i) buf[i] = (char)('A' + i);
    hx421_handle f = sys_open("save.bin", HX421_O_WRITE | HX421_O_CREATE);
    sys_write(f, buf, 8);
    sys_seek(f, 0);
    char back[9] = {0};
    sys_read(f, back, 8);
    sys_close(f);
    sys_printf("file=%s\n", back);
    sys_free(buf);
    sys_yield();
}

int qemu_main(void) {
    qh_write0("hx421 syscall boundary (qemu, cortex-m4)\n");

    /* the target ABI, proven by execution not just compilation */
    if (sizeof(Hx421Sys) != 14 * 4) { qh_write0("QEMU FAIL: Hx421Sys not 56 bytes\n"); return 1; }

    g_cap_len = 0; g_yields = 0; g_aborted = 0; g_pad[0] = 0x55;
    Hx421Sys t = make_table(HX421_ABI_VERSION);
    demo_game(&t);

    int ok = 1;
    if (g_aborted)                              { qh_write0("QEMU FAIL: matched game aborted\n"); ok=0; }
    if (!qstr(g_cap, "HX421 GAME UP"))          { qh_write0("QEMU FAIL: print\n"); ok=0; }
    if (!qstr(g_cap, "abi 1.0"))                { qh_write0("QEMU FAIL: printf\n"); ok=0; }
    if (!qstr(g_cap, "pad0=00000055"))          { qh_write0("QEMU FAIL: input/width\n"); ok=0; }
    if (!qstr(g_cap, "file=ABCDEFGH"))          { qh_write0("QEMU FAIL: file round-trip\n"); ok=0; }
    if (g_yields != 1)                          { qh_write0("QEMU FAIL: yield\n"); ok=0; }

    /* the ABI gate must refuse a wrong-MAJOR firmware */
    g_aborted = 0; g_cap_len = 0; g_cap[0] = 0;
    Hx421Sys wrong = make_table(((uint32_t)(HX421_ABI_MAJOR + 1) << 16));
    demo_game(&wrong);
    if (!(g_aborted && g_abort_code == 0xBAD))  { qh_write0("QEMU FAIL: mismatch not refused\n"); ok=0; }
    if (qstr(g_cap, "GAME UP"))                 { qh_write0("QEMU FAIL: ran past version check\n"); ok=0; }

    qh_write0(ok ? "QEMU PASS\n" : "QEMU FAIL\n");
    return ok ? 0 : 1;
}
