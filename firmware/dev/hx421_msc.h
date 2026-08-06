/* ============================================================
 *  hx421_msc.h — USB Mass Storage (Bulk-Only Transport + SCSI) exposing the SD
 *  card as a removable drive, so you can drag .hxg files onto it in Explorer
 *  instead of pulling the card. Runs as a second function alongside the CDC
 *  terminal (composite device); MSC bulk lives on EP3.
 *
 *  Block I/O proxies straight to the firmware's FatFs diskio (disk_read/
 *  disk_write). Interlock: the host and the firmware both reach the SD through
 *  the same block layer, so raw sectors stay coherent; the FS *caches* do not,
 *  hence: eject on the PC before running a game, and the firmware remounts.
 *
 *  Public domain (CC0). No warranty.
 * ============================================================ */

#ifndef HX421_MSC_H
#define HX421_MSC_H

#include <stdint.h>

/* MSC bulk endpoint addresses (composite: CDC owns EP1/EP2, MSC owns EP3). */
#define MSC_EP_IN   0x83
#define MSC_EP_OUT  0x03

void msc_reset(void);        /* USB_Configure_Event: reset the BOT state machine */
void msc_bulk_out(void);     /* EP3 OUT event (USB ISR): CBW, or WRITE data      */
void msc_bulk_in(void);      /* EP3 IN complete (USB ISR): stream READ data / CSW */

/* Set by a WRITE(10) from the host, so the firmware knows its FatFs view is
 * stale and remounts before its next SD access (e.g. before `run`). */
extern volatile uint8_t msc_host_wrote;

#endif /* HX421_MSC_H */
