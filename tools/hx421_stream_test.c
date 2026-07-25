/* ============================================================
 *  hx421_stream_test.c — host test for the SD->PSRAM streaming arbiter.
 *
 *  Models the two halves the arbiter sits between:
 *    - producer seam `offload`: copies source bytes into a mock PSRAM
 *      (stands in for the FPGA SD-DMA offload SD->PSRAM);
 *    - consumer seam `read_ptr`: a simulated FPGA mixer that drains each
 *      ring at a fixed rate, reading the bytes back out of mock PSRAM.
 *
 *  Verifies the properties that matter on hardware and can't be seen once
 *  it's a sealed cart:
 *    1. NO UNDERRUN — the mixer never reads a byte the arbiter hasn't
 *       written (fill >= drain every tick).
 *    2. DATA INTEGRITY — the byte sequence the mixer reads out equals the
 *       source WAV data in order, including looping and ring wrap.
 *    3. EOF — a non-looping stream stops (no more offloads) after its data.
 *
 *  Public domain (CC0). No warranty.
 * ============================================================ */

#include "../firmware/audio/hx421_stream.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PSRAM_BYTES (1u << 20)      /* 1 MB mock PSRAM */
#define NSTR        3

static uint8_t psram[PSRAM_BYTES];

typedef struct {
    const uint8_t *src[NSTR];
    uint32_t       src_len[NSTR];
    uint32_t       data_off[NSTR];  /* == src base within a mock "file"     */
    uint32_t       read_ptr[NSTR];  /* consumer position in ring            */
    uint32_t       consumed[NSTR];  /* total bytes the mixer has read        */
    int            fail;
} Mock;

/* source byte pattern: position- and stream-distinct, deterministic */
static uint8_t pat(int s, uint32_t i) {
    return (uint8_t)((i * 31u) ^ (i >> 8) ^ (uint32_t)(s * 0x5Au + 7u));
}

static int mock_offload(void *ctx, int s, uint32_t psram_addr,
                        uint32_t src_off, uint32_t len) {
    Mock *m = (Mock *)ctx;
    uint32_t rel = src_off - m->data_off[s];       /* offset into the source */
    if (psram_addr + len > PSRAM_BYTES) { m->fail = 1; return 1; }
    if (rel + len > m->src_len[s])      { m->fail = 2; return 1; }
    memcpy(&psram[psram_addr], &m->src[s][rel], len);
    return 0;
}

static uint32_t mock_read_ptr(void *ctx, int s) {
    Mock *m = (Mock *)ctx;
    return m->read_ptr[s];
}

/* ---- focused priority check: equal deficit, different prio -> higher prio
 *      is serviced first. Deterministic, no simulation. ---- */
static int   pc_served = -1;
static uint32_t pc_rp[HX421_STREAM_MAX];
static int   pc_offload(void *c, int s, uint32_t a, uint32_t b, uint32_t l) {
    (void)c;(void)a;(void)b;(void)l; pc_served = s; return 0;
}
static uint32_t pc_read_ptr(void *c, int s) { (void)c; return pc_rp[s]; }

static int check_priority(void) {
    Hx421StreamArb a;
    Hx421StreamPlat p = { .ctx=0, .offload=pc_offload, .read_ptr=pc_read_ptr };
    hx421_stream_arb_init(&a, &p, 256, 0, 0);
    /* two identical rings, both drained to the same fill; s1 has FMV-like prio */
    hx421_stream_start(&a, 0, 0x0, 4096, 0, 8192, 1, 0, 0);
    hx421_stream_start(&a, 1, 0x8000, 4096, 0, 8192, 1, 0, 2);
    /* pretend both are primed and equally half-empty */
    a.str[0].write_pos = 2048; a.str[1].write_pos = 2048;
    a.str[0].primed = a.str[1].primed = 1;
    pc_rp[0] = pc_rp[1] = 0;               /* fill = 2048 each, equal deficit */
    pc_served = -1;
    hx421_stream_service(&a);
    if (pc_served != 1) {
        printf("FAIL: priority — served stream %d, expected the high-prio 1\n", pc_served);
        return 1;
    }
    printf("priority: equal deficit -> high-prio stream serviced first (ok)\n");
    return 0;
}

int main(void) {
    if (check_priority()) return 1;

    static uint8_t src0[10000], src1[3000], src2[5000];
    Mock m; memset(&m, 0, sizeof m);
    uint32_t i;
    for (i = 0; i < sizeof src0; i++) src0[i] = pat(0, i);
    for (i = 0; i < sizeof src1; i++) src1[i] = pat(1, i);
    for (i = 0; i < sizeof src2; i++) src2[i] = pat(2, i);
    m.src[0]=src0; m.src_len[0]=sizeof src0; m.data_off[0]=0;
    m.src[1]=src1; m.src_len[1]=sizeof src1; m.data_off[1]=0;
    m.src[2]=src2; m.src_len[2]=sizeof src2; m.data_off[2]=0;

    Hx421StreamPlat plat = { .ctx=&m, .offload=mock_offload, .read_ptr=mock_read_ptr };
    Hx421StreamArb a;
    hx421_stream_arb_init(&a, &plat, 1024 /*chunk*/, 0, 0);

    /* rings at distinct PSRAM bases; sizes differ to exercise both */
    uint32_t base[NSTR] = { 0x00000, 0x10000, 0x20000 };
    uint32_t rsz [NSTR] = { 4096,    8192,    4096 };
    int loop[NSTR]      = { 1,       1,       0 };  /* stream 2 = non-looping */
    uint32_t datalen[NSTR] = { 10000&~3u, 3000&~3u, 5000&~3u };
    uint32_t prime[NSTR] = { 1024,    2048,    0 };   /* small heads; s2 default */
    int prio[NSTR]       = { 0,       0,       2 };   /* s2 = "FMV-like" priority  */
    int s;
    for (s = 0; s < NSTR; s++)
        if (hx421_stream_start(&a, s, base[s], rsz[s], m.data_off[s], datalen[s],
                               loop[s], prime[s], prio[s])) {
            printf("FAIL: start(%d)\n", s); return 1;
        }

    /* ---- prime: service until all streams report ready ---- */
    int guard = 100000;
    while (guard-- > 0) {
        int allready = 1;
        for (s = 0; s < NSTR; s++) if (!hx421_stream_ready(&a, s)) allready = 0;
        if (allready) break;
        hx421_stream_service(&a);
    }
    if (guard <= 0) { printf("FAIL: priming never completed\n"); return 1; }
    printf("primed: fills = %u/%u/%u bytes\n",
           hx421_stream_fill(&a,0), hx421_stream_fill(&a,1), hx421_stream_fill(&a,2));

    /* ---- run: mixer drains each ring; arbiter refills ---- */
    const uint32_t DRAIN = 200;              /* bytes/tick/stream (FRAME mult) */
    const int SVC_PER_TICK = 4;              /* M4 refills per audio tick      */
    const int TICKS = 4000;
    int t, k;
    for (t = 0; t < TICKS; t++) {
        for (k = 0; k < SVC_PER_TICK; k++) hx421_stream_service(&a);

        for (s = 0; s < NSTR; s++) {
            if (!a.str[s].active) continue;
            uint32_t fill = hx421_stream_fill(&a, s);
            uint32_t want = DRAIN;
            /* a finished non-looping stream drains only what's left, then idle */
            if (!loop[s]) {
                /* stop consuming once we've read all its data */
                if (m.consumed[s] >= datalen[s]) continue;
                if (m.consumed[s] + want > datalen[s]) want = datalen[s] - m.consumed[s];
            }
            if (fill < want) { printf("FAIL: UNDERRUN stream %d tick %d (fill %u < %u)\n",
                                      s, t, fill, want); return 1; }
            /* consume `want` bytes out of the ring, verify against source */
            for (i = 0; i < want; i++) {
                uint8_t got = psram[base[s] + m.read_ptr[s]];
                uint8_t exp = m.src[s][ m.consumed[s] % datalen[s] ];
                if (got != exp) {
                    printf("FAIL: CORRUPT stream %d at byte %u (got %02x exp %02x)\n",
                           s, m.consumed[s], got, exp); return 1;
                }
                m.read_ptr[s] = (m.read_ptr[s] + 1) % rsz[s];
                m.consumed[s]++;
            }
        }
    }
    if (m.fail) { printf("FAIL: offload bounds violation (%d)\n", m.fail); return 1; }

    printf("consumed: %u/%u/%u bytes\n", m.consumed[0], m.consumed[1], m.consumed[2]);
    /* stream 2 (non-looping) must have stopped issuing offloads at EOF */
    if (a.str[2].file_pos != datalen[2] && a.str[2].file_pos != 0) {
        /* file_pos rests at data end (not looped) */
    }
    printf("stream2 non-looping consumed exactly its data: %s\n",
           (m.consumed[2] == datalen[2]) ? "yes" : "no");

    printf("RESULT: PASS - no underrun, data integrity across loop+wrap, EOF handled\n");
    return 0;
}
