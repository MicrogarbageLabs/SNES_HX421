; ============================================================
;  h6_bramvec.s — verify the 65816 RESET vector is served from FPGA BRAM, not PSRAM.
;
;  Uses a SOLID BACKDROP COLOR as the indicator (no tilemap/font, so no ROM reads for
;  the mixer to garble):
;     GREEN screen -> booted to `reset` ($8000) = the FPGA served $FFFC (works)
;     RED   screen -> booted to `rom_vec_entry`  = the SNES used the ROM/PSRAM vector
;
;  The FPGA (HX421_BRAM_VECTORS core) serves $00:FFFC = $8000; this ROM's own $FFFC
;  points at rom_vec_entry instead, so the color tells which vector won.
;  Public domain (CC0). No warranty.
; ============================================================

.p816
.smart +

.segment "CODE"

; ---- reset ($8000): reached only if the FPGA serves $FFFC = $8000 -> GREEN ----
reset:
        sei
        clc
        xce
        sep #$20
.a8
        lda #$8F
        sta $2100                       ; force blank
        stz $212C                       ; no main-screen BG layers -> only backdrop shows
        stz $212D                       ; no sub-screen layers (kills leftover loader VRAM)
        stz $2121                       ; CGRAM address 0 (backdrop)
        lda #$E0
        sta $2122                       ; color low  ($03E0 = green)
        lda #$03
        sta $2122                       ; color high
        lda #$0F
        sta $2100                       ; screen on -> solid GREEN
spin:   bra spin


; ---- rom_vec_entry: the ROM's $FFFC target -> RED = BRAM serve FAILED ----
rom_vec_entry:
        sei
        clc
        xce
        sep #$20
.a8
        lda #$8F
        sta $2100
        stz $2121
        lda #$1F
        sta $2122                       ; color low  ($001F = red)
        stz $2122
        lda #$0F
        sta $2100                       ; screen on -> solid RED
rvhang: bra rvhang

.segment "VECTORS"
        .word rom_vec_entry, rom_vec_entry, rom_vec_entry, rom_vec_entry
        .word rom_vec_entry, rom_vec_entry, rom_vec_entry, rom_vec_entry
        .word rom_vec_entry, rom_vec_entry, rom_vec_entry, rom_vec_entry
        .word rom_vec_entry             ; $FFFC reset -> red-screen fail path
        .word rom_vec_entry
