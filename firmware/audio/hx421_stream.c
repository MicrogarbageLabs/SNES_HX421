/* ============================================================
 *  hx421_stream.c — SD -> PSRAM-ring streaming arbiter (see .h)
 *  Public domain (CC0). No warranty.
 * ============================================================ */

#include "hx421_stream.h"

#define FRAME 4u   /* 16-bit interleaved stereo */

static uint32_t umin(uint32_t a, uint32_t b) { return a < b ? a : b; }

/* bytes the mixer has NOT yet consumed = write_pos - read_ptr (mod ring) */
static uint32_t fill_of(const Hx421StreamArb *a, int s) {
    const Hx421Stream *st = &a->str[s];
    uint32_t rp = a->plat.read_ptr ? a->plat.read_ptr(a->plat.ctx, s) : 0;
    if (rp >= st->ring_size) rp %= st->ring_size;   /* defensive */
    return (st->write_pos + st->ring_size - rp) % st->ring_size;
}

uint32_t hx421_stream_fill(const Hx421StreamArb *a, int s) {
    if (s < 0 || s >= HX421_STREAM_MAX || !a->str[s].active) return 0;
    return fill_of(a, s);
}

/* per-stream target fill: leave one chunk of gap so the write pointer can
 * never lap the read pointer (fill stays <= ring_size - chunk, and chunk
 * >= FRAME, so write_pos != read_ptr while "full"). */
static uint32_t target_of(const Hx421StreamArb *a, int s) {
    uint32_t rs = a->str[s].ring_size;
    uint32_t t  = (rs > a->chunk) ? rs - a->chunk : (rs > FRAME ? rs - FRAME : 0);
    if (a->high_wm && a->high_wm < t) t = a->high_wm;   /* optional cap */
    return t;
}

void hx421_stream_arb_init(Hx421StreamArb *a, const Hx421StreamPlat *plat,
                           uint32_t chunk, uint32_t low_wm, uint32_t high_wm) {
    int i;
    for (i = 0; i < HX421_STREAM_MAX; i++) a->str[i].active = 0;
    a->plat    = *plat;
    a->chunk   = chunk ? (chunk & ~(FRAME - 1)) : 4096u;
    if (a->chunk < FRAME) a->chunk = FRAME;
    a->low_wm  = low_wm;
    a->high_wm = high_wm;
    a->rr      = 0;
}

int hx421_stream_start(Hx421StreamArb *a, int s,
                       uint32_t psram_base, uint32_t ring_size,
                       uint32_t data_off, uint32_t data_bytes, int looping,
                       uint32_t prime_bytes, int prio) {
    Hx421Stream *st;
    if (s < 0 || s >= HX421_STREAM_MAX) return -1;
    if (ring_size < 2 * a->chunk || (ring_size & (FRAME - 1))) return -2;
    if (data_bytes < FRAME) return -3;
    st = &a->str[s];
    st->psram_base  = psram_base;
    st->ring_size   = ring_size;
    st->data_off    = data_off;
    st->data_bytes  = data_bytes & ~(FRAME - 1);
    st->prime_bytes = prime_bytes & ~(FRAME - 1);
    st->write_pos   = 0;
    st->file_pos    = 0;
    st->looping     = looping ? 1 : 0;
    st->primed      = 0;
    st->prio        = (prio < 0) ? 0 : (prio > 3 ? 3 : (uint8_t)prio);
    st->active      = 1;
    return 0;
}

void hx421_stream_stop(Hx421StreamArb *a, int s) {
    if (s >= 0 && s < HX421_STREAM_MAX) a->str[s].active = 0;
}

int hx421_stream_ready(const Hx421StreamArb *a, int s) {
    if (s < 0 || s >= HX421_STREAM_MAX) return 0;
    return a->str[s].active && a->str[s].primed;
}

int hx421_stream_service(Hx421StreamArb *a) {
    int i, best = -1;
    uint32_t best_score = 0;
    Hx421Stream *st;
    uint32_t fill, target, run, room;

    /* choose the most-urgent active stream below its target that still has
     * data to give. Urgency = deficit weighted by priority, so the FMV
     * stream (higher prio, frame-drop on underrun) is refilled ahead of the
     * music streams when SD bandwidth is contended. Index order breaks ties. */
    for (i = 0; i < HX421_STREAM_MAX; i++) {
        uint32_t score;
        if (!a->str[i].active) continue;
        /* source exhausted and not looping -> nothing to do */
        if (!a->str[i].looping && a->str[i].file_pos >= a->str[i].data_bytes) continue;
        fill   = fill_of(a, i);
        target = target_of(a, i);
        if (fill >= target) continue;                 /* full enough */
        score = (target - fill) * (uint32_t)(a->str[i].prio + 1);
        if (best < 0 || score > best_score) {
            best_score = score;
            best = i;
        }
    }
    if (best < 0) return 0;

    st     = &a->str[best];
    fill   = fill_of(a, best);
    target = target_of(a, best);
    room   = target - fill;

    /* loop point: wrap the source read cursor before sizing the run */
    if (st->file_pos >= st->data_bytes) {
        st->file_pos = 0;                              /* looping (checked above) */
    }

    run = umin(a->chunk, room);
    run = umin(run, st->ring_size - st->write_pos);    /* stop at ring end */
    run = umin(run, st->data_bytes - st->file_pos);    /* stop at source end */
    run &= ~(FRAME - 1);
    if (run == 0) return 0;

    if (a->plat.offload(a->plat.ctx, best,
                        st->psram_base + st->write_pos,
                        st->data_off + st->file_pos, run) != 0)
        return 0;                                      /* platform busy; retry later */

    st->write_pos += run;
    if (st->write_pos >= st->ring_size) st->write_pos -= st->ring_size;
    st->file_pos  += run;
    if (st->file_pos >= st->data_bytes && st->looping) st->file_pos = 0;

    /* ready once the primed head is loaded (prime_bytes, capped at target;
     * default = full target). Lets playback start after a small head rather
     * than waiting for the whole ring, while refill continues to target. */
    if (!st->primed) {
        uint32_t head = st->prime_bytes ? st->prime_bytes : target;
        if (head > target) head = target;
        if (fill_of(a, best) >= head) st->primed = 1;
    }

    a->rr = (best + 1) % HX421_STREAM_MAX;
    return 1;
}
