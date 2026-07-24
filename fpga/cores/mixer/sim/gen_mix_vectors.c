/* ============================================================
 *  gen_mix_vectors.c — golden 8-channel render for hx_mixer.v
 *
 *  Drives the real mixer_render with a mixed scene — varied rates, cubic and
 *  linear, per-channel volume and pan, some muted, some inactive — and dumps
 *  config + per-channel source + the finalized stereo output. tb_mix.v
 *  configures the RTL identically, renders, and compares full output frames.
 *
 *  Build: gcc -I<engine> gen_mix_vectors.c audio_mixer.c ring_buffer.c
 *  Public domain (CC0). No warranty.
 * ============================================================ */

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include "audio/audio_mixer.h"

#define OUT_RATE  44100
#define N         8
#define NOUT      300
#define NSRC      512        /* samples written to a non-loop channel          */
#define LLEN      64         /* loop buffer length: small, so loops wrap often */
#define STRIDE    1024
#define HEADROOM  3

typedef int16_t q15_t;

static uint32_t rng;
static uint32_t xr(void){ rng^=rng<<13; rng^=rng>>17; rng^=rng<<5; return rng; }

static uint64_t step_q32_32(int s, int o){ return ((uint64_t)s<<32)/(uint64_t)o; }

/* verbatim compute_pan_gains */
static void pan_gains(q15_t pan, q15_t *l, q15_t *r){
    if (pan >= 0){ *l=(q15_t)(Q15_ONE - pan); *r=Q15_ONE; }
    else         { *l=Q15_ONE; *r=(q15_t)(Q15_ONE + pan); }
}

/* the scene: {rate, cubic, active, muted, loop, volume, pan} per channel */
static const struct { int rate, cubic, active, muted, loop; int vol, pan; } scene[N] = {
    { 44100, 1, 1, 0, 0, Q15_ONE,     0        },  /* EXACT 1:1 -> C fast path   */
    { 22050, 1, 1, 0, 1, 24000,       0        },  /* LOOP, half-rate, cubic     */
    { 11025, 0, 1, 0, 0, Q15_ONE,    -20000    },  /* linear SFX, panned left    */
    {  8000, 1, 1, 0, 1, 30000,       28000    },  /* LOOP, upsampled, panned R  */
    { 32000, 0, 1, 0, 1, 16000,       10000    },  /* LOOP, linear, mild right   */
    { 48000, 1, 1, 1, 0, Q15_ONE,     0        },  /* MUTED (produces, not mixed)*/
    { 44101, 1, 0, 0, 0, Q15_ONE,     0        },  /* INACTIVE (skipped)         */
    { 16000, 1, 1, 0, 0, 20000,      -30000    },  /* upsampled, panned left     */
};

int main(int argc, char **argv){
    FILE *f = fopen(argc>1?argv[1]:"mix_vectors.txt","w");
    if(!f){ perror("open"); return 1; }

    int16_t src[N][NSRC];
    int nwrite[N];
    MixerChannelConfig cfg[N];
    for(int i=0;i<N;i++){
        nwrite[i] = scene[i].loop ? LLEN : NSRC;
        rng = 0xABCD00u + (uint32_t)scene[i].rate + (uint32_t)i;
        for(int k=0;k<nwrite[i];k++) src[i][k]=(int16_t)(xr()&0xFFFF);
        cfg[i].format=MIXER_SRC_PCM16_MONO;
        cfg[i].buffer_samples=1024;
        cfg[i].volume=(q15_t)scene[i].vol;
        cfg[i].source_sample_rate=scene[i].rate;
        cfg[i].pan=(q15_t)scene[i].pan;
        cfg[i].loop=scene[i].loop?true:false;
        cfg[i].interp=scene[i].cubic?MIXER_INTERP_CUBIC:MIXER_INTERP_LINEAR;
    }
    MixerOutputFormat ofmt={16,true,16,2};
    AudioMixer *m=mixer_create(cfg,N,OUT_RATE,ofmt,HEADROOM);
    if(!m){ fprintf(stderr,"create failed\n"); return 1; }

    for(int i=0;i<N;i++){
        mixer_write_channel(m,i,src[i],nwrite[i]);
        if(scene[i].active){ mixer_channel_start(m,i); }
        if(scene[i].muted)   mixer_mute(m,i,true);
    }

    int16_t out[NOUT*2];
    mixer_render(m,out,NOUT);
    mixer_destroy(m);

    /* header: 16-bit signed finalize params (out_shift 0, offset 0, +-q15) */
    fprintf(f,"HDR %d %d %08x %08x %08x %d %d %d\n",
            HEADROOM, 0, 0u, (uint32_t)(-32768), (uint32_t)32767, N, NOUT, STRIDE);

    for(int i=0;i<N;i++){
        q15_t pl,pr; pan_gains((q15_t)scene[i].pan,&pl,&pr);
        uint64_t st = step_q32_32(scene[i].rate, OUT_RATE);
        /* loop_len == samples written (the C's rb.count) */
        int llen = scene[i].loop ? nwrite[i] : 1;
        fprintf(f,"CH %d %d %d %d %d %d %08x %08x %04x %04x %04x\n",
                i, scene[i].cubic, scene[i].active, scene[i].muted, scene[i].loop, llen,
                (uint32_t)(st>>32), (uint32_t)(st&0xFFFFFFFFu),
                (uint16_t)scene[i].vol, (uint16_t)pl, (uint16_t)pr);
    }
    for(int i=0;i<N;i++){
        fprintf(f,"SRC %d %d\n", i, nwrite[i]);
        for(int k=0;k<nwrite[i];k++) fprintf(f,"%04x\n",(uint16_t)src[i][k]);
    }
    fprintf(f,"OUT\n");
    for(int i=0;i<NOUT;i++) fprintf(f,"%04x %04x\n",(uint16_t)out[i*2],(uint16_t)out[i*2+1]);

    fclose(f);
    return 0;
}
