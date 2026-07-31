/* qemu_hostsys.c — firmware-side syscall table for the QEMU tests. See the
 * header. No newlib: a mini formatter + tiny str/mem helpers. */

#include "qemu_hostsys.h"

void qh_write0(const char *s);   /* startup_m4.c */

/* ---- tiny freestanding helpers ---- */
static uint32_t qslen(const char *s) { uint32_t n = 0; while (s[n]) n++; return n; }
static void     qcpy(void *d, const void *s, uint32_t n) { uint8_t *a=d; const uint8_t *b=s; while (n--) *a++=*b++; }
int qstr(const char *hay, const char *needle) {
    for (; *hay; ++hay) {
        const char *h = hay, *n = needle;
        while (*n && *h == *n) { h++; n++; }
        if (!*n) return 1;
    }
    return 0;
}

/* ---- mini vsnprintf: %s %c %u %d %x %X with 0-pad + width ---- */
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

/* ---- inspection state ---- */
char     g_cap[2048];
uint32_t g_cap_len;
uint32_t g_yields;
uint32_t g_abort_code;
int      g_aborted;
uint32_t g_pad[4];

void hostsys_reset(void) {
    g_cap_len = 0; g_cap[0] = 0;
    g_yields = 0; g_abort_code = 0; g_aborted = 0;
}

static void cap(const char *s, uint32_t n) {
    if (g_cap_len + n >= sizeof g_cap) n = sizeof g_cap - g_cap_len - 1;
    qcpy(g_cap + g_cap_len, s, n); g_cap_len += n; g_cap[g_cap_len] = 0;
}

/* ---- table impls ---- */
static void print(const char *s) { cap(s, qslen(s)); qh_write0(s); }
static int  print_f(const char *fmt, ...) {
    char buf[256];
    __builtin_va_list ap; __builtin_va_start(ap, fmt);
    int n = qvsnprintf(buf, sizeof buf, fmt, ap);
    __builtin_va_end(ap);
    cap(buf, (uint32_t)n); qh_write0(buf);
    return n;
}
static void term(uint32_t o, uint32_t a, uint32_t b) { (void)o;(void)a;(void)b; }
static void t_yield(void) { g_yields++; }
static void t_abort(uint32_t code) { g_abort_code = code; g_aborted = 1; }

static uint8_t  fbuf[64]; static uint32_t flen, fpos; static int fopen_;
static hx421_handle t_open(const char *p, uint32_t m) { (void)p; if (fopen_) return -1;
    fopen_ = 1; fpos = 0; if (m & HX421_O_CREATE) flen = 0; return 3; }
static uint32_t t_write(hx421_handle h, const void *s, uint32_t n) {
    if (h != 3) return 0;
    if (fpos + n > sizeof fbuf) n = sizeof fbuf - fpos;
    qcpy(fbuf + fpos, s, n); fpos += n; if (fpos > flen) flen = fpos; return n; }
static uint32_t t_read(hx421_handle h, void *d, uint32_t n) {
    if (h != 3) return 0;
    if (fpos + n > flen) n = flen - fpos;
    qcpy(d, fbuf + fpos, n); fpos += n; return n; }
static uint32_t t_seek(hx421_handle h, uint32_t o) {
    if (h != 3) return (uint32_t)-1;
    fpos = o <= flen ? o : flen; return fpos; }
static void t_close(hx421_handle h) { if (h == 3) fopen_ = 0; }

static uint8_t  heap[256]; static uint32_t hp;
static void *t_malloc(uint32_t n) { n = (n + 3) & ~3u; if (hp + n > sizeof heap) return 0;
    void *p = &heap[hp]; hp += n; return p; }
static void t_free(void *p) { (void)p; }

static uint32_t t_input(uint32_t pad) { return pad < 4 ? g_pad[pad] : 0; }

Hx421Sys hostsys_table(uint32_t version) {
    Hx421Sys s;
    s.abi_version = version;
    s.yield = t_yield; s.sys_abort = t_abort;
    s.print = print;   s.print_f = print_f;  s.term_ctrl = term;
    s.open = t_open;   s.read = t_read;       s.write = t_write;
    s.seek = t_seek;   s.close = t_close;
    s.mem_alloc = t_malloc; s.mem_free = t_free; s.input_read = t_input;
    return s;
}
