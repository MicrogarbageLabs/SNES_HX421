/* ============================================================
 *  hx421_mode.h — the STM32 "HX-421 mode": two WAV music streams from SD,
 *  streamed into PSRAM rings for the FPGA mixer, joystick-controlled.
 *
 *  This is the firmware glue that binds the host-tested arbiter (hx421_stream)
 *  and WAV parser (hx421_wav) to the sd2snes/mk3 platform seams:
 *    - SD-DMA offload SD->PSRAM  (set_mcu_addr + ff_sd_offload + f_read)
 *    - the mixer's drain pointer (FPGA status read; time-estimated until the
 *      FPGA exposes it)
 *    - joystick input            (snes_get_snes_cmd)
 *
 *  Hooked in main.c's game-run loop like MSU-1: `while(!hx421_mode_loop());`.
 *  Public domain (CC0). No warranty.
 * ============================================================ */

#ifndef HX421_MODE_H
#define HX421_MODE_H

#include <stdint.h>

/* Open music1.wav / music2.wav, parse them, lay out the PSRAM rings, and prime.
 * Returns 0 on success, non-zero if the assets are missing/unsupported. */
int  hx421_mode_init(void);

/* Cooperative service — call repeatedly from the run loop. Refills the emptiest
 * ring (one SD-DMA offload per call), applies joystick start/stop, and keeps the
 * FPGA mixer channels configured. Returns non-zero to exit the mode (reset). */
int  hx421_mode_loop(void);

#endif /* HX421_MODE_H */
