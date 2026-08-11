/* ============================================================
 *  hx421_media_fw.c — sd2snes/mk3 binding for the HX-421 media service.
 *
 *  The M4's whole RPG-scope job (docs/architecture-pivot.md): stream audio +
 *  FMV files SD -> PSRAM rings for the FPGA mixer / FMV path, driven by SNES
 *  mailbox commands. All the streaming/arbitration logic is in the host-tested
 *  hx421_media + hx421_stream modules; this file is only the thin adapters that
 *  bind them to the firmware's SD-DMA offload, FatFs, and (pending) the FPGA
 *  mailbox + mixer registers. Hooked in main.c like MSU-1, gated on has_hx421.
 *
 *  Public domain (CC0). No warranty.
 * ============================================================ */

#include "config.h"
#include "ff.h"
#include "fileops.h"
#include "fpga.h"
#include "fpga_spi.h"
#include "diskio.h"
#include "snes.h"
#include "timer.h"
#include "hx421_stream.h"
#include "hx421_wav.h"
#include "hx421_media.h"
#include "hx421_media_fw.h"

extern uint8_t file_buf[];             /* shared SD scratch (offload dst = FPGA) */

/* ---- PSRAM ring layout (well above any core payload; 16 MB available) ---- *
 * The FMV stream gets a bigger ring (it also carries video sub-frames and must
 * absorb SD-fetch bursts without a frame drop); music rings are smaller.       */
#define RING_FMV_BASE    0x800000u
#define RING_FMV_SIZE    0x20000u       /* 128 KB */
#define RING_MUS0_BASE   0x820000u
#define RING_MUS1_BASE   0x830000u
#define RING_MUS_SIZE    0x10000u       /* 64 KB  */
#define MEDIA_CHUNK      0x1000u        /* 4 KB per refill offload */

/* stereo 16-bit @ mixer rate -> bytes/ms, for the time-estimated drain until
 * the FPGA mixer exposes its real per-channel read pointer. 44100*4/1000 ~=176 */
#define BYTES_PER_MS     176u

/* ---- asset manifest (placeholder: a fixed table until a per-game manifest
 * file is defined). id -> SD file + whether it is a WAV (parse header) or a raw
 * stream (FMV: whole-file extent). Files live under /sd2snes/hx421/. ---- */
typedef struct { const char *path; uint8_t is_wav; } MediaAsset;
static const MediaAsset ASSETS[] = {
    /* 0 */ { "/sd2snes/hx421/music1.wav", 1 },
    /* 1 */ { "/sd2snes/hx421/music2.wav", 1 },
    /* 2 */ { "/sd2snes/hx421/fmv.str",    0 },   /* raw FMV byte stream */
};
#define ASSET_COUNT (int)(sizeof(ASSETS)/sizeof(ASSETS[0]))

/* per-slot binding state */
typedef struct {
    FIL      file;
    int      open;
    uint32_t start_tick;   /* for the time-estimated read pointer */
    uint32_t ring_size;
    uint32_t psram_base;   /* ring base in PSRAM (told to the mixer at enable) */
} SlotBind;

static struct {
    SlotBind  b[HX421_STREAM_MAX];
    Hx421Media media;
} FW;

/* ---- platform seams ---- */

static int fw_offload(void *ctx, int s, uint32_t psram_addr,
                      uint32_t src_off, uint32_t len) {
    (void)ctx;
    if (s < 0 || s >= HX421_STREAM_MAX || !FW.b[s].open) return 1;
    if (f_lseek(&FW.b[s].file, src_off) != FR_OK) return 1;   /* src_off absolute */
    set_mcu_addr(psram_addr);
    ff_sd_offload  = 1;
    sd_offload_tgt = 0;                    /* 0 = PSRAM (the ROM/data store) */
    UINT br = 0;
    FRESULT r = f_read(&FW.b[s].file, file_buf, len, &br);
    ff_sd_offload = 0;
    return (r == FR_OK && br == len) ? 0 : 1;
}

/* Mixer control register groups (FPGA base, via the generic config SPI opcodes):
 *   0x11 FA-write: cfg bytes,  index = { ch[2:0], field[2:0], bytesel[1:0] }
 *   0x11 F9-read : drain ptr,  index = { ch[2:0], byte[1:0] } (24-bit sample pos)
 *   0x12 FA-write: re-prime (any value) */
#define MIXER_GROUP 0x11u
#define PRIME_GROUP 0x12u
#define BASE_GROUP  0x13u   /* per-channel PSRAM ring base (3 bytes: {ch,byte}) */

/* Read the mixer's real per-channel drain pointer (sample index) and convert to
 * a byte offset within the ring, so the arbiter refills at the true drain rate. */
static uint32_t fw_read_ptr(void *ctx, int s) {
    (void)ctx;
    if (s < 0 || s >= HX421_STREAM_MAX || FW.b[s].ring_size == 0) return 0;
    uint32_t pos = (uint32_t) fpga_read_config(MIXER_GROUP, (uint8_t)((s << 2) | 0))
                 | ((uint32_t)fpga_read_config(MIXER_GROUP, (uint8_t)((s << 2) | 1)) << 8)
                 | ((uint32_t)fpga_read_config(MIXER_GROUP, (uint8_t)((s << 2) | 2)) << 16);
    uint32_t bytes = pos * 4u;                     /* stereo 16-bit frame = 4 B */
    return bytes % FW.b[s].ring_size;
}

static int fw_asset_open(void *ctx, int s, uint16_t id,
                         uint32_t *data_off, uint32_t *data_bytes) {
    (void)ctx;
    if (s < 0 || s >= HX421_STREAM_MAX || id >= (uint16_t)ASSET_COUNT) return 1;
    const MediaAsset *a = &ASSETS[id];
    if (FW.b[s].open) { f_close(&FW.b[s].file); FW.b[s].open = 0; }
    if (f_open(&FW.b[s].file, a->path, FA_READ) != FR_OK) {
        printf("hx421 media: %s not found\n", a->path);
        return 1;
    }
    if (a->is_wav) {
        UINT br = 0;
        Hx421WavInfo w;
        f_lseek(&FW.b[s].file, 0);
        if (f_read(&FW.b[s].file, file_buf, 1024, &br) != FR_OK || br < 44 ||
            hx421_wav_parse_header(file_buf, br, &w) != HX421_WAV_OK ||
            !hx421_wav_is_streamable(&w)) {
            printf("hx421 media: %s not a streamable 16-bit stereo WAV\n", a->path);
            f_close(&FW.b[s].file);
            return 1;
        }
        *data_off = w.data_off;
        *data_bytes = w.data_bytes;
    } else {
        *data_off = 0;
        *data_bytes = f_size(&FW.b[s].file);      /* raw stream: whole file */
    }
    FW.b[s].open       = 1;
    FW.b[s].start_tick = getticks();
    printf("hx421 media: slot %d <- %s data@%lu +%lu\n", s, a->path,
           (unsigned long)*data_off, (unsigned long)*data_bytes);
    return 0;
}

static void fw_asset_close(void *ctx, int s, uint16_t id) {
    (void)ctx; (void)id;
    if (s >= 0 && s < HX421_STREAM_MAX && FW.b[s].open) {
        f_close(&FW.b[s].file);
        FW.b[s].open = 0;
    }
}

/* Write `nbytes` low-to-high bytes of a mixer cfg field via FA group 0x11;
 * hx_mixer_cfg assembles them and commits on the field's last byte. */
static void mixcfg_write(uint8_t ch, uint8_t field, uint32_t data, int nbytes) {
    for (int b = 0; b < nbytes; b++)
        fpga_write_config(MIXER_GROUP, (uint8_t)((ch << 5) | (field << 2) | b),
                          (uint8_t)(data >> (b * 8)), 0);
}

/* Configure/enable a mixer channel. On enable, set the full streaming config
 * (native-rate step, active+loop, loop_len = the ring in frames, gain, pan) and
 * re-prime; on disable, clear the active flag. Pan is a linear balance law
 * (matches the C reference's mixer_set_pan). Gain/pan are Q15. */
static void fw_mixer_ctl(void *ctx, int s, int enable, uint8_t gain, uint8_t pan) {
    (void)ctx;
    if (s < 0 || s >= HX421_STREAM_MAX) return;

    if (!enable) {
        mixcfg_write((uint8_t)s, 2, 0x00000000u, 1);        /* flags: inactive */
        return;
    }

    uint16_t vol   = (uint16_t)(((uint32_t)gain * 32767u) / 255u);
    uint16_t pan_l = (pan <= 128) ? 32767u : (uint16_t)((uint32_t)32767u * (255u - pan) / 127u);
    uint16_t pan_r = (pan >= 128) ? 32767u : (uint16_t)((uint32_t)32767u * pan / 127u);
    uint32_t loop_frames = FW.b[s].ring_size / 4u;          /* stereo 16-bit frames */

    /* tell the mixer where this channel's ring lives in PSRAM (3 bytes, 24-bit) */
    uint32_t base = FW.b[s].psram_base;
    fpga_write_config(BASE_GROUP, (uint8_t)(((unsigned)s << 2) | 0), (uint8_t)(base),       0);
    fpga_write_config(BASE_GROUP, (uint8_t)(((unsigned)s << 2) | 1), (uint8_t)(base >> 8),  0);
    fpga_write_config(BASE_GROUP, (uint8_t)(((unsigned)s << 2) | 2), (uint8_t)(base >> 16), 0);

    mixcfg_write((uint8_t)s, 0, 0x00000000u, 4);            /* step_lo = 0            */
    mixcfg_write((uint8_t)s, 1, 0x00000001u, 4);            /* step_hi = 1 -> 1.0     */
    mixcfg_write((uint8_t)s, 6, loop_frames, 3);            /* loop_len (24-bit)      */
    mixcfg_write((uint8_t)s, 3, vol, 2);                    /* vol                    */
    mixcfg_write((uint8_t)s, 4, pan_l, 2);                  /* pan_l                  */
    mixcfg_write((uint8_t)s, 5, pan_r, 2);                  /* pan_r                  */
    mixcfg_write((uint8_t)s, 2, 0x0000000Au, 1);            /* flags: active + loop   */
    fpga_write_config(PRIME_GROUP, 0, 0, 0);               /* re-prime the channels  */
}

/* Poll the SNES->cart command mailbox (hx_cmdbox_snes, FPGA base). The 65816
 * writes a command block into the $3F:F1xx window and rings the doorbell; we read
 * it here over the MCU bridge via the generic config-register opcodes: register
 * group MBOX_GROUP, index = mailbox offset. Index 0xFD returns the pending flag;
 * a write to the group acks (clears pending). Mailbox byte layout matches
 * Hx421MediaCmd. Returns 1 and fills *out if a command was pending. */
#define MBOX_GROUP    0x10u
#define MBOX_PEND_IDX 0xFDu
static uint8_t mbox_rd(uint8_t idx) { return fpga_read_config(MBOX_GROUP, idx); }

static int fw_cmd_poll(void *ctx, Hx421MediaCmd *out) {
    (void)ctx;
    if (!(mbox_rd(MBOX_PEND_IDX) & 1u)) return 0;      /* nothing posted */

    out->op    = mbox_rd(0);
    out->slot  = mbox_rd(1);
    out->asset = (uint16_t)(mbox_rd(2) | (mbox_rd(3) << 8));
    out->flags = mbox_rd(4);
    out->prio  = mbox_rd(5);
    out->gain  = mbox_rd(6);
    out->pan   = mbox_rd(7);
    out->arg   = (uint32_t)mbox_rd(8)        | ((uint32_t)mbox_rd(9)  << 8)
               | ((uint32_t)mbox_rd(10) << 16) | ((uint32_t)mbox_rd(11) << 24);

    fpga_write_config(MBOX_GROUP, 0, 0, 0);            /* ack -> clears pending */
    return 1;
}

/* ---- entry points ---- */

int hx421_media_fw_init(void) {
    Hx421MediaPlat plat = {
        .ctx = &FW, .offload = fw_offload, .read_ptr = fw_read_ptr,
        .asset_open = fw_asset_open, .asset_close = fw_asset_close,
        .mixer_ctl = fw_mixer_ctl, .cmd_poll = fw_cmd_poll
    };
    Hx421MediaCfg cfg;
    int i;
    for (i = 0; i < HX421_STREAM_MAX; i++) { FW.b[i].open = 0; FW.b[i].ring_size = 0; }

    for (i = 0; i < HX421_STREAM_MAX; i++) {
        cfg.slot[i].psram_base = 0; cfg.slot[i].ring_size = 0; cfg.slot[i].prime_bytes = 0;
    }
    cfg.chunk = MEDIA_CHUNK;
    cfg.slot[0] = (Hx421MediaSlotCfg){ RING_FMV_BASE,  RING_FMV_SIZE, RING_FMV_SIZE/2 };
    cfg.slot[1] = (Hx421MediaSlotCfg){ RING_MUS0_BASE, RING_MUS_SIZE, RING_MUS_SIZE/2 };
    cfg.slot[2] = (Hx421MediaSlotCfg){ RING_MUS1_BASE, RING_MUS_SIZE, RING_MUS_SIZE/2 };
    FW.b[0].ring_size = RING_FMV_SIZE;   FW.b[0].psram_base = RING_FMV_BASE;
    FW.b[1].ring_size = RING_MUS_SIZE;   FW.b[1].psram_base = RING_MUS0_BASE;
    FW.b[2].ring_size = RING_MUS_SIZE;   FW.b[2].psram_base = RING_MUS1_BASE;

    hx421_media_init(&FW.media, &plat, &cfg);

    /* bring-up default: loop music1 on slot 1 at full gain if it is present, so
     * the SD->PSRAM->mixer path runs on hardware before the mailbox is wired.
     * A real command from the SNES supersedes this. */
    Hx421MediaCmd play = { .op = HX421_MEDIA_PLAY, .slot = 1, .asset = 0,
                           .flags = HX421_MEDIA_FLAG_LOOP, .prio = 0,
                           .gain = 255, .pan = 128, .arg = 0 };
    if (hx421_media_apply(&FW.media, &play) != 0)
        printf("hx421 media: no default music (music1.wav absent) — waiting for commands\n");

    printf("hx421 media: init ok (FMV ring %luB @ %06lx; music %luB)\n",
           (unsigned long)RING_FMV_SIZE, (unsigned long)RING_FMV_BASE,
           (unsigned long)RING_MUS_SIZE);
    return 0;
}

int hx421_media_fw_loop(void) {
    hx421_media_service(&FW.media);
    /* exit if the SNES reset / the FPGA core went away (matches base loop) */
    return (fpga_test() != FPGA_TEST_TOKEN);
}
