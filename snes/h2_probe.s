; ============================================================
;  h2_probe.s — hardware bring-up milestone H2
;
;  H1 proved a bitstream from our Quartus flow configures on real hardware.
;  It did NOT prove any logic we wrote runs, because a stock core would have
;  behaved identically. This does.
;
;  The h2_sig core intercepts four cartridge reads at $F000-$F003 and serves
;  them from the fabric: 'H','X','4','2'. In a LoROM image that address is
;  file offset $7000, which this ROM leaves as $FF filler. So the SAME ROM
;  reads a different thing depending on which core is loaded:
;
;      $FF $FF $FF $FF   -> stock core (or ours not loaded)
;      $48 $58 $34 $32   -> our logic is running
;
;  One ROM, two outcomes, and the failing case is the ROM's own filler — so
;  there is no way to mistake "our core did nothing" for "the test did not
;  run". A header declaring HX-421 ($30/$E4) selects /sd2snes/fpga_hx421.bi3.
;
;  Public domain (CC0). No warranty.
; ============================================================

.p816
.smart +

SIG   = $60             ; 4 bytes read back from the window
MATCH = $64

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

        ; ---- read the signature window ------------------------------
        ; Plain CPU reads of cartridge space. Nothing here is special; the
        ; whole point is that these four addresses look ordinary to the ROM
        ; and are answered by the FPGA instead of by PSRAM.
        ; Long addressing into BANK $3F. The window is deliberately confined
        ; to a bank the FXPak menu never touches: an earlier version matched
        ; $F000 in every bank and corrupted the menu's own reads, which hung
        ; the cart on "Loading ..." and was indistinguishable from a
        ; bitstream that would not configure.
        ldx #$0000
@rd:    lda f:$3FF000,x
        sta SIG,x
        inx
        cpx #4
        bne @rd

        ; ---- verdict ------------------------------------------------
        lda #1
        sta MATCH
        ldx #$0000
@cmp:   lda SIG,x
        cmp sig_want,x
        beq :+
        stz MATCH
:       inx
        cpx #4
        bne @cmp

        rep #$30
.a16
.i16
        jsr tm_setup

        lda #.loword(str_title)
        ldx #(2 | (2 << 8))
        jsr tm_print_str

        lda #.loword(str_got)
        ldx #(2 | (5 << 8))
        jsr tm_print_str
        lda #.loword(str_want)
        ldx #(2 | (6 << 8))
        jsr tm_print_str

        ; The four bytes read, then the four expected. Unrolled: it is only
        ; four entries, `lda dp,y` is not a 65816 addressing mode, and using
        ; X for both the index and the screen position is precisely the
        ; register juggling that has broken every display loop this session.
        lda SIG+0
        and #$00FF
        ldx #(8  | (5 << 8))
        jsr tm_print_dec5
        lda SIG+1
        and #$00FF
        ldx #(14 | (5 << 8))
        jsr tm_print_dec5
        lda SIG+2
        and #$00FF
        ldx #(20 | (5 << 8))
        jsr tm_print_dec5
        lda SIG+3
        and #$00FF
        ldx #(26 | (5 << 8))
        jsr tm_print_dec5

        lda sig_want+0
        and #$00FF
        ldx #(8  | (6 << 8))
        jsr tm_print_dec5
        lda sig_want+1
        and #$00FF
        ldx #(14 | (6 << 8))
        jsr tm_print_dec5
        lda sig_want+2
        and #$00FF
        ldx #(20 | (6 << 8))
        jsr tm_print_dec5
        lda sig_want+3
        and #$00FF
        ldx #(26 | (6 << 8))
        jsr tm_print_dec5

        ; ---- the headline -------------------------------------------
        lda MATCH
        and #$00FF
        beq @stock
        lda #.loword(str_pass)
        bra @verdict
@stock: lda #.loword(str_fail)
@verdict:
        ldx #(2 | (9 << 8))
        jsr tm_print_str

        sep #$20
.a8
        lda #$01
        sta $212C               ; BG1 on the main screen
        lda #$0F
        sta $2100               ; screen on
spin:   bra spin


sig_want:   .byte 'H', 'X', '4', '2'

str_title:  .byte "HX421 H2 SIGNATURE", 0
str_got:    .byte "READ", 0
str_want:   .byte "WANT", 0
str_pass:   .byte "OUR LOGIC IS RUNNING", 0
str_fail:   .byte "STOCK CORE  NOT OURS", 0

.include "textmode.inc"


; ---- vectors -----------------------------------------------------------
stop:   bra stop

.segment "VECTORS"
        .word stop, stop, stop, stop, stop, stop, stop, stop
        .word stop, stop, stop, stop
        .word reset             ; $FFFC RESET
        .word stop
