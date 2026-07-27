; ============================================================
;  h6_rdchk.s — show what the mixer actually reads from PSRAM.
;
;  Constant wavetable ($1000 every sample) at PSRAM 0x2000. Raw ROM_DATA for a
;  $1000 little-endian word is $0010, so the diagnostic window MIX_DINr ($3F:F008
;  low / F009 high) should read low=16 high=0 on EVERY sample if the read path is
;  correct. This samples MIX_DINr four times (with WRAM-write delays between, so the
;  mixer performs fresh reads) and prints all four as decimal low/high pairs.
;
;    all four = "16   0"  -> the read is CORRECT (buzz is elsewhere)
;    varying / not 16,0   -> the read returns garbage; the values hint at the cause
;                            (SNES-bus-like values = bus capture; random = timing)
;
;  Public domain (CC0). No warranty.
; ============================================================

.p816
.smart +

APUIO0 = $2140
APUIO1 = $2141
APUIO2 = $2142
APUIO3 = $2143

V0L=$60
V0H=$61
V1L=$62
V1H=$63
V2L=$64
V2H=$65
V3L=$66
V3H=$67

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

        ; ---- sample MIX_DINr four times, WRAM-write delay between each ----
        lda f:$3FF008
        sta V0L
        lda f:$3FF009
        sta V0H
        jsr rd_delay
        lda f:$3FF008
        sta V1L
        lda f:$3FF009
        sta V1H
        jsr rd_delay
        lda f:$3FF008
        sta V2L
        lda f:$3FF009
        sta V2H
        jsr rd_delay
        lda f:$3FF008
        sta V3L
        lda f:$3FF009
        sta V3H

        rep #$30
.a16
.i16
        jsr tm_setup
        lda #.loword(str_title)
        ldx #(2 | (2 << 8))
        jsr tm_print_str
        lda #.loword(str_hdr)
        ldx #(2 | (4 << 8))
        jsr tm_print_str

        ; row per sample: low then high at cols 8 / 14
        lda V0L
        and #$00FF
        ldx #(8  | (5 << 8))
        jsr tm_print_dec5
        lda V0H
        and #$00FF
        ldx #(14 | (5 << 8))
        jsr tm_print_dec5

        lda V1L
        and #$00FF
        ldx #(8  | (6 << 8))
        jsr tm_print_dec5
        lda V1H
        and #$00FF
        ldx #(14 | (6 << 8))
        jsr tm_print_dec5

        lda V2L
        and #$00FF
        ldx #(8  | (7 << 8))
        jsr tm_print_dec5
        lda V2H
        and #$00FF
        ldx #(14 | (7 << 8))
        jsr tm_print_dec5

        lda V3L
        and #$00FF
        ldx #(8  | (8 << 8))
        jsr tm_print_dec5
        lda V3H
        and #$00FF
        ldx #(14 | (8 << 8))
        jsr tm_print_dec5

        sep #$20
.a8
        lda #$01
        sta $212C
        lda #$0F
        sta $2100
spin:   bra spin


; give the mixer free slots (WRAM writes = non-ROM cycles) so it performs fresh
; reads between samples
.a8
.i16
rd_delay:
        ldx #$4000
@d:     stz $0000
        stz $0001
        stz $0002
        stz $0003
        dex
        bne @d
        rts


; ---- APU: silence DSP voices, then unmute (so the mixer output, if any, is clean) ----
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

str_title:  .byte "HX421 READ CHECK", 0
str_hdr:    .byte "MIX_DINR  LOW=16 HIGH=0 OK", 0

.include "textmode.inc"

stop:   bra stop

.segment "WAVE"
.include "const_words.inc"

.segment "VECTORS"
        .word stop, stop, stop, stop, stop, stop, stop, stop
        .word stop, stop, stop, stop
        .word reset
        .word stop
