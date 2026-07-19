/* ============================================================
 *  demo/player.c — HX-421 audio engine end-to-end demo
 *
 *  The M0 exit artifact: proves the owner (service.c) renders the whole
 *  engine end to end. It
 *    - creates the service,
 *    - loads an in-code sine SFX and a click SFX into the pool (and a
 *      WAV file too, if a path is given on the command line),
 *    - triggers them,
 *    - plays a streamed WAV as looping music (if a path is given),
 *    - runs hxa_render for ~2 seconds into an output buffer,
 *    - writes the result to out.wav via the engine's WAV sink backend,
 *    - and prints the FFT band meter over the mixed output.
 *
 *  Usage:
 *    player [sfx.wav] [music.wav]
 *  Both args optional. With no args it still produces a valid out.wav
 *  from the procedurally-generated SFX (a self-contained smoke test).
 *
 *  Public domain (CC0). No warranty.
 * ============================================================ */

#include "service.h"
#include "audio/sink.h"
#include "audio/audio_fft.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* ---- stdio-backed AudioFileReader (the platform file seam) ---- */

static void *rd_open(void *ctx, const char *path) {
    (void)ctx;
    return fopen(path, "rb");
}
static uint32_t rd_read(void *ctx, void *fh, void *dst, uint32_t bytes) {
    (void)ctx;
    return (uint32_t)fread(dst, 1, bytes, (FILE *)fh);
}
static bool rd_seek(void *ctx, void *fh, uint32_t off) {
    (void)ctx;
    return fseek((FILE *)fh, (long)off, SEEK_SET) == 0;
}
static void rd_close(void *ctx, void *fh) {
    (void)ctx;
    if (fh) fclose((FILE *)fh);
}

/* ---- procedural SFX generators (mono16) ---- */

/* A short decaying sine tone. Returns bytes written. */
static uint32_t gen_sine(int16_t *dst, uint32_t frames, uint32_t rate,
                         double freq, double amp) {
    for (uint32_t i = 0; i < frames; i++) {
        double t   = (double)i / (double)rate;
        double env = exp(-3.0 * t);                 /* ~decay */
        double v   = sin(2.0 * M_PI * freq * t) * env * amp;
        int32_t iv = (int32_t)lround(v * 32767.0);
        if (iv >  32767) iv =  32767;
        if (iv < -32768) iv = -32768;
        dst[i] = (int16_t)iv;
    }
    return frames * (uint32_t)sizeof(int16_t);
}

/* A short percussive click (noise burst, fast decay). */
static uint32_t gen_click(int16_t *dst, uint32_t frames, uint32_t rate,
                          double amp) {
    unsigned st = 0x1234567u;
    for (uint32_t i = 0; i < frames; i++) {
        double t   = (double)i / (double)rate;
        double env = exp(-40.0 * t);
        st = st * 1103515245u + 12345u;
        double n = ((double)((st >> 9) & 0xFFFF) / 32768.0) - 1.0;
        int32_t iv = (int32_t)lround(n * env * amp * 32767.0);
        if (iv >  32767) iv =  32767;
        if (iv < -32768) iv = -32768;
        dst[i] = (int16_t)iv;
    }
    return frames * (uint32_t)sizeof(int16_t);
}

int main(int argc, char **argv) {
    const uint32_t RATE   = 44100;
    const uint32_t FRAMES = RATE * 2;          /* ~2 seconds */
    const char *sfx_path   = (argc > 1) ? argv[1] : NULL;
    const char *music_path = (argc > 2) ? argv[2] : NULL;

    HxaConfig cfg = {
        .sample_rate  = RATE,
        .track_count  = 8,
        .pool_bytes   = 4u * 1024u * 1024u,
        .headroom_bits = 1,                    /* ~6 dB, avoid summed clip */
        .reader = { rd_open, rd_read, rd_seek, rd_close, NULL },
    };
    HxaService *svc = hxa_create(&cfg);
    if (!svc) { fprintf(stderr, "hxa_create failed\n"); return 1; }

    hxa_fft_set_enabled(svc, true);

    /* --- load two procedural SFX into the pool --- */
    uint32_t sine_frames  = RATE / 4;          /* 0.25 s */
    uint32_t click_frames = RATE / 20;         /* 0.05 s */
    int16_t *sine  = malloc(sine_frames  * sizeof(int16_t));
    int16_t *click = malloc(click_frames * sizeof(int16_t));
    uint32_t sine_bytes  = gen_sine(sine,  sine_frames,  RATE, 440.0, 0.6);
    uint32_t click_bytes = gen_click(click, click_frames, RATE, 0.7);

    AudioObjHandle h_sine  = hxa_load_sfx_pcm(svc, sine,  sine_bytes,  RATE);
    AudioObjHandle h_click = hxa_load_sfx_pcm(svc, click, click_bytes, RATE);
    free(sine);
    free(click);
    printf("loaded SFX: sine=0x%08x (%u frames)  click=0x%08x (%u frames)\n",
           h_sine, sine_frames, h_click, click_frames);

    AudioObjHandle h_wav = 0;
    if (sfx_path) {
        h_wav = hxa_load_sfx_wav(svc, sfx_path);
        printf("loaded WAV SFX '%s': 0x%08x\n", sfx_path, h_wav);
    }

    /* --- optional streamed music voice --- */
    AudioVoiceHandle music_voice = 0;
    if (music_path) {
        music_voice = hxa_play_stream_wav(svc, music_path);
        printf("stream music '%s': voice=0x%08x\n", music_path, music_voice);
    }

    /* --- open output sink --- */
    AudioSink sink;
    if (!audio_sink_open(&sink, "wav", "out.wav", RATE)) {
        fprintf(stderr, "audio_sink_open failed\n");
        hxa_destroy(svc);
        return 1;
    }

    /* --- render loop: trigger SFX at scheduled points, render blocks --- */
    const uint32_t BLK = 1024;
    int16_t *buf = malloc(BLK * 2 * sizeof(int16_t));

    double sum_sq = 0.0;
    uint32_t peak = 0;
    uint64_t total_samples = 0;   /* individual int16 samples (L+R) */
    uint32_t done = 0;
    uint32_t next_trigger = 0;
    int trig_idx = 0;

    /* Snapshot the FFT bands at the loudest block, so the meter shows a
     * signal-rich spectrum (sampling at t=end would catch decayed silence). */
    uint32_t best_bands[AUDIO_FFT_BANDS] = {0};
    uint32_t best_sum = 0, best_nb = 0;

    while (done < FRAMES) {
        uint32_t n = FRAMES - done;
        if (n > BLK) n = BLK;

        /* schedule a few triggers across the 2 s: sine, click, wav, sine */
        while (done >= next_trigger && trig_idx < 4) {
            switch (trig_idx) {
            case 0: hxa_trigger_sfx(svc, h_sine,  0x7FFF, 0);      break;
            case 1: hxa_trigger_sfx(svc, h_click, 0x7FFF, -16000); break;
            case 2:
                if (h_wav) hxa_trigger_sfx(svc, h_wav, 0x7FFF, 16000);
                else       hxa_trigger_sfx(svc, h_sine, 0x6000, 8000);
                break;
            case 3: hxa_trigger_sfx(svc, h_sine,  0x5000, 0);      break;
            }
            trig_idx++;
            next_trigger += RATE / 2;   /* every 0.5 s */
        }

        hxa_render(svc, buf, n);
        audio_sink_write(&sink, buf, n);

        /* poll the band meter this block. Snapshot the strongest reading
         * from the pure-sine window (before the 0.5 s click) so the meter
         * shows the tonal 440 Hz peak rather than the click's flat noise. */
        uint32_t bnow[AUDIO_FFT_BANDS];
        uint32_t nbnow = hxa_fft_bands(svc, bnow, AUDIO_FFT_BANDS);
        if (done < RATE / 2) {
            uint32_t bsum = 0;
            for (uint32_t i = 0; i < nbnow; i++) bsum += bnow[i];
            if (bsum > best_sum) {
                best_sum = bsum; best_nb = nbnow;
                memcpy(best_bands, bnow, sizeof(best_bands));
            }
        }

        for (uint32_t i = 0; i < n * 2; i++) {
            int32_t sv = buf[i];
            sum_sq += (double)sv * (double)sv;
            uint32_t a = (uint32_t)(sv < 0 ? -sv : sv);
            if (a > peak) peak = a;
        }
        total_samples += (uint64_t)n * 2;
        done += n;
    }

    audio_sink_close(&sink);
    free(buf);

    double rms = (total_samples > 0) ? sqrt(sum_sq / (double)total_samples) : 0.0;

    printf("\nrendered %u frames (%.2f s) @ %u Hz stereo s16\n",
           done, (double)done / RATE, RATE);
    printf("RMS = %.1f   peak = %u   (full scale 32767)\n", rms, peak);
    printf("FFT bands (%u, peak block): ", best_nb);
    for (uint32_t i = 0; i < best_nb; i++) printf("%3u ", best_bands[i]);
    printf("\n");

    if (music_voice) hxa_stop_voice(svc, music_voice);
    hxa_destroy(svc);

    if (rms < 1.0) {
        fprintf(stderr, "WARNING: output is essentially silent\n");
        return 2;
    }
    printf("out.wav written OK\n");
    return 0;
}
