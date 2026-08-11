/* ============================================================
 *  hx421_media_fw.h — sd2snes/mk3 binding for the HX-421 media service.
 *
 *  Compiled INSIDE the sd2snes fork. Binds hx421_media's platform seams to the
 *  firmware's real services (FatFs, the FPGA SD-DMA offload, the SNES command
 *  mailbox) and exposes the two entry points main.c calls when an HX-421 audio
 *  core is loaded — hooked like MSU-1's loop, gated on romprops.has_hx421.
 *
 *  Public domain (CC0). No warranty.
 * ============================================================ */

#ifndef HX421_MEDIA_FW_H
#define HX421_MEDIA_FW_H

/* Set up the media service: bind seams, lay out the PSRAM rings, and (as a
 * bring-up convenience until the SNES command mailbox is wired) auto-start a
 * default background music stream if its asset is present on the SD card.
 * Returns 0 on success, non-zero if the core's assets are missing/unusable. */
int  hx421_media_fw_init(void);

/* One cooperative service tick — call repeatedly from the run loop. Drains SNES
 * mailbox commands, keeps the PSRAM rings fed, and manages the mixer channels.
 * Returns non-zero to leave the mode (SNES reset / FPGA gone), matching the
 * base loop's `while(!..._loop());` contract. */
int  hx421_media_fw_loop(void);

#endif /* HX421_MEDIA_FW_H */
