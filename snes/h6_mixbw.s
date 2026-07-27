; ============================================================
;  h6_mixbw.s — isolate "chord mixing" from "full-WRAM execution".
;
;  Same idea as h6_wram (give the mixer PSRAM bandwidth) but the CPU stays
;  EXECUTING FROM ROM — it just spends most cycles doing WRAM writes (stz), which
;  are non-ROM cycles => free_strobe => the mixer gets bus time. Crucially the CPU
;  still fetches opcodes from ROM every loop (the bra), so the ROM bus is NOT
;  silent, unlike h6_wram's pure-WRAM BRA* loop.
;
;  Run this on the SAME core that gave static under h6_wram:
;    - clean chord/tone here  => the mixing is fine; h6_wram's TOTAL absence of ROM
;      accesses is what breaks the mixer (full-WRAM finding).
;    - still static here      => the mixer output itself is bad on this core
;      (independent of WRAM execution).
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
        sta $2100                       ; screen on

        ; ---- mixer-bandwidth loop: many WRAM writes (free slots) but the CPU
        ; keeps fetching from ROM (the bra). ROM bus stays active, unlike h6_wram.
mixbw:
        stz $0000
        stz $0001
        stz $0002
        stz $0003
        stz $0004
        stz $0005
        stz $0006
        stz $0007
        stz $0008
        stz $0009
        stz $000A
        stz $000B
        stz $000C
        stz $000D
        stz $000E
        stz $000F
        bra mixbw


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

str_title:  .byte "HX421 MIXER BANDWIDTH", 0
str_hint:   .byte "ROM EXEC + WRAM WRITES - LISTEN", 0

.include "textmode.inc"

stop:   bra stop

.segment "WAVE"
.include "sine_words.inc"

.segment "VECTORS"
        .word stop, stop, stop, stop, stop, stop, stop, stop
        .word stop, stop, stop, stop
        .word reset
        .word stop
