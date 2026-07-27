; ============================================================
;  h6_wram.s — execute-from-WRAM: give the mixer FULL PSRAM bandwidth.
;
;  The drain test proved the mixer starves when the 65816 hammers the cart ROM
;  bus (a ROM spin loop). The HX-421 model runs the engine from WRAM instead: the
;  CPU fetches from $7E WRAM (internal to the SNES), so the cart ROM/PSRAM bus is
;  100% free for the FPGA mixer -> free_strobe every cycle -> the mixer runs at the
;  full 44.1 kHz rate.
;
;  This boots, plays the 128-sample sine from PSRAM (as 6b.1b), copies a tiny loop
;  into WRAM, and jml's into it. If the sine now plays as a CLEAN, STEADY ~344 Hz
;  tone (vs. the warbly/stuttery sine when running from ROM), the WRAM model gives
;  the mixer its bandwidth -- the foundation the streaming demo needs.
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
        sta $2100                       ; screen on (static display below)

        ; ---- copy a spin loop into WRAM and run from there ----
        ; $7E:2000 <- BRA * (80 FE). Fetching it from WRAM keeps the cart ROM bus
        ; free for the mixer. jml into WRAM; the SNES never touches ROM again.
        lda #$80
        sta f:$7E2000
        lda #$FE
        sta f:$7E2001
        jml $7E2000


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
        ; --- silence the DSP so only the cart (mixer) audio is heard ---
        .byte $8F,$4C,$F2   ; DSPADDR = KON
        .byte $8F,$00,$F3   ; KON = 0  (key on nothing)
        .byte $8F,$5C,$F2   ; DSPADDR = KOFF
        .byte $8F,$FF,$F3   ; KOFF = FF (key OFF all 8 voices -> no garbage voices)
        .byte $8F,$2C,$F2   ; DSPADDR = EVOL_L (echo out L)
        .byte $8F,$00,$F3   ; = 0
        .byte $8F,$3C,$F2   ; DSPADDR = EVOL_R (echo out R)
        .byte $8F,$00,$F3   ; = 0
        ; --- unmute + master volume ---
        .byte $8F,$6C,$F2   ; DSPADDR = FLG
        .byte $8F,$20,$F3   ; = 20 (unmute, echo-buffer write disabled, no soft reset)
        .byte $8F,$0C,$F2   ; DSPADDR = MVOL_L
        .byte $8F,$7F,$F3   ; = 7F
        .byte $8F,$1C,$F2   ; DSPADDR = MVOL_R
        .byte $8F,$7F,$F3   ; = 7F
        .byte $2F,$FE       ; loop forever
apu_prog_end:

str_title:  .byte "HX421 STEREO TEST", 0
str_hint:   .byte "L=LOW-LEFT  R=HIGH-RIGHT", 0

.include "textmode.inc"

stop:   bra stop

; sine wavetable at file 0x2000 (= raw PSRAM MIX_WAVE_BASE) so the mixer plays it.
.segment "WAVE"
.include "stereo_words.inc"

.segment "VECTORS"
        .word stop, stop, stop, stop, stop, stop, stop, stop
        .word stop, stop, stop, stop
        .word reset
        .word stop
