/* gen_vectors.c — co-sim golden model for hx_actor.v.
 *
 * There is no external C reference for the actor engine, so this is an
 * independent straight-line model of the INTENDED algorithm (world->screen via
 * the top-layer camera, Y-band counting sort, priority-lock, per-frame rotating
 * admission window). Expressed as a plain loop rather than an FSM, so a match
 * still catches transcription/FSM bugs in the RTL.
 *
 * Emits: actors.hex  — 24 packed 64-bit actors for the TB to load
 *        golden_a.hex — expected OAM (case A: n_lock=2, phase=0)
 *        golden_b.hex — expected OAM (case B: n_lock=0, phase=3, rotation)
 * OAM entry per slot = {attr,tile,sy,sx} 32-bit, or FFFFFFFF = untouched. CC0. */

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define NACT 24
#define N    128            /* OAM slots */
#define SH   240            /* screen height */
#define NB   ((SH+7)/8)     /* Y bands */
#define CAMX 0
#define CAMY 0

static uint16_t wx[NACT], wy[NACT], tile[NACT], attr[NACT];

static int onscreen(int a, int *sx, int *sy) {
    *sx = (int)wx[a] - CAMX;
    *sy = (int)wy[a] - CAMY;
    return (*sx > -16) && (*sx < 256) && (*sy >= 0) && (*sy < SH);
}

/* the golden model -> oam[N] (32b {attr,tile,sy,sx}, 0xFFFFFFFF = untouched) */
static void model(int n_lock, int phase, uint32_t *oam) {
    for (int i = 0; i < N; i++) oam[i] = 0xFFFFFFFFu;
    int sx, sy, lockcnt = 0;

    /* priority-lock: first n_lock on-screen actors -> lowest OAM indices */
    for (int a = 0; a < n_lock && a < NACT; a++)
        if (onscreen(a, &sx, &sy) && lockcnt < N)
            oam[lockcnt++] = ((uint32_t)attr[a]<<24)|((uint32_t)(tile[a]&0xFF)<<16)
                           | ((uint32_t)(sy&0xFF)<<8)|(uint32_t)(sx&0xFF);

    /* histogram the crowd by band */
    int hist[NB]; memset(hist,0,sizeof hist);
    int cw = 0;
    for (int a = n_lock; a < NACT; a++)
        if (onscreen(a,&sx,&sy)) { hist[(sy>>3)]++; cw++; }

    /* prefix sum -> band start rank */
    int acc = 0;
    for (int b = 0; b < NB; b++) { int t = hist[b]; hist[b] = acc; acc += t; }
    int budget = (N > lockcnt) ? (N - lockcnt) : 0;
    int rstart = phase; while (cw && rstart >= cw) rstart -= cw;   /* phase % cw */

    /* scatter: depth rank -> rotating admission window */
    for (int a = n_lock; a < NACT; a++) {
        if (!onscreen(a,&sx,&sy)) continue;
        int rank = hist[(sy>>3)]++;
        int rot = (rank >= rstart) ? (rank - rstart) : (rank - rstart + cw);
        if (rot < budget) {
            int oidx = lockcnt + rot;
            oam[oidx] = ((uint32_t)attr[a]<<24)|((uint32_t)(tile[a]&0xFF)<<16)
                      | ((uint32_t)(sy&0xFF)<<8)|(uint32_t)(sx&0xFF);
        }
    }
}

static void dump_oam(const char *name, uint32_t *oam) {
    FILE *f = fopen(name, "w");
    for (int i = 0; i < N; i++) fprintf(f, "%08x\n", oam[i]);
    fclose(f);
}

int main(void) {
    for (int a = 0; a < NACT; a++) {
        wx[a]   = (uint16_t)(10 + a*3);
        wy[a]   = (uint16_t)(8 + (a*53) % 224);   /* assorted bands, some collisions */
        tile[a] = (uint16_t)a;
        attr[a] = (uint16_t)(0x30 + a);
    }

    /* packed actor list: [15:0] wx,[31:16] wy,[40:32] tile,[48:41] attr,[49] size */
    FILE *p = fopen("actors.hex", "w");
    for (int a = 0; a < NACT; a++) {
        uint64_t v = (uint64_t)wx[a]
                   | ((uint64_t)wy[a] << 16)
                   | ((uint64_t)(tile[a] & 0x1FF) << 32)
                   | ((uint64_t)(attr[a] & 0xFF) << 41);
        fprintf(p, "%016llx\n", (unsigned long long)v);
    }
    fclose(p);

    uint32_t oam[N];
    model(2, 0, oam); dump_oam("golden_a.hex", oam);   /* lock */
    model(0, 3, oam); dump_oam("golden_b.hex", oam);   /* rotation */
    printf("gen_vectors: actors.hex + golden_a/b.hex written (NACT=%d)\n", NACT);
    return 0;
}
