; ============================================================
;  h4_diag.s — HX-421 audio-seam diagnostic (H4a debug)
;
;  The tone core loads (H2 signature reads) but no tone is heard. A sealed cart
;  can't be scoped, so the tone core exports live counters + sticky evidence
;  flags for each stage of the DAC seam through the read window $3F:F000-F007:
;
;      F000-F003  signature  'H','X','4','2'   (core-loaded check, unchanged)
;      F004       TICK   ++ each 44.1 kHz sample tick   (phase acc + SNES_SYSCLK)
;      F005       TONE   ++ each square edge             (tone generator advancing)
;      F006       SDO    ++ each sdout transition        (I2S serializing data)
;      F007       STAT   sticky bits: b0 tick_seen b1 tone_seen b2 sdout_seen
;                                     b3 smp_nonzero  b4 vol_ramped
;
;  This ROM reads the window, waits ~0.2 s, reads F004-F007 again, and shows
;  both. Whichever counter STOPS changing (or whichever STAT bit stays 0) is the
;  stage that is dead on hardware:
;
;      TICK frozen           -> sample tick not firing (sysclk / phase acc / reset)
;      TICK moves, SDO frozen -> data not reaching the I2S shifter
;      all move, STAT = $1F   -> FPGA is emitting real audio; fault is analog/DAC
;
;  Public domain (CC0). No warranty.
; ============================================================

.p816
.smart +

SIG   = $60             ; 8 bytes: F000-F007, first read
SIG2  = $70             ; 4 bytes: F004-F007, second read

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

        ; ---- first read: all 8 window bytes ------------------------
        ldx #$0000
@rd1:   lda f:$3FF000,x
        sta SIG,x
        inx
        cpx #8
        bne @rd1

        ; ---- delay ~0.2 s so the free-running counters advance ------
        ; TICK wraps every ~5.8 ms, so any counter that is alive will read a
        ; different value on the second pass; a dead one stays put.
        ldy #$0004
@dly1:  ldx #$0000
@dly0:  dex
        bne @dly0
        dey
        bne @dly1

        ; ---- second read: just the four diagnostic bytes -----------
        ldx #$0000
@rd2:   lda f:$3FF004,x
        sta SIG2,x
        inx
        cpx #4
        bne @rd2

        ; ---- display -----------------------------------------------
        rep #$30
.a16
.i16
        jsr tm_setup

        lda #.loword(str_title)
        ldx #(2 | (2 << 8))
        jsr tm_print_str

        lda #.loword(str_sig)
        ldx #(2 | (4 << 8))
        jsr tm_print_str
        lda #.loword(str_hdr)
        ldx #(2 | (6 << 8))
        jsr tm_print_str
        lda #.loword(str_r1)
        ldx #(2 | (7 << 8))
        jsr tm_print_str
        lda #.loword(str_r2)
        ldx #(2 | (8 << 8))
        jsr tm_print_str

        ; signature bytes F000-F003 at row 4
        lda SIG+0
        and #$00FF
        ldx #(8  | (4 << 8))
        jsr tm_print_dec5
        lda SIG+1
        and #$00FF
        ldx #(14 | (4 << 8))
        jsr tm_print_dec5
        lda SIG+2
        and #$00FF
        ldx #(20 | (4 << 8))
        jsr tm_print_dec5
        lda SIG+3
        and #$00FF
        ldx #(26 | (4 << 8))
        jsr tm_print_dec5

        ; read-1 diagnostics F004-F007 at row 7 (cols 8/14/20/26)
        lda SIG+4
        and #$00FF
        ldx #(8  | (7 << 8))
        jsr tm_print_dec5
        lda SIG+5
        and #$00FF
        ldx #(14 | (7 << 8))
        jsr tm_print_dec5
        lda SIG+6
        and #$00FF
        ldx #(20 | (7 << 8))
        jsr tm_print_dec5
        lda SIG+7
        and #$00FF
        ldx #(26 | (7 << 8))
        jsr tm_print_dec5

        ; read-2 diagnostics at row 8
        lda SIG2+0
        and #$00FF
        ldx #(8  | (8 << 8))
        jsr tm_print_dec5
        lda SIG2+1
        and #$00FF
        ldx #(14 | (8 << 8))
        jsr tm_print_dec5
        lda SIG2+2
        and #$00FF
        ldx #(20 | (8 << 8))
        jsr tm_print_dec5
        lda SIG2+3
        and #$00FF
        ldx #(26 | (8 << 8))
        jsr tm_print_dec5

        sep #$20
.a8
        lda #$01
        sta $212C               ; BG1 on the main screen
        lda #$0F
        sta $2100               ; screen on
spin:   bra spin


str_title:  .byte "HX421 H4a AUDIO DIAG", 0
str_sig:    .byte "SIG", 0
str_hdr:    .byte "      TICK  TONE   SDO  STAT", 0
str_r1:     .byte "R1", 0
str_r2:     .byte "R2", 0

.include "textmode.inc"


; ---- vectors -----------------------------------------------------------
stop:   bra stop

.segment "VECTORS"
        .word stop, stop, stop, stop, stop, stop, stop, stop
        .word stop, stop, stop, stop
        .word reset             ; $FFFC RESET
        .word stop
