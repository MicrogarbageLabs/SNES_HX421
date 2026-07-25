; ============================================================
;  h6_1b.s — HX-421 6b.1b: the FPGA mixer plays a sine sourced FROM PSRAM.
;
;  The 6b core's mixer now fetches its wavetable over the SNES ROM bus (MIX_RD,
;  free_strobe-gated) from raw PSRAM address MIX_WAVE_BASE = 0x2000. This ROM
;  places a 128-sample sine at file offset 0x2000 (WAVE segment = SNES $A000,
;  raw PSRAM 0x2000 by the linear load) and APU-unmutes so the mixer's output is
;  audible. If a clean ~344 Hz sine plays, the mixer is sourcing audio from PSRAM
;  through the shared bus. (Noise instead = flip the byte-swap in main.v.)
;
;  Public domain (CC0). No warranty.
; ============================================================

.p816
.smart +

APUIO0 = $2140
APUIO1 = $2141
APUIO2 = $2142
APUIO3 = $2143

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

        jsr apu_init                    ; unmute so the mixer output is audible

        rep #$30
.a16
.i16
        jsr tm_setup
        lda #.loword(str_title)
        ldx #(2 | (2 << 8))
        jsr tm_print_str
        lda #.loword(str_hint)
        ldx #(2 | (4 << 8))
        jsr tm_print_str

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

str_title:  .byte "HX421 6b.1b PSRAM SINE", 0
str_hint:   .byte "MIXER READS WAVETABLE FROM PSRAM", 0

.include "textmode.inc"

stop:   bra stop

; ---- the sine wavetable, pinned at file offset 0x2000 (raw PSRAM 0x2000) ----
.segment "WAVE"
.include "sine_words.inc"

.segment "VECTORS"
        .word stop, stop, stop, stop, stop, stop, stop, stop
        .word stop, stop, stop, stop
        .word reset
        .word stop
