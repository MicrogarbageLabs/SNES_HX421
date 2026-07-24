; ============================================================
;  textmode.inc — minimal BG1 text output for hardware test ROMs
;
;  Mode 0, BG1 2bpp, an 8x8 font covering space + 0-9 + A-Z. Enough to put
;  numbers and short labels on screen, which is the only diagnostic channel
;  a sealed FXPak Pro has: its status LEDs are inside the cartridge shell.
;
;  Contract:
;    - direct page $E0-$EF belongs to this file; the includer must not use it
;    - call tm_setup under FORCED BLANK, then tm_print_* freely, then unblank
;    - BG1 tilemap at VRAM word $0000, CHR at word $1000, palette 0
;
;  Include AFTER your own code but inside the CODE segment.
;
;  Public domain (CC0). No warranty.
; ============================================================

TM_STRPTR = $E0
TM_DIVTMP = $E2
TM_CURX   = $E4         ; CURX/CURY adjacent so a 16-bit STX sets both
TM_CURY   = $E5
TM_DIGITS = $E6         ; 5 bytes, most significant first
TM_PCTMP  = $EC
TM_LEAD   = $EE

TM_MAP_W  = $0000
TM_CHR_W  = $1000
TM_GLYPHS = 37          ; space + 0-9 + A-Z


; ---- tm_setup: palette, cleared tilemap, font. FORCED BLANK required. ----
.proc tm_setup
.a16
.i16
        sep #$20
.a8
        stz $2105               ; BGMODE 0
        stz $2107               ; BG1SC: tilemap at word 0, 32x32
        lda #$01
        sta $210B               ; BG1 CHR base = word $1000

        stz $2121               ; palette 0
        lda #$00
        sta $2122
        lda #$14                ; dark blue backdrop
        sta $2122
        lda #$FF
        sta $2122               ; white
        lda #$7F
        sta $2122

        lda #$80
        sta $2115               ; VMAIN: +1 word after $2119
        stz $2116
        stz $2117
        rep #$20
.a16
        ldx #$0000
        lda #$0000
@clr:   sta $2118               ; clear the tilemap to tile 0
        inx
        cpx #1024
        bne @clr

        ; 1bpp glyphs expanded to 2bpp: a 2bpp tile is 16 bytes with the
        ; rows interleaved plane0/plane1, so writing the glyph row to $2118
        ; and zero to $2119 makes set pixels colour 1, the rest transparent.
        lda #TM_CHR_W
        sta $2116
        ldx #$0000
@row:   sep #$20
.a8
        lda tm_font,x
        sta $2118
        stz $2119
        rep #$20
.a16
        inx
        cpx #(TM_GLYPHS * 8)
        bne @row
        rts
.endproc


; ---- tm_print_str: A = string address, X = col | (row << 8) -------------
.proc tm_print_str
.a16
.i16
        sta TM_STRPTR
        stx TM_CURX             ; X is 16-bit, so this sets col and row
        ldy #$0000
@next:  sep #$20
.a8
        lda (TM_STRPTR),y
        beq @done
        jsr tm_put_char
        inc TM_CURX
        rep #$20
.a16
        iny
        bra @next
@done:  rep #$20
.a16
        rts
.endproc


; ---- tm_print_dec5: A = value, X = col | (row << 8) ---------------------
; Right-aligned in five columns, leading zeros blanked.
.proc tm_print_dec5
.a16
.i16
        sta TM_DIVTMP
        stx TM_CURX

        ldx #4                  ; repeated division by ten
@div:   lda TM_DIVTMP
        sta $4204
        sep #$20
.a8
        lda #10
        sta $4206               ; starts the divide
        rep #$20
.a16
        nop                     ; the divider needs 16 machine cycles
        nop
        nop
        nop
        nop
        nop
        nop
        nop
        lda $4216               ; remainder = this digit
        sep #$20
.a8
        sta TM_DIGITS,x
        rep #$20
.a16
        lda $4214               ; quotient
        sta TM_DIVTMP
        dex
        bpl @div

        ldx #$0000
        sep #$20
.a8
        lda #1
        sta TM_LEAD
@emit:  lda TM_DIGITS,x
        bne @show               ; a non-zero digit ends the leading run
        cpx #4                  ; the units column always prints
        beq @show
        lda TM_LEAD
        beq @show0
        lda #' '
        jsr tm_put_char
        bra @adv
@show0: lda TM_DIGITS,x
@show:  stz TM_LEAD
        clc
        adc #'0'
        jsr tm_put_char
        ; A STAYS 8-BIT ACROSS THIS LOOP. A rep #$20 here would re-enter
        ; @emit with a 16-bit accumulator; tm_put_char would then push two
        ; bytes and pull one, and exactly ONE character of each number would
        ; print before the return address was corrupted. Index width is the
        ; X flag, not the M flag, so inx/cpx need no rep at all.
@adv:   inc TM_CURX
        inx
        cpx #5
        bne @emit
        rep #$20
.a16
        rts
.endproc


; ---- tm_put_char: A (8-bit) = ASCII, drawn at TM_CURX/TM_CURY ----------
.proc tm_put_char
.a8
.i16
        pha
        jsr tm_char_to_tile
        pha
        lda #$80
        sta $2115
        rep #$20
.a16
        lda TM_CURY
        and #$00FF
        asl
        asl
        asl
        asl
        asl                     ; row * 32
        sta TM_PCTMP
        lda TM_CURX
        and #$00FF
        clc
        adc TM_PCTMP
        clc
        adc #TM_MAP_W
        sta $2116               ; VMADD
        sep #$20
.a8
        pla
        sta $2118               ; tile index
        stz $2119               ; palette 0, no flip
        pla
        rts
.endproc


; ---- tm_char_to_tile: ASCII -> tile. space 0, 0-9 1..10, A-Z 11..36 -----
.proc tm_char_to_tile
.a8
.i16
        cmp #'0'
        bcc @space
        cmp #'9'+1
        bcs @alpha
        sec
        sbc #'0'
        clc
        adc #1
        rts
@alpha: cmp #'A'
        bcc @space
        cmp #'Z'+1
        bcs @space
        sec
        sbc #'A'
        clc
        adc #11
        rts
@space: lda #0
        rts
.endproc


; ---- 8x8 1bpp font: space, 0-9, A-Z ------------------------------------
tm_font:
    .byte $00,$00,$00,$00,$00,$00,$00,$00   ; space
    .byte $3C,$66,$6E,$7E,$76,$66,$3C,$00   ; 0
    .byte $18,$38,$18,$18,$18,$18,$7E,$00   ; 1
    .byte $3C,$66,$06,$0C,$18,$30,$7E,$00   ; 2
    .byte $3C,$66,$06,$1C,$06,$66,$3C,$00   ; 3
    .byte $0C,$1C,$3C,$6C,$7E,$0C,$0C,$00   ; 4
    .byte $7E,$60,$7C,$06,$06,$66,$3C,$00   ; 5
    .byte $1C,$30,$60,$7C,$66,$66,$3C,$00   ; 6
    .byte $7E,$06,$0C,$18,$30,$30,$30,$00   ; 7
    .byte $3C,$66,$66,$3C,$66,$66,$3C,$00   ; 8
    .byte $3C,$66,$66,$3E,$06,$0C,$38,$00   ; 9
    .byte $3C,$66,$66,$7E,$66,$66,$66,$00   ; A
    .byte $7C,$66,$66,$7C,$66,$66,$7C,$00   ; B
    .byte $3C,$66,$60,$60,$60,$66,$3C,$00   ; C
    .byte $78,$6C,$66,$66,$66,$6C,$78,$00   ; D
    .byte $7E,$60,$60,$7C,$60,$60,$7E,$00   ; E
    .byte $7E,$60,$60,$7C,$60,$60,$60,$00   ; F
    .byte $3C,$66,$60,$6E,$66,$66,$3C,$00   ; G
    .byte $66,$66,$66,$7E,$66,$66,$66,$00   ; H
    .byte $3C,$18,$18,$18,$18,$18,$3C,$00   ; I
    .byte $1E,$0C,$0C,$0C,$0C,$6C,$38,$00   ; J
    .byte $66,$6C,$78,$70,$78,$6C,$66,$00   ; K
    .byte $60,$60,$60,$60,$60,$60,$7E,$00   ; L
    .byte $63,$77,$7F,$6B,$63,$63,$63,$00   ; M
    .byte $66,$76,$7E,$6E,$66,$66,$66,$00   ; N
    .byte $3C,$66,$66,$66,$66,$66,$3C,$00   ; O
    .byte $7C,$66,$66,$7C,$60,$60,$60,$00   ; P
    .byte $3C,$66,$66,$66,$6E,$6C,$3A,$00   ; Q
    .byte $7C,$66,$66,$7C,$78,$6C,$66,$00   ; R
    .byte $3C,$66,$60,$3C,$06,$66,$3C,$00   ; S
    .byte $7E,$18,$18,$18,$18,$18,$18,$00   ; T
    .byte $66,$66,$66,$66,$66,$66,$3C,$00   ; U
    .byte $66,$66,$66,$66,$66,$3C,$18,$00   ; V
    .byte $63,$63,$63,$6B,$7F,$77,$63,$00   ; W
    .byte $66,$66,$3C,$18,$3C,$66,$66,$00   ; X
    .byte $66,$66,$3C,$18,$18,$18,$18,$00   ; Y
    .byte $7E,$06,$0C,$18,$30,$60,$7E,$00   ; Z
