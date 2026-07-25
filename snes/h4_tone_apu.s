; ============================================================
;  h4_tone_apu.s — HX-421 audio-seam final step (H4a)
;
;  The tone core drives correct, full-volume I2S (proven: sim + the on-hardware
;  diagnostic + the base-slot MSU test where the tone WAS audible). The only
;  reason the plain OBC1 probe was silent: a bare ROM never initializes the SNES
;  APU/DSP, so the console leaves its audio output muted -- and the cartridge
;  audio-in (where the MSU DAC and our tone inject) rides through that same muted
;  stage. An MSU-1 game unmutes it as a side effect; this ROM does it directly.
;
;  It uploads a 20-byte SPC-700 program that clears the DSP soft-reset/mute (FLG)
;  and sets main volume to max, then loops. No FPGA change -- the tone already
;  plays; this just opens the console's audio gate.
;
;  If the tone is now heard: DONE. The seam works end to end.
;
;  APU boot handshake follows the verified pvsneslib spcBoot protocol.
;  Public domain (CC0). No warranty.
; ============================================================

.p816
.smart +

APUIO0 = $2140
APUIO1 = $2141
APUIO2 = $2142
APUIO3 = $2143

SIG    = $60

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
        sta $2100               ; forced blank
        stz $4200
        stz $420C
        stz $212C
        stz $212D
        stz $2130
        stz $2131
        stz $2133

        ; ---- bring up the SNES APU/DSP (unmute the audio output) -----
        jsr apu_init

        ; ---- confirm the tone core is loaded (read the signature) -----
        rep #$30
.a16
.i16
        sep #$20
.a8
        ldx #$0000
@rd:    lda f:$3FF000,x
        sta SIG,x
        inx
        cpx #4
        bne @rd

        rep #$30
.a16
.i16
        jsr tm_setup

        lda #.loword(str_title)
        ldx #(2 | (2 << 8))
        jsr tm_print_str
        lda #.loword(str_listen)
        ldx #(2 | (4 << 8))
        jsr tm_print_str
        lda #.loword(str_sig)
        ldx #(2 | (6 << 8))
        jsr tm_print_str

        lda SIG+0
        and #$00FF
        ldx #(8  | (6 << 8))
        jsr tm_print_dec5
        lda SIG+1
        and #$00FF
        ldx #(14 | (6 << 8))
        jsr tm_print_dec5
        lda SIG+2
        and #$00FF
        ldx #(20 | (6 << 8))
        jsr tm_print_dec5
        lda SIG+3
        and #$00FF
        ldx #(26 | (6 << 8))
        jsr tm_print_dec5

        sep #$20
.a8
        lda #$01
        sta $212C
        lda #$0F
        sta $2100               ; screen on
spin:   bra spin


; ============================================================
;  apu_init — upload apu_prog to APU $0200 and run it (unmute DSP).
;  Enters/exits with A 8-bit, X/Y 16-bit. Verified pvsneslib handshake.
; ============================================================
.a8
.i16
apu_init:
        ; wait for the IPL 'ready' signal ($2140/$2141 = $AA/$BB)
@rdy:   ldx APUIO0              ; 16-bit read: $2141:$2140
        cpx #$BBAA
        bne @rdy
        stx APUIO1              ; port1 = $AA (nonzero "kind" = execute after)
        ldx #$0200
        stx APUIO2              ; port2/3 = transfer address $0200
        lda #$CC
        sta APUIO0              ; port0 = $CC (start)
@cc:    cmp APUIO0              ; wait for the SPC to echo $CC
        bne @cc

        ; first byte: counter=0, data=byte0, src index=1
        lda f:apu_prog
        xba                     ; B = data byte0
        lda #$00                ; A = counter 0
        ldx #$0001              ; source index
        bra @start
@send:
        xba                     ; A = prev data, B = counter
        lda f:apu_prog,x        ; A = next data byte
        inx
        xba                     ; A = counter, B = next data
@w1:    cmp APUIO0              ; wait for the SPC to ack the previous counter
        bne @w1
        ina                     ; counter++
@start:
        rep #$20
.a16
        sta APUIO0              ; port0 = counter (lo), port1 = data (hi)
        sep #$20
.a8
        cpx #(apu_prog_end - apu_prog)
        bcc @send

        ; all bytes sent: terminate and jump to $0200
@w2:    cmp APUIO0
        bne @w2
        ina
        ina                     ; counter + 2 = run terminator
        stz APUIO1              ; port1 = 0
        ldx #$0200
        stx APUIO2              ; port2/3 = entry point
        sta APUIO0              ; port0 = terminator
@w3:    cmp APUIO0
        bne @w3
        stz APUIO0
        rts

; The SPC-700 payload, loaded at $0200 and executed there:
;   MOV $F2,#$6C : DSPADDR = FLG
;   MOV $F3,#$20 : FLG = unmute + un-reset + echo-writes-off
;   MOV $F2,#$0C / $F3,#$7F : MVOL_L = max
;   MOV $F2,#$1C / $F3,#$7F : MVOL_R = max
;   BRA *        : loop forever (keep the SPC running)
apu_prog:
        .byte $8F,$6C,$F2
        .byte $8F,$20,$F3
        .byte $8F,$0C,$F2
        .byte $8F,$7F,$F3
        .byte $8F,$1C,$F2
        .byte $8F,$7F,$F3
        .byte $2F,$FE
apu_prog_end:


str_title:  .byte "HX421 H4a TONE + APU", 0
str_listen: .byte "APU UNMUTED - LISTEN FOR TONE", 0
str_sig:    .byte "SIG", 0

.include "textmode.inc"


; ---- vectors -----------------------------------------------------------
stop:   bra stop

.segment "VECTORS"
        .word stop, stop, stop, stop, stop, stop, stop, stop
        .word stop, stop, stop, stop
        .word reset             ; $FFFC RESET
        .word stop
