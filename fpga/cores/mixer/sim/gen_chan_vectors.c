/* ============================================================
 *  gen_chan_vectors.c — golden output sequences for hx_chan.v
 *
 *  Drives the REAL shipping mixer (engine/audio/audio_mixer.c), one mono
 *  channel at unity volume / centre pan / zero headroom / 16-bit-signed
 *  output, so mixer_render's output IS the raw interpolated stream from
 *  produce_channel_sample. Dumps { step, interp, source[], expected_out[] }
 *  per case; tb_chan.v replays the same source and step through the RTL and
 *  checks the sequences match.
 *
 *  Testing against the assembled mixer (ring buffer, prime, advance and all)
 *  rather than a re-derived model is the point: it catches an RTL that is
 *  self-consistent but disagrees with what actually ships.
 *
 *  Build: gcc -I<engine> gen_chan_vectors.c audio_mixer.c ring_buffer.c
 *
 *  Public domain (CC0). No warranty.
 * ============================================================ */

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include "audio/audio_mixer.h"

#define OUT_RATE 44100
#define N_OUT    200
#define N_SRC    320

static uint32_t rng;
static uint32_t xr(void) { rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5; return rng; }

/* Same formula as the mixer's compute_step_q32_32, so the step handed to the
 * RTL is identical to the one the mixer computed internally. */
static uint64_t step_q32_32(int src_rate, int out_rate) {
    return ((uint64_t)src_rate << 32) / (uint64_t)out_rate;
}

static void run_case(FILE *f, int cubic, int src_rate) {
    int16_t src[N_SRC];
    rng = 0xC0FFEEu + (uint32_t)src_rate + (cubic ? 1u : 0u);
    for (int i = 0; i < N_SRC; ++i) src[i] = (int16_t)(xr() & 0xFFFF);

    MixerChannelConfig cfg = {
        .format             = MIXER_SRC_PCM16_MONO,
        .buffer_samples     = 512,
        .volume             = Q15_ONE,
        .source_sample_rate = src_rate,          /* != OUT_RATE -> needs_resample */
        .pan                = 0,                 /* centre: both gains Q15_ONE     */
        .loop               = false,
        .interp             = cubic ? MIXER_INTERP_CUBIC : MIXER_INTERP_LINEAR,
    };
    MixerOutputFormat ofmt = { 16, true, 16, 2 };   /* 16-bit signed stereo */

    AudioMixer *m = mixer_create(&cfg, 1, OUT_RATE, ofmt, /*headroom*/0);
    if (!m) { fprintf(stderr, "mixer_create failed\n"); exit(1); }

    mixer_write_channel(m, 0, src, N_SRC);
    mixer_channel_start(m, 0);

    int16_t out[N_OUT * 2];
    mixer_render(m, out, N_OUT);
    mixer_destroy(m);

    uint64_t step = step_q32_32(src_rate, OUT_RATE);
    fprintf(f, "CASE %d %08x %08x %d %d\n", cubic,
            (uint32_t)(step >> 32), (uint32_t)(step & 0xFFFFFFFFu), N_SRC, N_OUT);
    for (int i = 0; i < N_SRC; ++i) fprintf(f, "%04x\n", (uint16_t)src[i]);
    for (int i = 0; i < N_OUT; ++i) fprintf(f, "%04x\n", (uint16_t)out[i * 2]);  /* L */
}

int main(int argc, char **argv) {
    FILE *f = fopen(argc > 1 ? argv[1] : "chan_vectors.txt", "w");
    if (!f) { perror("open"); return 1; }

    /* varied steps: heavy upsample .. mild downsample, cubic and linear */
    static const int rates[] = { 8000, 11025, 22050, 32000, 48000 };
    for (int interp = 0; interp <= 1; ++interp)
        for (unsigned i = 0; i < sizeof rates / sizeof *rates; ++i)
            run_case(f, interp, rates[i]);

    fclose(f);
    return 0;
}
