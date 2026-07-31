/* qemu_hostsys.h — the firmware side of the syscall table for the QEMU tests:
 * host-backed implementations routed to semihosting + a capture buffer, plus
 * the inspection state the harness asserts on. Shared by the boundary test and
 * the loader test so the table impls live in one place. */

#ifndef QEMU_HOSTSYS_H
#define QEMU_HOSTSYS_H

#include "hx421_syscall.h"

extern char     g_cap[2048];   /* everything the game printed */
extern uint32_t g_cap_len;
extern uint32_t g_yields;
extern uint32_t g_abort_code;
extern int      g_aborted;
extern uint32_t g_pad[4];

void       hostsys_reset(void);              /* clear capture + inspection state */
Hx421Sys   hostsys_table(uint32_t version);  /* a table with the given abi_version */
int        qstr(const char *hay, const char *needle);   /* tiny strstr-bool */

#endif /* QEMU_HOSTSYS_H */
