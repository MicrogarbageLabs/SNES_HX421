; ============================================================
;  h6_drain.s — confirm the mixer DRAIN POINTER advances on hardware.
;
;  The 6b core plays a 128-sample sine from PSRAM (as in 6b.1b) and exposes the
;  mixer's channel-0 read position (the drain pointer the STM32 needs) at
;  $3F:F00B-F00D. This ROM APU-unmutes (so the sine is audible), reads the drain
;  pointer, waits, reads it again, and shows both. R2 != R1 (the low byte cycling
;  0..127 for the 128-sample loop) = the drain feedback is live on silicon.
;
;  Public domain (CC0). No warranty.
; ============================================================

.p816
.smart +

APUIO0 = $2140
APUIO1 = $2141
APUIO2 = $2142
APUIO3 = $2143

DLO1 = $60
DMID1= $61
DHI1 = $62
DLO2 = $63
DMID2= $64
DHI2 = $65

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

        jsr apu_init                    ; unmute so the sine (and its drain) run

        ; ---- read drain pointer, wait, read again ----
        lda f:$3FF00B
        sta DLO1
        lda f:$3FF00C
        sta DMID1
        lda f:$3FF00D
        sta DHI1
        ldx #$8000
@dly:   dex
        bne @dly
        lda f:$3FF00B
        sta DLO2
        lda f:$3FF00C
        sta DMID2
        lda f:$3FF00D
        sta DHI2

        rep #$30
.a16
.i16
        jsr tm_setup
        lda #.loword(str_title)
        ldx #(2 | (2 << 8))
        jsr tm_print_str
        lda #.loword(str_hdr)
        ldx #(2 | (4 << 8))
        jsr tm_print_str
        lda #.loword(str_r1)
        ldx #(2 | (5 << 8))
        jsr tm_print_str
        lda #.loword(str_r2)
        ldx #(2 | (7 << 8))
        jsr tm_print_str

        lda DLO1
        and #$00FF
        ldx #(8  | (5 << 8))
        jsr tm_print_dec5
        lda DMID1
        and #$00FF
        ldx #(14 | (5 << 8))
        jsr tm_print_dec5
        lda DHI1
        and #$00FF
        ldx #(20 | (5 << 8))
        jsr tm_print_dec5

        lda DLO2
        and #$00FF
        ldx #(8  | (7 << 8))
        jsr tm_print_dec5
        lda DMID2
        and #$00FF
        ldx #(14 | (7 << 8))
        jsr tm_print_dec5
        lda DHI2
        and #$00FF
        ldx #(20 | (7 << 8))
        jsr tm_print_dec5

        sep #$20
.a8
        lda #$01
        sta $212C
        lda #$0F
        sta $2100
spin:   bra spin


; ---- APU unmute (verified pvsneslib spcBoot handshake) -----------------
.a8
.i16
apu_init:
@rdy:   ldx APUIO0
        cpx #$BBAA
        bne @rdy
        stx APUIO1
        ldx #$0200
        stx APUIO2
        lda #$CC
        sta APUIO0
@cc:    cmp APUIO0
        bne @cc
        lda f:apu_prog
        xba
        lda #$00
        ldx #$0001
        bra @start
@send:
        xba
        lda f:apu_prog,x
        inx
        xba
@w1:    cmp APUIO0
        bne @w1
        ina
@start:
        rep #$20
.a16
        sta APUIO0
        sep #$20
.a8
        cpx #(apu_prog_end - apu_prog)
        bcc @send
@w2:    cmp APUIO0
        bne @w2
        ina
        ina
        stz APUIO1
        ldx #$0200
        stx APUIO2
        sta APUIO0
@w3:    cmp APUIO0
        bne @w3
        stz APUIO0
        rts

apu_prog:
        .byte $8F,$6C,$F2
        .byte $8F,$20,$F3
        .byte $8F,$0C,$F2
        .byte $8F,$7F,$F3
        .byte $8F,$1C,$F2
        .byte $8F,$7F,$F3
        .byte $2F,$FE
apu_prog_end:

str_title:  .byte "HX421 DRAIN POINTER", 0
str_hdr:    .byte "DRAIN  LO MID HI", 0
str_r1:     .byte "R1", 0
str_r2:     .byte "R2", 0

.include "textmode.inc"

stop:   bra stop

; sine wavetable at file 0x2000 (= raw PSRAM MIX_WAVE_BASE) so the mixer plays +
; the drain advances.
.segment "WAVE"
.include "sine_words.inc"

.segment "VECTORS"
        .word stop, stop, stop, stop, stop, stop, stop, stop
        .word stop, stop, stop, stop
        .word reset
        .word stop
