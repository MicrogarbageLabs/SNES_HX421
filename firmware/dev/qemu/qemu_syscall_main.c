/* qemu_syscall_main.c — tier-3 execution proof for the syscall BOUNDARY: an
 * inline game calls sys_* and runs against the shared host table on a Cortex-M4
 * in QEMU. (The loader test in qemu_loader_main.c proves loading a SEPARATE
 * blob.) See docs/dev-mode.md. */

#include "hx421_gamert.h"
#include "qemu_hostsys.h"

void qh_write0(const char *s);

/* the game references ONLY sys_* */
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
    if (sizeof(Hx421Sys) != 14 * 4) { qh_write0("QEMU FAIL: Hx421Sys not 56 bytes\n"); return 1; }

    hostsys_reset(); g_pad[0] = 0x55;
    Hx421Sys t = hostsys_table(HX421_ABI_VERSION);
    demo_game(&t);

    int ok = 1;
    if (g_aborted)                     { qh_write0("QEMU FAIL: matched game aborted\n"); ok=0; }
    if (!qstr(g_cap, "HX421 GAME UP"))  { qh_write0("QEMU FAIL: print\n"); ok=0; }
    if (!qstr(g_cap, "abi 1.0"))        { qh_write0("QEMU FAIL: printf\n"); ok=0; }
    if (!qstr(g_cap, "pad0=00000055"))  { qh_write0("QEMU FAIL: input/width\n"); ok=0; }
    if (!qstr(g_cap, "file=ABCDEFGH"))  { qh_write0("QEMU FAIL: file round-trip\n"); ok=0; }
    if (g_yields != 1)                  { qh_write0("QEMU FAIL: yield\n"); ok=0; }

    hostsys_reset();
    Hx421Sys wrong = hostsys_table(((uint32_t)(HX421_ABI_MAJOR + 1) << 16));
    demo_game(&wrong);
    if (!(g_aborted && g_abort_code == 0xBAD)) { qh_write0("QEMU FAIL: mismatch not refused\n"); ok=0; }
    if (qstr(g_cap, "GAME UP"))                { qh_write0("QEMU FAIL: ran past version check\n"); ok=0; }

    qh_write0(ok ? "QEMU PASS\n" : "QEMU FAIL\n");
    return ok ? 0 : 1;
}
