; ============================================================
;  h1_probe.s — hardware bring-up milestone H1 probe ROM
;
;  A minimal standalone LoROM image whose header declares HX-421
;  (map $30, carttype $E4) so the FXPak loads /sd2snes/fpga_hx421.bi3 —
;  which for this experiment is OUR bitstream. See docs/bringup.md.
;
;  THE SCREEN IS THE DIAGNOSTIC. The FXPak Pro is a sealed cartridge, so
;  the sd2snes status LEDs (and led_panic's blink codes) are inside the
;  shell and unobservable. Everything this ROM does is therefore aimed at
;  producing three states that can be told apart by eye:
;
;      BLACK screen    -> the ROM never executed
;      SOLID RED       -> reset + init ran, but the main loop is stuck
;      CYCLING COLOUR  -> the 65816 is executing our code continuously
;
;  Solid-vs-cycling is the distinction that matters: a static screen
;  cannot be told apart from a frozen CPU, so a colour is set during the
;  initial forced blank (always safe) and only then does the loop animate.
;
;  CGRAM IS WRITTEN IN V-BLANK, for a stable picture rather than for
;  correctness. Measured on real hardware 2026-07-21: a mid-frame CGRAM
;  write is NOT dropped — it takes effect at the raster position it lands
;  on. An unsynced loop therefore paints horizontal colour bands that
;  drift up or down the screen as the loop period beats against the frame.
;  (I predicted those writes would be discarded and the screen would stay
;  black. Wrong: the first build of this probe ran on hardware and showed
;  exactly that drifting band.) Syncing to v-blank trades the drift for a
;  clean whole-screen colour, which is easier to read at a glance — but
;  the drifting version is the livelier proof that the CPU is running.
;
;  H1 does not require the cycling state. baseline_mini may well not map a
;  ROM at all, in which case black is an expected result — but that must
;  be established by comparison against the STOCK core, not assumed.
;
;  Public domain (CC0). No warranty.
; ============================================================

.p816
.smart +

.segment "CODE"

reset:
        sei
        clc
        xce                     ; emulation -> native
        rep #$38                ; A/X/Y 16-bit, decimal off
.a16
.i16
        ldx #$01FF
        txs                     ; stack
        lda #$0000
        tcd                     ; direct page = $0000
        phk
        plb                     ; data bank = $00, for $21xx/$42xx

        sep #$20
.a8
        lda #$8F                ; forced blank
        sta $2100
        stz $4200               ; NMI off, auto-joypad off

        ; --- quiet every PPU feature that could hide the backdrop ---
        stz $2101               ; OBSEL
        stz $2105               ; BGMODE 0
        stz $2106               ; MOSAIC
        stz $2107               ; BG1SC
        stz $210B               ; BG12NBA
        stz $212C               ; TM  — no layers on main screen
        stz $212D               ; TS  — none on sub
        stz $2123               ; window mask designation
        stz $2124
        stz $2125
        stz $2126               ; window positions
        stz $2127
        stz $2128
        stz $2129
        stz $212A               ; window logic
        stz $212B
        stz $212E               ; window main/sub disable
        stz $212F
        stz $2130               ; CGWSEL — no colour math
        stz $2131               ; CGADSUB
        stz $2133               ; SETINI

        ; --- backdrop = bright red, written under FORCED BLANK ---
        ; This is the "the ROM got here" signal. Safe because the screen
        ; is blanked: CGRAM always accepts writes in that state.
        stz $2121               ; CGADD = 0 (backdrop)
        lda #$1F
        sta $2122               ; BGR555 low  — red 31
        lda #$00
        sta $2122               ; BGR555 high

        lda #$0F                ; screen on, full brightness -> RED
        sta $2100

        rep #$30
.a16
.i16
        ldy #$0000              ; colour phase

loop:
        ; --- wait for the START of v-blank -------------------------------
        ; Leave any v-blank we are already inside first, or a single long
        ; v-blank would be mistaken for many and the cycle would race.
        sep #$20
.a8
@wait_active:
        lda $4212
        bmi @wait_active        ; bit 7 set = in v-blank; wait for active
@wait_vblank:
        lda $4212
        bpl @wait_vblank        ; wait for v-blank to begin

        ; --- in v-blank: CGRAM writes are accepted --------------------
        stz $2121               ; CGADD = 0
        rep #$20
.a16
        tya
        lsr
        lsr
        lsr                     ; slow the cycle to ~8 frames per step
        sep #$20
.a8
        sta $2122               ; low byte
        rep #$20
.a16
        tya
        lsr
        lsr
        lsr
        lsr
        lsr
        lsr
        sep #$20
.a8
        sta $2122               ; high byte
        rep #$30
.a16
.i16

        iny
        bra loop


; ---- vectors -------------------------------------------------------
; Everything but RESET parks in a spin, so a stray interrupt stops
; visibly (screen freezes on its current colour) rather than running off
; into unmapped space and producing a black screen that would be
; misread as "the ROM never ran".
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
