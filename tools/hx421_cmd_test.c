/* hx421_cmd_test.c — host test for the FPGA command mailbox (engine/hx421_cmd).
 *
 * Drives the mailbox exactly as the native SNES demo would: write a LOAD_SFX
 * command with a file path, ring the doorbell, then TRIGGER_SFX by slot, render
 * audio, and confirm the SFX was heard (RMS rises) and the FFT bands populate.
 * Verifies the new native model's backend without bsnes. CC0. */

#include "service.h"
#include "hx421_cmd.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* reader seam (fopen-backed), same as demo/player.c */
static void *rd_open(void *c, const char *p) { (void)c; return fopen(p, "rb"); }
static uint32_t rd_read(void *c, void *f, void *d, uint32_t n) { (void)c; return (uint32_t)fread(d,1,n,(FILE*)f); }
static bool rd_seek(void *c, void *f, uint32_t o) { (void)c; return fseek((FILE*)f,(long)o,SEEK_SET)==0; }
static void rd_close(void *c, void *f) { (void)c; if (f) fclose((FILE*)f); }

/* write a minimal mono16 WAV (a 440 Hz decaying tone) */
static void write_wav(const char *path, uint32_t rate, uint32_t frames) {
    FILE *f = fopen(path, "wb");
    uint32_t data = frames * 2, riff = 36 + data;
    fwrite("RIFF",1,4,f); fwrite(&riff,4,1,f); fwrite("WAVE",1,4,f);
    fwrite("fmt ",1,4,f); uint32_t sz=16; fwrite(&sz,4,1,f);
    uint16_t fmt=1, ch=1, bps=16; uint32_t br=rate*2; uint16_t ba=2;
    fwrite(&fmt,2,1,f); fwrite(&ch,2,1,f); fwrite(&rate,4,1,f);
    fwrite(&br,4,1,f); fwrite(&ba,2,1,f); fwrite(&bps,2,1,f);
    fwrite("data",1,4,f); fwrite(&data,4,1,f);
    for (uint32_t i=0;i<frames;i++){
        double t=(double)i/rate, v=sin(2*M_PI*440*t)*exp(-3*t)*0.6;
        int16_t s=(int16_t)lround(v*32767); fwrite(&s,2,1,f);
    }
    fclose(f);
}

static double rms16(const int16_t *x, uint32_t n) {
    double a=0; for (uint32_t i=0;i<n;i++) a += (double)x[i]*x[i];
    return sqrt(a/n);
}

int main(void) {
    const uint32_t RATE=44100;
    write_wav("cmd_sfx.wav", RATE, RATE/4);   /* 0.25 s tone */

    HxaConfig cfg = { .sample_rate=RATE, .track_count=8, .pool_bytes=4u*1024*1024,
                      .headroom_bits=1, .reader={rd_open,rd_read,rd_seek,rd_close,NULL} };
    HxaService *s = hxa_create(&cfg);
    if (!s) { fprintf(stderr,"hxa_create failed\n"); return 1; }
    hxa_fft_set_enabled(s, true);

    static uint8_t win[64*1024];
    Hx421Cmd cmd; hx421_cmd_init(&cmd);
    int errs = 0;

    /* --- SNES: LOAD_SFX "cmd_sfx.wav" --- */
    win[HX421_CMD_CMD] = HX421_OP_LOAD_SFX;
    strcpy((char*)&win[HX421_CMD_PATH], "cmd_sfx.wav");
    win[HX421_CMD_DOORBELL] = 1;
    hx421_cmd_poll(&cmd, s, win);
    if (win[HX421_CMD_RESULT] != 0) { printf("FAIL: LOAD_SFX result=%d\n", win[HX421_CMD_RESULT]); errs++; }
    int slot = win[HX421_CMD_STATUS];
    printf("LOAD_SFX -> slot %d (result %d)\n", slot, win[HX421_CMD_RESULT]);

    /* render 0.1 s of silence (no trigger yet) -> should be quiet */
    int16_t buf[8820*2];
    hxa_render(s, buf, 8820);
    double quiet = rms16(buf, 8820*2);

    /* --- SNES: TRIGGER_SFX slot, full gain, center pan --- */
    win[HX421_CMD_CMD]=HX421_OP_TRIGGER_SFX; win[HX421_CMD_ARG]=(uint8_t)slot;
    win[HX421_CMD_GAIN]=255; win[HX421_CMD_PAN]=128; win[HX421_CMD_DOORBELL]=1;
    hx421_cmd_poll(&cmd, s, win);

    /* render 0.1 s -> the SFX should be audible now */
    hxa_render(s, buf, 8820);
    double loud = rms16(buf, 8820*2);
    hx421_cmd_publish_fft(s, win);

    printf("RMS: quiet=%.1f  after-trigger=%.1f\n", quiet, loud);
    if (!(loud > quiet + 100.0)) { printf("FAIL: trigger did not raise RMS\n"); errs++; }

    /* FFT readback should have some energy */
    int fftsum=0; for (int i=0;i<HX421_CMD_FFT_N;i++) fftsum += win[HX421_CMD_FFT+i];
    printf("FFT bands @$7900:");
    for (int i=0;i<HX421_CMD_FFT_N;i++) printf(" %d", win[HX421_CMD_FFT+i]);
    printf("  (sum=%d)\n", fftsum);
    if (fftsum == 0) { printf("FAIL: FFT bands empty after audio\n"); errs++; }

    /* --- SNES: PRIME_STREAM (reuse the WAV as looping music) --- */
    win[HX421_CMD_CMD]=HX421_OP_PRIME_STREAM;
    strcpy((char*)&win[HX421_CMD_PATH], "cmd_sfx.wav"); win[HX421_CMD_DOORBELL]=1;
    hx421_cmd_poll(&cmd, s, win);
    printf("PRIME_STREAM result=%d\n", win[HX421_CMD_RESULT]);
    if (win[HX421_CMD_RESULT]!=0) errs++;

    hxa_destroy(s);
    remove("cmd_sfx.wav");
    if (errs==0) printf("CMDTEST PASS: mailbox load/trigger/stream + FFT readback OK\n");
    else         printf("CMDTEST FAIL: %d errors\n", errs);
    return errs ? 1 : 0;
}
