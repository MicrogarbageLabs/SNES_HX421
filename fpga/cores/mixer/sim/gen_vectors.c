/* ============================================================
 *  gen_vectors.c — emit co-simulation vectors for the mixer primitives
 *
 *  Each golden function below is VERBATIM from engine/audio/audio_mixer.c or
 *  engine/math/fixed_point.h. It runs random + directed inputs through the C
 *  and writes { inputs : expected } lines the matching testbench replays
 *  through the RTL. Any disagreement is a real divergence.
 *
 *  Emits: cubic_vectors.txt, lerp_vectors.txt, scale_vectors.txt,
 *         finalize_vectors.txt
 *
 *  Public domain (CC0). No warranty.
 * ============================================================ */

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>

typedef int16_t q15_t;

/* ---- VERBATIM: interp_cubic_q15 ---------------------------------------- */
static q15_t interp_cubic_q15(q15_t p0, q15_t p1, q15_t p2, q15_t p3, uint32_t frac_q32) {
    int32_t t = (int32_t)(frac_q32 >> 17);
    int32_t t2 = (t * t) >> 15;
    int32_t t3 = (t2 * t) >> 15;
    int32_t a = -(int32_t)p0 +  3*(int32_t)p1 - 3*(int32_t)p2 + (int32_t)p3;
    int32_t b =  2*(int32_t)p0 - 5*(int32_t)p1 + 4*(int32_t)p2 -    (int32_t)p3;
    int32_t c = -(int32_t)p0                     + (int32_t)p2;
    int32_t d =  2*(int32_t)p1;
    int32_t v = (a * t3) >> 15;
    v += (b * t2) >> 15;
    v += (c * t)  >> 15;
    v += d;
    v >>= 1;
    if (v >  32767) v =  32767;
    if (v < -32768) v = -32768;
    return (q15_t)v;
}

/* ---- VERBATIM: interp_linear_q15 --------------------------------------- */
static q15_t interp_linear_q15(q15_t a, q15_t b, uint32_t frac_q32) {
    uint32_t f = frac_q32 >> 16;
    int32_t diff = (int32_t)b - (int32_t)a;
    int32_t scaled = (diff * (int32_t)f) >> 16;
    return (q15_t)((int32_t)a + scaled);
}

/* ---- VERBATIM: q15_sat_mul (+ fx_sat16_) ------------------------------- */
static q15_t fx_sat16_(int64_t v) {
    if (v >  32767) return  32767;
    if (v < -32768) return -32768;
    return (q15_t)v;
}
static q15_t q15_sat_mul(q15_t a, q15_t b) {
    return fx_sat16_(((int32_t)a * (int32_t)b) >> 15);
}

/* ---- VERBATIM: finalize_output ----------------------------------------- */
static int32_t finalize_output(int32_t accum, uint8_t headroom_bits, uint8_t out_shift,
                               int32_t out_offset, int32_t out_min, int32_t out_max) {
    int32_t v = accum >> headroom_bits;
    if (v >  32767) v =  32767;
    if (v < -32768) v = -32768;
    v = (v >> out_shift) + out_offset;
    if (v > out_max) v = out_max;
    if (v < out_min) v = out_min;
    return v;
}

/* ------------------------------------------------------------------------- */

static uint32_t rng = 0x1234567u;
static uint32_t xr(void) { rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5; return rng; }
static q15_t rq15(void) { return (q15_t)(xr() & 0xFFFF); }

static void gen_cubic(void) {
    FILE *f = fopen("cubic_vectors.txt", "w");
    static const uint32_t fr[] = {0,0x20000u,0x40000000u,0x7FFFFFFFu,0x80000000u,0xFFFFFFFFu};
    for (unsigned i = 0; i < 6; ++i) {
        fprintf(f, "%04x %04x %04x %04x %08x %04x\n", 0x8000,0x7FFF,0x8000,0x7FFF, fr[i],
                (uint16_t)interp_cubic_q15(-32768,32767,-32768,32767,fr[i]));
        fprintf(f, "%04x %04x %04x %04x %08x %04x\n", 0,0,0,0, fr[i],
                (uint16_t)interp_cubic_q15(0,0,0,0,fr[i]));
    }
    for (int i = 0; i < 50000; ++i) {
        q15_t p0=rq15(),p1=rq15(),p2=rq15(),p3=rq15(); uint32_t fc=xr();
        fprintf(f, "%04x %04x %04x %04x %08x %04x\n",(uint16_t)p0,(uint16_t)p1,(uint16_t)p2,
                (uint16_t)p3, fc, (uint16_t)interp_cubic_q15(p0,p1,p2,p3,fc));
    }
    fclose(f);
}

static void gen_lerp(void) {
    FILE *f = fopen("lerp_vectors.txt", "w");
    static const uint32_t fr[] = {0,0x8000u,0x40000000u,0x80000000u,0xFFFF0000u,0xFFFFFFFFu};
    for (unsigned i = 0; i < 6; ++i)
        fprintf(f, "%04x %04x %08x %04x\n", 0x8000, 0x7FFF, fr[i],
                (uint16_t)interp_linear_q15(-32768,32767,fr[i]));
    for (int i = 0; i < 50000; ++i) {
        q15_t a=rq15(),b=rq15(); uint32_t fc=xr();
        fprintf(f, "%04x %04x %08x %04x\n",(uint16_t)a,(uint16_t)b,fc,
                (uint16_t)interp_linear_q15(a,b,fc));
    }
    fclose(f);
}

static void gen_scale(void) {
    FILE *f = fopen("scale_vectors.txt", "w");
    /* the one overflow case: -32768 * -32768 -> +32768, must saturate to 32767 */
    fprintf(f, "%04x %04x %04x\n", 0x8000, 0x8000, (uint16_t)q15_sat_mul(-32768,-32768));
    for (int i = 0; i < 50000; ++i) {
        q15_t a=rq15(),b=rq15();
        fprintf(f, "%04x %04x %04x\n",(uint16_t)a,(uint16_t)b,(uint16_t)q15_sat_mul(a,b));
    }
    fclose(f);
}

static void gen_finalize(void) {
    FILE *f = fopen("finalize_vectors.txt", "w");
    /* realistic output-format tuples: {out_shift, out_offset, out_min, out_max}
     *   16-bit signed  (the DAC): 0, 0, -32768, 32767
     *   12-bit unsigned:          3, 2048, 0, 4095
     *   8-bit unsigned:           7, 128, 0, 255
     *   8-bit signed:             7, 0, -128, 127 */
    struct { uint8_t sh; int32_t off, mn, mx; } fmt[4] = {
        {0, 0, -32768, 32767}, {3, 2048, 0, 4095}, {7, 128, 0, 255}, {7, 0, -128, 127}
    };
    for (int i = 0; i < 50000; ++i) {
        /* accum spans well beyond q15 so the headroom-shift + both saturations
         * are all exercised. */
        int32_t accum = (int32_t)xr() >> (xr() & 7);
        uint8_t hr = xr() % 7;                 /* 0..6 */
        int k = xr() & 3;
        int32_t e = finalize_output(accum, hr, fmt[k].sh, fmt[k].off, fmt[k].mn, fmt[k].mx);
        fprintf(f, "%08x %x %x %08x %08x %08x %08x\n",
                (uint32_t)accum, hr, fmt[k].sh,
                (uint32_t)fmt[k].off, (uint32_t)fmt[k].mn, (uint32_t)fmt[k].mx, (uint32_t)e);
    }
    fclose(f);
}

int main(void) {
    gen_cubic();
    gen_lerp();
    gen_scale();
    gen_finalize();
    return 0;
}
