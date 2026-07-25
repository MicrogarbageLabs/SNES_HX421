/* ============================================================
 *  hx421_stream.h — HX-421 SD -> PSRAM-ring streaming arbiter (M4 side)
 *
 *  Keeps N audio streams' PSRAM ring buffers fed from SD so the FPGA
 *  8-channel mixer can read them as channels. The M4 never touches an
 *  audio byte: each refill is an FPGA SD-DMA *offload* (SD -> PSRAM,
 *  bypassing the MCU). This module owns only the bookkeeping — where
 *  each ring's write pointer is, how far the mixer has drained it
 *  (read pointer, reported by the FPGA), and which stream to refill
 *  next — and issues offload kicks through platform seams.
 *
 *  The seams (offload, read_ptr) are function pointers so the whole
 *  arbiter is exercised on the host against a mock SD + a simulated
 *  mixer drain, before any hardware. See tools/hx421_stream_test.c.
 *
 *  Stream data must be the mixer-native format: 16-bit interleaved
 *  stereo at the mixer's rate. Then the offload is a raw byte copy of
 *  the WAV data chunk into the ring (no conversion on the hot path);
 *  non-canonical assets are pre-converted offline.
 *
 *  Public domain (CC0). No warranty.
 * ============================================================ */

#ifndef HX421_STREAM_H
#define HX421_STREAM_H

#include <stdint.h>

#ifndef HX421_STREAM_MAX
#define HX421_STREAM_MAX 4
#endif

/* Platform seams — set once, mockable on the host. */
typedef struct {
    void *ctx;

    /* Kick an SD-DMA offload: copy `len` bytes from stream `s`'s source
     * file at byte offset `src_off` into PSRAM at absolute byte address
     * `psram_addr`. The FPGA performs the SD->PSRAM transfer; this call
     * only sets it up (returns after the transfer is queued/complete,
     * platform's choice). Returns 0 on success, non-zero to retry later. */
    int (*offload)(void *ctx, int s, uint32_t psram_addr,
                   uint32_t src_off, uint32_t len);

    /* The FPGA mixer's current read position for stream `s`, as a byte
     * offset within its ring [0, ring_size). This is the consumer side;
     * the arbiter keeps the write pointer ahead of it. */
    uint32_t (*read_ptr)(void *ctx, int s);
} Hx421StreamPlat;

typedef struct {
    uint32_t psram_base;   /* ring base, absolute PSRAM byte address     */
    uint32_t ring_size;    /* ring size in bytes                         */
    uint32_t data_off;     /* WAV data-chunk file offset (bytes)         */
    uint32_t data_bytes;   /* WAV data-chunk length (bytes)              */

    uint32_t prime_bytes;  /* primed-head size: fill this much before the  *
                            * stream reports ready (covers SD-fetch latency *
                            * at start). 0 -> default (target fill).        */

    uint32_t write_pos;    /* next write offset in ring [0,ring_size)    */
    uint32_t file_pos;     /* bytes consumed from the data chunk         */

    uint8_t  active;       /* stream slot in use                         */
    uint8_t  looping;      /* wrap the source at data end vs. stop       */
    uint8_t  primed;       /* primed head loaded; mixer may read         */
    uint8_t  prio;         /* refill priority: FMV (A/V, frame-drop on    *
                            * underrun) > PCM music. Higher = serviced    *
                            * sooner under SD contention.                 */
} Hx421Stream;

typedef struct {
    Hx421StreamPlat plat;
    Hx421Stream str[HX421_STREAM_MAX];
    uint32_t chunk;        /* max bytes per refill offload               */
    uint32_t low_wm;       /* refill when fill (bytes) drops below this  */
    uint32_t high_wm;      /* prime/refill target fill (bytes)           */
    int      rr;           /* round-robin cursor across streams          */
} Hx421StreamArb;

/* Initialise. chunk/low_wm/high_wm may be 0 to accept defaults derived
 * from the first started stream's ring_size. */
void hx421_stream_arb_init(Hx421StreamArb *a, const Hx421StreamPlat *plat,
                           uint32_t chunk, uint32_t low_wm, uint32_t high_wm);

/* Start stream `s` playing the data region [data_off, data_off+data_bytes)
 * of its source into a ring at `psram_base` of `ring_size` bytes. Does not
 * fill the ring yet — the first hx421_stream_service() calls load the primed
 * head (`prime_bytes`, 0 = default). `prio` orders refills under contention
 * (0 = PCM music; give the FMV stream a higher value). Returns 0 on success.
 * `ring_size`/`chunk`/`prime_bytes` should be multiples of the frame (4 B). */
int  hx421_stream_start(Hx421StreamArb *a, int s,
                        uint32_t psram_base, uint32_t ring_size,
                        uint32_t data_off, uint32_t data_bytes, int looping,
                        uint32_t prime_bytes, int prio);

/* Stop stream `s` (mixer channel should be muted separately). */
void hx421_stream_stop(Hx421StreamArb *a, int s);

/* True once a stream's ring has been primed to high_wm and the FPGA
 * mixer channel can safely be enabled to read it. */
int  hx421_stream_ready(const Hx421StreamArb *a, int s);

/* Cooperative service — call from the run loop. Issues at most ONE
 * refill offload (the emptiest stream needing data), so the loop stays
 * responsive and streams share SD bandwidth round-robin. Returns 1 if it
 * issued an offload, 0 if nothing needed doing. */
int  hx421_stream_service(Hx421StreamArb *a);

/* Bytes currently buffered ahead of the mixer for stream `s` (fill). */
uint32_t hx421_stream_fill(const Hx421StreamArb *a, int s);

#endif /* HX421_STREAM_H */
