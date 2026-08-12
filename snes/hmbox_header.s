; ============================================================
;  hmbox_header.s — SNES header declaring the HX-421 registered core, so our
;  (media) firmware loads /sd2snes/fpga_hx421.bi3 — its OWN slot, leaving stock
;  fpga_obc1.bi3 intact. sd2snes smc.c (our patch) matches map 0x30 / carttype
;  0xE4:  else if (header->map == 0x30 && header->carttype == 0xe4) {
;             props->has_hx421 = 1; props->fpga_conf = FPGA_HX421; }
;  (For BASE firmware without our patch, change 0xE4 -> 0x25 to borrow OBC1.)
;  Public domain (CC0). No warranty.
; ============================================================

.segment "HDR"
        .byte "HX421 MAILBOX TEST   "   ; $FFC0: 21-byte title
        .byte $30               ; $FFD5 map mode: LoROM + FastROM
        .byte $E4               ; $FFD6 cart type: HX-421  <-- selects fpga_hx421.bi3
        .byte $0A               ; $FFD7 ROM size: 1 MB slot (128 KB image)
        .byte $00               ; $FFD8 SRAM size: none
        .byte $01               ; $FFD9 country: NTSC
        .byte $33               ; $FFDA developer id
        .byte $00               ; $FFDB version
        .word $FFFF             ; $FFDC checksum complement (patched)
        .word $0000             ; $FFDE checksum            (patched)
