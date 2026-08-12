; ============================================================
;  hmbox.s — hardware bring-up: the HX-421 media command mailbox ($3F:F1xx).
;
;  Our core adds a 256-byte SNES->cart mailbox: the 65816 byte-writes a command
;  block into $3F:F100.., writes the DOORBELL at $3F:F1FF, and the STM32 reads it
;  back. This ROM exercises the SNES half in isolation (no STM32): write four
;  bytes + doorbell, read them back, and read the pending flag at $3F:F1FD.
;
;  Three distinguishable outcomes (the read bytes are shown raw so they cannot be
;  confused):
;      255 255 255 255            -> our core is NOT loaded (ROM filler)
;      0   0   0   0   (pend 0)   -> core loaded, but SNES writes are NOT reaching
;                                    the mailbox (the interesting failure)
;      17  34  51  68  (pend 1)   -> MAILBOX OK — the whole write/doorbell/read
;                                    path works on silicon
;
;  Load on BASE firmware by overriding the OBC1 core (header carttype 0x25 ->
;  /sd2snes/fpga_obc1.bi3). Public domain (CC0). No warranty.
; ============================================================

.p816
.smart +

MB    = $60             ; 4 bytes read back from the mailbox
PEND  = $64             ; pending flag read back
MATCH = $65

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

        ; ---- write the command block + doorbell ---------------------
        ; Plain long stores into cart bank $3F. On the stock core these go
        ; nowhere; our fabric latches them into the mailbox BRAM.
        lda #$11
        sta f:$3FF100
        lda #$22
        sta f:$3FF101
        lda #$33
        sta f:$3FF102
        lda #$44
        sta f:$3FF103
        lda #$AA
        sta f:$3FF1FF           ; DOORBELL (any non-zero) -> pending

        ; ---- read it back -------------------------------------------
        lda f:$3FF100
        sta MB+0
        lda f:$3FF101
        sta MB+1
        lda f:$3FF102
        sta MB+2
        lda f:$3FF103
        sta MB+3
        lda f:$3FF1FD           ; pending status
        sta PEND

        ; ---- verdict: bytes match AND pending bit0 set --------------
        lda #1
        sta MATCH
        lda MB+0
        cmp #$11
        beq :+
        stz MATCH
:       lda MB+1
        cmp #$22
        beq :+
        stz MATCH
:       lda MB+2
        cmp #$33
        beq :+
        stz MATCH
:       lda MB+3
        cmp #$44
        beq :+
        stz MATCH
:       lda PEND
        and #$01
        bne :+
        stz MATCH
:

        rep #$30
.a16
.i16
        jsr tm_setup

        lda #.loword(str_title)
        ldx #(2 | (2 << 8))
        jsr tm_print_str
        lda #.loword(str_read)
        ldx #(2 | (5 << 8))
        jsr tm_print_str
        lda #.loword(str_pend)
        ldx #(2 | (6 << 8))
        jsr tm_print_str

        ; the four read bytes (unrolled — the same register discipline as h2_probe)
        lda MB+0
        and #$00FF
        ldx #(8  | (5 << 8))
        jsr tm_print_dec5
        lda MB+1
        and #$00FF
        ldx #(14 | (5 << 8))
        jsr tm_print_dec5
        lda MB+2
        and #$00FF
        ldx #(20 | (5 << 8))
        jsr tm_print_dec5
        lda MB+3
        and #$00FF
        ldx #(26 | (5 << 8))
        jsr tm_print_dec5

        lda PEND
        and #$00FF
        ldx #(8 | (6 << 8))
        jsr tm_print_dec5

        ; ---- headline ----------------------------------------------
        lda MATCH
        and #$00FF
        beq @fail
        lda #.loword(str_ok)
        bra @verdict
@fail:  lda #.loword(str_bad)
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


str_title:  .byte "HX421 MAILBOX 3F:F1xx", 0
str_read:   .byte "READ", 0
str_pend:   .byte "PEND", 0
str_ok:     .byte "MAILBOX OK  WRITE+DOORBELL", 0
str_bad:    .byte "MAILBOX FAIL (SEE READ/PEND)", 0

.include "textmode.inc"


; ---- vectors -----------------------------------------------------------
stop:   bra stop

.segment "VECTORS"
        .word stop, stop, stop, stop, stop, stop, stop, stop
        .word stop, stop, stop, stop
        .word reset             ; $FFFC RESET
        .word stop
