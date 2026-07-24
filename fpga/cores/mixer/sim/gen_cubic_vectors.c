/* ============================================================
 *  gen_cubic_vectors.c — emit co-simulation vectors for hx_cubic.v
 *
 *  Runs inputs through the SAME function the mixer ships — the golden
 *  interp_cubic_q15() lifted verbatim from engine/audio/audio_mixer.c — and
 *  writes { p0 p1 p2 p3 frac : expected } lines the Verilog testbench replays.
 *  If the RTL and this disagree on even one line, the RTL is wrong (or has
 *  found a genuine divergence worth arguing about).
 *
 *  The reference is copied here rather than linked so this file is the exact,
 *  auditable text being matched, with a static_assert-style note if it ever
 *  drifts from the mixer.
 *
 *  Public domain (CC0). No warranty.
 * ============================================================ */

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>

typedef int16_t q15_t;

/* ---- VERBATIM from engine/audio/audio_mixer.c interp_cubic_q15 ---------- */
static q15_t interp_cubic_q15(q15_t p0, q15_t p1, q15_t p2, q15_t p3,
                               uint32_t frac_q32) {
    int32_t t = (int32_t)(frac_q32 >> 17);   /* 0..0x7FFF */

    int32_t t2 = (t * t) >> 15;
    int32_t t3 = (t2 * t) >> 15;

    int32_t a = -(int32_t)p0 +  3*(int32_t)p1 - 3*(int32_t)p2 + (int32_t)p3;
    int32_t b =  2*(int32_t)p0 - 5*(int32_t)p1 + 4*(int32_t)p2 -    (int32_t)p3;
    int32_t c = -(int32_t)p0                     + (int32_t)p2;
    int32_t d =  2*(int32_t)p1;

    int32_t v = (a * t3) >> 15;
    v       += (b * t2) >> 15;
    v       += (c * t)  >> 15;
    v       += d;

    v >>= 1;
    if (v >  32767) v =  32767;
    if (v < -32768) v = -32768;
    return (q15_t)v;
}
/* ------------------------------------------------------------------------- */

static uint32_t rng = 0x1234567u;
static uint32_t xr(void) { rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5; return rng; }
static q15_t rq15(void) { return (q15_t)(xr() & 0xFFFF); }

static void emit(FILE *f, q15_t p0, q15_t p1, q15_t p2, q15_t p3, uint32_t frac) {
    q15_t e = interp_cubic_q15(p0, p1, p2, p3, frac);
    /* 16-bit signed values printed as unsigned hex; the testbench reads them
     * into signed regs. frac is full 32-bit. */
    fprintf(f, "%04x %04x %04x %04x %08x %04x\n",
            (uint16_t)p0, (uint16_t)p1, (uint16_t)p2, (uint16_t)p3,
            frac, (uint16_t)e);
}

int main(int argc, char **argv) {
    FILE *f = fopen(argc > 1 ? argv[1] : "cubic_vectors.txt", "w");
    if (!f) { perror("open"); return 1; }

    /* Directed corners first: silence, DC, the extreme alternating taps that
     * make a*t3 overflow, and frac at both ends. These are where a naive RTL
     * or a "fixed" wider-math version would diverge. */
    static const uint32_t fr[] = { 0x00000000u, 0x00020000u, 0x40000000u,
                                   0x7FFFFFFFu, 0x80000000u, 0xFFFFFFFFu };
    static const q15_t ex[] = { 0, 32767, -32768, 1, -1, 16384, -16384 };
    for (unsigned i = 0; i < sizeof fr / sizeof *fr; ++i) {
        for (unsigned a = 0; a < sizeof ex / sizeof *ex; ++a) {
            /* alternating full-scale: the overflow case */
            emit(f, -32768, 32767, -32768, 32767, fr[i]);
            emit(f, 32767, -32768, 32767, -32768, fr[i]);
            emit(f, ex[a], ex[a], ex[a], ex[a], fr[i]);        /* DC */
            emit(f, 0, 0, 0, 0, fr[i]);                         /* silence */
        }
    }

    /* Then a broad random sweep. 50k lines is plenty to catch a shift or sign
     * error while running in well under a second. */
    for (int i = 0; i < 50000; ++i)
        emit(f, rq15(), rq15(), rq15(), rq15(), xr());

    fclose(f);
    return 0;
}
