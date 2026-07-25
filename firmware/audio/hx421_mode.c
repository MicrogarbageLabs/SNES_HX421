/* ============================================================
 *  hx421_mode.c — STM32 "HX-421 mode": stream two WAVs from SD into PSRAM
 *  rings for the FPGA mixer. Firmware glue over the host-tested arbiter. See .h.
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
#include "hx421_mode.h"

/* ---- PSRAM ring layout (well above any loaded ROM; 16 MB available) ---- */
#define HX421_RING_SIZE   0x10000u          /* 64 KB per stream ring        */
#define HX421_RING0_BASE  0x800000u         /* 8 MB                         */
#define HX421_RING1_BASE  0x810000u
#define HX421_CHUNK       0x1000u           /* 4 KB per refill offload      */

/* stereo 16-bit @ the mixer rate -> bytes drained per ms (time-estimated drain
 * until the FPGA exposes the mixer's real read pointer). 44100*4/1000 ~= 176. */
#define HX421_BYTES_PER_MS 176u

extern uint8_t file_buf[];                  /* shared SD scratch (offload dst is the FPGA) */

typedef struct {
    FIL          file[2];
    Hx421WavInfo wav[2];
    int          open[2];
    uint32_t     base[2];
    uint32_t     start_tick;
    Hx421StreamArb arb;
} Hx421Mode;

static Hx421Mode M;

/* ---- platform seams ---- */

/* Kick an SD-DMA offload: FPGA copies `len` bytes from stream s's file (at
 * data_off+src_off) into PSRAM at psram_addr. The MCU only sets it up. */
static int hx421_plat_offload(void *ctx, int s, uint32_t psram_addr,
                              uint32_t src_off, uint32_t len) {
    (void)ctx;
    if (!M.open[s]) return 1;
    if (f_lseek(&M.file[s], M.wav[s].data_off + src_off) != FR_OK) return 1;
    set_mcu_addr(psram_addr);
    ff_sd_offload  = 1;
    sd_offload_tgt = 0;                      /* 0 = PSRAM (the ROM store) */
    UINT br = 0;
    FRESULT r = f_read(&M.file[s], file_buf, len, &br);
    ff_sd_offload = 0;
    return (r == FR_OK && br == len) ? 0 : 1;
}

/* The mixer's drain position within stream s's ring. TODO: read the real value
 * from an FPGA status register once the mixer exposes it; time-estimated for now
 * so the arbiter refills at the true drain rate. */
static uint32_t hx421_plat_read_ptr(void *ctx, int s) {
    (void)ctx; (void)s;
    uint32_t elapsed = getticks() - M.start_tick;      /* ms */
    uint32_t bytes   = elapsed * HX421_BYTES_PER_MS;
    return bytes % HX421_RING_SIZE;
}

static int open_stream(int s, const char *name, uint32_t base) {
    UINT br = 0;
    if (f_open(&M.file[s], name, FA_READ) != FR_OK) {
        printf("hx421: %s not found\n", name);
        return 1;
    }
    /* read the header block and parse the data extent */
    f_lseek(&M.file[s], 0);
    if (f_read(&M.file[s], file_buf, 1024, &br) != FR_OK || br < 44) {
        f_close(&M.file[s]); return 1;
    }
    if (hx421_wav_parse_header(file_buf, br, &M.wav[s]) != HX421_WAV_OK
        || !hx421_wav_is_streamable(&M.wav[s])) {
        printf("hx421: %s not a streamable 16-bit stereo WAV\n", name);
        f_close(&M.file[s]); return 1;
    }
    M.open[s] = 1;
    M.base[s] = base;
    printf("hx421: %s %luHz data@%lu +%lu\n", name, M.wav[s].sample_rate,
           M.wav[s].data_off, M.wav[s].data_bytes);
    return 0;
}

int hx421_mode_init(void) {
    Hx421StreamPlat plat = {
        .ctx = &M, .offload = hx421_plat_offload, .read_ptr = hx421_plat_read_ptr
    };
    M.open[0] = M.open[1] = 0;
    if (open_stream(0, "music1.wav", HX421_RING0_BASE)) return 1;
    if (open_stream(1, "music2.wav", HX421_RING1_BASE)) { f_close(&M.file[0]); return 1; }

    hx421_stream_arb_init(&M.arb, &plat, HX421_CHUNK, 0, 0);
    /* both music streams: loop, priority 0 (FMV would be prio > 0). */
    hx421_stream_start(&M.arb, 0, HX421_RING0_BASE, HX421_RING_SIZE,
                       0, M.wav[0].data_bytes, 1, HX421_RING_SIZE/2, 0);
    hx421_stream_start(&M.arb, 1, HX421_RING1_BASE, HX421_RING_SIZE,
                       0, M.wav[1].data_bytes, 1, HX421_RING_SIZE/2, 0);
    M.start_tick = getticks();

    /* prime both rings before the mixer starts (loop until ready) */
    while (!hx421_stream_ready(&M.arb, 0) || !hx421_stream_ready(&M.arb, 1))
        hx421_stream_service(&M.arb);

    printf("hx421: primed; rings %luB @ %06lx / %06lx\n",
           (unsigned long)HX421_RING_SIZE, (unsigned long)HX421_RING0_BASE,
           (unsigned long)HX421_RING1_BASE);
    return 0;
}

int hx421_mode_loop(void) {
    /* joystick: the SNES ROM forwards buttons via the snescmd channel.
     * TODO: map buttons to per-stream start/stop + bleep triggers. */
    uint8_t buttons = snes_get_snes_cmd();
    (void)buttons;

    /* refill the emptiest ring (one offload per call, priority-weighted). */
    hx421_stream_service(&M.arb);

    /* exit if the FPGA/SNES went away (matches the base loop's guard). */
    return (fpga_test() != FPGA_TEST_TOKEN);
}
