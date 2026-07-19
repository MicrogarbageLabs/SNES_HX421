; ============================================================
;  boot.s — HX-421 standalone boot stub (milestone 1).
;
;  Lives in the coprocessor window's $8000-$FFFF at reset. Brings up
;  the 65816 in native mode, forces blank, disables NMI + auto-joypad,
;  copies the kernel blob from the ROM window into low WRAM, then hands
;  off. The hardware NMI/IRQ/BRK vectors point at tiny trampolines in
;  the static top page that bounce through a WRAM pointer, so the RAM
;  kernel owns the interrupts.
;
;  This is the NON-VIRTUALIZED model: there is NO copro "kernel ready"
;  handshake to spin on (the blob is already in the window the DLL
;  serves), NO cart-release / boot-runtime sentinel, and NO VM
;  apparatus. The only coprocessor touch is a single boot-progress
;  read-strobe the DLL logs (HX421_BOOT_STROBE).
;
;  Contract: the copro must keep $FF00-$FFFF (trampolines + vectors)
;  static after boot.
;
;  Public domain (CC0). No warranty.
; ============================================================
.p816
.include "snes.inc"
.include "hx421.inc"

; symbols the linker exports for the KERNEL segment (LOAD in ROM window,
; RUN in low WRAM): where it sits in the window, where it runs, its size.
.import __KERNEL_LOAD__, __KERNEL_RUN__, __KERNEL_SIZE__

.segment "BOOT"
.proc reset
    .a8
    .i8
    sei
    clc
    xce                     ; emulation -> native
    rep #$38                ; A/X/Y 16-bit, decimal off
    .a16
    .i16
    ldx #$01FF
    txs                     ; stack at $01FF
    lda #$0000
    tcd                     ; direct page = $0000
    phk
    plb                     ; data bank = $00 (for $21xx/$42xx + low RAM)
    sep #$20
    .a8

    lda #$8F
    sta INIDISP             ; forced blank, brightness 0
    stz NMITIMEN            ; NMI off, auto-joypad off

    ; --- copy kernel: window(LOAD, bank $00 upper half) -> WRAM(RUN) ---
    ; Byte copy for clarity (the blob is ~1 KB). long,x reads the ROM
    ; window ($8000+); abs,x writes low WRAM with DBR=$00.
    ldx #$0000
@copy:
    lda f:__KERNEL_LOAD__,x
    sta a:__KERNEL_RUN__,x
    inx
    cpx #__KERNEL_SIZE__
    bne @copy

    ; --- boot-progress strobe: tell the DLL we reached the handoff ---
    ; Read-only bus: the ACCESS is the message (byte ignored). The DLL
    ; logs this read, proving the reset stub ran through the copy loop.
    lda f:HX421_BOOT_STROBE

    ; --- hand off to the RAM kernel (entry = first byte of KERNEL) ---
    jmp a:__KERNEL_RUN__        ; bank $00, native, A8/I16
.endproc

; ------------------------------------------------------------------
; Interrupt trampolines (top page, copro-preserved). The RAM kernel
; writes its handler addresses into RAMVEC_* before enabling interrupts.
; Milestone 1 runs interrupts-off, but a stray BRK still lands safely.
; ------------------------------------------------------------------
.segment "TRAMP"
nmi_trampoline:
    jmp (RAMVEC_NMI)
irq_trampoline:
    jmp (RAMVEC_IRQ)
; A stray BRK (opcode $00) should not trap the CPU in an infinite
; "BRK -> vector -> $0000 -> read $00 -> BRK" loop; a clean RTI returns
; to whatever was executing so the last picture survives a transient.
brk_trampoline:
    rti

; ------------------------------------------------------------------
; Minimal SNES header ($FFC0-$FFDF) — lets stock bsnes detect/load the
; trigger cart as a HiROM; the hx421 chip serves the real bytes.
; ------------------------------------------------------------------
.segment "HEADER"
    .byte "HX-421 BOOT M2      "   ; $FFC0 title, 21 bytes...
    .byte $20                      ; ...padded with a space (21st)
    .byte $21                      ; $FFD5 map mode: HiROM, slow
    .byte $00                      ; $FFD6 cart type: ROM only
    .byte $06                      ; $FFD7 ROM size (~64 KB)
    .byte $00                      ; $FFD8 RAM size: none
    .byte $01                      ; $FFD9 country: NTSC
    .byte $00                      ; $FFDA developer id
    .byte $00                      ; $FFDB version
    .word $0000                    ; $FFDC-DD checksum complement (placeholder)
    .word $FFFF                    ; $FFDE-DF checksum (placeholder)

; ------------------------------------------------------------------
; 65816 native + emulation vectors ($FFE4-$FFFF).
; ------------------------------------------------------------------
.segment "VECTORS"
    .word $0000             ; FFE4 COP   (native)
    .word brk_trampoline    ; FFE6 BRK  -- "just RTI"
    .word $0000             ; FFE8 ABORT
    .word nmi_trampoline    ; FFEA NMI
    .word $0000             ; FFEC (reserved)
    .word irq_trampoline    ; FFEE IRQ
    .word $0000             ; FFF0 (reserved)
    .word $0000             ; FFF2 (reserved)
    .word $0000             ; FFF4 COP   (emulation, unused in native)
    .word $0000             ; FFF6 (reserved)
    .word $0000             ; FFF8 ABORT
    .word $0000             ; FFFA NMI   (emulation)
    .word reset             ; FFFC RESET
    .word $0000             ; FFFE IRQ/BRK (emulation)
