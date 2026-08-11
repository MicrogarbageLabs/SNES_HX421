/* hx421_media_test.c — host test for the HX-421 media service
 * (firmware/media/hx421_media.c) over the arbiter (firmware/audio/hx421_stream.c).
 *
 * Drives the service exactly as the firmware would: the SNES writes commands to
 * a mailbox (here a scripted queue), the M4 polls and dispatches, refills rings
 * from a mock SD into mock PSRAM, and enables mixer channels as rings prime.
 * Verifies: PLAY primes + enables, offload lands the right bytes, the FMV stream
 * (higher priority) is refilled ahead of music under contention, SEEK re-primes,
 * live GAIN reaches the mixer, and STOP/STOP_ALL tear down. CC0. */

#include "hx421_media.h"
#include <stdio.h>
#include <string.h>

/* ---- mock world ---------------------------------------------------------- */
#define FAKE_SD_SIZE   0x20000u
#define FAKE_PSRAM_SZ  0x100000u

#define ASSET_FMV   10          /* id -> extent below                        */
#define ASSET_MUSIC 20

/* mock PSRAM is a flat FAKE_PSRAM_SZ array, so the rings live at low offsets
 * here (on hardware they sit at ~8 MB — the arbiter treats the base opaquely) */
#define RING0_BASE  0x00000000u  /* FMV ring                                 */
#define RING1_BASE  0x00008000u  /* music ring                               */
#define RING_SIZE   0x4000u
#define CHUNK       0x1000u

typedef struct { uint32_t off, bytes; } Extent;

typedef struct {
    uint8_t  sd[FAKE_SD_SIZE];       /* mock SD: byte value == its own offset */
    uint8_t  psram[FAKE_PSRAM_SZ];   /* mock PSRAM                            */
    uint32_t drain[HX421_STREAM_MAX];/* mixer read pointer (bytes), test-driven*/
    int      open_ct[64];            /* per-asset open refcount               */
    /* mixer state as last seen */
    int      mx_enabled[HX421_STREAM_MAX];
    uint8_t  mx_gain[HX421_STREAM_MAX];
    uint8_t  mx_pan[HX421_STREAM_MAX];
    /* command queue */
    Hx421MediaCmd q[32];
    int      qhead, qtail;
    /* last offload seen (for the priority check) */
    int      last_offload_slot;
    uint32_t last_offload_len;
} World;

static World W;

static Extent extent_of(uint16_t id) {
    Extent e = {0, 0};
    if (id == ASSET_FMV)   { e.off = 0x1000; e.bytes = 0x8000; }
    if (id == ASSET_MUSIC) { e.off = 0x9000; e.bytes = 0x8000; }
    return e;
}

static int mk_offload(void *c, int s, uint32_t psram_addr, uint32_t src_off, uint32_t len) {
    (void)c;
    if (src_off + len > FAKE_SD_SIZE || psram_addr + len > FAKE_PSRAM_SZ) return 1;
    memcpy(&W.psram[psram_addr], &W.sd[src_off], len);
    W.last_offload_slot = s;
    W.last_offload_len  = len;
    return 0;
}
static uint32_t mk_read_ptr(void *c, int s) { (void)c; return W.drain[s] % RING_SIZE; }
static int mk_asset_open(void *c, int s, uint16_t id, uint32_t *off, uint32_t *bytes) {
    (void)c; (void)s; Extent e = extent_of(id);
    if (e.bytes == 0) return 1;
    *off = e.off; *bytes = e.bytes; W.open_ct[id]++; return 0;
}
static void mk_asset_close(void *c, int s, uint16_t id) { (void)c; (void)s; if (id < 64) W.open_ct[id]--; }
static void mk_mixer_ctl(void *c, int s, int en, uint8_t g, uint8_t p) {
    (void)c; W.mx_enabled[s] = en; W.mx_gain[s] = g; W.mx_pan[s] = p;
}
static int mk_cmd_poll(void *c, Hx421MediaCmd *out) {
    (void)c;
    if (W.qhead == W.qtail) return 0;
    *out = W.q[W.qhead]; W.qhead = (W.qhead + 1) % 32; return 1;
}
static void enqueue(Hx421MediaCmd cmd) { W.q[W.qtail] = cmd; W.qtail = (W.qtail + 1) % 32; }

/* run the service until it stops doing work (drains cmds + primes), capped */
static void pump(Hx421Media *m, int max) { while (max-- > 0 && hx421_media_service(m)) {} }

static int errs = 0;
#define CHECK(cond, msg) do { if(!(cond)){ printf("FAIL: %s\n", msg); errs++; } } while(0)

int main(void) {
    for (uint32_t i = 0; i < FAKE_SD_SIZE; i++) W.sd[i] = (uint8_t)i;   /* byte == offset */
    W.qhead = W.qtail = 0;
    W.last_offload_slot = -1;

    Hx421MediaPlat plat = {
        .ctx = &W, .offload = mk_offload, .read_ptr = mk_read_ptr,
        .asset_open = mk_asset_open, .asset_close = mk_asset_close,
        .mixer_ctl = mk_mixer_ctl, .cmd_poll = mk_cmd_poll
    };
    Hx421MediaCfg cfg;
    memset(&cfg, 0, sizeof cfg);
    cfg.chunk = CHUNK;
    cfg.slot[0] = (Hx421MediaSlotCfg){ RING0_BASE, RING_SIZE, 0 };   /* FMV   */
    cfg.slot[1] = (Hx421MediaSlotCfg){ RING1_BASE, RING_SIZE, 0 };   /* music */

    Hx421Media m;
    hx421_media_init(&m, &plat, &cfg);

    /* --- SNES: PLAY FMV on slot 0 (prio 2), PLAY music on slot 1 (prio 0) --- */
    enqueue((Hx421MediaCmd){ .op=HX421_MEDIA_PLAY, .slot=0, .asset=ASSET_FMV,
                             .flags=HX421_MEDIA_FLAG_LOOP, .prio=2, .gain=255, .pan=128 });
    enqueue((Hx421MediaCmd){ .op=HX421_MEDIA_PLAY, .slot=1, .asset=ASSET_MUSIC,
                             .flags=HX421_MEDIA_FLAG_LOOP, .prio=0, .gain=200, .pan=128 });
    pump(&m, 10000);

    CHECK(hx421_media_ready(&m, 0), "FMV slot did not prime");
    CHECK(hx421_media_ready(&m, 1), "music slot did not prime");
    CHECK(W.mx_enabled[0] == 1 && W.mx_gain[0] == 255, "FMV mixer not enabled at gain 255");
    CHECK(W.mx_enabled[1] == 1 && W.mx_gain[1] == 200, "music mixer not enabled at gain 200");
    printf("primed: FMV ready=%d music ready=%d  mx0=%d/g%u mx1=%d/g%u\n",
           hx421_media_ready(&m,0), hx421_media_ready(&m,1),
           W.mx_enabled[0], W.mx_gain[0], W.mx_enabled[1], W.mx_gain[1]);

    /* offload correctness: PSRAM byte 0 of the FMV ring == FMV data's first byte
     * (source byte value == its file offset, so it equals extent.off & 0xFF) */
    Extent ef = extent_of(ASSET_FMV);
    CHECK(W.psram[RING0_BASE] == (uint8_t)ef.off, "FMV ring head does not match source");

    /* --- priority: drain both rings hard, then ONE service must refill FMV --- */
    uint32_t wp0 = m.arb.str[0].write_pos, wp1 = m.arb.str[1].write_pos;
    W.drain[0] = wp0 - 0x100;    /* leave both with a tiny fill = big deficit */
    W.drain[1] = wp1 - 0x100;
    W.last_offload_slot = -1;
    hx421_media_service(&m);     /* exactly one offload */
    CHECK(W.last_offload_slot == 0, "under contention the FMV (higher-prio) ring was not refilled first");
    printf("contention: single-offload went to slot %d (expect 0 = FMV)\n", W.last_offload_slot);

    /* --- SEEK music to a new offset: must drop ready, then re-prime --- */
    W.drain[0] = W.drain[1] = 0;
    enqueue((Hx421MediaCmd){ .op=HX421_MEDIA_SEEK, .slot=1, .arg=0x2000 });
    hx421_media_service(&m);                       /* applies SEEK */
    CHECK(!hx421_media_ready(&m, 1), "music still ready immediately after SEEK");
    CHECK(W.mx_enabled[1] == 0, "music mixer not muted during SEEK re-prime");
    pump(&m, 10000);
    CHECK(hx421_media_ready(&m, 1), "music did not re-prime after SEEK");
    /* the ring head now holds the byte at the sought offset */
    Extent em = extent_of(ASSET_MUSIC);
    CHECK(W.psram[RING1_BASE] == (uint8_t)(em.off + 0x2000), "post-SEEK ring head wrong offset");
    printf("seek: re-primed=%d ring1_head=%02x expect=%02x\n",
           hx421_media_ready(&m,1), W.psram[RING1_BASE], (uint8_t)(em.off + 0x2000));

    /* --- live GAIN on the (enabled) FMV channel --- */
    enqueue((Hx421MediaCmd){ .op=HX421_MEDIA_GAIN, .slot=0, .gain=64, .pan=200 });
    hx421_media_service(&m);
    CHECK(W.mx_enabled[0] == 1 && W.mx_gain[0] == 64 && W.mx_pan[0] == 200, "live GAIN did not reach mixer");

    /* --- STOP music: mixer off, asset closed, slot free --- */
    enqueue((Hx421MediaCmd){ .op=HX421_MEDIA_STOP, .slot=1 });
    hx421_media_service(&m);
    CHECK(!hx421_media_ready(&m, 1) && W.mx_enabled[1] == 0, "music not stopped");
    CHECK(W.open_ct[ASSET_MUSIC] == 0, "music asset not closed on STOP");

    /* --- STOP_ALL: everything down, all assets closed --- */
    enqueue((Hx421MediaCmd){ .op=HX421_MEDIA_STOP_ALL });
    hx421_media_service(&m);
    CHECK(!hx421_media_ready(&m, 0), "FMV not stopped by STOP_ALL");
    CHECK(W.open_ct[ASSET_FMV] == 0, "FMV asset not closed on STOP_ALL");

    if (errs == 0) printf("MEDIATEST PASS: play/prime/offload/priority/seek/gain/stop OK\n");
    else           printf("MEDIATEST FAIL: %d errors\n", errs);
    return errs ? 1 : 0;
}
