/* qemu_loader_main.c — tier-3 execution proof for the RAM LOADER: take a game
 * .hxg embedded in flash, copy it into the game region, JUMP to it, and confirm
 * the loaded code ran through the syscall table. This is the whole dev loop in
 * miniature — the piece the host test cannot exercise because it executes real
 * relocated Thumb code. See docs/dev-mode.md step 3. */

#include "hx421_loader.h"
#include "hx421_memmap.h"
#include "qemu_hostsys.h"

void qh_write0(const char *s);

/* The packed game, linked in as a binary blob by objcopy (symbols named from
 * the file "game.hxg"). It lives in flash; the loader copies it to the region. */
extern const uint8_t _binary_game_hxg_start[];
extern const uint8_t _binary_game_hxg_end[];

/* The game region comes from the FROZEN map (hx421_memmap.h) — the same region
 * the real firmware uses, so this test validates the production layout, not an
 * arbitrary one. The game blob was linked to run at HX421_GAME_BASE. */
#define GAME_BASE  ((void *)(uintptr_t)HX421_GAME_BASE)
#define GAME_SIZE  HX421_GAME_SIZE
#define GAME_STACK HX421_GAME_STACK

int qemu_main(void) {
    qh_write0("hx421 RAM loader (qemu, cortex-m4)\n");

    hostsys_reset();
    g_pad[0] = 0x1234;
    Hx421Sys t = hostsys_table(HX421_ABI_VERSION);

    const uint32_t len = (uint32_t)(_binary_game_hxg_end - _binary_game_hxg_start);
    Hx421LoadResult r = hx421_loader_run(GAME_BASE, GAME_SIZE, GAME_STACK,
                                         _binary_game_hxg_start, len, &t);

    int ok = 1;
    if (r != HX421_LOAD_OK)                 { qh_write0("QEMU FAIL: load result != OK\n"); ok = 0; }
    /* the loaded code must have run to completion, through the table */
    if (!qstr(g_cap, "running from the game region")) { qh_write0("QEMU FAIL: game entry did not run\n"); ok = 0; }
    if (!qstr(g_cap, "pad0=00001234"))      { qh_write0("QEMU FAIL: table call from loaded code\n"); ok = 0; }
    if (!qstr(g_cap, "file=ABCDEFGH"))      { qh_write0("QEMU FAIL: file round-trip from loaded code\n"); ok = 0; }
    if (g_yields != 1)                      { qh_write0("QEMU FAIL: yield from loaded code\n"); ok = 0; }
    if (!qstr(g_cap, "LOADED GAME OK"))     { qh_write0("QEMU FAIL: game did not reach the end\n"); ok = 0; }

    qh_write0(ok ? "QEMU PASS\n" : "QEMU FAIL\n");
    return ok ? 0 : 1;
}
