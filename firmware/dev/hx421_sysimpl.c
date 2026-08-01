/* ============================================================
 *  hx421_sysimpl.c — see hx421_sysimpl.h. The arena and sandbox are real; the
 *  table slots are thin adapters over the firmware backend.
 * ============================================================ */

#include "hx421_sysimpl.h"

/* tiny freestanding string helpers (no libc dependency in the sketch) */
static uint32_t slen(const char *s) { uint32_t n = 0; while (s[n]) n++; return n; }

/* ---- game arena: bump with LIFO reclaim ---- */

void hx421_arena_init(Hx421SysImpl *s, void *arena, uint32_t size) {
    s->arena = (uint8_t *)arena;
    s->arena_size = size;
    s->arena_top = 0;
    s->arena_last = 0xFFFFFFFFu;   /* no block yet */
}

void *hx421_arena_alloc(Hx421SysImpl *s, uint32_t n) {
    n = (n + 3u) & ~3u;                        /* 4-byte align */
    if (n == 0) n = 4;
    if (n > s->arena_size - s->arena_top) return 0;   /* overflow-safe: no add */
    s->arena_last = s->arena_top;
    void *p = &s->arena[s->arena_top];
    s->arena_top += n;
    return p;
}

void hx421_arena_free(Hx421SysImpl *s, void *p) {
    /* LIFO: reclaim only the most recent block. Older frees are no-ops; the
     * arena is fully reset when the next game loads. */
    if (!p) return;
    uint32_t off = (uint32_t)((uint8_t *)p - s->arena);
    if (off == s->arena_last) {
        s->arena_top = s->arena_last;
        s->arena_last = 0xFFFFFFFFu;
    }
}

/* ---- path sandbox ---- */

int hx421_sandbox_path(char *out, uint32_t cap, const char *prefix, const char *rel) {
    /* reject escapes before building anything */
    if (rel[0] == '/' || rel[0] == '\\') return -1;         /* absolute */
    for (const char *p = rel; *p; ++p) {
        if (p[0] == '.' && p[1] == '.' &&
            (p == rel || p[-1] == '/' || p[-1] == '\\') &&
            (p[2] == 0 || p[2] == '/' || p[2] == '\\'))
            return -1;                                       /* a ".." component */
    }
    uint32_t pl = slen(prefix), rl = slen(rel);
    if (pl + 1u + rl + 1u > cap) return -1;                  /* would not fit */

    uint32_t o = 0;
    for (uint32_t i = 0; i < pl; ++i) out[o++] = prefix[i];
    if (pl && prefix[pl - 1] != '/') out[o++] = '/';
    for (uint32_t i = 0; i < rl; ++i) out[o++] = rel[i];
    out[o] = 0;
    return 0;
}

/* ---- table slots: adapters over the firmware backend ---- *
 * A single Hx421SysImpl is captured by these via a file-static pointer, because
 * the Hx421Sys function-pointer signatures carry no context argument (the game
 * calls them bare). One loaded game at a time, so one impl. */

static Hx421SysImpl *g_impl;

static void        s_yield(void)              { g_impl->fw->yield(g_impl->fw->ctx); }
static void        s_abort(uint32_t code)     { g_impl->fw->fatal(g_impl->fw->ctx, code); }
static void        s_print(const char *str)   { g_impl->fw->puts(g_impl->fw->ctx, str); }
static int         s_printf(const char *fmt, ...) {
    va_list ap; va_start(ap, fmt);
    int n = g_impl->fw->vprint(g_impl->fw->ctx, fmt, ap);
    va_end(ap);
    return n;
}
static void        s_term(uint32_t op, uint32_t a, uint32_t b) { (void)op;(void)a;(void)b; }

static int slot_find_free(void) {
    for (unsigned i = 0; i < HX421_MAX_OPEN; ++i) if (g_impl->slot[i] < 0) return (int)i;
    return -1;
}
static hx421_handle s_open(const char *path, uint32_t mode) {
    int slot = slot_find_free();
    if (slot < 0) return -1;                                /* too many open */
    char abs[HX421_SANDBOX_MAX];
    if (hx421_sandbox_path(abs, sizeof abs, g_impl->prefix, path) != 0) return -1;
    hx421_handle h = g_impl->fw->fopen(g_impl->fw->ctx, abs, mode);
    if (h < 0) return -1;
    g_impl->slot[slot] = h;
    return slot;                                            /* game sees the slot index */
}
static int slot_ok(hx421_handle sh) { return sh >= 0 && sh < (hx421_handle)HX421_MAX_OPEN && g_impl->slot[sh] >= 0; }
static uint32_t s_read(hx421_handle sh, void *d, uint32_t n)        { return slot_ok(sh) ? g_impl->fw->fread(g_impl->fw->ctx, g_impl->slot[sh], d, n) : 0; }
static uint32_t s_write(hx421_handle sh, const void *d, uint32_t n) { return slot_ok(sh) ? g_impl->fw->fwrite(g_impl->fw->ctx, g_impl->slot[sh], d, n) : 0; }
static uint32_t s_seek(hx421_handle sh, uint32_t off)              { return slot_ok(sh) ? g_impl->fw->fseek(g_impl->fw->ctx, g_impl->slot[sh], off) : (uint32_t)-1; }
static void     s_close(hx421_handle sh) {
    if (!slot_ok(sh)) return;
    g_impl->fw->fclose(g_impl->fw->ctx, g_impl->slot[sh]);
    g_impl->slot[sh] = -1;
}

static void *s_malloc(uint32_t n) { return hx421_arena_alloc(g_impl, n); }
static void  s_free(void *p)      { hx421_arena_free(g_impl, p); }

static uint32_t s_input(uint32_t pad) { return g_impl->fw->input(g_impl->fw->ctx, pad); }

void hx421_sys_build(Hx421SysImpl *s, const Hx421FwBackend *fw,
                     void *arena, uint32_t arena_size, const char *prefix,
                     Hx421Sys *out) {
    s->fw = fw;
    hx421_arena_init(s, arena, arena_size);
    uint32_t i = 0;
    for (; prefix[i] && i < HX421_SANDBOX_MAX - 1u; ++i) s->prefix[i] = prefix[i];
    s->prefix[i] = 0;
    for (unsigned k = 0; k < HX421_MAX_OPEN; ++k) s->slot[k] = -1;

    g_impl = s;   /* the single active game */

    out->abi_version = HX421_ABI_VERSION;
    out->yield = s_yield;   out->sys_abort = s_abort;
    out->print = s_print;   out->print_f = s_printf;   out->term_ctrl = s_term;
    out->open = s_open;     out->read = s_read;         out->write = s_write;
    out->seek = s_seek;     out->close = s_close;
    out->mem_alloc = s_malloc; out->mem_free = s_free;
    out->input_read = s_input;
}
