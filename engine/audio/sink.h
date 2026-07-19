/* ============================================================
 *  sink.h — audio output backend seam
 *
 *  A tiny swappable seam between "the engine renders mixed PCM" and
 *  "something consumes it" — the desktop analogue of the hardware's
 *  SAI+DMA->DAC path. Backends implement the vtable; callers use the
 *  AudioSink wrappers and never touch the vtable directly.
 *
 *  The bedrock backend is the WAV-file writer (audio_sink_wav.c): fully
 *  testable without any sound hardware — render, dump, read back,
 *  compare. Live output backends (per-DAC on hardware, per audio device
 *  on PC) are provided by the firmware/ and host/ layers, not here.
 *
 *  Format is fixed to what the engine produces: interleaved signed
 *  16-bit stereo at the engine sample rate.
 *
 *  Split out of microgarbage's audio_sink.h; the WAV *reader* decls now
 *  live in wav.h.
 *
 *  Public domain (CC0). No warranty.
 * ============================================================ */

#ifndef HX421_AUDIO_SINK_H
#define HX421_AUDIO_SINK_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

/* The output format is fixed: 16-bit signed, 2 channels (stereo). */
#define AUDIO_SINK_CHANNELS  2
#define AUDIO_SINK_BITS      16

typedef struct AudioSink AudioSink;

/* Backend vtable. A backend implements these; callers use the
 * AudioSink wrappers below and never touch the vtable directly. */
typedef struct {
    const char *name;
    /* Open the device/file for `sample_rate` Hz stereo s16. Returns a
     * backend context pointer, or NULL on failure. */
    void *(*open)(uint32_t sample_rate);
    /* Write `frames` interleaved-stereo s16 frames (frames*2 samples).
     * Returns the number of frames accepted (may be < frames if a
     * device buffer is momentarily full; callers retry). Returns -1 on
     * error. */
    int   (*write)(void *ctx, const int16_t *interleaved, uint32_t frames);
    /* Flush any buffered audio and close. */
    void  (*close)(void *ctx);
} AudioSinkBackend;

struct AudioSink {
    const AudioSinkBackend *backend;
    void                   *ctx;
    uint32_t                sample_rate;
};

/* Open a sink using the named backend ("wav"). For the WAV backend,
 * `path` is the output file; for live backends `path` is ignored (may
 * be NULL). Returns false on failure. */
bool audio_sink_open(AudioSink *sink, const char *backend_name,
                     const char *path, uint32_t sample_rate);

/* Write interleaved stereo s16 frames. Returns frames accepted or -1. */
int  audio_sink_write(AudioSink *sink, const int16_t *interleaved,
                      uint32_t frames);

/* Flush + close. */
void audio_sink_close(AudioSink *sink);

/* ---- backends (exposed so tests can use them directly) ---- */
extern const AudioSinkBackend audio_sink_wav;       /* always available */

#endif /* HX421_AUDIO_SINK_H */
