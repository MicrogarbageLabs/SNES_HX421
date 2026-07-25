/* ============================================================
 *  hx421_wav.c — streaming WAV header parse (see .h)
 *  Public domain (CC0). No warranty.
 * ============================================================ */

#include "hx421_wav.h"
#include "wav.h"   /* engine/audio/wav.h — canonical parser (wav_parse) */

Hx421WavResult hx421_wav_parse_header(const uint8_t *hdr, size_t len,
                                      Hx421WavInfo *out) {
    WavInfo wi;
    WavResult r = wav_parse(hdr, len, &wi);
    if (r != WAV_OK) {
        /* WAV_ERR_NO_DATA within a too-short block reads as "header didn't
         * span the data chunk" — a distinct, actionable case for the caller
         * (read more of the file and retry). */
        return (r == WAV_ERR_NO_DATA) ? HX421_WAV_ERR_HDR_SPAN
                                      : HX421_WAV_ERR_PARSE;
    }
    /* wi.data aliases into hdr; hdr starts at file offset 0, so the pointer
     * difference is the data chunk's file byte-offset. */
    out->channels    = wi.channels;
    out->bits        = wi.bits;
    out->sample_rate = wi.sample_rate;
    out->data_off    = (uint32_t)(wi.data - hdr);
    out->data_bytes  = wi.data_bytes;
    return HX421_WAV_OK;
}

int hx421_wav_is_streamable(const Hx421WavInfo *info) {
    return info->bits == 16 && info->channels == 2;
}
