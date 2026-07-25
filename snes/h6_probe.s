; ============================================================
;  h6_probe.s — HX-421 6b PSRAM-read probe (rebuilt on the proven-clean
;  h2_probe display structure; no overlapping/overflowing positions).
;
;  The 6b core reads PSRAM byte 0x1000 over the SNES ROM bus (free_strobe-gated)
;  and exposes the value + a read counter at $3F:F008-F00A. This ROM reads them,
;  plus a normal SNES read of the SAME location ($00:9000 == file 0x1000, LoROM),
;  and shows them so we can confirm the mixer's PSRAM read is correct and pin the
;  byte order for wiring the real mixer source (6b.1b).
;
;  Public domain (CC0). No warranty.
; ============================================================

.p816
.smart +

SIG   = $60          ; F000-F003
MIXLO = $64          ; F008
MIXHI = $65          ; F009
CNT   = $66          ; F00A
SNLO  = $67          ; $9000
SNHI  = $68          ; $9001

.segment "CODE"

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
        stz $420C
        stz $212C
        stz $212D
        stz $2130
        stz $2131
        stz $2133

        ; ---- reads ----
        ldx #$0000
@rd:    lda f:$3FF000,x
        sta SIG,x
        inx
        cpx #4
        bne @rd
        lda f:$3FF008
        sta MIXLO
        lda f:$3FF009
        sta MIXHI
        lda f:$3FF00A
        sta CNT
        lda f:$009000
        sta SNLO
        lda f:$009001
        sta SNHI

        rep #$30
.a16
.i16
        jsr tm_setup

        lda #.loword(str_title)
        ldx #(2 | (2 << 8))
        jsr tm_print_str
        lda #.loword(str_mix)
        ldx #(2 | (5 << 8))
        jsr tm_print_str
        lda #.loword(str_snes)
        ldx #(2 | (7 << 8))
        jsr tm_print_str

        ; MIX row (row 5): LO col8, HI col14, CNT col20
        lda MIXLO
        and #$00FF
        ldx #(8  | (5 << 8))
        jsr tm_print_dec5
        lda MIXHI
        and #$00FF
        ldx #(14 | (5 << 8))
        jsr tm_print_dec5
        lda CNT
        and #$00FF
        ldx #(20 | (5 << 8))
        jsr tm_print_dec5

        ; SNES row (row 7): LO col8, HI col14
        lda SNLO
        and #$00FF
        ldx #(8  | (7 << 8))
        jsr tm_print_dec5
        lda SNHI
        and #$00FF
        ldx #(14 | (7 << 8))
        jsr tm_print_dec5

        sep #$20
.a8
        lda #$01
        sta $212C
        lda #$0F
        sta $2100
spin:   bra spin


str_title:  .byte "HX421 6b PSRAM PROBE", 0
str_mix:    .byte "MIX  LO HI CNT", 0
str_snes:   .byte "SNES LO HI", 0

.include "textmode.inc"

stop:   bra stop

.segment "VECTORS"
        .word stop, stop, stop, stop, stop, stop, stop, stop
        .word stop, stop, stop, stop
        .word reset
        .word stop
