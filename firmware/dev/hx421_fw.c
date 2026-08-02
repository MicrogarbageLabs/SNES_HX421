/* ============================================================
 *  hx421_fw.c — firmware-side backend adapters + the load-and-run entry.
 *
 *  Compiled INSIDE the sd2snes firmware fork, against its FatFs (ff.h) and the
 *  debug UART. Everything with real logic (loader, sysimpl, arena, sandbox) is
 *  in the portable, host+QEMU-tested modules; this file is only the thin
 *  adapters that bind them to the firmware's actual services.
 *
 *  Public domain (CC0). No warranty.
 * ============================================================ */

#include <stdint.h>
#include <stdarg.h>
#include "ff.h"                 /* sd2snes FatFs */
#include "hx421_syscall.h"
#include "hx421_sysimpl.h"
#include "hx421_loader.h"
#include "hx421_memmap.h"
#include "hx421_fw.h"

extern void uart_putc(char c);  /* sd2snes debug UART (stm32f4xx/uart.c) */

/* Region split within the frozen game region: [ image + bss ][ heap arena ].
 * The game runs on the FIRMWARE's stack (the loader jumps via a plain call), so
 * no stack is reserved in the region — a proper stack switch is a later
 * refinement for games that recurse deeply. */
#define HX421_FW_ARENA (8u * 1024u)                       /* game heap        */
#define HX421_FW_IMAGE (HX421_GAME_SIZE - HX421_FW_ARENA) /* image + bss room */

/* ---- mini formatter for vprint (self-contained: sd2snes printf takes ...,
 * not va_list) ---- */
static void put_u(char **d, char *end, uint32_t v, unsigned base, int upper, int width, int zero) {
    char tmp[16]; int n = 0;
    const char *digs = upper ? "0123456789ABCDEF" : "0123456789abcdef";
    if (v == 0) tmp[n++] = '0';
    while (v) { tmp[n++] = digs[v % base]; v /= base; }
    for (int pad = width - n; pad > 0; --pad) if (*d < end) *(*d)++ = zero ? '0' : ' ';
    while (n-- > 0) if (*d < end) *(*d)++ = tmp[n];
}
static int mini_vsn(char *out, int cap, const char *fmt, va_list ap) {
    char *d = out, *end = out + cap - 1;
    for (; *fmt; ++fmt) {
        if (*fmt != '%') { if (d < end) *d++ = *fmt; continue; }
        ++fmt; int zero = 0, width = 0;
        if (*fmt == '0') { zero = 1; ++fmt; }
        while (*fmt >= '0' && *fmt <= '9') { width = width*10 + (*fmt-'0'); ++fmt; }
        switch (*fmt) {
            case 's': { const char *s = va_arg(ap, const char*); while (*s && d<end) *d++=*s++; } break;
            case 'c': { char c=(char)va_arg(ap,int); if(d<end)*d++=c; } break;
            case 'u': put_u(&d,end,va_arg(ap,unsigned),10,0,width,zero); break;
            case 'd': { int v=va_arg(ap,int); if(v<0){if(d<end)*d++='-';put_u(&d,end,(unsigned)(-v),10,0,width,zero);} else put_u(&d,end,(unsigned)v,10,0,width,zero);} break;
            case 'x': put_u(&d,end,va_arg(ap,unsigned),16,0,width,zero); break;
            case 'X': put_u(&d,end,va_arg(ap,unsigned),16,1,width,zero); break;
            case '%': if(d<end)*d++='%'; break;
            default: if(d<end)*d++='%'; if(d<end)*d++=*fmt; break;
        }
    }
    *d = 0; return (int)(d - out);
}

/* ---- backend: adapters over uart + FatFs ---- */

static FIL     g_fil[HX421_MAX_OPEN];
static uint8_t g_used[HX421_MAX_OPEN];

static void fw_puts(void *c, const char *s) { (void)c; while (*s) uart_putc(*s++); }
static int  fw_vprint(void *c, const char *fmt, va_list ap) { (void)c;
    char b[256]; int n = mini_vsn(b, sizeof b, fmt, ap); fw_puts(0, b); return n; }
static void fw_fatal(void *c, uint32_t code) { (void)c; (void)code;
    fw_puts(0, "\n[HX421 GAME ABORT]\n"); for (;;) { } }
static void fw_yield(void *c) { (void)c; /* TODO: service USB/SD from the main loop */ }

static hx421_handle fw_open(void *c, const char *abspath, uint32_t mode) {
    (void)c;
    int slot = -1;
    for (unsigned i = 0; i < HX421_MAX_OPEN; ++i) if (!g_used[i]) { slot = (int)i; break; }
    if (slot < 0) return -1;
    BYTE m = 0;
    if (mode & HX421_O_READ)   m |= FA_READ;
    if (mode & HX421_O_WRITE)  m |= FA_WRITE;
    if (mode & HX421_O_CREATE) m |= FA_CREATE_ALWAYS;
    else if (mode & HX421_O_WRITE) m |= FA_OPEN_ALWAYS;
    if (f_open(&g_fil[slot], abspath, m) != FR_OK) return -1;
    g_used[slot] = 1;
    return slot;
}
static int ok(hx421_handle h) { return h >= 0 && h < (hx421_handle)HX421_MAX_OPEN && g_used[h]; }
static uint32_t fw_read(void *c, hx421_handle h, void *d, uint32_t n) {
    (void)c;
    if (!ok(h)) return 0;
    UINT br = 0;
    f_read(&g_fil[h], d, n, &br);
    return br;
}
static uint32_t fw_write(void *c, hx421_handle h, const void *s, uint32_t n) {
    (void)c;
    if (!ok(h)) return 0;
    UINT bw = 0;
    f_write(&g_fil[h], s, n, &bw);
    return bw;
}
static uint32_t fw_seek(void *c, hx421_handle h, uint32_t off) {
    (void)c;
    if (!ok(h)) return (uint32_t)-1;
    if (f_lseek(&g_fil[h], off) != FR_OK) return (uint32_t)-1;
    return off;
}
static void fw_close(void *c, hx421_handle h) {
    (void)c;
    if (ok(h)) { f_close(&g_fil[h]); g_used[h] = 0; }
}
static uint32_t fw_input(void *c, uint32_t pad) {
    (void)c; (void)pad;
    return 0; /* TODO: SNES->cart mailbox — the SNES writes pad state the cart reads */
}

static const Hx421FwBackend g_backend = {
    0, fw_puts, fw_vprint, fw_fatal, fw_yield,
    fw_open, fw_read, fw_write, fw_seek, fw_close, fw_input
};

/* read_fn for the streaming loader: pull from the open .hxg FIL. f_read can come
 * back short; the loader's read_full loop handles that. */
static uint32_t hxg_read(void *ctx, void *dst, uint32_t n) {
    UINT br = 0; f_read((FIL *)ctx, dst, n, &br); return br;
}

int hx421_fw_run_game(const char *hxg_path, const char *sandbox_prefix) {
    FIL hxg;
    if (f_open(&hxg, hxg_path, FA_READ) != FR_OK) return -1;

    static Hx421SysImpl impl;
    Hx421Sys table;
    void *arena = (void *)(uintptr_t)(HX421_GAME_BASE + HX421_FW_IMAGE);   /* heap after the image room */
    hx421_sys_build(&impl, &g_backend, arena, HX421_FW_ARENA, sandbox_prefix, &table);

    Hx421LoadResult r = hx421_loader_run_stream((void *)(uintptr_t)HX421_GAME_BASE,
                                                HX421_FW_IMAGE, /*reserve_stack*/ 0,
                                                hxg_read, &hxg, &table);
    f_close(&hxg);
    return (r == HX421_LOAD_OK) ? 0 : -(int)r;
}
