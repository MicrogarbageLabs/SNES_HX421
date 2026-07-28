; ============================================================
;  h6_bramvec.s — verify the 65816 RESET vector is served from FPGA BRAM, not PSRAM.
;
;  The FPGA (HX421_BRAM_VECTORS core) serves $00:FFFC = $8000 from a register, so the
;  SNES boots to `reset` (CODE start = $8000) and shows "BRAM VECTOR OK".
;  This ROM's own $FFFC vector points instead at `rom_vec_entry`, which paints the
;  screen RED and hangs. So:
;     text "BRAM VECTOR OK"  -> the fabric served the reset vector (works)
;     solid red screen       -> the SNES used the ROM's PSRAM vector (intercept failed)
;
;  Public domain (CC0). No warranty.
; ============================================================

.p816
.smart +

.segment "CODE"

; ---- reset ($8000): reached only if the FPGA serves $FFFC = $8000 ----
reset:
        sei
        clc
        xce
        rep #$38
.a16
.i16
        ldx #$01FF
        txs
        lda #$0000
        tcd
        phk
        plb

        sep #$20
.a8
        lda #$8F
        sta $2100
        stz $4200
        stz $212C
        stz $212D
        stz $2130
        stz $2131
        stz $2133

        rep #$30
.a16
.i16
        jsr tm_setup
        lda #.loword(str1)
        ldx #(2 | (2 << 8))
        jsr tm_print_str
        lda #.loword(str2)
        ldx #(2 | (4 << 8))
        jsr tm_print_str

        sep #$20
.a8
        lda #$01
        sta $212C
        lda #$0F
        sta $2100
spin:   bra spin


; ---- rom_vec_entry: the ROM's $FFFC target. Red screen = BRAM serve FAILED. ----
rom_vec_entry:
        sei
        clc
        xce
        sep #$20
.a8
        lda #$8F
        sta $2100                       ; force blank while we set the color
        stz $2121                       ; CGRAM address 0 (backdrop)
        lda #$1F
        sta $2122                       ; color low  = red
        stz $2122                       ; color high
        lda #$0F
        sta $2100                       ; screen on -> solid red backdrop
rvhang: bra rvhang

str1:   .byte "BRAM VECTOR OK", 0
str2:   .byte "BOOTED VIA FPGA - NOT ROM", 0

.include "textmode.inc"

.segment "VECTORS"
        .word rom_vec_entry, rom_vec_entry, rom_vec_entry, rom_vec_entry
        .word rom_vec_entry, rom_vec_entry, rom_vec_entry, rom_vec_entry
        .word rom_vec_entry, rom_vec_entry, rom_vec_entry, rom_vec_entry
        .word rom_vec_entry             ; $FFFC reset -> red-screen fail path
        .word rom_vec_entry
