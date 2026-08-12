; ============================================================
;  hmbox_header.s — SNES header declaring OBC1, so BASE (stock) sd2snes loads
;  /sd2snes/fpga_obc1.bi3 — where we put our media core for hardware bring-up
;  without a custom firmware. sd2snes smc.c matches map 0x30 / carttype 0x25:
;      else if (header->map == 0x30 && header->carttype == 0x25) {
;          props->has_obc1 = 1; props->fpga_conf = FPGA_OBC1; }
;  Public domain (CC0). No warranty.
; ============================================================

.segment "HDR"
        .byte "HX421 MAILBOX TEST   "   ; $FFC0: 21-byte title
        .byte $30               ; $FFD5 map mode: LoROM + FastROM
        .byte $25               ; $FFD6 cart type: OBC1  <-- selects fpga_obc1.bi3
        .byte $0A               ; $FFD7 ROM size: 1 MB slot (128 KB image)
        .byte $00               ; $FFD8 SRAM size: none
        .byte $01               ; $FFD9 country: NTSC
        .byte $33               ; $FFDA developer id
        .byte $00               ; $FFDB version
        .word $FFFF             ; $FFDC checksum complement (patched)
        .word $0000             ; $FFDE checksum            (patched)
