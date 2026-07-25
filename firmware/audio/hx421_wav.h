/* ============================================================
 *  hx421_wav.h — streaming-oriented WAV header parse (M4 side)
 *
 *  For streaming we only need the *header*: where the PCM data starts in
 *  the file and how long it is, plus the format, so the arbiter can
 *  offload the raw data region SD -> PSRAM ring. This wraps the canonical
 *  parser (engine/audio/audio_wav_read.c) applied to the first block of
 *  the file, and returns the data chunk's file byte-offset (not a
 *  pointer). The demo assets are mixer-native (16-bit interleaved stereo);
 *  hx421_wav_is_streamable() checks that so the offload can be a raw copy.
 *
 *  Public domain (CC0). No warranty.
 * ============================================================ */

#ifndef HX421_WAV_H
#define HX421_WAV_H

#include <stdint.h>
#include <stddef.h>

typedef struct {
    uint16_t channels;
    uint16_t bits;
    uint32_t sample_rate;
    uint32_t data_off;     /* file byte offset of the PCM data chunk    */
    uint32_t data_bytes;   /* PCM data length in bytes                  */
} Hx421WavInfo;

typedef enum {
    HX421_WAV_OK = 0,
    HX421_WAV_ERR_PARSE,      /* not a valid PCM WAV in the given block */
    HX421_WAV_ERR_HDR_SPAN,   /* data chunk not reached within `len`    */
} Hx421WavResult;

/* Parse the WAV header from `hdr` (the first `len` bytes of the file,
 * starting at file offset 0). Fills *out with the data chunk's FILE
 * offset + length and the format. `len` must be large enough to reach the
 * data chunk header (a few hundred bytes for canonical WAVs; 512–1024 is
 * safe). Returns HX421_WAV_OK on success. */
Hx421WavResult hx421_wav_parse_header(const uint8_t *hdr, size_t len,
                                      Hx421WavInfo *out);

/* True if `info` is directly streamable to the mixer with no conversion:
 * 16-bit, 2 channels. (Rate is the mixer's problem — the FPGA channel step
 * handles resample; but the demo uses the mixer's native rate.) */
int hx421_wav_is_streamable(const Hx421WavInfo *info);

#endif /* HX421_WAV_H */
