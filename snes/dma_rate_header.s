; ============================================================
;  dma_rate_header.s — plain LoROM header for the DMA rate test.
;
;  carttype $00 (ROM only), NOT the $E4 the H1 probe uses. This test needs
;  no coprocessor: it measures a property of the CONSOLE, so it runs on the
;  stock FXPak core with no bitstream swap and no risk to the card.
;
;  Public domain (CC0). No warranty.
; ============================================================

.segment "HDR"

        .byte "HX421 DMA RATE       "   ; $FFC0, 21 bytes

        .byte $30               ; $FFD5 map: LoROM + FastROM
        .byte $00               ; $FFD6 cart type: ROM only
        .byte $0A               ; $FFD7 ROM size
        .byte $00               ; $FFD8 SRAM: none
        .byte $01               ; $FFD9 country: NTSC
        .byte $33               ; $FFDA developer
        .byte $00               ; $FFDB version
        .word $FFFF             ; $FFDC checksum complement (patched)
        .word $0000             ; $FFDE checksum            (patched)
