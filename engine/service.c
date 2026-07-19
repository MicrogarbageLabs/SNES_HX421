/* ============================================================
 *  service.c — HX-421 audio engine runtime owner
 *
 *  Fresh re-implementation of the "owner" that wires the component
 *  engine into one unit (see service.h / PROVENANCE.md). It builds a
 *  pool + sync-enabled stereo mixer + arbiter (driven through an
 *  AudioArbiterSink) + per-voice music_player staging + FFT meter, and
 *  exposes direct C command entry points and a pull-style render.
 *
 *  Deliberately NOT the microgarbage audio_service.c: no VM
 *  service_channel / REQ_AUDIO_* IPC, no host/OS dependency. All file
 *  I/O is delegated to a caller-supplied AudioFileReader vtable, so
 *  this translation unit reaches only the engine + libc alloc/mem.
 *
 *  Public domain (CC0). No warranty.
 * ============================================================ */

#include "service.h"

#include "audio/audio_pool_stream.h"
#include "audio/audio_ring_stream.h"
#include "audio/audio_fft.h"
#include "audio/wav.h"
#include "math/fixed_point.h"

#include <stdlib.h>
#include <string.h>

/* ---- fixed tunables ---- */

#define HXA_DEFAULT_RATE     44100u
#define HXA_DEFAULT_TRACKS   8u
#define HXA_DEFAULT_POOL     (4u * 1024u * 1024u)   /* 4 MiB sound RAM  */

/* Per-music-player staging (stereo16: 2 int16/frame). The streaming
 * buffer is comfortably larger than a render quantum; the pinned heads
 * cover the intro->loop / restart seams. */
#define HXA_MUSIC_STREAM_FRAMES  8192u
#define HXA_MUSIC_HEAD_FRAMES    1024u

/* Low-latency (ring-fed / FMV) staging: ~46 ms each at 44.1 kHz. Must stay
 * <= HXA_MUSIC_STREAM_FRAMES since the slot's buffer is sized for music. */
#define HXA_FMV_STREAM_FRAMES    2048u
#define HXA_FMV_CHANNEL_FILL     2048u

/* SFX chunk-expansion stack buffer (mono -> L==R), in frames. */
#define HXA_SFX_EXPAND_FRAMES    512u

/* What a music voice's slot is currently sourcing from. */
typedef enum {
    HXA_SRC_NONE = 0,
    HXA_SRC_FILE,     /* file-backed WAV stream (file_ctx)    */
    HXA_SRC_RING,     /* push ring (svc->ring)                */
} HxaMusicSource;

/* One music_player instance bound to a mixer channel while a streamed
 * voice plays on it. One per track. */
typedef struct {
    bool                in_use;
    uint32_t            track;
    HxaMusicSource      source;
    MusicPlayer        *player;
    AudioFileStreamCtx  file_ctx;      /* HXA_SRC_FILE */
    /* contiguous stereo16 staging (NOT block pool) */
    int16_t            *streaming_buf;
    int16_t            *intro_head;
    int16_t            *loop_head;
} HxaMusicSlot;

struct HxaService {
    AudioPool     pool;
    AudioArbiter  arbiter;
    AudioMixer   *mixer;
    AudioFft      fft;

    uint32_t      sample_rate;
    uint32_t      track_count;

    uint8_t      *pool_region;         /* malloc'd sound RAM (owned)   */

    HxaMusicSlot *slots;               /* [track_count]                */

    /* Single push ring shared by the one live pcm-stream voice. */
    AudioRingStream ring;
    uint32_t        ring_rate;

    /* Single-context handoff to svc_sink_start (mirrors the reference's
     * pend_* pattern). Valid only across one arbiter_play* call. */
    /* Ring-fed (FMV) staging depths in frames — see hxa_set_lowlat(). */
    size_t          ll_stream;
    size_t          ll_chanfill;

    HxaMusicSource  pend_source;
    const char     *pend_path;         /* HXA_SRC_FILE */
    uint32_t        pend_rate;         /* HXA_SRC_RING */
    AudioFileReader reader;

    /* Cumulative frames rendered (internal clock, drift diagnostics). */
    uint64_t        total_output;

    /* SFX staging scratch (bytes from the pool block chain). */
    uint8_t         scratch[AUDIO_POOL_BLOCK_SIZE];
};

/* ---- music slot helpers ---- */

static HxaMusicSlot *slot_free(HxaService *s) {
    for (uint32_t i = 0; i < s->track_count; i++)
        if (!s->slots[i].in_use) return &s->slots[i];
    return NULL;
}
static HxaMusicSlot *slot_for_track(HxaService *s, uint32_t track) {
    for (uint32_t i = 0; i < s->track_count; i++)
        if (s->slots[i].in_use && s->slots[i].track == track)
            return &s->slots[i];
    return NULL;
}

/* Build a streamed music_player on `track` for slot `ms`, given a
 * stream_fn + user_data. Returns true if the player was created,
 * primed and started. */
static bool start_music(HxaService *s, HxaMusicSlot *ms, uint32_t track,
                        music_stream_fn stream_fn, void *user_data,
                        bool loop, const AudioVoiceParams *p, bool low_latency) {
    if (p) {
        mixer_set_volume(s->mixer, track, (q15_t)p->gain);
        mixer_set_pan(s->mixer, track, (q15_t)p->pan);
    }
    /* Streamed music is at the mixer output rate already: no per-channel
     * resample step (identity). Reset so it starts clean. */
    mixer_set_source_rate(s->mixer, track, 0);
    mixer_channel_reset(s->mixer, track);

    MusicPlayerConfig cfg = {
        .mixer                    = s->mixer,
        .mixer_channel            = track,
        .format                   = MIXER_SRC_PCM16_STEREO,
        .stream_fn                = stream_fn,
        .stream_user_data         = user_data,
        .intro_stream_id          = 0,
        .loop_stream_id           = loop ? 1 : MUSIC_STREAM_NONE,
        .streaming_buffer         = ms->streaming_buf,
        /* A ring-fed (FMV) voice must stay phase-aligned with a near-instant
         * video path, so its output latency has to be SMALL. The upstream ring
         * (743 ms) is the underrun cushion, so the player's own staging can be
         * tiny AND the mixer-channel prefill must be capped — left uncapped it
         * runs to the channel's full 743 ms and puts audio ~0.8 s behind the
         * picture. Music voices keep the deep buffers (latency is free there). */
        .streaming_buffer_samples = low_latency ? s->ll_stream : HXA_MUSIC_STREAM_FRAMES,
        .max_channel_fill_samples = low_latency ? s->ll_chanfill : 0,
        .intro_head_buffer        = ms->intro_head,
        .intro_head_samples       = HXA_MUSIC_HEAD_FRAMES,
        .loop_head_buffer         = ms->loop_head,
        .loop_head_samples        = HXA_MUSIC_HEAD_FRAMES,
    };
    ms->player = music_create(&cfg);
    if (!ms->player) return false;

    ms->track  = track;
    ms->in_use = true;
    music_prime_intro(ms->player);
    if (loop) music_prime_loop(ms->player);
    music_play(ms->player);
    return true;
}

/* ---- arbiter sink ---- */

/* Start a voice on a track. SFX: feed the whole pool sample into the
 * mixer channel (mono16 promoted to L==R). MUSIC (external): file or
 * ring streamed via a music_player, per the pend_* handoff. */
static bool svc_sink_start(void *ctx, uint32_t track, AudioVoiceKind kind,
                           AudioObjHandle object, const AudioVoiceParams *p) {
    HxaService *s = (HxaService *)ctx;

    if (kind == AUDIO_VOICE_MUSIC) {
        HxaMusicSlot *ms = slot_free(s);
        if (!ms) return false;

        if (s->pend_source == HXA_SRC_FILE) {
            if (!s->reader.open || !s->pend_path) return false;
            if (!audio_file_stream_open(&ms->file_ctx, &s->reader, s->pend_path))
                return false;
            ms->source = HXA_SRC_FILE;
            if (!start_music(s, ms, track, audio_file_stream_read,
                             &ms->file_ctx, /*loop=*/true, p, /*low_latency=*/false)) {
                audio_file_stream_close(&ms->file_ctx);
                ms->source = HXA_SRC_NONE;
                return false;
            }
            return true;
        }

        if (s->pend_source == HXA_SRC_RING) {
            ms->source = HXA_SRC_RING;
            /* The mixer channel plays at the ring's PCM rate; the mixer's
             * per-channel resampler reconciles it to the output rate. */
            audio_ring_stream_reset(&s->ring);
            if (!start_music(s, ms, track, audio_ring_stream_read,
                             &s->ring, /*loop=*/false, p, /*low_latency=*/true)) {
                ms->source = HXA_SRC_NONE;
                return false;
            }
            /* Ring PCM may be at a non-output rate: arm the channel's
             * resampler AFTER start_music reset it (start_music forced
             * identity for file voices). */
            if (s->ring_rate && s->ring_rate != s->sample_rate)
                mixer_set_source_rate(s->mixer, track, s->ring_rate);
            return true;
        }

        return false;   /* pool-backed music not exposed by this owner */
    }

    /* SFX one-shot: object is a mono16 pool sample. Set its source rate
     * (recorded at load) so the mixer resamples to the output rate, then
     * feed the whole sample in as promoted stereo. */
    uint32_t size = audio_pool_object_size(&s->pool, object);
    if (size == 0) return false;

    if (p) {
        mixer_set_volume(s->mixer, track, (q15_t)p->gain);
        mixer_set_pan(s->mixer, track, (q15_t)p->pan);
    }
    uint32_t src_rate = audio_pool_object_sample_rate(&s->pool, object);
    mixer_set_source_rate(s->mixer, track, src_rate);
    mixer_channel_reset(s->mixer, track);

    int16_t st[HXA_SFX_EXPAND_FRAMES * 2];
    uint32_t off = 0;
    while (off < size) {
        uint32_t want = size - off;
        if (want > sizeof(s->scratch)) want = (uint32_t)sizeof(s->scratch);
        uint32_t got = 0;
        if (audio_pool_read(&s->pool, object, off, s->scratch, want, &got)
                != AUDIO_POOL_OK || got == 0)
            break;
        const int16_t *mono = (const int16_t *)s->scratch;
        uint32_t mono_n = got / 2u;
        uint32_t done = 0;
        while (done < mono_n) {
            uint32_t sub = mono_n - done;
            if (sub > HXA_SFX_EXPAND_FRAMES) sub = HXA_SFX_EXPAND_FRAMES;
            for (uint32_t i = 0; i < sub; i++) {
                int16_t m = mono[done + i];
                st[i * 2]     = m;
                st[i * 2 + 1] = m;
            }
            mixer_write_channel(s->mixer, track, st, sub);
            done += sub;
        }
        off += got;
    }

    mixer_channel_start(s->mixer, track);
    return true;
}

static void svc_sink_stop(void *ctx, uint32_t track) {
    HxaService *s = (HxaService *)ctx;

    HxaMusicSlot *ms = slot_for_track(s, track);
    if (ms) {
        if (ms->player) { music_destroy(ms->player); ms->player = NULL; }
        if (ms->source == HXA_SRC_FILE) audio_file_stream_close(&ms->file_ctx);
        ms->source = HXA_SRC_NONE;
        ms->in_use = false;
    }
    mixer_channel_stop(s->mixer, track);
    mixer_channel_reset(s->mixer, track);
}

/* A one-shot SFX track is finished once its mixer channel has drained
 * (the whole sample was fed at start and nothing refills it). The
 * arbiter only asks about SFX tracks (music is never reaped). */
static bool svc_sink_is_done(void *ctx, uint32_t track) {
    HxaService *s = (HxaService *)ctx;
    return mixer_channel_buffered(s->mixer, track) == 0;
}

/* ---- lifecycle ---- */

HxaService *hxa_create(const HxaConfig *cfg) {
    HxaConfig c = cfg ? *cfg : (HxaConfig){0};
    uint32_t rate   = c.sample_rate ? c.sample_rate : HXA_DEFAULT_RATE;
    uint32_t tracks = c.track_count ? c.track_count : HXA_DEFAULT_TRACKS;
    if (tracks > AUDIO_ARBITER_MAX_TRACKS) tracks = AUDIO_ARBITER_MAX_TRACKS;
    size_t   pool_bytes = c.pool_bytes ? c.pool_bytes : HXA_DEFAULT_POOL;

    HxaService *s = calloc(1, sizeof(*s));
    if (!s) return NULL;
    s->sample_rate = rate;
    s->track_count = tracks;
    s->reader      = c.reader;

    /* sound RAM region for the pool */
    s->pool_region = malloc(pool_bytes);
    if (!s->pool_region) goto fail_svc;
    if (audio_pool_init(&s->pool, s->pool_region, pool_bytes) != AUDIO_POOL_OK)
        goto fail_region;

    /* mixer: full-stereo, every channel PCM16_STEREO so stereo streams
     * stay intact and mono SFX are promoted L==R (never downmixed). */
    MixerChannelConfig *chans = calloc(tracks, sizeof(MixerChannelConfig));
    if (!chans) goto fail_pool;
    for (uint32_t i = 0; i < tracks; i++) {
        chans[i].format         = MIXER_SRC_PCM16_STEREO;
        chans[i].buffer_samples = 32768;   /* ~743 ms @44.1k, holds a whole SFX */
        chans[i].volume         = Q15_ONE;
        chans[i].interp         = MIXER_INTERP_CUBIC;
    }
    MixerOutputFormat out = { .bits = 16, .is_signed = true,
                              .storage_bits = 16, .channels = 2 };
    MixerSyncConfig sync = {
        .external_ticks_per_second = (uint64_t)rate,
        .correction_smoothing      = (q15_t)(Q15_ONE / 128),
        .max_correction            = (q15_t)(Q15_ONE / 50),
    };
    s->mixer = mixer_create_with_sync(chans, tracks, (int)rate, out,
                                      c.headroom_bits, &sync, NULL, NULL);
    free(chans);
    if (!s->mixer) goto fail_pool;

    s->ll_stream   = HXA_FMV_STREAM_FRAMES;      /* ring-fed voice defaults */
    s->ll_chanfill = HXA_FMV_CHANNEL_FILL;

    AudioArbiterSink sink = { svc_sink_start, svc_sink_stop, s, svc_sink_is_done };
    if (!audio_arbiter_init(&s->arbiter, tracks, &s->pool, &sink))
        goto fail_mixer;

    audio_fft_init(&s->fft, rate);
    audio_ring_stream_reset(&s->ring);

    /* per-track music staging buffers (stereo16) */
    s->slots = calloc(tracks, sizeof(HxaMusicSlot));
    if (!s->slots) goto fail_mixer;
    for (uint32_t i = 0; i < tracks; i++) {
        HxaMusicSlot *ms = &s->slots[i];
        ms->streaming_buf = malloc(HXA_MUSIC_STREAM_FRAMES * 2u * sizeof(int16_t));
        ms->intro_head    = malloc(HXA_MUSIC_HEAD_FRAMES   * 2u * sizeof(int16_t));
        ms->loop_head     = malloc(HXA_MUSIC_HEAD_FRAMES   * 2u * sizeof(int16_t));
        if (!ms->streaming_buf || !ms->intro_head || !ms->loop_head)
            goto fail_slots;
    }
    return s;

fail_slots:
    for (uint32_t i = 0; i < tracks; i++) {
        free(s->slots[i].streaming_buf);
        free(s->slots[i].intro_head);
        free(s->slots[i].loop_head);
    }
    free(s->slots);
fail_mixer:
    mixer_destroy(s->mixer);
fail_pool:
    audio_pool_destroy(&s->pool);
fail_region:
    free(s->pool_region);
fail_svc:
    free(s);
    return NULL;
}

void hxa_destroy(HxaService *s) {
    if (!s) return;
    if (s->slots) {
        for (uint32_t i = 0; i < s->track_count; i++) {
            HxaMusicSlot *ms = &s->slots[i];
            if (ms->player) music_destroy(ms->player);
            free(ms->streaming_buf);
            free(ms->intro_head);
            free(ms->loop_head);
        }
        free(s->slots);
    }
    if (s->mixer) mixer_destroy(s->mixer);
    audio_pool_destroy(&s->pool);
    free(s->pool_region);
    free(s);
}

/* ---- loading ---- */

/* Slurp an entire file into a malloc'd buffer via the reader vtable.
 * Returns the buffer (caller frees) and sets *out_len, or NULL. */
static uint8_t *slurp_file(HxaService *s, const char *path, size_t *out_len) {
    if (!s->reader.open || !s->reader.read || !path) return NULL;
    void *fh = s->reader.open(s->reader.ctx, path);
    if (!fh) return NULL;

    size_t   cap = 65536, len = 0;
    uint8_t *buf = malloc(cap);
    if (!buf) { s->reader.close(s->reader.ctx, fh); return NULL; }
    for (;;) {
        if (len == cap) {
            size_t ncap = cap * 2;
            uint8_t *nb = realloc(buf, ncap);
            if (!nb) { free(buf); buf = NULL; break; }
            buf = nb; cap = ncap;
        }
        uint32_t want = (uint32_t)(cap - len);
        uint32_t got  = s->reader.read(s->reader.ctx, fh, buf + len, want);
        len += got;
        if (got < want) break;   /* short read == EOF (stdio fread) */
    }
    s->reader.close(s->reader.ctx, fh);
    if (!buf) return NULL;
    *out_len = len;
    return buf;
}

AudioObjHandle hxa_load_sfx_wav(HxaService *s, const char *path) {
    if (!s) return AUDIO_POOL_HANDLE_NONE;
    size_t len = 0;
    uint8_t *file = slurp_file(s, path, &len);
    if (!file) return AUDIO_POOL_HANDLE_NONE;

    WavInfo info;
    if (wav_parse(file, len, &info) != WAV_OK) { free(file); return 0; }

    /* mono16 at native rate; the mixer resamples per-channel at play. */
    uint32_t src_frames = info.data_bytes / (info.channels * (info.bits / 8u));
    if (src_frames == 0) { free(file); return 0; }
    int16_t *mono = malloc((size_t)src_frames * sizeof(int16_t));
    if (!mono) { free(file); return 0; }
    uint32_t frames = wav_to_mono_pcm16(&info, mono, src_frames);
    free(file);
    if (frames == 0) { free(mono); return 0; }

    AudioObjHandle h = AUDIO_POOL_HANDLE_NONE;
    uint32_t bytes = frames * (uint32_t)sizeof(int16_t);
    if (audio_pool_alloc_with_rate(&s->pool, bytes, 0, info.sample_rate, &h)
            != AUDIO_POOL_OK) { free(mono); return 0; }
    uint32_t wrote = 0;
    audio_pool_write(&s->pool, h, 0, mono, bytes, &wrote);
    free(mono);
    return h;
}

AudioObjHandle hxa_load_sfx_pcm(HxaService *s, const void *data,
                                uint32_t bytes, uint32_t rate) {
    if (!s || !data || bytes == 0) return AUDIO_POOL_HANDLE_NONE;
    AudioObjHandle h = AUDIO_POOL_HANDLE_NONE;
    if (audio_pool_alloc_with_rate(&s->pool, bytes, 0, rate, &h)
            != AUDIO_POOL_OK) return 0;
    uint32_t wrote = 0;
    audio_pool_write(&s->pool, h, 0, data, bytes, &wrote);
    return h;
}

/* ---- playback ---- */

AudioVoiceHandle hxa_trigger_sfx(HxaService *s, AudioObjHandle obj,
                                 int32_t gain_q15, int32_t pan_q15) {
    if (!s) return AUDIO_VOICE_NONE;
    AudioVoiceParams p = { .gain = gain_q15, .pan = pan_q15,
                           .priority = 0, .loop = 0 };
    AudioVoiceHandle v = AUDIO_VOICE_NONE;
    audio_arbiter_play(&s->arbiter, obj, AUDIO_VOICE_SFX, &p, 0, &v);
    return v;
}

AudioVoiceHandle hxa_play_stream_wav(HxaService *s, const char *path) {
    if (!s || !path) return AUDIO_VOICE_NONE;
    AudioVoiceParams p = { .gain = Q15_ONE, .pan = 0, .priority = 0, .loop = 0 };
    AudioVoiceHandle v = AUDIO_VOICE_NONE;
    s->pend_source = HXA_SRC_FILE;
    s->pend_path   = path;
    audio_arbiter_play_external(&s->arbiter, &p, 0, &v);
    s->pend_source = HXA_SRC_NONE;
    s->pend_path   = NULL;
    return v;
}

void hxa_stop_voice(HxaService *s, AudioVoiceHandle voice) {
    if (!s) return;
    audio_arbiter_stop(&s->arbiter, voice);
}

/* ---- push PCM ring ---- */

AudioVoiceHandle hxa_open_pcm_stream(HxaService *s, uint32_t rate,
                                     int32_t gain_q15, int32_t pan_q15) {
    if (!s) return AUDIO_VOICE_NONE;
    AudioVoiceParams p = { .gain = gain_q15, .pan = pan_q15,
                           .priority = 0, .loop = 0 };
    AudioVoiceHandle v = AUDIO_VOICE_NONE;
    s->ring_rate   = rate ? rate : s->sample_rate;
    s->pend_source = HXA_SRC_RING;
    s->pend_rate   = s->ring_rate;
    audio_arbiter_play_external(&s->arbiter, &p, 0, &v);
    s->pend_source = HXA_SRC_NONE;
    s->pend_rate   = 0;
    return v;
}

size_t hxa_feed_pcm(HxaService *s, const int16_t *stereo, size_t frames) {
    if (!s || !stereo) return 0;
    return audio_ring_stream_push(&s->ring, stereo, frames);
}

void hxa_set_lowlat(HxaService *s, size_t stream_frames, size_t channel_fill) {
    if (!s) return;
    if (stream_frames == 0)                     stream_frames = HXA_FMV_STREAM_FRAMES;
    if (stream_frames > HXA_MUSIC_STREAM_FRAMES) stream_frames = HXA_MUSIC_STREAM_FRAMES;
    if (channel_fill == 0)                      channel_fill = HXA_FMV_CHANNEL_FILL;
    s->ll_stream   = stream_frames;
    s->ll_chanfill = channel_fill;
}

void hxa_ring_stats(HxaService *s, uint32_t *underruns, uint32_t *overflows,
                    uint32_t *fill_frames) {
    if (underruns)   *underruns   = s ? atomic_load(&s->ring.underruns) : 0u;
    if (overflows)   *overflows   = s ? atomic_load(&s->ring.overflows) : 0u;
    if (fill_frames) *fill_frames = s ? (uint32_t)audio_ring_stream_avail(&s->ring) : 0u;
}

/* ---- render ---- */

void hxa_render(HxaService *s, int16_t *out, uint32_t frames) {
    if (!s || !out) return;

    /* Non-RT pump: keep each active streamed voice's mixer channel fed
     * from its source (file / ring) before we render. */
    for (uint32_t i = 0; i < s->track_count; i++)
        if (s->slots[i].in_use && s->slots[i].player)
            music_update(s->slots[i].player);

    /* Reclaim one-shot SFX tracks whose channel drained, so the arbiter
     * doesn't fill up and reject new triggers. */
    audio_arbiter_reap(&s->arbiter);

    mixer_render(s->mixer, out, frames);
    s->total_output += frames;

    /* RT-safe capture; the FFT itself runs in hxa_fft_bands (non-RT). */
    audio_fft_capture(&s->fft, out, frames);
}

/* ---- drift sync passthrough ---- */

int32_t hxa_observe_sync(HxaService *s, uint64_t internal_frames,
                         uint64_t external_ticks) {
    if (!s) return 0;
    return mixer_observe_sync(s->mixer, internal_frames, external_ticks);
}

void hxa_set_drift_ppm(HxaService *s, int32_t ppm) {
    if (s) mixer_set_drift_ppm(s->mixer, ppm);
}

/* ---- FFT ---- */

void hxa_fft_set_enabled(HxaService *s, bool on) {
    if (s) audio_fft_set_enabled(&s->fft, on);
}

uint32_t hxa_fft_bands(HxaService *s, uint32_t *out, uint32_t n) {
    if (!s || !out) return 0;
    audio_fft_update(&s->fft);           /* non-RT recompute */
    uint8_t bands[AUDIO_FFT_BANDS];
    uint32_t got = audio_fft_get_bands(&s->fft, bands, AUDIO_FFT_BANDS);
    if (got > n) got = n;
    for (uint32_t i = 0; i < got; i++) out[i] = bands[i];
    return got;
}

/* ---- accessors ---- */

AudioMixer *hxa_mixer(HxaService *s) { return s ? s->mixer : NULL; }
AudioPool  *hxa_pool(HxaService *s)  { return s ? &s->pool : NULL; }
