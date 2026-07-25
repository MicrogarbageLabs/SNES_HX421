; ============================================================
;  h6_probe.s — HX-421 6b PSRAM-read probe (H4b build + MIX_RD requestor).
;
;  The 6b core adds the mixer as a fifth ROM-bus requestor that reads a fixed
;  PSRAM byte offset (0x1000) over the SNES ROM bus in free slots. This ROM
;  verifies that integration ON HARDWARE (the main.v change can't be sim'd):
;
;   * The mixer's BRAM sine still plays (APU-unmuted here) and the SNES does not
;     crash  -> the new requestor didn't break the ROM bus.
;   * F008/F009 = the value the probe read from PSRAM byte 0x1000; F00A = a read
;     counter (must change between two reads -> the requestor is cycling).
;   * $00:9000 is the SAME PSRAM location (LoROM: bank0 $9000 -> file 0x1000).
;     Reading it here and comparing tells us the read is correct + the byte order:
;     per the ROM byte-lane wiring we expect MIX high byte == SNES low byte and
;     MIX low byte == SNES high byte (a byte-swap) -> shown as MATCH.
;
;  Public domain (CC0). No warranty.
; ============================================================

.p816
.smart +

APUIO0 = $2140
APUIO1 = $2141
APUIO2 = $2142
APUIO3 = $2143

SIG    = $60          ; F000-F003
MIXLO  = $64          ; F008
MIXHI  = $65          ; F009
CNT1   = $66          ; F00A first read
CNT2   = $67          ; F00A second read
SNLO   = $68          ; $9000 low
SNHI   = $69          ; $9000 high

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

        jsr apu_init                    ; unmute so the BRAM sine is audible

        ; ---- read the diagnostic window + the mirror location -------
        sep #$20
.a8
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
        sta CNT1
        ; SNES read of the same PSRAM location (bank0 $9000 -> file 0x1000)
        lda f:$009000
        sta SNLO
        lda f:$009001
        sta SNHI
        ; small delay, then read the probe counter again (must change)
        ldx #$4000
@dl:    dex
        bne @dl
        lda f:$3FF00A
        sta CNT2

        ; ---- display -----------------------------------------------
        rep #$30
.a16
.i16
        jsr tm_setup
        lda #.loword(str_title)
        ldx #(2 | (2 << 8))
        jsr tm_print_str
        lda #.loword(str_mix)
        ldx #(2 | (4 << 8))
        jsr tm_print_str
        lda #.loword(str_snes)
        ldx #(2 | (6 << 8))
        jsr tm_print_str

        lda MIXLO
        and #$00FF
        ldx #(10 | (4 << 8))
        jsr tm_print_dec5
        lda MIXHI
        and #$00FF
        ldx #(16 | (4 << 8))
        jsr tm_print_dec5
        lda CNT1
        and #$00FF
        ldx #(22 | (4 << 8))
        jsr tm_print_dec5
        lda CNT2
        and #$00FF
        ldx #(28 | (4 << 8))
        jsr tm_print_dec5

        lda SNLO
        and #$00FF
        ldx #(10 | (6 << 8))
        jsr tm_print_dec5
        lda SNHI
        and #$00FF
        ldx #(16 | (6 << 8))
        jsr tm_print_dec5

        ; verdict: count changed AND byte-swap match
        sep #$20
.a8
        lda #1
        sta $70                          ; assume OK
        lda CNT1
        cmp CNT2
        bne :+
        stz $70                          ; count didn't change -> not live
:       lda MIXHI
        cmp SNLO
        beq :+
        stz $70
:       lda MIXLO
        cmp SNHI
        beq :+
        stz $70
:
        rep #$30
.a16
.i16
        lda $70
        and #$00FF
        beq @bad
        lda #.loword(str_ok)
        bra @vd
@bad:   lda #.loword(str_bad)
@vd:    ldx #(2 | (9 << 8))
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

str_title:  .byte "HX421 6b PSRAM PROBE", 0
str_mix:    .byte "MIX  LO    HI    CNT1  CNT2", 0
str_snes:   .byte "SNES LO    HI", 0
str_ok:     .byte "READS LIVE + BYTESWAP MATCH", 0
str_bad:    .byte "MISMATCH - SEE NUMBERS", 0

.include "textmode.inc"

stop:   bra stop

.segment "VECTORS"
        .word stop, stop, stop, stop, stop, stop, stop, stop
        .word stop, stop, stop, stop
        .word reset
        .word stop
