/* game_blob.c — a real loadable game, compiled and linked SEPARATELY at the
 * game region base (0x20010000), packed into an .hxg with a header, and loaded
 * by hx421_loader into that region at run time. It references ONLY sys_* — no
 * libc, no firmware internals — so it is the genuine article, not a function
 * the firmware already contains. game_main is placed first (entry_off 0). */

#include "hx421_gamert.h"

__attribute__((section(".text.game_main"), used))
void game_main(const Hx421Sys *sys) {
    if (hx421_game_bind(sys) != 0) { sys_abort(0xBAD); return; }
    sys_print("LOADED GAME: running from the game region\n");
    sys_printf("pad0=%08X abi=%u.%u\n",
               (unsigned)sys_input(0),
               (unsigned)(sys->abi_version >> 16),
               (unsigned)(sys->abi_version & 0xFFFF));

    /* exercise the heap (arena) and a file (sandboxed) through the table */
    char *buf = (char *)sys_malloc(16);
    for (int i = 0; i < 8; ++i) buf[i] = (char)('A' + i);
    hx421_handle f = sys_open("save.dat", HX421_O_WRITE | HX421_O_CREATE);
    sys_write(f, buf, 8);
    sys_seek(f, 0);
    char back[9] = {0};
    sys_read(f, back, 8);
    sys_close(f);
    sys_printf("file=%s\n", back);
    sys_free(buf);

    sys_yield();
    sys_print("LOADED GAME OK\n");
}
