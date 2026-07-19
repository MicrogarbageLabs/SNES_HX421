/* ============================================================
 *  service.h — HX-421 audio engine runtime owner (public API)
 *
 *  This is the "owner" seam that turns the already-green component
 *  engine (pool + mixer + arbiter + music_player + fft + the stream
 *  sources) into a single usable unit. It builds and wires them, and
 *  exposes a small command surface plus a pull-style render entry
 *  point — the function the eventual `hx421_audio_pull` calls.
 *
 *  It is re-implemented FRESH for HX-421 (see PROVENANCE.md). It is
 *  NOT the microgarbage `audio_service.c`: there is no VM
 *  `service_channel` / `REQ_AUDIO_*` IPC and no host/OS dependency.
 *  All commands are direct C calls; all file I/O is delegated to a
 *  caller-supplied `AudioFileReader` vtable, so `service.c` itself is
 *  filesystem-agnostic (the desktop demo binds a stdio reader; an MCU
 *  port binds SD/flash).
 *
 *  Threading: single-context, exactly like the components it owns.
 *  Command entry points and `hxa_render` run on the same context
 *  (serialized). Not internally locked.
 *
 *  Public domain (CC0). No warranty.
 * ============================================================ */

#ifndef HX421_SERVICE_H
#define HX421_SERVICE_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#include "audio/audio_pool.h"          /* AudioObjHandle              */
#include "audio/audio_arbiter.h"       /* AudioVoiceHandle            */
#include "audio/audio_mixer.h"         /* AudioMixer (accessor)       */
#include "audio/audio_file_stream.h"   /* AudioFileReader vtable      */

typedef struct HxaService HxaService;

/* ---- configuration ---- */

typedef struct {
    /* Output sample rate in Hz. 0 => 44100. */
    uint32_t        sample_rate;
    /* Number of mixer channels / arbiter tracks (voices). 0 => 8.
     * Clamped to AUDIO_ARBITER_MAX_TRACKS. */
    uint32_t        track_count;
    /* Bytes of audio "sound RAM" to reserve for the pool (carved into
     * blocks by audio_pool). 0 => 4 MiB. The service malloc()s this
     * region on the desktop; on the MCU it would be a fixed PSRAM base. */
    size_t          pool_bytes;
    /* Mixer output headroom in bits (right-shift before saturating).
     * 0 = max level (clip on peaks), 3 = ~18 dB. Default 0. */
    uint8_t         headroom_bits;
    /* File-reader seam for the WAV loaders and file-stream music voice.
     * May be left zeroed if no file-based entry point is used
     * (load_sfx_pcm / feed_pcm need no reader). */
    AudioFileReader reader;
} HxaConfig;

/* ---- lifecycle ---- */

/* Build the whole engine: pool over a malloc'd region, a sync-enabled
 * 16-bit signed stereo mixer at `sample_rate`, an arbiter wired to the
 * mixer via an AudioArbiterSink, per-voice music_player staging, and an
 * FFT band meter. Returns NULL on bad args or allocation failure. */
HxaService *hxa_create(const HxaConfig *cfg);

/* Tear everything down (mixer, players, pool region, staging). NULL-safe. */
void hxa_destroy(HxaService *s);

/* ---- loading samples into the pool ---- */

/* Slurp a WAV file (via the configured reader), decode to mono16, and
 * stage it in the pool at its native sample rate (the mixer resamples
 * per-channel at play time). Returns an object handle, or
 * AUDIO_POOL_HANDLE_NONE (0) on failure. */
AudioObjHandle hxa_load_sfx_wav(HxaService *s, const char *path);

/* Stage raw mono16 PCM (`bytes` bytes) at source `rate` Hz into the
 * pool. Returns an object handle, or 0 on failure. */
AudioObjHandle hxa_load_sfx_pcm(HxaService *s, const void *data,
                                uint32_t bytes, uint32_t rate);

/* ---- playback ---- */

/* Trigger a one-shot SFX from a pool object, claiming a voice via the
 * arbiter (FCFS reject-on-full). gain/pan are q15. Returns the voice
 * handle, or AUDIO_VOICE_NONE (0) if all voices are busy / bad object. */
AudioVoiceHandle hxa_trigger_sfx(HxaService *s, AudioObjHandle obj,
                                 int32_t gain_q15, int32_t pan_q15);

/* Play a long WAV as a streamed music voice: read incrementally from the
 * file (via the configured reader) into a music_player, loop forever.
 * Returns the voice handle, or 0 on failure. */
AudioVoiceHandle hxa_play_stream_wav(HxaService *s, const char *path);

/* Stop a voice (SFX or stream). */
void hxa_stop_voice(HxaService *s, AudioVoiceHandle voice);

/* ---- push PCM ring source (optional) ---- */

/* Open a music voice fed by a push ring of interleaved-stereo PCM16 at
 * `rate` Hz. Feed it with hxa_feed_pcm. Only one push stream is live at
 * a time. Returns the voice handle, or 0 on failure. */
AudioVoiceHandle hxa_open_pcm_stream(HxaService *s, uint32_t rate,
                                     int32_t gain_q15, int32_t pan_q15);

/* Push `frames` interleaved-stereo PCM16 frames into the open push ring.
 * Returns the number of frames accepted (< frames if the ring is full). */
size_t hxa_feed_pcm(HxaService *s, const int16_t *stereo, size_t frames);

/* Push-ring diagnostics. `overflows` counts frames DROPPED on push (ring full):
 * dropped content makes audio run permanently EARLY vs video, so any nonzero
 * value is an A/V bug. `underruns` counts reads padded with silence (pushes
 * audio late). `fill_frames` is the current ring occupancy. Any may be NULL. */
void hxa_ring_stats(HxaService *s, uint32_t *underruns, uint32_t *overflows,
                    uint32_t *fill_frames);

/* Staging depths for a ring-fed (FMV) voice, in frames. These ARE output
 * latency: the player's streaming buffer plus the mixer-channel prefill sit
 * ahead of the playhead, so deep values put audio behind a near-instant video
 * path (left uncapped the channel runs to its full 743 ms). Defaults 2048 each
 * (~46 ms @44.1k). Raising them buys underrun cushion at the cost of A/V lag;
 * prefer deepening the upstream ring instead, which costs no lag. Takes effect
 * on the next voice opened. Clamped to the slot's buffer size. */
void hxa_set_lowlat(HxaService *s, size_t stream_frames, size_t channel_fill);

/* ---- render (the audio-pull entry point) ---- */

/* Pump active stream sources into their mixer channels, reap finished
 * one-shot SFX, then render `frames` of interleaved-stereo s16 into
 * `out` (out must hold frames*2 int16). Also feeds the FFT capture
 * window. This is what hx421_audio_pull calls. */
void hxa_render(HxaService *s, int16_t *out, uint32_t frames);

/* ---- drift sync passthrough (mixer PLL) ---- */

/* Report (internal_frames rendered, external_ticks elapsed) to the
 * mixer's drift estimator. Returns the current correction in PPM. */
int32_t hxa_observe_sync(HxaService *s, uint64_t internal_frames,
                         uint64_t external_ticks);

/* Set the mixer playback-rate correction directly, in PPM. */
void hxa_set_drift_ppm(HxaService *s, int32_t ppm);

/* ---- FFT band meter ---- */

/* Enable/disable the meter (off by default; zero cost while off). */
void hxa_fft_set_enabled(HxaService *s, bool on);

/* Refresh (non-RT) and copy up to `n` band levels (each 0..255) into
 * `out`. Returns the number written. 0 while disabled. */
uint32_t hxa_fft_bands(HxaService *s, uint32_t *out, uint32_t n);

/* ---- accessors (diagnostics) ---- */

AudioMixer *hxa_mixer(HxaService *s);
AudioPool  *hxa_pool(HxaService *s);

#endif /* HX421_SERVICE_H */
