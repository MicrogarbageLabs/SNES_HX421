/* ============================================================
 *  hx421_cmd.h — FPGA command mailbox for the audio coprocessor.
 *
 *  The streamlined model: NO software coprocessor / VM, NO joypad-to-coprocessor
 *  path. The SNES reads its own joypad and mailboxes the FPGA only for FPGA
 *  commands. The SNES paths WAV files for loading (SFX -> the fragmentation-free
 *  block sound RAM; music -> a primed-head stream), triggers by slot, and reads
 *  back the FFT band levels to draw the meter natively (65816-side OAM). This
 *  module maps that mailbox onto the engine's hxa_* calls; it replaces the old
 *  DLL-emits-the-display "coprocessor kernel" path.
 *
 *  Mailbox (absolute offsets in the 64 KB cart window):
 *    $7000 CMD   $7001 ARG(slot)   $7002 GAIN(0..255)   $7003 PAN(128=center)
 *    $7010.. PATH (null-terminated, LOAD/PRIME)          $70FF DOORBELL(!=0 = go)
 *  Readback:
 *    $7900..$790F FFT bands (0..255)   $7910 STATUS(slot)   $7911 RESULT(0=ok)
 *
 *  Public domain (CC0). No warranty.
 * ============================================================ */
#ifndef HX421_CMD_H
#define HX421_CMD_H

#include <stdint.h>
#include "service.h"

#define HX421_CMD_CMD       0x7000u
#define HX421_CMD_ARG       0x7001u
#define HX421_CMD_GAIN      0x7002u
#define HX421_CMD_PAN       0x7003u
#define HX421_CMD_PATH      0x7010u
#define HX421_CMD_PATH_MAX  0x00E0u
#define HX421_CMD_DOORBELL  0x70FFu
#define HX421_CMD_FFT       0x7900u
#define HX421_CMD_FFT_N     16u
#define HX421_CMD_STATUS    0x7910u
#define HX421_CMD_RESULT    0x7911u

typedef enum {
    HX421_OP_NONE         = 0,
    HX421_OP_LOAD_SFX     = 1,   /* PATH -> block RAM; STATUS = slot */
    HX421_OP_PRIME_STREAM = 2,   /* PATH -> primed-head looping music */
    HX421_OP_TRIGGER_SFX  = 3,   /* ARG = slot; GAIN/PAN q from bytes */
    HX421_OP_STOP_MUSIC   = 4,
    HX421_OP_STOP_ALL     = 5
} Hx421CmdOp;

#define HX421_CMD_SFX_SLOTS 8
typedef struct {
    AudioObjHandle   sfx[HX421_CMD_SFX_SLOTS];
    int              nsfx;
    AudioVoiceHandle music;
} Hx421Cmd;

void hx421_cmd_init(Hx421Cmd *c);

/* Poll the doorbell; if a command is pending, run it against `s`, write
 * STATUS/RESULT, and clear the doorbell. Returns 1 if a command ran. */
int  hx421_cmd_poll(Hx421Cmd *c, HxaService *s, uint8_t *win);

/* Publish the current FFT band levels into the readback region (call per frame,
 * after hxa_render). */
void hx421_cmd_publish_fft(HxaService *s, uint8_t *win);

#endif /* HX421_CMD_H */
