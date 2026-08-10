/* gen_vectors.c — co-sim golden model for hx_fmv.v.
 *
 * The FMV prep engine is a deterministic PSRAM->BRAM sub-frame copy + descriptor
 * emitter, so the golden is a direct model of its intended behaviour. Emits:
 *   psram.hex        — a frame image (identity: word w holds w)
 *   golden_stage_N   — expected staging words for case N
 *   golden_desc_N    — expected 2 descriptors (CHR, tilemap) for case N
 * See hx_fmv.v. Two cases exercise sub-frame offset + double-buffer overlap. CC0. */

#include <stdint.h>
#include <stdio.h>

#define CHR_BASE   0x0100
#define TMAP_BASE  0x0800
#define CHR_PS     64        /* words/sub-frame CHR     */
#define TMAP_PS    16        /* words/sub-frame tilemap */
#define STAGE      0
#define VRAM_CHR   0x2000
#define VRAM_OVL   0x1000
#define VRAM_TMAP  0
#define PSRAM_N    4096
#define STAGE_N    (CHR_PS + TMAP_PS)

static uint16_t psram[PSRAM_N];

static void emit_case(int idx, int sub, int parity) {
    char fn[64];
    /* staging: CHR then tilemap */
    sprintf(fn, "golden_stage_%d.hex", idx);
    FILE *s = fopen(fn, "w");
    for (int i = 0; i < CHR_PS;  i++) fprintf(s, "%04x\n", psram[CHR_BASE  + sub*CHR_PS  + i]);
    for (int i = 0; i < TMAP_PS; i++) fprintf(s, "%04x\n", psram[TMAP_BASE + sub*TMAP_PS + i]);
    fclose(s);

    /* two descriptors packed as {vmain[7:0], vdst[15:0], size[15:0], src[15:0]} */
    sprintf(fn, "golden_desc_%d.hex", idx);
    FILE *d = fopen(fn, "w");
    uint32_t chr_vdst = (parity ? (VRAM_CHR - VRAM_OVL) : VRAM_CHR) + sub*CHR_PS;
    uint64_t chr = ((uint64_t)0x80 << 48) | ((uint64_t)(chr_vdst & 0xFFFF) << 32)
                 | ((uint64_t)(CHR_PS*2) << 16) | (uint64_t)STAGE;
    uint32_t tm_vdst = VRAM_TMAP + sub*TMAP_PS;
    uint64_t tm  = ((uint64_t)0x80 << 48) | ((uint64_t)(tm_vdst & 0xFFFF) << 32)
                 | ((uint64_t)(TMAP_PS*2) << 16) | (uint64_t)(STAGE + CHR_PS);
    fprintf(d, "%016llx\n%016llx\n", (unsigned long long)chr, (unsigned long long)tm);
    fclose(d);
}

int main(void) {
    for (int w = 0; w < PSRAM_N; w++) psram[w] = (uint16_t)w;   /* identity image */
    FILE *p = fopen("psram.hex", "w");
    for (int w = 0; w < PSRAM_N; w++) fprintf(p, "%04x\n", psram[w]);
    fclose(p);

    emit_case(0, /*sub*/0, /*parity*/0);
    emit_case(1, /*sub*/1, /*parity*/1);
    printf("gen_vectors: psram.hex + golden_stage/desc_{0,1}.hex written\n");
    return 0;
}
