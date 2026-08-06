; ============================================================
;  h1_header.s — SNES header for the H1 probe, declaring the HX-421 core.
;
;  The two fields that matter are map and carttype: sd2snes' smc.c picks
;  the per-game FPGA core from exactly this pair.
;
;      else if (header->map == 0x30 && header->carttype == 0xE4) {
;          props->has_hx421 = 1;
;          props->fpga_conf = FPGA_HX421;
;      }
;
;  So this ROM causes the FXPak to program /sd2snes/fpga_hx421.bi3 — our
;  own registered core (no longer the borrowed OBC1 slot). Nothing else in the header is
;  load-bearing, but the checksum is patched by build-h1.ps1 because
;  sd2snes SCORES candidate headers and a valid checksum helps the LoROM
;  location win over a spurious HiROM one.
;
;  Public domain (CC0). No warranty.
; ============================================================

.segment "HDR"

        ; $FFC0: 21-byte title, space padded
        .byte "HX421 H1 PROBE       "

        .byte $30               ; $FFD5 map mode: LoROM + FastROM
        .byte $E4               ; $FFD6 cart type: HX-421  <-- selects fpga_hx421.bi3
        .byte $0A               ; $FFD7 ROM size: 2^10 KB = 1 MB slot (128 KB image)
        .byte $00               ; $FFD8 SRAM size: none
        .byte $01               ; $FFD9 country: NTSC
        .byte $33               ; $FFDA developer id
        .byte $00               ; $FFDB version
        .word $FFFF             ; $FFDC checksum complement (patched)
        .word $0000             ; $FFDE checksum            (patched)
