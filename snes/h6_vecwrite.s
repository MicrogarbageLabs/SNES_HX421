; ============================================================
;  h6_vecwrite.s — verify the vec_mem WRITE window (step 2a).
;
;  Writes $AB to vec_mem[0] via $3F:F020, reads it back via $3F:F040, and colors the
;  backdrop by the result:
;     GREEN -> readback == $AB : the SNES-write path + write window work
;     BLUE  -> readback != $AB : the write didn't land
;     RED   -> booted via the ROM's PSRAM reset vector (step-1 serve failed; unexpected)
;
;  Runs on the HX421_BRAM_VECTORS core. Public domain (CC0). No warranty.
; ============================================================

.p816
.smart +

.segment "CODE"

reset:                                  ; $8000, reached via the BRAM reset vector
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
        ; ---- write vec_mem[0] = $AB, then read it back ----
        lda #$AB
        sta f:$3FF020                   ; write window: vec_mem[0] <= $AB
        lda f:$3FF040                   ; readback window: A <- vec_mem[0]
        cmp #$AB
        bne @mismatch

        ; match -> GREEN
        lda #$8F
        sta $2100
        stz $212C
        stz $212D
        stz $2121
        lda #$E0
        sta $2122                       ; $03E0 green
        lda #$03
        sta $2122
        bra @on

@mismatch:
        ; readback wrong -> BLUE
        lda #$8F
        sta $2100
        stz $212C
        stz $212D
        stz $2121
        lda #$00
        sta $2122                       ; $7C00 blue
        lda #$7C
        sta $2122

@on:
        lda #$0F
        sta $2100
spin:   bra spin


; ---- rom_vec_entry: ROM's $FFFC target -> RED (step-1 serve failed) ----
rom_vec_entry:
        sei
        clc
        xce
        sep #$20
.a8
        lda #$8F
        sta $2100
        stz $212C
        stz $212D
        stz $2121
        lda #$1F
        sta $2122                       ; $001F red
        stz $2122
        lda #$0F
        sta $2100
rvhang: bra rvhang

.segment "VECTORS"
        .word rom_vec_entry, rom_vec_entry, rom_vec_entry, rom_vec_entry
        .word rom_vec_entry, rom_vec_entry, rom_vec_entry, rom_vec_entry
        .word rom_vec_entry, rom_vec_entry, rom_vec_entry, rom_vec_entry
        .word rom_vec_entry             ; $FFFC
        .word rom_vec_entry
