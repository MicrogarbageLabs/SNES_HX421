/* ============================================================
 *  hx421_gamert.h — the game side of the syscall boundary.
 *
 *  A game .bin includes this and calls sys_*() instead of libc. Each wrapper
 *  is one indirect call through the firmware's table, so the game carries no
 *  libc, no FatFs, no USB stack — those are resident in firmware. See
 *  docs/dev-mode.md and hx421_syscall.h.
 *
 *  The game's entry is `void game_main(const Hx421Sys *sys)`. crt0 (target)
 *  stashes the table and calls it; on the host test the harness calls
 *  hx421_game_bind() directly. Either way, bind FIRST — every sys_* wrapper
 *  dereferences the stashed pointer.
 *
 *  Public domain (CC0). No warranty.
 * ============================================================ */

#ifndef HX421_GAMERT_H
#define HX421_GAMERT_H

#include "hx421_syscall.h"

/* The stashed table. Defined in hx421_gamert.c (one per game binary). */
extern const Hx421Sys *g_sys;

/* Stash the table. crt0 calls this before game_main on the target; the host
 * test calls it directly. Returns 0 if the firmware's MAJOR ABI differs from
 * what this game was built against — the game must NOT proceed on a mismatch,
 * because slot offsets it will call through may not mean what it expects. */
static inline int hx421_game_bind(const Hx421Sys *sys) {
    g_sys = sys;
    if (!sys) return -1;
    if ((sys->abi_version >> 16) != HX421_ABI_MAJOR) return -1;
    return 0;
}

/* ---- thin wrappers: one indirect call each ---- */
static inline void        sys_yield(void)                    { g_sys->yield(); }
static inline void        sys_abort(uint32_t code)           { g_sys->sys_abort(code); }
static inline void        sys_print(const char *s)           { g_sys->print(s); }
static inline void        sys_term(uint32_t op, uint32_t a, uint32_t b) { g_sys->term_ctrl(op, a, b); }

static inline hx421_handle sys_open(const char *p, uint32_t m){ return g_sys->open(p, m); }
static inline uint32_t    sys_read(hx421_handle h, void *d, uint32_t n)        { return g_sys->read(h, d, n); }
static inline uint32_t    sys_write(hx421_handle h, const void *s, uint32_t n) { return g_sys->write(h, s, n); }
static inline uint32_t    sys_seek(hx421_handle h, uint32_t o){ return g_sys->seek(h, o); }
static inline void        sys_close(hx421_handle h)          { g_sys->close(h); }

static inline void       *sys_malloc(uint32_t n)             { return g_sys->mem_alloc(n); }
static inline void        sys_free(void *p)                  { g_sys->mem_free(p); }

static inline uint32_t    sys_input(uint32_t pad)            { return g_sys->input_read(pad); }

/* printf is the one that must stay a real varargs call, not an inline wrapper:
 * forwarding varargs requires the callee to take va_list, so the table exposes
 * print_f directly. A game calls sys_printf(...) as a macro to the table. */
#define sys_printf(...) (g_sys->print_f(__VA_ARGS__))

#endif /* HX421_GAMERT_H */
