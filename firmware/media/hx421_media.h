/* ============================================================
 *  hx421_media.h — HX-421 media service (M4 side): command-mailbox-driven
 *  SD -> PSRAM streaming for the FPGA mixer + FMV path.
 *
 *  The RPG-scope role of the STM32 (docs/architecture-pivot.md): the SNES
 *  65816 runs the game in WRAM and mailboxes the FPGA; the M4 does NOT run
 *  game logic. Its only jobs are loading the core and running THIS service —
 *  keeping N audio/FMV stream rings in PSRAM fed from SD so the FPGA mixer can
 *  read them as channels.
 *
 *  This generalises firmware/audio/hx421_mode.c (a hardcoded two-fixed-WAV
 *  demo) into a mailbox-driven service: the SNES writes play/seek/stop/gain
 *  commands to an FPGA register; the M4 polls that mailbox and drives the
 *  host-tested arbiter (firmware/audio/hx421_stream.c). One stream slot is the
 *  FMV audio/video stream (higher refill priority — it frame-drops on
 *  underrun); the rest are music.
 *
 *  Everything with logic is here and host-tested (tools/hx421_media_test.c)
 *  against mock seams. The sd2snes/mk3 bindings (fpga_sddma, the mailbox
 *  register, FatFs) live in hx421_media_fw.c, compiled only inside the fork.
 *
 *  Public domain (CC0). No warranty.
 * ============================================================ */

#ifndef HX421_MEDIA_H
#define HX421_MEDIA_H

#include <stdint.h>
#include "hx421_stream.h"   /* the arbiter; HX421_STREAM_MAX = slot count */

/* ---- command protocol (SNES -> cart mailbox) ----
 * The SNES writes one of these; the platform cmd_poll seam fetches it. Assets
 * are referenced by a small id (the firmware maps id -> SD file), NOT a path —
 * the SNES cannot hand FatFs a filename over the bus. */
typedef enum {
    HX421_MEDIA_NOP      = 0,
    HX421_MEDIA_PLAY     = 1,  /* start asset on a slot (loop/prio/gain/pan)   */
    HX421_MEDIA_STOP     = 2,  /* stop a slot                                   */
    HX421_MEDIA_SEEK     = 3,  /* re-point a slot's source (arg = byte offset)  */
    HX421_MEDIA_GAIN     = 4,  /* live per-slot gain/pan update                 */
    HX421_MEDIA_STOP_ALL = 5   /* stop every slot                               */
} Hx421MediaOp;

#define HX421_MEDIA_FLAG_LOOP 0x01u   /* wrap the source at data end vs. stop   */

typedef struct {
    uint8_t  op;      /* Hx421MediaOp                                          */
    uint8_t  slot;    /* stream slot [0, HX421_STREAM_MAX)                     */
    uint16_t asset;   /* asset id (PLAY): firmware maps id -> SD file          */
    uint8_t  flags;   /* HX421_MEDIA_FLAG_*                                    */
    uint8_t  prio;    /* refill priority (PLAY): give the FMV stream > 0       */
    uint8_t  gain;    /* 0..255 (PLAY/GAIN)                                    */
    uint8_t  pan;     /* 128 = center (PLAY/GAIN)                              */
    uint32_t arg;     /* SEEK: byte offset into the asset's data (frame-aligned)*/
} Hx421MediaCmd;

/* ---- platform seams (mockable on the host) ---- */
typedef struct {
    void *ctx;

    /* SD-DMA offload: copy `len` bytes from slot `s`'s open asset at absolute
     * file byte offset `src_off` into PSRAM at `psram_addr`. The FPGA performs
     * the transfer; the M4 only sets it up. 0 = ok, non-zero = busy/retry. */
    int      (*offload)(void *ctx, int s, uint32_t psram_addr,
                        uint32_t src_off, uint32_t len);

    /* The FPGA mixer's current read position within slot `s`'s ring, as a byte
     * offset in [0, ring_size). Consumer side; the arbiter keeps write ahead. */
    uint32_t (*read_ptr)(void *ctx, int s);

    /* Open asset `id` for streaming ON slot `s` and report its streamable data
     * extent as an ABSOLUTE file byte offset + length. The binding keeps the
     * open handle per slot (offload/read_ptr are slot-keyed). 0 = ok. */
    int      (*asset_open)(void *ctx, int s, uint16_t id,
                           uint32_t *data_off, uint32_t *data_bytes);

    /* Release slot `s`'s asset `id` (close the file). */
    void     (*asset_close)(void *ctx, int s, uint16_t id);

    /* Enable/disable + configure the FPGA mixer channel for slot `s`. Called
     * with enable=1 (gain/pan applied) when a ring first primes, on live GAIN
     * updates, and with enable=0 on stop. */
    void     (*mixer_ctl)(void *ctx, int s, int enable, uint8_t gain, uint8_t pan);

    /* Fetch the next pending SNES command. 1 = *out filled, 0 = none pending. */
    int      (*cmd_poll)(void *ctx, Hx421MediaCmd *out);
} Hx421MediaPlat;

/* ---- per-slot ring layout + service config ---- */
typedef struct {
    uint32_t psram_base;    /* ring base, absolute PSRAM byte address          */
    uint32_t ring_size;     /* ring size in bytes (>= 2*chunk, frame-aligned)  */
    uint32_t prime_bytes;   /* primed-head size before ready (0 = default)     */
} Hx421MediaSlotCfg;

typedef struct {
    Hx421MediaSlotCfg slot[HX421_STREAM_MAX];
    uint32_t          chunk;   /* max bytes per refill offload (0 = default)    */
} Hx421MediaCfg;

/* ---- service ---- */
typedef struct {
    Hx421MediaPlat plat;
    Hx421MediaCfg  cfg;
    Hx421StreamArb arb;
    struct {
        uint8_t  active;    /* slot in use (mirrors arb, tracked for asset id)  */
        uint8_t  enabled;   /* mixer channel enabled (ring primed at least once)*/
        uint16_t asset;     /* asset id to close on stop                        */
        uint8_t  gain;
        uint8_t  pan;
    } s[HX421_STREAM_MAX];
} Hx421Media;

/* Initialise the service. `plat` and `cfg` are copied. */
void hx421_media_init(Hx421Media *m, const Hx421MediaPlat *plat,
                      const Hx421MediaCfg *cfg);

/* Apply one command directly (bypasses cmd_poll). Returns 0 on success, or a
 * negative code on a bad slot / asset-open failure / unknown op. Exposed for
 * the host test and for any firmware path that has a command in hand. */
int  hx421_media_apply(Hx421Media *m, const Hx421MediaCmd *cmd);

/* Cooperative service — call repeatedly from the run loop. Drains pending SNES
 * commands, enables mixer channels as rings prime, and issues at most ONE
 * refill offload (the most-urgent ring). Returns 1 if it did any work. */
int  hx421_media_service(Hx421Media *m);

/* True once slot `s`'s ring is primed and its mixer channel is safe to read. */
int  hx421_media_ready(const Hx421Media *m, int s);

#endif /* HX421_MEDIA_H */
