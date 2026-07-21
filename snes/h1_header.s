; ============================================================
;  h1_header.s — SNES header for the H1 probe, declaring OBC1.
;
;  The two fields that matter are map and carttype: sd2snes' smc.c picks
;  the per-game FPGA core from exactly this pair.
;
;      else if (header->map == 0x30 && header->carttype == 0x25) {
;          props->has_obc1 = 1;
;          props->fpga_conf = FPGA_OBC1;
;      }
;
;  So this ROM causes the FXPak to program /sd2snes/fpga_obc1.bi3 — the
;  slot we borrow for our own bitstream. Nothing else in the header is
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
        .byte $25               ; $FFD6 cart type: OBC1  <-- selects the core
        .byte $0A               ; $FFD7 ROM size: 2^10 KB = 1 MB slot (128 KB image)
        .byte $00               ; $FFD8 SRAM size: none
        .byte $01               ; $FFD9 country: NTSC
        .byte $33               ; $FFDA developer id
        .byte $00               ; $FFDB version
        .word $FFFF             ; $FFDC checksum complement (patched)
        .word $0000             ; $FFDE checksum            (patched)
