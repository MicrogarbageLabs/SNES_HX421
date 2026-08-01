/* ============================================================
 *  hx421_sysimpl.h — build the syscall table from the firmware's existing
 *  services. Most slots are one-line adapters over functions the stock sd2snes
 *  firmware already has (newlib printf, FatFs, joypad read); the only genuinely
 *  new logic is the game-scoped arena allocator and the path sandbox, which are
 *  here and host-tested. See docs/dev-mode.md.
 *
 *  The firmware's raw services sit behind Hx421FwBackend so this file compiles
 *  and tests off-target: on the STM32 the backend forwards to f_open / vsnprintf
 *  / the joypad path; the host test fills it with mocks.
 *
 *  Public domain (CC0). No warranty.
 * ============================================================ */

#ifndef HX421_SYSIMPL_H
#define HX421_SYSIMPL_H

#include <stdint.h>
#include <stdarg.h>
#include "hx421_syscall.h"

#define HX421_SANDBOX_MAX   96u   /* longest sandboxed absolute path */
#define HX421_MAX_OPEN       4u   /* files a game may hold at once   */

/* Raw firmware services. On the target: puts/vprint -> CDC or SNES screen,
 * f* -> FatFs on an ALREADY-SANDBOXED absolute path, input -> joypad read. The
 * fs ops take absolute paths because sysimpl does the sandboxing before calling. */
typedef struct {
    void    *ctx;
    void   (*puts)(void *ctx, const char *s);
    int    (*vprint)(void *ctx, const char *fmt, va_list ap);
    void   (*fatal)(void *ctx, uint32_t code);      /* error screen + halt */
    void   (*yield)(void *ctx);
    hx421_handle (*fopen)(void *ctx, const char *abspath, uint32_t mode);
    uint32_t (*fread)(void *ctx, hx421_handle h, void *dst, uint32_t n);
    uint32_t (*fwrite)(void *ctx, hx421_handle h, const void *src, uint32_t n);
    uint32_t (*fseek)(void *ctx, hx421_handle h, uint32_t off);
    void   (*fclose)(void *ctx, hx421_handle h);
    uint32_t (*input)(void *ctx, uint32_t pad);
} Hx421FwBackend;

/* Per-loaded-game syscall state. One instance lives in FIRMWARE RAM (not the
 * game region), so a game cannot reach the firmware's own allocator or handles. */
typedef struct {
    const Hx421FwBackend *fw;

    /* game-scoped heap: a bump arena carved from the GAME region, with LIFO
     * reclaim. free() of the most-recent block returns it; older frees are
     * no-ops (the whole arena resets when the next game loads). A game that
     * allocates its level data up front — the common case — is fully served. */
    uint8_t  *arena;
    uint32_t  arena_size;
    uint32_t  arena_top;      /* bytes used */
    uint32_t  arena_last;     /* offset of the most recent block (for LIFO free) */

    /* file sandbox: every game path is forced under this prefix, and any escape
     * (absolute, or a ".." component) is rejected. So a game sees only its own
     * tree and cannot touch firmware/menu/core files on the same card. */
    char      prefix[HX421_SANDBOX_MAX];

    /* open-file table: caps how many files a game holds and lets the firmware
     * force-close them all when the game exits. Maps game handles to backend. */
    hx421_handle slot[HX421_MAX_OPEN];   /* backend handle, or -1 if free */
} Hx421SysImpl;

/* Build a ready-to-hand-to-the-game table. `arena`/`arena_size` is the heap
 * carved from the game region; `prefix` is the game's sandbox root (e.g.
 * "/sd2snes/hx421/mygame"). Fills *out. */
void hx421_sys_build(Hx421SysImpl *s, const Hx421FwBackend *fw,
                     void *arena, uint32_t arena_size, const char *prefix,
                     Hx421Sys *out);

/* ---- the two new pieces, exposed for host testing ---- */

void  hx421_arena_init(Hx421SysImpl *s, void *arena, uint32_t size);
void *hx421_arena_alloc(Hx421SysImpl *s, uint32_t n);
void  hx421_arena_free(Hx421SysImpl *s, void *p);

/* Build prefix + "/" + rel into out. Returns 0 on success, <0 if rel escapes
 * the sandbox (absolute path, a ".." component, or too long). */
int   hx421_sandbox_path(char *out, uint32_t cap, const char *prefix, const char *rel);

#endif /* HX421_SYSIMPL_H */
