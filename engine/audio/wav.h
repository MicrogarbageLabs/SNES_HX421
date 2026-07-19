/* ============================================================
 *  wav.h — canonical PCM WAV reader (for drop-in assets)
 *
 *  Parses a canonical PCM WAV from a memory buffer (e.g. the bytes read
 *  from a *.wav file). Zero-dep; the mirror of the RIFF writer in
 *  audio_sink_wav.c. Accepts 8- or 16-bit PCM, mono or stereo, any
 *  sample rate. The caller converts to the format the audio pool wants:
 *  samples are stored mono16 in the pool (wav_to_mono_pcm16) and the
 *  mixer promotes them to L==R stereo at playback (the mixer is
 *  full-stereo). For long streamed music the file-stream source emits
 *  stereo16 directly (wav_to_stereo_pcm16) — see audio_file_stream.h.
 *
 *  Split out of microgarbage's audio_sink.h so the WAV-reader surface is
 *  independent of the output-sink vtable (see sink.h).
 *
 *  Public domain (CC0). No warranty.
 * ============================================================ */

#ifndef HX421_AUDIO_WAV_H
#define HX421_AUDIO_WAV_H

#include <stdint.h>
#include <stddef.h>

typedef struct {
    uint16_t  format;        /* 1 = PCM (only PCM supported)        */
    uint16_t  channels;      /* 1 or 2                              */
    uint32_t  sample_rate;
    uint16_t  bits;          /* 8 or 16                             */
    const uint8_t *data;     /* points into the source buffer       */
    uint32_t  data_bytes;    /* size of the data chunk              */
} WavInfo;

typedef enum {
    WAV_OK = 0,
    WAV_ERR_TOO_SMALL,       /* buffer too small for a header       */
    WAV_ERR_BAD_MAGIC,       /* not RIFF/WAVE                       */
    WAV_ERR_NOT_PCM,         /* compressed / non-PCM format         */
    WAV_ERR_NO_DATA,         /* no data chunk found                 */
    WAV_ERR_UNSUPPORTED,     /* bits/channels we don't handle       */
} WavResult;

/* Parse a WAV from `buf` (`len` bytes). On success fills *out (its
 * `data` pointer aliases into `buf`, so keep `buf` alive). Walks the
 * chunk list to find fmt + data (tolerates extra chunks like LIST). */
WavResult wav_parse(const uint8_t *buf, size_t len, WavInfo *out);

/* Convert parsed WAV PCM into mono PCM16 (the pool sample format) in
 * `dst` (caller-allocated, room for `max_frames` int16). Downmixes
 * stereo->mono and promotes 8-bit->16-bit. Used by audio_load_wav to
 * stage a sample into the pool; the mixer promotes it to stereo at
 * playback. Returns frames written. */
uint32_t wav_to_mono_pcm16(const WavInfo *info, int16_t *dst,
                           uint32_t max_frames);

/* Decode + downmix-to-mono + linear-resample to dst_rate Hz. Used by
 * loaders that want every SFX in the pool to land at a fixed rate so
 * the per-channel mixer step can stay at the identity (1.0) — avoids
 * needing per-sample source-rate tracking inside the mixer/arbiter.
 * Returns frames written at dst_rate. */
uint32_t wav_to_mono_pcm16_resample(const WavInfo *info, int16_t *dst,
                                    uint32_t max_dst_frames,
                                    uint32_t dst_rate);

/* Convert parsed WAV PCM into INTERLEAVED STEREO PCM16 in `dst`
 * (caller-allocated, room for `max_frames * 2` int16). Stereo is
 * preserved as-is; mono is promoted to L==R (no downmix); 8-bit is
 * promoted to 16-bit. Returns frames written. */
uint32_t wav_to_stereo_pcm16(const WavInfo *info, int16_t *dst,
                             uint32_t max_frames);

#endif /* HX421_AUDIO_WAV_H */
