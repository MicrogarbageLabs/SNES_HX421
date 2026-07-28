; ============================================================
;  h6_vecnmi.s — verify a custom NMI handler served from FPGA vec_mem (step 2b).
;
;  Installs an NMI handler address into vec_mem (native NMI = $FFEA -> offsets 0x0A/0x0B)
;  via the write window, flips the widen-enable ($3F:F060) so NMI/IRQ are served from
;  vec_mem, then enables NMI. Each vblank the CPU fetches the NMI vector FROM THE FABRIC
;  and runs our handler, which cycles the backdrop color.
;
;     backdrop cycles colors  -> the NMI vector came from vec_mem, handler runs (works)
;     static screen           -> the fabric NMI vector didn't take
;
;  Runs on the HX421_BRAM_VECTORS core. Public domain (CC0). No warranty.
; ============================================================

.p816
.smart +

.segment "CODE"

reset:                                  ; $8000, via the BRAM reset vector
        sei
        clc
        xce                             ; -> native mode (NMI vector = $FFEA)
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
        ; ---- install NMI handler into vec_mem ($FFEA -> offset 0x0A/0x0B) ----
        lda #<nmi_handler
        sta f:$3FF02A                   ; vec_mem[0x0A] = NMI low
        lda #>nmi_handler
        sta f:$3FF02B                   ; vec_mem[0x0B] = NMI high

        ; ---- widen the serve to include NMI/IRQ ----
        lda #$01
        sta f:$3FF060                   ; vec_widen_en = 1

        ; ---- display: backdrop black, layers off, screen on ----
        lda #$8F
        sta $2100
        stz $212C
        stz $212D
        stz $2121
        stz $2122
        stz $2122                       ; backdrop = black
        stz $00                         ; nmi_ctr = 0
        lda #$0F
        sta $2100

        ; ---- enable NMI (fires each vblank) ----
        lda #$80
        sta $4200
main:   bra main                        ; spin; the NMI handler does the work


; ---- NMI handler (served from vec_mem[$FFEA]) : cycle the backdrop color ----
nmi_handler:
        sep #$20
.a8
        inc $00                         ; nmi_ctr++ (proves we ran)
        lda $00
        stz $2121                       ; CGRAM address 0 (backdrop)
        sta $2122                       ; color low  = nmi_ctr
        sta $2122                       ; color high = nmi_ctr (fuller color sweep)
        lda $4210                       ; clear NMI flag
        rti


; ---- rom_vec_entry: ROM's $FFFC target -> RED (step-1 reset serve failed) ----
rom_vec_entry:
        sei
        clc
        xce
        sep #$20
.a8
        lda #$8F
        sta $2100
        stz $212C
        stz $212D
        stz $2121
        lda #$1F
        sta $2122
        stz $2122
        lda #$0F
        sta $2100
rvhang: bra rvhang

.segment "VECTORS"
        .word rom_vec_entry, rom_vec_entry, rom_vec_entry, rom_vec_entry
        .word rom_vec_entry, rom_vec_entry, rom_vec_entry, rom_vec_entry
        .word rom_vec_entry, rom_vec_entry, rom_vec_entry, rom_vec_entry
        .word rom_vec_entry             ; $FFFC
        .word rom_vec_entry
