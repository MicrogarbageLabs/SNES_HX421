/* ============================================================
 *  hx421_mask.h — 1-bit sprite masks, per-pixel 2D actor collision
 *
 *  The narrow phase for SPRITES, which is most of what a game actually
 *  collides. A 1-bit-per-pixel mask is a quarter the size of the 4bpp artwork
 *  it comes from, so the whole actor set is resident and collision never
 *  touches CHR — it does not compete with the renderer for PSRAM bandwidth.
 *
 *  MASKS ARE DERIVED, NEVER AUTHORED. Any non-transparent pixel becomes a 1.
 *  Hand-authored masks drift from the art the moment a sprite is redrawn, and
 *  the resulting bug is "hits register slightly off the picture", which is
 *  maddening to trace back to an asset.
 *
 *  Derivation runs per ACTOR at PSRAM stage time, not per sprite and not per
 *  frame. An actor is usually several OAM sprites (a 48x64 character might be
 *  six 16x32s); compositing them into ONE mask over the actor's bounding box
 *  means one entry, one test, one answer instead of N results to reconcile.
 *
 *  See docs/collision.md. Integer-only, no allocation — the RTL spec as much as
 *  the reference.
 *
 *  Public domain (CC0). No warranty.
 * ============================================================ */

#ifndef HX421_MASK_H
#define HX421_MASK_H

#include <stdint.h>

/* A row must fit one 64-bit accumulator, which covers every SNES sprite size
 * and most actors. Wider actors need multi-word rows; deferred until something
 * actually needs one, rather than paying for it everywhere now. */
#define HX421_MASK_MAX_W   64u
#define HX421_MASK_MAX_H   64u

/* A mask DESCRIPTOR. The bits live wherever the caller put them (PSRAM for the
 * full actor set, BRAM for the spawned ones) — this struct does not own them.
 *
 * Rows are padded to whole bytes and the width is stored, so a 48-wide actor
 * costs 6 B/row rather than being rounded up to 8. A uniform 64-wide row would
 * simplify the shifter but waste 8x on small sprites: 64 actors at 64x64
 * uniform is 32 KB, well past what BRAM should hold.
 *
 * Bit order is MSB-first: pixel x of a row is bit (7 - (x & 7)) of byte x>>3. */
typedef struct {
    uint8_t        w, h;      /* pixels                    */
    uint8_t        stride;    /* bytes per row = (w + 7)/8 */
    const uint8_t *bits;      /* h * stride bytes          */
} Hx421Mask;

/* One OAM sprite making up an actor, as staged CHR.
 *
 * `chr` is SNES 4bpp tile data, 32 B per tile. On real OBJ VRAM, vertically
 * adjacent tiles of a multi-tile sprite are 16 TILES apart in the name table,
 * not tiles_w apart — set `row_stride_tiles` to 16 for hardware-layout data, or
 * 0 for a tightly packed (tiles_w) run. Getting this wrong produces a mask that
 * is subtly, silently wrong rather than obviously broken. */
typedef struct {
    const uint8_t *chr;
    uint8_t  tiles_w, tiles_h;    /* sprite size in 8x8 tiles (1, 2, 4 or 8) */
    uint8_t  row_stride_tiles;    /* 16 on SNES OBJ VRAM; 0 = packed          */
    int16_t  ox, oy;              /* position within the actor's bounding box */
} Hx421SpritePart;

/* Which corner the scan starts from — it decides WHICH contact point you get
 * when several pixels overlap. Top-left is the natural fall-out and is fine for
 * impacts; a projectile usually wants the contact nearest its travel direction,
 * so it should scan from the side it came from. Worth fixing the convention
 * early: it is invisible until effects start spawning on the wrong side of a
 * hit. */
typedef enum {
    HX421_SCAN_TOPLEFT = 0,
    HX421_SCAN_TOPRIGHT,
    HX421_SCAN_BOTTOMLEFT,
    HX421_SCAN_BOTTOMRIGHT
} Hx421ScanOrder;

typedef struct {
    int16_t x, y;      /* contact point in WORLD pixels (not mask-relative) */
} Hx421MaskHit;

/* Composite `n` sprite parts into one actor mask of w x h pixels.
 *
 * Writes ceil(w/8)*h bytes into `bits`, which must be at least `cap`. Returns
 * the bytes written, or 0 if the size is unsupported or the buffer is too
 * small. Any pixel whose 4bpp index is non-zero sets its bit; index 0 is SNES
 * transparency and stays clear.
 *
 * Cost is irrelevant because this runs at load, not per frame: a full 64x64
 * actor is 4096 pixel tests once, against nothing per frame. */
unsigned hx421_derive_mask(const Hx421SpritePart *parts, unsigned n,
                           uint8_t w, uint8_t h, uint8_t *bits, unsigned cap);

/* Per-pixel overlap of two placed masks. (ax, ay) and (bx, by) are the masks'
 * top-left corners in world pixels.
 *
 * Broad phase first: the AABB reject is ~10 cycles and culls almost everything,
 * so the row loop only runs on survivors. Returns 1 on overlap and fills *hit
 * with the contact point in world pixels — free, because the row scan already
 * found it, but only if the API carries it out. Retrofitting a position onto a
 * boolean means re-running the whole test. */
int hx421_mask_overlap(const Hx421Mask *ma, int ax, int ay,
                       const Hx421Mask *mb, int bx, int by,
                       Hx421ScanOrder order, Hx421MaskHit *hit);

/* AABB overlap alone, exposed because it is the broad phase and callers with
 * many candidates want it without the mask work. */
int hx421_mask_aabb(const Hx421Mask *ma, int ax, int ay,
                    const Hx421Mask *mb, int bx, int by);

/* Read one pixel of a mask (bounds-checked, 0 outside). Mostly for tests and
 * for asserting a derived mask against its source art. */
int hx421_mask_get(const Hx421Mask *m, int x, int y);

#endif /* HX421_MASK_H */
