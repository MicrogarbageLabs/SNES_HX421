/* ============================================================
 *  hx421_media.c — HX-421 media service: mailbox commands -> stream arbiter.
 *  See hx421_media.h. Public domain (CC0). No warranty.
 * ============================================================ */

#include "hx421_media.h"

#define FRAME 4u   /* 16-bit interleaved stereo */

/* Bridge the arbiter's platform seams to ours (offload + read_ptr pass through
 * unchanged; the service owns the higher-level asset/mixer/command seams). */
static void arb_plat_from(const Hx421MediaPlat *p, Hx421StreamPlat *out) {
    out->ctx      = p->ctx;
    out->offload  = p->offload;
    out->read_ptr = p->read_ptr;
}

void hx421_media_init(Hx421Media *m, const Hx421MediaPlat *plat,
                      const Hx421MediaCfg *cfg) {
    Hx421StreamPlat ap;
    int i;
    m->plat = *plat;
    m->cfg  = *cfg;
    for (i = 0; i < HX421_STREAM_MAX; i++) {
        m->s[i].active  = 0;
        m->s[i].enabled = 0;
        m->s[i].asset   = 0;
        m->s[i].gain    = 0;
        m->s[i].pan     = 128;
    }
    arb_plat_from(plat, &ap);
    hx421_stream_arb_init(&m->arb, &ap, m->cfg.chunk, 0, 0);
}

int hx421_media_ready(const Hx421Media *m, int s) {
    if (s < 0 || s >= HX421_STREAM_MAX) return 0;
    return m->s[s].active && hx421_stream_ready(&m->arb, s);
}

/* Tear a slot down: mute + release its mixer channel and asset. */
static void slot_stop(Hx421Media *m, int s) {
    if (s < 0 || s >= HX421_STREAM_MAX || !m->s[s].active) return;
    hx421_stream_stop(&m->arb, s);
    if (m->s[s].enabled && m->plat.mixer_ctl)
        m->plat.mixer_ctl(m->plat.ctx, s, 0, 0, m->s[s].pan);
    if (m->plat.asset_close)
        m->plat.asset_close(m->plat.ctx, s, m->s[s].asset);
    m->s[s].active  = 0;
    m->s[s].enabled = 0;
}

static int cmd_play(Hx421Media *m, const Hx421MediaCmd *c) {
    uint32_t off = 0, bytes = 0;
    const Hx421MediaSlotCfg *sc;
    int s = c->slot;
    if (s < 0 || s >= HX421_STREAM_MAX) return -1;
    sc = &m->cfg.slot[s];
    if (sc->ring_size == 0) return -2;               /* slot has no ring configured */

    slot_stop(m, s);                                 /* reuse: drop whatever was here */

    if (!m->plat.asset_open ||
        m->plat.asset_open(m->plat.ctx, s, c->asset, &off, &bytes) != 0)
        return -3;

    if (hx421_stream_start(&m->arb, s, sc->psram_base, sc->ring_size,
                           off, bytes, (c->flags & HX421_MEDIA_FLAG_LOOP) ? 1 : 0,
                           sc->prime_bytes, c->prio) != 0) {
        if (m->plat.asset_close) m->plat.asset_close(m->plat.ctx, s, c->asset);
        return -4;
    }
    m->s[s].active  = 1;
    m->s[s].enabled = 0;                             /* enable the mixer once primed */
    m->s[s].asset   = c->asset;
    m->s[s].gain    = c->gain;
    m->s[s].pan     = c->pan;
    return 0;
}

/* Re-point an active slot's source to a new byte offset and re-prime. Precise
 * mid-stream seek awaits the FPGA's real read pointer (the drain is time-
 * estimated today); until then this restarts the ring from the new point, which
 * is exact for start/loop-boundary seeks and close enough elsewhere. */
static int cmd_seek(Hx421Media *m, const Hx421MediaCmd *c) {
    int s = c->slot;
    Hx421Stream *st;
    if (s < 0 || s >= HX421_STREAM_MAX || !m->s[s].active) return -1;
    st = &m->arb.str[s];
    uint32_t off = c->arg & ~(FRAME - 1);
    if (off >= st->data_bytes) off = 0;
    st->file_pos  = off;
    st->write_pos = 0;
    st->primed    = 0;
    if (m->s[s].enabled && m->plat.mixer_ctl)        /* mute until re-primed */
        m->plat.mixer_ctl(m->plat.ctx, s, 0, 0, m->s[s].pan);
    m->s[s].enabled = 0;
    return 0;
}

static int cmd_gain(Hx421Media *m, const Hx421MediaCmd *c) {
    int s = c->slot;
    if (s < 0 || s >= HX421_STREAM_MAX || !m->s[s].active) return -1;
    m->s[s].gain = c->gain;
    m->s[s].pan  = c->pan;
    if (m->s[s].enabled && m->plat.mixer_ctl)
        m->plat.mixer_ctl(m->plat.ctx, s, 1, c->gain, c->pan);
    return 0;
}

int hx421_media_apply(Hx421Media *m, const Hx421MediaCmd *c) {
    int i;
    switch (c->op) {
    case HX421_MEDIA_NOP:      return 0;
    case HX421_MEDIA_PLAY:     return cmd_play(m, c);
    case HX421_MEDIA_STOP:
        if (c->slot >= HX421_STREAM_MAX) return -1;
        slot_stop(m, c->slot);
        return 0;
    case HX421_MEDIA_SEEK:     return cmd_seek(m, c);
    case HX421_MEDIA_GAIN:     return cmd_gain(m, c);
    case HX421_MEDIA_STOP_ALL:
        for (i = 0; i < HX421_STREAM_MAX; i++) slot_stop(m, i);
        return 0;
    default:                   return -5;            /* unknown op */
    }
}

int hx421_media_service(Hx421Media *m) {
    int did = 0, guard, i;
    Hx421MediaCmd c;

    /* drain pending commands (bounded so a stuck mailbox can't spin us) */
    for (guard = 0; guard < 2 * HX421_STREAM_MAX + 4; guard++) {
        if (!m->plat.cmd_poll || !m->plat.cmd_poll(m->plat.ctx, &c)) break;
        hx421_media_apply(m, &c);
        did = 1;
    }

    /* enable mixer channels whose rings have primed */
    for (i = 0; i < HX421_STREAM_MAX; i++) {
        if (m->s[i].active && !m->s[i].enabled && hx421_stream_ready(&m->arb, i)) {
            if (m->plat.mixer_ctl)
                m->plat.mixer_ctl(m->plat.ctx, i, 1, m->s[i].gain, m->s[i].pan);
            m->s[i].enabled = 1;
            did = 1;
        }
    }

    /* one refill offload for the most-urgent ring */
    if (hx421_stream_service(&m->arb)) did = 1;
    return did;
}
