/* hello.c — the first hardware dev-mode game. Linked SEPARATELY at the frozen
 * game region base (HX421_GAME_BASE), packed into hello.hxg, dropped on the SD,
 * and loaded by the firmware's `run hello.hxg` CLI command. It references ONLY
 * sys_* (no libc, no firmware internals), so its output arriving on the serial
 * console proves the whole chain: load-into-region -> jump -> syscall table ->
 * uart. game_main is placed first (entry_off 0). See docs/dev-mode.md. CC0. */

#include "hx421_gamert.h"

__attribute__((section(".text.game_main"), used))
void game_main(const Hx421Sys *sys) {
    if (hx421_game_bind(sys) != 0) { sys_abort(0xBAD); return; }

    sys_print("\n=== HX-421 dev-mode: HELLO from the game region ===\n");
    sys_printf("abi=%u.%u  entry ran at the region base\n",
               (unsigned)(sys->abi_version >> 16),
               (unsigned)(sys->abi_version & 0xFFFF));

    /* heap: allocate from the game's arena and prove it's writable */
    char *buf = (char *)sys_malloc(64);
    if (buf) {
        for (int i = 0; i < 16; ++i) buf[i] = (char)('A' + i);
        buf[16] = 0;
        /* the firmware's mini formatter supports x/X/u/d/s/c/%, not p */
        sys_printf("arena: got 0x%08X, wrote '%s'\n", (unsigned)(uintptr_t)buf, buf);
    } else {
        sys_print("arena: malloc FAILED\n");
    }

    /* input: reads through the table (the fw_input stub returns 0 for now, so
     * this just proves the path — real joypad state needs the SNES->cart mailbox) */
    sys_printf("input: pad0=%08X (0 expected until the mailbox lands)\n",
               (unsigned)sys_input(0));

    /* file: sandboxed to /sd2snes/hx421. GUARDED — if that dir doesn't exist the
     * open fails and we say so instead of hanging or crashing. */
    hx421_handle f = sys_open("hello.txt", HX421_O_WRITE | HX421_O_CREATE);
    if (f >= 0) {
        const char *msg = "written by hello.hxg\n";
        unsigned n = 0; while (msg[n]) n++;
        sys_write(f, msg, n);
        sys_close(f);
        sys_print("file: wrote /sd2snes/hx421/hello.txt\n");
    } else {
        sys_print("file: skipped (create the /sd2snes/hx421 dir to enable)\n");
    }

    if (buf) sys_free(buf);
    sys_yield();
    sys_print("hello.hxg finished OK -- returning to the CLI\n\n");
}
