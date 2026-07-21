; ============================================================
;  h1_probe.s — hardware bring-up milestone H1 probe ROM
;
;  A minimal standalone LoROM image whose header declares OBC1
;  (map $30, carttype $25) so the FXPak loads /sd2snes/fpga_obc1.bi3 —
;  which for this experiment is OUR bitstream. See docs/bringup.md.
;
;  What it does if it runs at all: cycles the backdrop colour through a
;  slow rainbow. ANIMATION is the point, not colour — a static screen
;  cannot be told apart from a frozen CPU or a stale framebuffer, but
;  something visibly cycling can only be the 65816 executing our ROM.
;
;  H1 does NOT require this to run. baseline_mini almost certainly does
;  not map a ROM, so a black screen is an expected pass as long as the
;  LEDs show no panic blink. This ROM is what turns a *better than
;  expected* result into a legible one.
;
;  Public domain (CC0). No warranty.
; ============================================================

.p816
.smart +

.segment "CODE"

reset:
        sei
        clc
        xce                     ; native mode
        rep #$30                ; A/X/Y 16-bit
.a16
.i16
        ldx #$1FFF
        txs

        sep #$20                ; A 8-bit
.a8
        lda #$8F                ; force blank, full brightness
        sta $2100

        ; --- silence everything the PPU might otherwise show ---
        stz $2101               ; OBSEL
        stz $2105               ; BGMODE 0
        stz $2106               ; MOSAIC
        stz $210B               ; BG12NBA
        stz $212C               ; TM: no layers on main
        stz $212D               ; TS
        stz $2123               ; window mask
        stz $2124
        stz $2125
        stz $2130               ; colour math off
        stz $2131
        lda #$E0
        sta $2132               ; fixed colour = black

        lda #$0F                ; screen on, full brightness
        sta $2100

        rep #$30
.a16
.i16
        ldy #$0000              ; colour phase

loop:
        ; backdrop colour 0 = a slow walk through BGR555
        sep #$20
.a8
        stz $2121               ; CGADD = 0 (backdrop)
        tya
        sta $2122               ; low byte
        rep #$20
.a16
        tya
        lsr
        lsr
        lsr
        sep #$20
.a8
        sta $2122               ; high byte
        rep #$20
.a16

        ; crude delay so the cycle is watchable rather than a blur
        ldx #$2000
delay:
        dex
        bne delay

        iny
        bra loop


; ---- vectors -------------------------------------------------------
; Everything but RESET points at a spin, so a stray interrupt parks
; visibly rather than running off into unmapped space.
stop:   bra stop

.segment "VECTORS"
        ; native
        .word stop              ; $FFE4 COP
        .word stop              ; $FFE6 BRK
        .word stop              ; $FFE8 ABORT
        .word stop              ; $FFEA NMI
        .word stop              ; $FFEC unused
        .word stop              ; $FFEE IRQ
        .word stop, stop        ; $FFF0, $FFF2
        ; emulation
        .word stop              ; $FFF4 COP
        .word stop              ; $FFF6 unused
        .word stop              ; $FFF8 ABORT
        .word stop              ; $FFFA NMI
        .word reset             ; $FFFC RESET
        .word stop              ; $FFFE IRQ/BRK
