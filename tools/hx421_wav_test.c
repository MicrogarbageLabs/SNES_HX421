/* host test for hx421_wav_parse_header — builds WAVs in memory and checks
 * the streaming header parse (data file-offset + extent + format).
 * Public domain (CC0). */

#include "../firmware/audio/hx421_wav.h"
#include <stdio.h>
#include <string.h>

static void w32(uint8_t *p, uint32_t v){ p[0]=v; p[1]=v>>8; p[2]=v>>16; p[3]=v>>24; }
static void w16(uint8_t *p, uint16_t v){ p[0]=v; p[1]=v>>8; }

/* Build a canonical PCM WAV, optionally injecting a `pad`-byte LIST chunk
 * before the data chunk. Returns total size; *data_off_out = data offset. */
static size_t build_wav(uint8_t *b, uint16_t ch, uint16_t bits, uint32_t rate,
                        uint32_t data_len, uint32_t pad, uint32_t *data_off_out) {
    uint8_t *p = b;
    uint32_t blockalign = ch * (bits/8);
    memcpy(p, "RIFF", 4); p+=4; w32(p, 0); p+=4;      /* riff size fixed later */
    memcpy(p, "WAVE", 4); p+=4;
    memcpy(p, "fmt ", 4); p+=4; w32(p, 16); p+=4;
    w16(p, 1); p+=2;                                   /* PCM */
    w16(p, ch); p+=2;
    w32(p, rate); p+=4;
    w32(p, rate*blockalign); p+=4;
    w16(p, blockalign); p+=2;
    w16(p, bits); p+=2;
    if (pad) {                                         /* optional LIST chunk */
        memcpy(p, "LIST", 4); p+=4; w32(p, pad); p+=4;
        memset(p, 0xAB, pad); p+=pad;
    }
    memcpy(p, "data", 4); p+=4; w32(p, data_len); p+=4;
    *data_off_out = (uint32_t)(p - b);
    memset(p, 0x11, data_len); p += data_len;
    w32(b+4, (uint32_t)(p - b) - 8);                   /* RIFF size */
    return (size_t)(p - b);
}

int main(void) {
    static uint8_t buf[4096];
    uint32_t exp_off; size_t len;
    Hx421WavInfo info;

    /* 1. canonical 44.1k 16-bit stereo -> data at offset 44 */
    len = build_wav(buf, 2, 16, 44100, 2000, 0, &exp_off);
    if (hx421_wav_parse_header(buf, len, &info) != HX421_WAV_OK) { printf("FAIL: parse canonical\n"); return 1; }
    if (info.data_off != exp_off || info.data_off != 44) { printf("FAIL: data_off %u (exp %u)\n", info.data_off, exp_off); return 1; }
    if (info.data_bytes != 2000 || info.channels != 2 || info.bits != 16 || info.sample_rate != 44100) { printf("FAIL: fields\n"); return 1; }
    if (!hx421_wav_is_streamable(&info)) { printf("FAIL: should be streamable\n"); return 1; }
    printf("canonical: off=%u bytes=%u %uHz %uch/%ub streamable=1 (ok)\n",
           info.data_off, info.data_bytes, info.sample_rate, info.channels, info.bits);

    /* 2. extra LIST chunk before data -> parser must walk past it */
    len = build_wav(buf, 2, 16, 44100, 1000, 40, &exp_off);
    if (hx421_wav_parse_header(buf, len, &info) != HX421_WAV_OK) { printf("FAIL: parse w/ LIST\n"); return 1; }
    if (info.data_off != exp_off) { printf("FAIL: LIST data_off %u (exp %u)\n", info.data_off, exp_off); return 1; }
    printf("with LIST chunk: data_off=%u (walked past extra chunk) (ok)\n", info.data_off);

    /* 3. mono 8-bit -> parses but not directly streamable */
    len = build_wav(buf, 1, 8, 22050, 500, 0, &exp_off);
    if (hx421_wav_parse_header(buf, len, &info) != HX421_WAV_OK) { printf("FAIL: parse mono8\n"); return 1; }
    if (hx421_wav_is_streamable(&info)) { printf("FAIL: mono8 must not be streamable\n"); return 1; }
    printf("mono/8-bit: parsed, not streamable (would need offline convert) (ok)\n");

    /* 4. truncated header (data chunk not reached) -> HDR_SPAN */
    len = build_wav(buf, 2, 16, 44100, 2000, 0, &exp_off);
    if (hx421_wav_parse_header(buf, 40, &info) != HX421_WAV_ERR_HDR_SPAN) { printf("FAIL: truncated should be HDR_SPAN\n"); return 1; }
    printf("truncated header: HDR_SPAN reported (read more + retry) (ok)\n");

    printf("RESULT: PASS - streaming WAV header parse\n");
    return 0;
}
