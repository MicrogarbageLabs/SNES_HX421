; ============================================================
;  dma_rate.s — measure the real GP-DMA byte rate into VRAM (H3)
;
;  Every bandwidth figure in docs/{raycaster,tbdr,fmv-engine}.md rests on
;  one constant: how many bytes a general-purpose DMA moves into VRAM per
;  scanline of blanking. bsnes gives ~163 B/line. This measures it on
;  silicon.
;
;  METHOD. Force-blank the whole frame, read the V counter, run a DMA of a
;  known size, read V again. Lines elapsed is the difference; bytes per
;  line is the size divided by it. The CPU is HALTED for the duration of a
;  DMA, so it cannot count lines itself — reading the counter either side
;  is the only way, and it is exact.
;
;  THREE SIZES, NOT ONE. 8 KB, 16 KB and 32 KB. If the per-line rate is
;  constant across them there is no significant fixed cost per DMA; if it
;  climbs with size, there is, and the engine should prefer fewer larger
;  transfers. A single measurement cannot tell those apart, and the answer
;  changes how the emitted DMA body should be structured.
;
;  Plain LoROM, carttype $00 — no OBC1 header, so it runs on the stock
;  FXPak core and needs no bitstream swap. What it measures is a property
;  of the console, not of the cartridge.
;
;  Public domain (CC0). No warranty.
; ============================================================

.p816
.smart +

; ---- direct page ----
VS      = $00           ; V at DMA start
VE      = $02           ; V at DMA end
VTMP    = $04           ; scratch for read_v
LINES   = $06
NBYTES  = $08
DIVTMP  = $0A
STRPTR  = $0C
RESIDX  = $0E
CURX    = $10           ; CURX/CURY are adjacent so a 16-bit STX sets both
CURY    = $11
DIGITS  = $12           ; 5 bytes, most significant first
PCTMP   = $18           ; put_char scratch — must NOT overlap LEAD, which
LEAD    = $1A           ; print_dec5_at holds across its calls to put_char
; 3 runs x { VS, VE, LINES, BPL } words = 24 bytes.
; The raw counter readings are kept and displayed, not just the derived
; figures: if the arithmetic is wrong the derived numbers are meaningless,
; and there is no way to tell that apart from a bad measurement without
; seeing what went in.
RESULTS = $20
RESSTEP = 8
CHUNKSZ = $40
CHUNKN  = $42
CRES    = $44           ; 2 x lines, for the split-transfer runs
WLO     = $48           ; binary search bounds for the vblank-fit walk
WHI     = $4A
WMID    = $4C
WSTRUCT = $4E           ; 0 = one DMA, 1 = 8 chained, 2 = 8 sequential
WRES    = $50           ; 3 words: largest size that fits, per structure

; NTSC: active display is V=0..224, vblank is V=225..261. A transfer that
; is still inside vblank when it ends has fit; one that has wrapped back
; into active display has not.
VBL_START = 225

TILEMAP_W = $0000
CHR_W     = $1000
FONT_GLYPHS = 37        ; space + 0-9 + A-Z

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
        stz $4200               ; NMI + auto-joypad off
        stz $420C               ; HDMA off — it would steal DMA cycles
        stz $420B
        stz $2101
        stz $2105               ; BGMODE 0
        stz $2106
        stz $210B
        stz $212C
        stz $212D
        stz $2123
        stz $2124
        stz $2125
        stz $212E
        stz $212F
        stz $2130
        stz $2131
        stz $2133

        ; ---- measure FIRST ---------------------------------------------
        ; A 32 KB DMA fills 16384 VRAM words and would obliterate the font
        ; and tilemap, so take the readings before building the picture.
        rep #$30
.a16
.i16
        stz RESIDX
@meas:  lda RESIDX
        asl
        tax
        lda sizes,x             ; byte count for this run
        jsr measure
        inc RESIDX
        lda RESIDX
        cmp #3
        bcc @meas

        ; ---- how much actually fits in one real vblank ----------------
        ; The rate above is derived arithmetic. This measures the thing the
        ; engine actually needs END TO END: trigger at the top of vblank,
        ; then ask whether the transfer was still inside vblank when it
        ; finished. A binary search on the size finds the largest that fits.
        ;
        ; Three structures, so per-transfer cost is isolated rather than
        ; inferred: one big DMA, eight channels fired by a single $420B
        ; write, and eight separate triggers of the same total.
        ;
        ; The screen is black (brightness 0, NOT forced blank) throughout —
        ; forcing blank would grant VRAM access all frame and destroy the
        ; very window being measured.
        sep #$20
.a8
        stz $2100               ; brightness 0, display still running
        rep #$20
.a16
        stz WSTRUCT
@walk:  jsr walk_fit
        lda WSTRUCT
        asl
        tax
        lda WLO
        sta WRES,x
        inc WSTRUCT
        lda WSTRUCT
        cmp #3
        bcc @walk

        sep #$20
.a8
        lda #$8F
        sta $2100               ; back to forced blank to build the picture
        rep #$20
.a16

        jsr setup_display
        jsr draw_results

        sep #$20
.a8
        lda #$01
        sta $212C               ; BG1 on the main screen
        lda #$0F
        sta $2100               ; screen on
spin:   bra spin

sizes:  .word $2000, $4000, $8000


; ============================================================
;  measure — A = byte count. Stores {lines, bpl} at RESULTS[RESIDX].
; ============================================================
.proc measure
.a16
.i16
        sta NBYTES

        sep #$20
.a8
        lda #$80
        sta $2115               ; VMAIN: +1 word after $2119
        stz $2116
        stz $2117               ; VMADD = 0

        lda #$01
        sta $4300               ; A->B, mode 1 ($2118/$2119)
        lda #$18
        sta $4301               ; B-bus = $2118
        stz $4302               ; A-bus = $00:$8000 (our own ROM).
        lda #$80                ; The A-bus address wraps INSIDE the bank,
        sta $4303               ; so $8000 + 32768 ends exactly at $FFFF
        stz $4304               ; and never leaves ROM.
        rep #$20
.a16
        lda NBYTES
        sta $4305

        jsr wait_v0             ; start from a known line, well clear of
                                ; the 262-line wrap
        jsr read_v
        sta VS

        sep #$20
.a8
        lda #$01
        sta $420B               ; GO — CPU halted until the DMA completes
        rep #$20
.a16

        jsr read_v
        sta VE

        ; lines elapsed, allowing for a wrap past the end of the frame
        lda VE
        sec
        sbc VS
        bpl :+
        clc
        adc #262
:       cmp #1
        bcs :+
        lda #1                  ; never divide by zero
:       sta LINES

        ; bytes per line = size / lines, on the CPU divider
        lda NBYTES
        sta $4204               ; 16-bit unsigned dividend
        sep #$20
.a8
        lda LINES               ; < 256 for every size used here
        sta $4206               ; 8-bit divisor; starts the division
        rep #$20
.a16
        ; the divider needs 16 machine cycles before the result is valid
        nop
        nop
        nop
        nop
        nop
        nop
        nop
        nop

        lda RESIDX
        asl
        asl
        asl                     ; 8 bytes per result record
        tax
        lda VS
        sta RESULTS+0,x
        lda VE
        sta RESULTS+2,x
        lda LINES
        sta RESULTS+4,x
        lda $4214               ; quotient
        sta RESULTS+6,x
        rts
.endproc


; ============================================================
;  walk_fit — binary search for the largest transfer that completes
;  inside one vblank, using the structure in WSTRUCT. Answer in WLO.
; ============================================================
.proc walk_fit
.a16
.i16
        stz WLO
        lda #16384              ; ~99 lines: far past a 37-line vblank, so
        sta WHI                 ; the search can never run off the top
@loop:  lda WHI
        sec
        sbc WLO
        ; Terminate at a gap of 16, not below it. Sizes are quantised to
        ; multiples of 16, so at a gap of exactly 16 the midpoint rounds
        ; back onto WLO, the bound never moves, and the search spins
        ; forever — a blank screen with no other symptom.
        cmp #17
        bcc @done
        lda WLO
        clc
        adc WHI
        lsr
        and #$FFF0              ; keep sizes a multiple of 16 so the
        sta WMID                ; 8-way split divides evenly
        lda WMID
        jsr try_size
        bcc @nofit
        lda WMID
        sta WLO
        bra @loop
@nofit: lda WMID
        sta WHI
        bra @loop
@done:  rts
.endproc


; ============================================================
;  try_size — A = total bytes. Carry SET if it finished inside vblank.
; ============================================================
.proc try_size
.a16
.i16
        sta NBYTES

        sep #$20
.a8
        lda #$80
        sta $2115               ; VMAIN: +1 word after $2119
        stz $2116
        stz $2117               ; VMADD = 0
        rep #$20
.a16

        lda WSTRUCT
        bne @multi

        ; ---- one transfer ---------------------------------------------
        ldx #$0000
        lda NBYTES
        jsr arm_channel
        jsr wait_vbl
        sep #$20
.a8
        lda #$01
        sta $420B
        rep #$20
.a16
        bra @check

@multi: ; ---- eight transfers, chained or sequential --------------------
        ; Every channel targets $2118 and VMADD keeps incrementing across
        ; them, so a chain writes one contiguous VRAM run with a single
        ; trigger. That is exactly why chaining is worth measuring.
        lda NBYTES
        lsr
        lsr
        lsr                     ; NBYTES / 8 per channel
        sta CHUNKSZ

        ldx #$0000
@arm:   lda CHUNKSZ
        jsr arm_channel
        inx
        cpx #8
        bne @arm

        jsr wait_vbl
        lda WSTRUCT
        cmp #1
        bne @seq

        sep #$20
.a8
        lda #$FF                ; all eight channels, ONE trigger
        sta $420B
        rep #$20
.a16
        bra @check

@seq:   ldx #$0000              ; eight separate triggers
@seqlp: sep #$20
.a8
        lda bitmask,x
        sta $420B
        rep #$20
.a16
        inx
        cpx #8
        bne @seqlp

@check: jsr read_v
        cmp #VBL_START
        bcc @overran            ; wrapped back into active display
        sec
        rts
@overran:
        clc
        rts
.endproc

bitmask: .byte $01,$02,$04,$08,$10,$20,$40,$80


; ---- arm_channel: X = channel number, A = size -------------------------
; X IS PRESERVED. It is also the caller's loop counter, and an earlier
; version left the register base in it — so `inx / cpx #8` never matched
; and the arming loop ran forever. A hang with the screen still blank and
; nothing else to go on.
.proc arm_channel
.a16
.i16
        phx                     ; the caller's channel index
        pha                     ; size
        txa
        asl
        asl
        asl
        asl                     ; channel * $10
        clc
        adc #$4300
        tax                     ; X = base of this channel's registers
        sep #$20
.a8
        lda #$01
        sta a:$0000,x           ; DMAP: A->B, mode 1 ($2118/$2119)
        lda #$18
        sta a:$0001,x           ; BBAD = $2118
        stz a:$0002,x           ; A-bus = $00:$8000, our own ROM
        lda #$80
        sta a:$0003,x
        stz a:$0004,x
        rep #$20
.a16
        pla                     ; size
        sta a:$0005,x           ; DAS
        plx                     ; restore the caller's index
        rts
.endproc


; ---- wait for the start of vblank --------------------------------------
.proc wait_vbl
.a16
.i16
        sep #$20
.a8
@active:
        lda $4212
        bmi @active             ; still in vblank: wait for active display
@vbl:   lda $4212
        bpl @vbl                ; now catch the moment vblank begins
        rep #$20
.a16
        rts
.endproc


; ============================================================
;  measure_chunked — A = chunk size, X = chunk count.
;  Returns the scanlines the whole sequence took, in A.
;
;  Every iteration rewrites the source address and size, because a
;  completed DMA leaves $4302/$4303 pointing past the end and $4305 at
;  zero. That is also what the engine's emitted body does, so the cost
;  measured here is the cost it actually pays.
; ============================================================
.proc measure_chunked
.a16
.i16
        sta CHUNKSZ
        stx CHUNKN

        sep #$20
.a8
        lda #$80
        sta $2115
        stz $2116
        stz $2117
        lda #$01
        sta $4300
        lda #$18
        sta $4301
        rep #$20
.a16

        jsr wait_v0
        jsr read_v
        sta VS

        ldy CHUNKN
@loop:  sep #$20
.a8
        stz $4302
        lda #$80
        sta $4303
        stz $4304
        rep #$20
.a16
        lda CHUNKSZ
        sta $4305
        sep #$20
.a8
        lda #$01
        sta $420B
        rep #$20
.a16
        dey
        bne @loop

        jsr read_v
        sta VE

        lda VE
        sec
        sbc VS
        bpl :+
        clc
        adc #262
:       rts
.endproc


; ---- wait for the start of a frame -------------------------------------
.proc wait_v0
.a16
.i16
@high:  jsr read_v              ; first get clear of the top of the frame
        cmp #20
        bcc @high
@low:   jsr read_v              ; then catch the wrap
        cmp #20
        bcs @low
        rts
.endproc


; ---- read the 9-bit V counter into A -----------------------------------
.proc read_v
.a16
.i16
        sep #$20
.a8
        lda $213F               ; reset the H/V latch flip-flop
        lda $2137               ; SLHV: latch H and V
        lda $213D               ; OPVCT low 8
        sta VTMP
        lda $213D               ; OPVCT bit 8
        and #$01
        sta VTMP+1
        rep #$20
.a16
        lda VTMP
        rts
.endproc


; ============================================================
;  Display
; ============================================================
.proc setup_display
.a16
.i16
        sep #$20
.a8
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
        sta $2115
        stz $2116
        stz $2117
        rep #$20
.a16
        ldx #$0000              ; clear the tilemap to tile 0
        lda #$0000
@clr:   sta $2118
        inx
        cpx #1024
        bne @clr

        ; --- font: 1bpp glyphs expanded to 2bpp ------------------------
        ; A 2bpp tile is 16 bytes, rows interleaved plane0/plane1. Writing
        ; the glyph row to $2118 and zero to $2119 makes set pixels colour
        ; 1 and everything else transparent.
        lda #CHR_W
        sta $2116
        ldx #$0000
@row:   sep #$20
.a8
        lda font,x
        sta $2118
        stz $2119
        rep #$20
.a16
        inx
        cpx #(FONT_GLYPHS * 8)
        bne @row
        rts
.endproc


.proc draw_results
.a16
.i16
        lda #.loword(str_title)
        ldx #(2 | (2 << 8))
        jsr print_str_at

        lda #.loword(str_head)
        ldx #(1 | (5 << 8))
        jsr print_str_at

        stz RESIDX
@row:
        lda RESIDX
        asl
        tax
        lda sizes,x             ; the transfer size
        ldx #0
        jsr col

        lda RESIDX
        asl
        asl
        asl                     ; 8 bytes per record
        tax
        lda RESULTS+0,x         ; V at start
        ldx #6
        jsr col

        lda RESIDX
        asl
        asl
        asl
        tax
        lda RESULTS+2,x         ; V at end
        ldx #12
        jsr col

        lda RESIDX
        asl
        asl
        asl
        tax
        lda RESULTS+4,x         ; lines
        ldx #18
        jsr col

        lda RESIDX
        asl
        asl
        asl
        tax
        lda RESULTS+6,x         ; bytes per line
        ldx #24
        jsr col

        inc RESIDX
        lda RESIDX
        cmp #3
        bcc @row

        ; ---- how much fits in one real vblank -------------------------
        lda #.loword(str_fit)
        ldx #(0 | (12 << 8))
        jsr print_str_at

        lda #.loword(str_fit2)
        ldx #(0 | (14 << 8))
        jsr print_str_at
        lda WRES+0
        ldx #(16 | (14 << 8))
        jsr print_dec5_at

        lda #.loword(str_fit3)
        ldx #(0 | (15 << 8))
        jsr print_str_at
        lda WRES+2
        ldx #(16 | (15 << 8))
        jsr print_dec5_at

        lda #.loword(str_fit4)
        ldx #(0 | (16 << 8))
        jsr print_str_at
        lda WRES+4
        ldx #(16 | (16 << 8))
        jsr print_dec5_at
        rts

; A = value, X = column. The ROW comes from RESIDX, so no call site has to
; rebuild it — doing that per column is how three rows quietly end up
; stacked on the same line.
col:
        pha
        txa
        sta DIVTMP              ; borrow: the column
        lda RESIDX
        clc
        adc #7                  ; first result row
        xba                     ; row into the high byte
        ora DIVTMP              ; column into the low byte
        tax
        pla
        jmp print_dec5_at
.endproc


; ---- print_str_at: A = string address, X low = col, X high = row -------
.proc print_str_at
.a16
.i16
        sta STRPTR
        stx CURX                ; X is 16-bit, so this sets CURX and CURY
        ldy #$0000
@next:  sep #$20
.a8
        lda (STRPTR),y
        beq @done
        jsr put_char
        inc CURX
        rep #$20
.a16
        iny
        bra @next
@done:  rep #$20
.a16
        rts
.endproc


; ---- print_dec5_at: A = value, X low = col, X high = row ---------------
; Right-aligned in five columns, leading zeros suppressed.
.proc print_dec5_at
.a16
.i16
        sta DIVTMP
        stx CURX                ; 16-bit STX sets CURX and CURY together

        ; repeated division by ten; remainder is the digit
        ldx #4
@div:   lda DIVTMP
        sta $4204
        sep #$20
.a8
        lda #10
        sta $4206
        rep #$20
.a16
        nop
        nop
        nop
        nop
        nop
        nop
        nop
        nop
        lda $4216               ; remainder
        sep #$20
.a8
        sta DIGITS,x
        rep #$20
.a16
        lda $4214               ; quotient
        sta DIVTMP
        dex
        bpl @div

        ; emit, suppressing leading zeros but always printing one digit
        ldx #$0000
        sep #$20
.a8
        lda #1
        sta LEAD
@emit:  lda DIGITS,x
        bne @show               ; a non-zero digit ends the leading run
        cpx #4                  ; the units column always prints
        beq @show
        lda LEAD
        beq @show0              ; past the leading run: print the zero
        lda #' '
        jsr put_char
        bra @adv
@show0: lda DIGITS,x
@show:  stz LEAD
        clc
        adc #'0'
        jsr put_char
        ; A STAYS 8-BIT ACROSS THE LOOP. An earlier version did rep #$20
        ; here so inx/cpx would work — but index width is the X flag, not
        ; the M flag, so the rep was never needed and it re-entered @emit
        ; with a 16-bit accumulator. put_char then pushed two bytes and
        ; pulled one, unbalancing the stack: exactly ONE character of each
        ; number printed before the return address was corrupted. The
        ; strings were unaffected only because print_str_at happens to
        ; re-issue sep #$20 at the top of its own loop.
@adv:   inc CURX
        inx
        cpx #5
        bne @emit
        rep #$20
.a16
        rts
.endproc


; ---- put_char: A (8-bit) = ASCII, drawn at CURX/CURY -------------------
.proc put_char
.a8
.i16
        pha
        jsr char_to_tile
        pha                     ; tile index

        ; VMADD = TILEMAP_W + CURY*32 + CURX
        lda #$80
        sta $2115
        rep #$20
.a16
        lda CURY
        and #$00FF
        asl
        asl
        asl
        asl
        asl                     ; * 32
        sta PCTMP
        lda CURX
        and #$00FF
        clc
        adc PCTMP
        clc
        adc #TILEMAP_W
        sta $2116
        sep #$20
.a8
        pla
        sta $2118               ; tile index low
        stz $2119               ; palette 0, no flip
        pla
        rts
.endproc


; ---- char_to_tile: ASCII in A (8-bit) -> tile index in A ---------------
; space -> 0, '0'-'9' -> 1..10, 'A'-'Z' -> 11..36, anything else -> 0.
.proc char_to_tile
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


; ---- strings -----------------------------------------------------------
str_title:  .byte "HX421 DMA RATE TEST", 0
str_head:   .byte "BYTES    VS    VE LINES   B/L", 0
str_fit:    .byte "BYTES THAT FIT IN ONE VBLANK", 0
str_fit2:   .byte "ONE DMA", 0
str_fit3:   .byte "EIGHT CHAINED", 0
str_fit4:   .byte "EIGHT TRIGGERS", 0


; ---- 8x8 1bpp font: space, 0-9, A-Z ------------------------------------
font:
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


; ---- vectors -----------------------------------------------------------
stop:   bra stop

.segment "VECTORS"
        .word stop, stop, stop, stop, stop, stop, stop, stop   ; native
        .word stop, stop, stop, stop
        .word reset             ; $FFFC RESET
        .word stop
