/* ============================================================
 *  hx421_msc.c — USB Mass Storage Bulk-Only Transport + a minimal SCSI target,
 *  exposing the SD card as a removable drive over EP3 (alongside the CDC
 *  terminal). Block reads/writes proxy to the FatFs diskio layer.
 *
 *  Flow (BOT): host sends a 31-byte CBW on EP3-OUT -> we run the SCSI command,
 *  transfer data on EP3-IN (read) or EP3-OUT (write), then send a 13-byte CSW on
 *  EP3-IN. All driven from the USB ISR via msc_bulk_out()/msc_bulk_in().
 *
 *  Disk I/O runs in ISR context here (simple); a slow SD access just makes the
 *  host NAK until the next packet is ready. If that ever starves other USB
 *  traffic, move the disk_read/disk_write to a main-loop poll.
 *
 *  Public domain (CC0). No warranty.
 * ============================================================ */

#include <stdint.h>
#include <string.h>
#include "config.h"
#include "usbhw.h"       /* USB_WriteEP / USB_ReadEP */
#include "diskio.h"      /* disk_read / disk_write / diskinfo0_t / RES_* / STA_* */
#include "hx421_msc.h"

/* disk_getinfo is defined (sdnative.c) but not prototyped in diskio.h. */
extern DRESULT disk_getinfo(BYTE pdrv, BYTE page, void *buff);

#define MSC_MPS       64u           /* FS bulk max packet size          */
#define MSC_BLOCK     512u          /* SD sector size                   */
#define CBW_SIG       0x43425355u   /* "USBC" */
#define CSW_SIG       0x53425355u   /* "USBS" */

typedef struct __attribute__((packed)) {
    uint32_t dSignature;
    uint32_t dTag;
    uint32_t dDataLength;
    uint8_t  bmFlags;               /* bit7: 1 = data IN (device->host) */
    uint8_t  bLUN;
    uint8_t  bCBLength;
    uint8_t  CB[16];
} msc_cbw_t;

typedef struct __attribute__((packed)) {
    uint32_t dSignature;
    uint32_t dTag;
    uint32_t dDataResidue;
    uint8_t  bStatus;               /* 0 pass, 1 fail, 2 phase error */
} msc_csw_t;

enum { MSC_WAIT_CBW = 0, MSC_DATA_IN, MSC_DATA_OUT, MSC_SEND_CSW };

static volatile int msc_state = MSC_WAIT_CBW;
static msc_csw_t     csw;
static uint32_t      msc_lba, msc_blocks;   /* current sector, sectors left */
static uint8_t       msc_buf[MSC_BLOCK];
static uint32_t      msc_bufpos;            /* byte cursor within msc_buf   */
static uint32_t      msc_datalen;           /* bytes left in the data phase */
static uint8_t       msc_reading_disk;      /* 1 = READ(10) streams sectors */
static uint8_t       sense_key, sense_asc, sense_ascq;

volatile uint8_t     msc_host_wrote = 0;

/* ---- helpers ---- */
static void set_sense(uint8_t key, uint8_t asc, uint8_t ascq) {
    sense_key = key; sense_asc = asc; sense_ascq = ascq;
}
static uint32_t be32(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}
static void wr_be32(uint8_t *p, uint32_t v) {
    p[0] = v >> 24; p[1] = v >> 16; p[2] = v >> 8; p[3] = v;
}

static uint32_t sd_sectorcount(void) {
    diskinfo0_t di;
    if (disk_getinfo(0, 0, &di) != RES_OK) return 0;
    return di.sectorcount;
}

/* Begin a data-IN phase from a small in-memory response buffer (INQUIRY, etc.). */
static void begin_reply(const uint8_t *data, uint32_t len, uint32_t host_wants) {
    if (len > host_wants) len = host_wants;
    if (len > sizeof msc_buf) len = sizeof msc_buf;
    memcpy(msc_buf, data, len);
    msc_bufpos = 0;
    msc_datalen = len;
    msc_reading_disk = 0;
    csw.dDataResidue = host_wants - len;
    msc_state = MSC_DATA_IN;
}

/* Send the CSW; the next EP3-IN completion returns us to waiting for a CBW. */
static void send_csw(void) {
    csw.dSignature = CSW_SIG;
    msc_state = MSC_SEND_CSW;
    USB_WriteEP(MSC_EP_IN, (uint8_t *)&csw, sizeof csw);
}

/* Push one packet of the data-IN phase (called on kickoff + each IN completion). */
static void data_in_next(void) {
    if (msc_datalen == 0) { send_csw(); return; }
    if (msc_reading_disk && msc_bufpos >= MSC_BLOCK) {
        if (disk_read(0, msc_buf, msc_lba, 1) != RES_OK) {
            csw.bStatus = 1;
            set_sense(0x03, 0x11, 0x00);   /* medium error, unrecovered read */
        }
        msc_lba++;
        msc_bufpos = 0;
    }
    uint32_t n = MSC_MPS;
    if (n > msc_datalen) n = msc_datalen;
    USB_WriteEP(MSC_EP_IN, msc_buf + msc_bufpos, n);
    msc_bufpos += n;
    msc_datalen -= n;
}

/* ---- SCSI command dispatch (called with a fresh CBW) ---- */
static void scsi_dispatch(const msc_cbw_t *cbw) {
    const uint8_t *cdb = cbw->CB;
    uint32_t host_wants = cbw->dDataLength;
    csw.dTag = cbw->dTag;
    csw.dDataResidue = host_wants;
    csw.bStatus = 0;                 /* assume pass; commands set fail as needed */

    switch (cdb[0]) {
    case 0x00: /* TEST UNIT READY */
        if (disk_status(0) & STA_NODISK) {
            csw.bStatus = 1;
            set_sense(0x02, 0x3A, 0x00);   /* not ready, medium not present */
        }
        send_csw();
        break;

    case 0x03: { /* REQUEST SENSE */
        uint8_t s[18];
        memset(s, 0, sizeof s);
        s[0] = 0x70;                  /* current errors, fixed format */
        s[2] = sense_key;
        s[7] = 10;                    /* additional sense length */
        s[12] = sense_asc;
        s[13] = sense_ascq;
        set_sense(0, 0, 0);           /* sense is consumed once reported */
        begin_reply(s, sizeof s, host_wants);
        data_in_next();
        break;
    }

    case 0x12: { /* INQUIRY */
        uint8_t inq[36];
        memset(inq, 0, sizeof inq);
        inq[0] = 0x00;                /* direct-access block device, connected */
        inq[1] = 0x80;                /* RMB=1: removable */
        inq[2] = 0x04;                /* SPC-2 */
        inq[3] = 0x02;                /* response data format */
        inq[4] = 31;                  /* additional length (36 - 5) */
        memcpy(inq + 8,  "HX421   ", 8);
        memcpy(inq + 16, "SD Card         ", 16);
        memcpy(inq + 32, "1.0 ", 4);
        begin_reply(inq, sizeof inq, host_wants);
        data_in_next();
        break;
    }

    case 0x1A: { /* MODE SENSE(6) */
        uint8_t m[4];
        m[0] = 3;                     /* mode data length (following bytes) */
        m[1] = 0;                     /* medium type */
        m[2] = 0x00;                  /* device-specific: 0 = writable (0x80 = WP) */
        m[3] = 0;                     /* block descriptor length */
        begin_reply(m, sizeof m, host_wants);
        data_in_next();
        break;
    }

    case 0x1E: /* PREVENT/ALLOW MEDIUM REMOVAL */
        send_csw();
        break;

    case 0x25: { /* READ CAPACITY(10) */
        uint8_t cap[8];
        uint32_t sc = sd_sectorcount();
        wr_be32(cap, sc ? sc - 1 : 0); /* last LBA */
        wr_be32(cap + 4, MSC_BLOCK);   /* block size */
        begin_reply(cap, sizeof cap, host_wants);
        data_in_next();
        break;
    }

    case 0x28: /* READ(10) */
        msc_lba = be32(cdb + 2);
        msc_blocks = ((uint32_t)cdb[7] << 8) | cdb[8];
        msc_datalen = msc_blocks * MSC_BLOCK;
        csw.dDataResidue = host_wants > msc_datalen ? host_wants - msc_datalen : 0;
        msc_bufpos = MSC_BLOCK;        /* force a disk_read on the first packet */
        msc_reading_disk = 1;
        msc_state = MSC_DATA_IN;
        data_in_next();
        break;

    case 0x2A: /* WRITE(10) */
        msc_lba = be32(cdb + 2);
        msc_blocks = ((uint32_t)cdb[7] << 8) | cdb[8];
        msc_datalen = msc_blocks * MSC_BLOCK;
        csw.dDataResidue = host_wants;
        msc_bufpos = 0;
        msc_state = MSC_DATA_OUT;      /* wait for the host's data on EP3-OUT */
        break;

    default:
        /* Unsupported command: fail it. If the host expected IN data, a short
         * CSW with full residue is the simplest legal response. */
        csw.bStatus = 1;
        set_sense(0x05, 0x20, 0x00);   /* illegal request, invalid opcode */
        send_csw();
        break;
    }
}

/* ---- USB ISR entry points ---- */

/* EP3 OUT: either a CBW (idle) or a chunk of WRITE(10) data. */
void msc_bulk_out(void) {
    if (msc_state == MSC_WAIT_CBW) {
        msc_cbw_t cbw;
        uint32_t n = USB_ReadEP(MSC_EP_OUT, (uint8_t *)&cbw);
        if (n != sizeof cbw || cbw.dSignature != CBW_SIG) {
            /* Malformed CBW: nothing safe to do but drop it and keep waiting. */
            return;
        }
        scsi_dispatch(&cbw);
        return;
    }

    if (msc_state == MSC_DATA_OUT) {
        uint32_t n = USB_ReadEP(MSC_EP_OUT, msc_buf + msc_bufpos);
        msc_bufpos += n;
        if (msc_datalen >= n) msc_datalen -= n; else msc_datalen = 0;
        if (msc_bufpos >= MSC_BLOCK) {
            if (disk_write(0, msc_buf, msc_lba, 1) != RES_OK) {
                csw.bStatus = 1;
                set_sense(0x03, 0x0C, 0x00);   /* medium error, write fault */
            }
            msc_host_wrote = 1;
            msc_lba++;
            msc_bufpos = 0;
        }
        if (msc_datalen == 0) send_csw();
        return;
    }
    /* Unexpected OUT in another state: ignore. */
}

/* EP3 IN complete: continue streaming READ data, or finish after the CSW. */
void msc_bulk_in(void) {
    if (msc_state == MSC_DATA_IN) {
        data_in_next();
    } else if (msc_state == MSC_SEND_CSW) {
        msc_state = MSC_WAIT_CBW;      /* CSW delivered; ready for the next CBW */
    }
}

/* Reset the state machine on (re)configuration. */
void msc_reset(void) {
    msc_state = MSC_WAIT_CBW;
    msc_bufpos = 0;
    msc_datalen = 0;
    set_sense(0, 0, 0);
}
