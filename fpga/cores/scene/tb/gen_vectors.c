/* gen_vectors.c — co-sim golden model for hx_scene.v.
 *
 * The scene engine emits a deterministic 65816 DMA body: a PPU register preamble
 * (lda #imm; sta abs per bank entry) then a VRAM DMA slot per descriptor. This
 * models that exact byte stream. The TB configures the SAME register bank +
 * descriptors, runs the engine, and diffs its code output against golden.hex.
 * Constants here MUST match hx_scene_tb.v. See hx_scene.v. CC0. */

#include <stdint.h>
#include <stdio.h>

/* SNES register addresses (match hx_scene.v localparams) */
#define DMAP0 0x4300u
#define BBAD0 0x4301u
#define A1T0L 0x4302u
#define DAS0L 0x4305u
#define VMAIN 0x2115u
#define VMADDL 0x2116u
#define MDMAEN 0x420Bu

static FILE *g;
static void e8(uint8_t b) { fprintf(g, "%02x\n", b); }
static void lda_sta8 (uint8_t imm, uint16_t a)  { e8(0xA9); e8(imm); e8(0x8D); e8(a&0xFF); e8(a>>8); }
static void lda_sta16(uint16_t imm, uint16_t a) { e8(0xA9); e8(imm&0xFF); e8(imm>>8); e8(0x8D); e8(a&0xFF); e8(a>>8); }
static void dma_slot(uint16_t src, uint16_t size, uint16_t vdst, uint8_t vmain) {
    lda_sta8(0x18, BBAD0);
    lda_sta8(0x01, DMAP0);
    e8(0xC2); e8(0x20);          /* rep #$20 */
    lda_sta16(src, A1T0L);
    lda_sta16(size, DAS0L);
    e8(0xE2); e8(0x20);          /* sep #$20 */
    lda_sta8(vmain, VMAIN);
    e8(0xC2); e8(0x20);
    lda_sta16(vdst, VMADDL);
    e8(0xE2); e8(0x20);
    lda_sta8(0x01, MDMAEN);
}

/* register bank (is16, addr, val) — MUST match the TB */
static const int      rb_is16[3] = { 0, 1, 0 };
static const uint16_t rb_addr[3] = { 0x2105, 0x2116, 0x212C };
static const uint16_t rb_val [3] = { 0x0009, 0x1234, 0x0017 };
/* descriptors (src, size, vdst, vmain) — MUST match the TB */
static const uint16_t d_src [2] = { 0x0000, 0x0800 };
static const uint16_t d_size[2] = { 0x0800, 0x0100 };
static const uint16_t d_vdst[2] = { 0x0000, 0x4000 };
static const uint8_t  d_vmain[2]= { 0x80,   0x81 };

int main(void) {
    g = fopen("golden.hex", "w");
    for (int r = 0; r < 3; r++)
        if (rb_is16[r]) lda_sta16(rb_val[r], rb_addr[r]);
        else            lda_sta8 (rb_val[r], rb_addr[r]);
    for (int d = 0; d < 2; d++)
        dma_slot(d_src[d], d_size[d], d_vdst[d], d_vmain[d]);
    fclose(g);
    printf("gen_vectors: golden.hex written (scene DMA body)\n");
    return 0;
}
