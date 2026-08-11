/* hx421_cmd.c — FPGA command mailbox -> engine dispatch. See hx421_cmd.h. */
#include "hx421_cmd.h"
#include <string.h>

void hx421_cmd_init(Hx421Cmd *c) { memset(c, 0, sizeof *c); }

static int32_t q15_gain(uint8_t b) { return ((int32_t)b * 32767) / 255; }
static int32_t q15_pan (uint8_t b) { return (((int32_t)b - 128) * 32767) / 128; }

int hx421_cmd_poll(Hx421Cmd *c, HxaService *s, uint8_t *win) {
    if (!win[HX421_CMD_DOORBELL]) return 0;

    uint8_t op  = win[HX421_CMD_CMD];
    uint8_t arg = win[HX421_CMD_ARG];
    win[HX421_CMD_RESULT] = 0;

    switch (op) {
    case HX421_OP_LOAD_SFX: {
        const char *path = (const char *)&win[HX421_CMD_PATH];
        AudioObjHandle h = hxa_load_sfx_wav(s, path);
        if (h && c->nsfx < HX421_CMD_SFX_SLOTS) {
            int slot = c->nsfx++;
            c->sfx[slot] = h;
            win[HX421_CMD_STATUS] = (uint8_t)slot;
        } else {
            win[HX421_CMD_RESULT] = 1;
        }
        break;
    }
    case HX421_OP_PRIME_STREAM: {
        const char *path = (const char *)&win[HX421_CMD_PATH];
        if (c->music) hxa_stop_voice(s, c->music);
        c->music = hxa_play_stream_wav(s, path);
        if (!c->music) win[HX421_CMD_RESULT] = 1;
        break;
    }
    case HX421_OP_TRIGGER_SFX:
        if (arg < c->nsfx && c->sfx[arg])
            hxa_trigger_sfx(s, c->sfx[arg],
                            q15_gain(win[HX421_CMD_GAIN]), q15_pan(win[HX421_CMD_PAN]));
        else
            win[HX421_CMD_RESULT] = 1;
        break;
    case HX421_OP_STOP_MUSIC:
        if (c->music) { hxa_stop_voice(s, c->music); c->music = 0; }
        break;
    case HX421_OP_STOP_ALL:
        if (c->music) { hxa_stop_voice(s, c->music); c->music = 0; }
        break;
    default:
        win[HX421_CMD_RESULT] = 2;   /* unknown op */
        break;
    }

    win[HX421_CMD_DOORBELL] = 0;      /* ack: command consumed */
    return 1;
}

void hx421_cmd_publish_fft(HxaService *s, uint8_t *win) {
    uint32_t bands[HX421_CMD_FFT_N] = {0};
    uint32_t n = hxa_fft_bands(s, bands, HX421_CMD_FFT_N);
    for (uint32_t i = 0; i < HX421_CMD_FFT_N; i++)
        win[HX421_CMD_FFT + i] = (i < n) ? (uint8_t)(bands[i] > 255 ? 255 : bands[i]) : 0;
}
