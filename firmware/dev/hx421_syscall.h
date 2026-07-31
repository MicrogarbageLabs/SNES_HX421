/* ============================================================
 *  hx421_syscall.h — the frozen ABI between the resident STM32 firmware
 *  and a game .bin loaded into RAM.
 *
 *  The firmware bakes in libc + the coprocessor runtime and exports this
 *  table of function pointers; a loaded game carries only its own logic
 *  plus thin trampolines that call through the table. See docs/dev-mode.md.
 *
 *  THE ONE RULE: this table is APPEND-ONLY. Adding a slot at the end is
 *  free — old binaries never see it. Reordering, removing, or changing the
 *  signature of any existing slot silently breaks every game ever compiled
 *  against it. The _Static_asserts at the bottom make that rule a BUILD
 *  ERROR instead of a field bug: they pin the byte offset of every anchor
 *  slot, so a reorder or insertion fails to compile.
 *
 *  Sized types only across the boundary (uint32_t, never bare enum/int) —
 *  the guest/host enum-ABI lesson: a game built against a drifted layout is
 *  far harder to debug than the mode7 black screen ever was.
 *
 *  Public domain (CC0). No warranty.
 * ============================================================ */

#ifndef HX421_SYSCALL_H
#define HX421_SYSCALL_H

#include <stdint.h>
#include <stddef.h>
#include <stdarg.h>

/* Bump MAJOR on any incompatible change (which should never happen given the
 * append-only rule — a MAJOR bump means "we broke old games on purpose").
 * Bump MINOR when appending slots. A game refuses to run on a mismatched
 * MAJOR and warns on a newer MINOR it might call past. */
#define HX421_ABI_MAJOR  1u
#define HX421_ABI_MINOR  0u
#define HX421_ABI_VERSION (((uint32_t)HX421_ABI_MAJOR << 16) | HX421_ABI_MINOR)

/* Open modes for sys.open (sized, explicit — not the platform's O_* which
 * differ between the host test and newlib). */
#define HX421_O_READ   0x01u
#define HX421_O_WRITE  0x02u
#define HX421_O_CREATE 0x04u

typedef int32_t hx421_handle;   /* < 0 on error */

/* The table. The firmware fills it once and hands the game a const pointer.
 * ORDER IS FROZEN — only append. Grouped with the same layout as the table in
 * docs/dev-mode.md; unimplemented future groups are reserved by leaving room,
 * not by placeholder slots (a placeholder that later gains a signature is a
 * silent break). */
typedef struct Hx421Sys {
    /* --- 0x00 lifecycle --- */
    uint32_t      abi_version;                 /* NOT a call: the value, read directly */
    void        (*yield)(void);                /* hand a slice back (audio/USB service) */
    void        (*sys_abort)(uint32_t code);   /* fatal; firmware shows the error screen */

    /* --- 0x10 console --- */
    void        (*print)(const char *s);       /* raw, already-formatted */
    int         (*print_f)(const char *fmt, ...); /* firmware owns the formatter */
    void        (*term_ctrl)(uint32_t op, uint32_t a, uint32_t b); /* cursor/color/clear */

    /* --- 0x20 file --- */
    hx421_handle(*open)(const char *path, uint32_t mode);
    uint32_t    (*read)(hx421_handle h, void *dst, uint32_t n);
    uint32_t    (*write)(hx421_handle h, const void *src, uint32_t n);
    uint32_t    (*seek)(hx421_handle h, uint32_t off);
    void        (*close)(hx421_handle h);

    /* --- 0x30 heap (backed by the GAME region, never firmware's) --- */
    void       *(*mem_alloc)(uint32_t n);
    void        (*mem_free)(void *p);

    /* --- 0x50 input --- */
    uint32_t    (*input_read)(uint32_t pad);   /* pad 0..3 -> button bitmap */

    /* APPEND NEW SLOTS BELOW THIS LINE ONLY. Never above it. */
} Hx421Sys;

/* A loaded game's single entry point. The firmware jumps here with the table
 * pointer in the first argument (r0, per AAPCS), after clearing bss and setting
 * the stack inside the game region. Return / sys_abort ends the game. */
typedef void (*Hx421GameEntry)(const Hx421Sys *sys);

/* ---- append-only enforcement --------------------------------------------
 * Pin every slot to its ORDINAL position (abi_version = 0, then each pointer).
 * Inserting or reordering a member shifts one of these and fails the build.
 * When you APPEND a slot, add a new assert with the next ordinal; never touch
 * an existing number.
 *
 * The offset is `ordinal * sizeof(void*)`, not a hardcoded byte count, so the
 * guard holds on BOTH the 32-bit ARM target (4-byte pointers) and the 64-bit
 * host test (8-byte). abi_version is a uint32_t at offset 0; the following
 * pointers are pointer-aligned and contiguous, so the first lands at
 * sizeof(void*) and each subsequent one a pointer further on. Contiguity IS the
 * append-only property, which is exactly what this proves. */
#define HX421_SLOT(name, ordinal) \
    _Static_assert(offsetof(Hx421Sys, name) == (size_t)(ordinal) * sizeof(void *), \
                   "Hx421Sys slot moved - the ABI is append-only")

HX421_SLOT(abi_version, 0);
HX421_SLOT(yield,       1);
HX421_SLOT(sys_abort,   2);
HX421_SLOT(print,       3);
HX421_SLOT(print_f,     4);
HX421_SLOT(term_ctrl,   5);
HX421_SLOT(open,        6);
HX421_SLOT(read,        7);
HX421_SLOT(write,       8);
HX421_SLOT(seek,        9);
HX421_SLOT(close,      10);
HX421_SLOT(mem_alloc,  11);
HX421_SLOT(mem_free,   12);
HX421_SLOT(input_read, 13);
/* next appended slot: ordinal 14 */

#endif /* HX421_SYSCALL_H */
