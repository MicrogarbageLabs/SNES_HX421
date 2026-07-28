; ============================================================
;  h6_stereo_btn.s — interactive L/R separation test. Hold L -> left channel
;  (low tone) plays; hold R -> right channel (high tone) plays; release -> that
;  channel silences. Triggers each channel independently or together, to test
;  stereo separation by ear without relying on hearing both at once.
;
;  Mechanism: a WRAM-resident loop reads the auto-joypad ($4218), builds a 2-bit
;  mute mask (bit0 = ch0/left mute when L not held, bit1 = ch1/right mute when R
;  not held), and READS $3F:F010 + mask. main.v latches the low 2 address bits of
;  any $3F:F01x read into the mixer's ext_mute (proven read-window path). Runs from
;  WRAM so the mixer keeps full PSRAM bandwidth.
;
;  Flash the STEREO core (ch0 = 0x2000 L-lane hard-left, ch1 = 0x2002 R-lane
;  hard-right). Public domain (CC0). No warranty.
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

        jsr apu_init

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
        lda #$01
        sta $4200                       ; enable auto-joypad (NMI off)

        ; ---- copy the button loop into WRAM and run it there ----
        sep #$30
.i8
        lda #$FF
        sta $01                         ; last-mask sentinel -> first pass always writes
        ldx #$00
@cp:    lda f:wram_blob,x
        sta f:$7E2000,x
        inx
        cpx #(wram_blob_end - wram_blob)
        bne @cp
        jml $7E2000


; ---- WRAM-resident button loop (position-independent: relative branches, absolute
;      HW-reg + long cart reads only). Assembled here, copied to $7E:2000. ----
.a8
.i8
wram_blob:
        stz $00                         ; mask = 0
        lda $4218                       ; JOY1L: A X L R 0 0 0 0
        and #$20                        ; L (bit5) held?
        bne :+
        lda $00
        ora #$01                        ; L released -> mute ch0 (left)
        sta $00
:       lda $4218
        and #$10                        ; R (bit4) held?
        bne :+
        lda $00
        ora #$02                        ; R released -> mute ch1 (right)
        sta $00
:       lda $00
        cmp $01                         ; only touch the cart bus when the mask CHANGES,
        beq :+                          ; so the mixer keeps ~full PSRAM bandwidth
        sta $01
        tax
        lda f:$3FF010,x                 ; read $3F:F010+mask -> FPGA latches ext_mute
:       bra wram_blob
wram_blob_end:


; ---- APU: silence DSP voices, then unmute ----
.a8
.i16
apu_init:
        rep #$10
.i16
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
        .byte $8F,$4C,$F2   ; KON = 0
        .byte $8F,$00,$F3
        .byte $8F,$5C,$F2   ; KOFF = FF
        .byte $8F,$FF,$F3
        .byte $8F,$6C,$F2   ; FLG = 20 (unmute)
        .byte $8F,$20,$F3
        .byte $8F,$0C,$F2   ; MVOL_L = 7F
        .byte $8F,$7F,$F3
        .byte $8F,$1C,$F2   ; MVOL_R = 7F
        .byte $8F,$7F,$F3
        .byte $2F,$FE
apu_prog_end:

str_title:  .byte "HX421 STEREO BUTTONS", 0
str_hint:   .byte "HOLD L=LEFT(LOW)  R=RIGHT(HIGH)", 0

.include "textmode.inc"

stop:   bra stop

.segment "WAVE"
.include "stereo_words.inc"

.segment "VECTORS"
        .word stop, stop, stop, stop, stop, stop, stop, stop
        .word stop, stop, stop, stop
        .word reset
        .word stop
