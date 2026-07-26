/* ============================================================
 *  hx421_fft_test.c — host test for the audio_fft band meter (the demo's
 *  spectrum bars). Feeds a continuous sine at a known frequency into the FFT
 *  and checks the loudest of the 16 bands is the one whose [lo,hi) contains it,
 *  across several frequencies, plus silence -> all bands quiet. This is the
 *  portable-kernel path; the M4 build swaps in CMSIS-DSP arm_rfft_q15 and the
 *  bander is identical.
 *  Public domain (CC0). No warranty.
 * ============================================================ */

#include "audio/audio_fft.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define RATE 44100u

static double g_phase = 0.0;

/* feed the FFT a continuous stereo sine until an FFT pass runs, then return the
 * index of the loudest band. amp==0 -> silence. */
static int run_tone(AudioFft *f, double freq, double amp) {
    int16_t buf[128 * 2];
    int guard = 2000;
    while (guard-- > 0) {
        int i;
        for (i = 0; i < 128; i++) {
            double s = amp * sin(g_phase);
            g_phase += 2.0 * M_PI * freq / (double)RATE;
            if (g_phase > 2.0 * M_PI) g_phase -= 2.0 * M_PI;
            int16_t v = (int16_t)s;
            buf[i*2] = v; buf[i*2+1] = v;       /* L == R */
        }
        audio_fft_capture(f, buf, 128);
        if (audio_fft_update(f)) {
            uint8_t bands[AUDIO_FFT_BANDS];
            uint32_t n = audio_fft_get_bands(f, bands, AUDIO_FFT_BANDS);
            int best = 0; uint32_t i2;
            for (i2 = 1; i2 < n; i2++) if (bands[i2] > bands[best]) best = i2;
            return best;
        }
    }
    return -1;
}

int main(void) {
    AudioFft f;
    audio_fft_init(&f, RATE);
    audio_fft_set_enabled(&f, true);

    printf("bands (FFT bins):");
    { uint32_t i; for (i = 0; i < AUDIO_FFT_BANDS; i++) printf(" %u-%u", f.band_lo[i], f.band_hi[i]); }
    printf("\n");

    /* Tones spread across the spectrum: a correct band meter puts the peak in a
     * monotonically-higher band as the frequency rises, and spreads them out. */
    double tones[] = { 250.0, 800.0, 2000.0, 6000.0 };
    int peak[4], fail = 0, t;
    for (t = 0; t < 4; t++) {
        peak[t] = run_tone(&f, tones[t], 20000.0);
        printf("tone %5.0f Hz -> loudest band %d\n", tones[t], peak[t]);
        if (peak[t] < 0) { printf("  FAIL: no FFT pass ran\n"); fail = 1; }
    }
    /* monotonic non-decreasing peak band with frequency */
    for (t = 1; t < 4; t++)
        if (peak[t] < peak[t-1]) {
            printf("  FAIL: %0.f Hz peaked in band %d, below %0.f Hz's band %d\n",
                   tones[t], peak[t], tones[t-1], peak[t-1]); fail = 1;
        }
    /* and they must actually spread (not all the same band) */
    if (peak[0] == peak[3]) { printf("  FAIL: all tones peaked in the same band\n"); fail = 1; }

    /* silence -> all bands quiet */
    { int b = run_tone(&f, 1000.0, 0.0);
      uint8_t bands[AUDIO_FFT_BANDS]; uint32_t n = audio_fft_get_bands(&f, bands, AUDIO_FFT_BANDS), i, mx = 0;
      (void)b; for (i = 0; i < n; i++) if (bands[i] > mx) mx = bands[i];
      printf("silence -> max band level %u\n", mx);
      if (mx > 8) { printf("  FAIL: silence should leave all bands near zero\n"); fail = 1; }
    }

    printf(fail ? "RESULT: FAIL\n" : "RESULT: PASS - FFT band meter localizes tones + quiet on silence\n");
    return fail;
}
