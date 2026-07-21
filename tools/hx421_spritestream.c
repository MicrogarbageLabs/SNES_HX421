/* ============================================================
 *  hx421_spritestream.c — CHR streaming budget for scaled raycaster enemies
 *
 *  SNES OBJ hardware cannot scale, so a billboarded enemy needs its CHR
 *  re-rendered whenever its on-screen size changes. The coprocessor has a 2D
 *  scaler for exactly that, but the bandwidth question is what decides how many
 *  enemies a scene can hold.
 *
 *  The naive answer is worst-case: a 64x64 sprite is 2048 B and the per-frame
 *  budget is ~4930 B, so two. That is the wrong number, because an enemy only
 *  needs re-uploading when its SCALE BUCKET changes or its ANIMATION FRAME
 *  advances — not every frame. A distant enemy walking slowly may hold the same
 *  bucket for seconds.
 *
 *  This simulates enemies moving through a level and reports the actual
 *  distribution of bytes per frame, because the mean and the worst case are
 *  what decide whether the design holds, and they differ by an order of
 *  magnitude.
 *
 *  Public domain (CC0). No warranty.
 * ============================================================ */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>

/* ---- budget, from docs/raycaster.md (all measured, not assumed) ---------- */
#define BLANK_LINES     54          /* 262 - 208 */
#define BYTES_PER_LINE  163         /* snes/dma_rate_test.s on bsnes-accuracy */
#define TILEMAP_DMA     3328        /* BG1 + BG2, 26 rows x 32 x 2 x 2 layers  */
#define OAM_DMA          544
#define CHR_BUDGET      (BLANK_LINES * BYTES_PER_LINE - TILEMAP_DMA - OAM_DMA)

/* SNES OBJ can address only 512 tiles — two tables of 256 — so the sprite CHR
 * pool is hard-capped at 16 KB no matter what VRAM is spare. */
#define OBJ_POOL_TILES  512
#define OBJ_POOL_BYTES  (OBJ_POOL_TILES * 32)

#define FRAMES          3600        /* one minute at 60 Hz */
#define MAX_ENEMIES     16

/* Scale buckets: on-screen sprite side in pixels, nearest first. SNES OBJ sizes
 * come in pairs, so a real build picks two of these per OBSEL setting; the
 * intermediate sizes are achieved by scaling INTO the larger cell. */
static const int bucket_px[] = { 64, 56, 48, 40, 32, 28, 24, 20, 16, 12, 8 };
#define N_BUCKETS ((int)(sizeof bucket_px / sizeof bucket_px[0]))

/* Bytes of 4bpp CHR for a sprite of `px` on a side, rounded to whole 8x8 tiles. */
static int chr_bytes(int px) {
    const int tiles = ((px + 7) / 8) * ((px + 7) / 8);
    return tiles * 32;
}

/* Which bucket does an enemy at distance d (world units) fall into?
 * Projected size ~ focal * height / d; bucket by nearest available size. */
static int bucket_for(double d, double focal, double height) {
    if (d < 0.5) d = 0.5;
    const double px = focal * height / d;
    int best = N_BUCKETS - 1;
    double berr = 1e30;
    for (int i = 0; i < N_BUCKETS; ++i) {
        const double e = (px > bucket_px[i]) ? px - bucket_px[i] : bucket_px[i] - px;
        if (e < berr) { berr = e; best = i; }
    }
    return best;
}

typedef struct {
    double d, vd;       /* distance and closing speed, world units */
    int    bucket;
    int    anim;
    int    anim_tick;
} Enemy;

static int cmp_int(const void *a, const void *b) {
    return (*(const int *)a) - (*(const int *)b);
}

static void run(int n_enemies, int anim_fps, double speed, const char *label) {
    Enemy e[MAX_ENEMIES];
    uint32_t seed = 0x2468acef;
    const double focal = 120.0, height = 1.0;

    for (int i = 0; i < n_enemies; ++i) {
        seed = seed * 1103515245u + 12345u;
        e[i].d = 2.0 + ((seed >> 16) % 1400) / 100.0;      /* 2..16 units */
        seed = seed * 1103515245u + 12345u;
        e[i].vd = (((seed >> 16) % 200) / 100.0 - 1.0) * speed;
        e[i].bucket = bucket_for(e[i].d, focal, height);
        e[i].anim = 0;
        e[i].anim_tick = (int)((seed >> 8) % 60);
    }

    int per_frame[FRAMES];
    long total = 0, over = 0, worst = 0;
    int resident_peak = 0;

    for (int f = 0; f < FRAMES; ++f) {
        int bytes = 0, resident = 0;
        for (int i = 0; i < n_enemies; ++i) {
            /* wander, bouncing between 2 and 16 units */
            e[i].d += e[i].vd;
            if (e[i].d < 2.0)  { e[i].d = 2.0;  e[i].vd = -e[i].vd; }
            if (e[i].d > 16.0) { e[i].d = 16.0; e[i].vd = -e[i].vd; }

            const int nb = bucket_for(e[i].d, focal, height);
            int dirty = 0;
            if (nb != e[i].bucket) { e[i].bucket = nb; dirty = 1; }

            if (++e[i].anim_tick >= 60 / anim_fps) {
                e[i].anim_tick = 0;
                e[i].anim = (e[i].anim + 1) & 7;
                dirty = 1;                    /* new pose, same scale: re-render */
            }
            if (dirty) bytes += chr_bytes(bucket_px[e[i].bucket]);
            resident += chr_bytes(bucket_px[e[i].bucket]);
        }
        per_frame[f] = bytes;
        total += bytes;
        if (bytes > worst) worst = bytes;
        if (bytes > CHR_BUDGET) over++;
        if (resident > resident_peak) resident_peak = resident;
    }

    int sorted[FRAMES];
    memcpy(sorted, per_frame, sizeof sorted);
    qsort(sorted, FRAMES, sizeof(int), cmp_int);

    printf("%-22s | %2d foes | mean %5ld B | p50 %5d | p95 %5d | p99 %5d | worst %5ld B "
           "| over budget %4ld/%d (%.1f%%) | resident peak %5d B %s\n",
           label, n_enemies, total / FRAMES, sorted[FRAMES/2], sorted[(FRAMES*95)/100],
           sorted[(FRAMES*99)/100], worst, over, FRAMES, 100.0 * over / FRAMES,
           resident_peak, resident_peak <= OBJ_POOL_BYTES ? "" : "<- EXCEEDS OBJ POOL");
}

int main(void) {
    printf("per-frame CHR budget: %d blank lines x %d B - %d tilemap - %d OAM = %d B\n",
           BLANK_LINES, BYTES_PER_LINE, TILEMAP_DMA, OAM_DMA, CHR_BUDGET);
    printf("OBJ pool hard cap: %d tiles = %d B (SNES addresses only 2 tables of 256)\n",
           OBJ_POOL_TILES, OBJ_POOL_BYTES);
    printf("worst single sprite: 64x64 = %d B\n\n", chr_bytes(64));

    printf("-- enemy count, 8 fps animation, moderate movement --\n");
    for (int n = 2; n <= 12; n += 2) run(n, 8, 0.02, "typical");

    printf("\n-- animation rate (8 enemies) --\n");
    run(8,  4, 0.02, "4 fps anim");
    run(8,  8, 0.02, "8 fps anim");
    run(8, 15, 0.02, "15 fps anim");
    run(8, 30, 0.02, "30 fps anim");

    printf("\n-- movement speed (8 enemies, 8 fps) — faster = more bucket crossings --\n");
    run(8, 8, 0.01, "slow");
    run(8, 8, 0.02, "moderate");
    run(8, 8, 0.05, "fast");
    run(8, 8, 0.12, "charging");

    return 0;
}
