/* startup_m4.c — minimal bare-metal Cortex-M4 startup for QEMU, with
 * semihosting for console output and exit. No newlib, no libc: everything the
 * harness needs is here. See docs/dev-mode.md tier 3. */

#include <stdint.h>

/* gcc may emit calls to these even in a freestanding build (struct copies,
 * initializers). With -nostdlib there is no libc to supply them, so provide
 * minimal versions here. */
void *memcpy(void *d, const void *s, __SIZE_TYPE__ n) {
    uint8_t *a = d; const uint8_t *b = s; while (n--) *a++ = *b++; return d;
}
void *memset(void *d, int c, __SIZE_TYPE__ n) {
    uint8_t *a = d; while (n--) *a++ = (uint8_t)c; return d;
}

/* ---- ARM semihosting (QEMU with -semihosting) ---- */
static int sh(int op, void *arg) {
    register int   r0 __asm__("r0") = op;
    register void *r1 __asm__("r1") = arg;
    __asm__ volatile("bkpt 0xAB" : "+r"(r0) : "r"(r1) : "memory");
    return r0;
}
void qh_write0(const char *s) { sh(0x04, (void *)s); }   /* SYS_WRITE0 */
void qh_exit(int code) {
    uint32_t block[2] = { 0x20026u /* ADP_Stopped_ApplicationExit */, (uint32_t)code };
    sh(0x18, block);                                     /* SYS_EXIT */
    for (;;) { }
}

/* ---- startup: copy .data, zero .bss, call the harness ---- */
extern uint32_t _sidata, _sdata, _edata, _sbss, _ebss, _estack;
int qemu_main(void);

void Reset_Handler(void) {
    uint32_t *src = &_sidata, *dst = &_sdata;
    while (dst < &_edata) *dst++ = *src++;
    for (dst = &_sbss; dst < &_ebss; ) *dst++ = 0;
    int rc = qemu_main();
    qh_exit(rc);
}
static void Default_Handler(void) { for (;;) { } }

/* SP + reset + the core fault vectors is all QEMU needs to boot. */
__attribute__((section(".isr_vector"), used))
const uintptr_t g_vectors[] = {
    (uintptr_t)&_estack,
    (uintptr_t)Reset_Handler,
    (uintptr_t)Default_Handler,   /* NMI       */
    (uintptr_t)Default_Handler,   /* HardFault */
    (uintptr_t)Default_Handler,   /* MemManage */
    (uintptr_t)Default_Handler,   /* BusFault  */
    (uintptr_t)Default_Handler,   /* UsageFault*/
};
