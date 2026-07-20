; ============================================================
;  kernel.s — HX-421 milestone-2 DYNAMIC per-frame renderer.
;
;  boot.s copies this blob into low WRAM ($0400) and jmp's to the first
;  byte. It runs entirely from WRAM. Unlike M1 (a static screen, no
;  interrupts) M2 is a live renderer driven by:
;
;    * a FREE-RUNNING H-timer IRQ (NMITIMEN=$10 — fires every scanline
;      at H=HTIME; bsnes-plus poll_irq honors H-only auto-repeat), which
;    * reads a PER-LINE ACTION TABLE indexed by the live V counter,
;      straight out of the served cart window (no WRAM staging), and
;    * drives the force-blank LETTERBOX (INIDISP) and, on the first blank
;      line, jsl's straight into a coprocessor-EMITTED 65816 DMA body in
;      the served window (baked-immediate slots, execute-from-window) to
;      push the staged frame into VRAM during the blank window.
;
;  The DLL (playing the RISC-V) freewheels frame data into the window,
;  DOUBLE-BUFFERED: the SNES reads the FRONT buffer, the DLL writes the
;  BACK buffer, and the swap is gated on a frame-ready flag. See the
;  window contract in hx421.inc + docs/window-contract.md.
;
;  Adapted from microgarbage/snes/kernel.s (the proven virtual-NMI
;  chainer) with the VM apparatus DROPPED (no guest-ELF, no ISR/FB image
;  swaps, no cart-release) and the 2-state machine replaced by the
;  owner-confirmed free-running-H-IRQ + action-table. The DMA push uses
;  the microgarbage mg_nmi/copro_r3d EMITTER technique: the coprocessor
;  bakes the per-frame 65816 DMA body (immediate slots, no descriptor
;  reads, no runtime dest dispatch) into the window and the kernel jsl's
;  it — the lowest-overhead path, maximizing blank-window DMA bandwidth.
;
;  Timing fix carried from microgarbage:
;    * the per-line IRQ ACK (read $4211) is UNCONDITIONAL every line —
;      never gated on frame-ready (their v2.30.14 black-screen bug).
;  The coprocessor owns the DMA fit budget at emit time (total must land
;  in the bottom-LB+vblank+top-LB window), so the kernel needs no runtime
;  cycle-budget/defer machinery.
;
;  entry (kmain) MUST be the first byte — boot.s does `jmp __KERNEL_RUN__`.
;  Public domain (CC0). No warranty.
; ============================================================
.p816
.include "snes.inc"
.include "hx421.inc"

.segment "KERNEL"

; entry MUST be first — boot.s does `jmp __KERNEL_RUN__`.
.proc kmain
    .a8
    .i16
    ; arrives native, A8/I16, DBR=$00, DP=$0000 (set by boot.s)

    ; --- install interrupt handlers via the WRAM trampolines ----------
    ; NMI stays OFF; the free-running H-IRQ (proc `irq`) does all work.
    rep #$20
    .a16
    lda #.loword(irq)
    sta RAMVEC_IRQ
    lda #.loword(nmi_stub)
    sta RAMVEC_NMI
    sep #$20
    .a8

    ; --- force blank while we upload VRAM/CGRAM -----------------------
    lda #$8F
    sta INIDISP

    ; --- zero window-mask / colour-math regs for a clean baseline -----
    stz W12SEL
    stz W34SEL
    stz WOBJSEL
    stz WH0
    stz WH1
    stz WH2
    stz WH3
    stz WBGLOG
    stz WOBJLOG
    stz TMW
    stz TSW
    stz CGWSEL
    stz CGADSUB
    stz COLDATA
    stz SETINI                  ; force non-interlace (fixed 262-line field)

    ; --- BG setup: Mode 1, BG1 4bpp (tilemap word $0000, CHR word $1000)
    lda #$01
    sta BGMODE
    stz BG1SC                   ; BG1 tilemap @ VRAM word $0000, 32x32
    lda #$01
    sta BG12NBA                 ; BG1 CHR base = 1  (word $1000)
    lda #$01
    sta TM                      ; BG1 on the main screen
    stz TS
    stz BG1HOFS
    stz BG1HOFS                 ; scroll 0 (write-twice)
    stz BG1VOFS
    stz BG1VOFS

    ; --- upload CHR: tile 0 = empty, tile 1 = solid colour-1 bar ------
    lda #$80
    sta VMAIN                   ; word step, increment after high byte
    rep #$20
    .a16
    lda #$1000                  ; CHR base word
    sta VMADDL
    sep #$20
    .a8
    ldx #0
@chr:
    lda chr_data,x
    sta VMDATAL
    inx
    lda chr_data,x
    sta VMDATAH
    inx
    cpx #CHR_SIZE
    bne @chr

    ; --- palette -> CGRAM (colours 0..1) -----------------------------
    stz CGADD
    ldx #0
@pal:
    lda pal_data,x
    sta CGDATA
    inx
    lda pal_data,x
    sta CGDATA
    inx
    cpx #PAL_SIZE
    bne @pal

    ; --- clear the BG1 tilemap (1024 words -> tile 0 / backdrop) ------
    lda #$80
    sta VMAIN
    rep #$20
    .a16
    stz VMADDL
    ldx #1024
@clr:
    stz VMDATAL                 ; 16-bit STZ = one map word
    dex
    bne @clr
    sep #$20
    .a8

    ; --- per-frame kernel state (WRAM) -------------------------------
    stz K_BODY_PENDING          ; no DMA body to fire until a frame latches
    lda #$FF
    sta K_FRONT_IDX             ; no buffer latched yet -> first READY swaps
    stz K_TOP_LB
    lda #224
    sta K_VIS_END
    stz K_SIPHON_STARTED
    stz K_FRAME_ARMED
    rep #$20
    .a16
    stz K_SIPHON_BPL            ; siphon off until frame_prep caches a descriptor
    stz K_SIPHON_VRAM
    stz K_SIPHON_SRC
    sep #$20
    .a8

    ; default front pointer = buffer 0 (harmless until the first swap;
    ; its action table is all-zero = ACT_NONE until the DLL publishes).
    stz K_FRONT_PTR+0
    stz K_FRONT_PTR+1
    lda #HX421_BANK
    sta K_FRONT_PTR+2

    ; --- arm the free-running H-timer IRQ (every scanline @ H=22) -----
    ; NMITIMEN bit4 = H-IRQ enable, bit5 (V) = 0 -> fires EVERY line at
    ; H=HTIME. NMI (b7) OFF; auto-joypad (b0) OFF. The V target is
    ; irrelevant in H-only mode. (Verified: bsnes-plus poll_irq honors
    ; H-only auto-repeat -> a fresh 0->1 edge at H=HTIME every scanline.
    ; This is the siphon-ready foundation: fixed-HTIME fire, no per-line
    ; VTIME re-arm, no jitter.)
    lda #22
    sta HTIMEL
    stz HTIMEH
    stz VTIMEL
    stz VTIMEH
    lda #$0F
    sta INIDISP                 ; visible until the DLL drives the letterbox
    lda #$11
    sta NMITIMEN                ; free-running H-IRQ armed + AUTO-JOYPAD (b0)
                                ; auto-joypad latches the pads during vblank;
                                ; the mailbox push at V=PAD_LINE reads them.
    cli

@hang:
    wai
    bra @hang
.endproc

; ------------------------------------------------------------------
; irq — the free-running per-line H-timer handler (fires every scanline
; at H=22). Reads the front buffer's action_table[V] and dispatches:
;   ACT_BLANK(1)   force-blank (INIDISP=$8F); on the first blank line of the
;                  frame, jsl into the coprocessor-emitted DMA body.
;   ACT_UNBLANK(2) unblank (INIDISP=$0F) — the top-letterbox reveal line.
;   ACT_SIPHON(3)  per-line H-blank chunked VRAM push (siphon_line).
;   ACT_NONE(0)    nothing (leave INIDISP as-is).
; At the V >= VIS_END crossing (once/frame) it runs frame_prep: latch the
; front buffer from the frame-ready flag + cache the siphon descriptor + patch
; the DMA-body jsl to the front buffer + arm it (so the bulk push runs in the
; big bottom-LB+vblank+top-LB window).
; A8/I16 body. DBR=$00, DP=$0000.
; ------------------------------------------------------------------
.proc irq
    rep #$30
    .a16
    .i16
    pha
    phx
    phy
    sep #$20
    .a8

    ; ACK the timer IRQ EVERY line (unconditional — microgarbage v2.30.14:
    ; gating the re-arm/ack on frame-ready black-screens the display).
    lda TIMEUP

    ; --- latch the live V counter ------------------------------------
    lda SLHV                    ; latch H/V
    lda STAT78                  ; reset OPVCT read toggle
    lda OPVCT
    sta K_VLO
    lda OPVCT
    and #$01
    sta K_VHI

    ; --- Y = V (action-table index) ----------------------------------
    rep #$20
    .a16
    lda K_VLO                   ; K_VLO|K_VHI = 9-bit V
    and #$01FF
    tay                         ; Y = V (line index into the action table)
    sep #$20
    .a8

    ; --- SNES -> cart mailbox: push joypad state once per frame -------
    ; MUST sit here, on the path EVERY line takes. It lived below the
    ; VIS_END block originally, which made it unreachable: at PAD_LINE the
    ; frame is already armed, so `bne @dispatch` skipped it, and the only
    ; line that DID reach it was VIS_END itself — where the PAD_LINE compare
    ; can never match. Dead code that assembled perfectly.
    ;
    ; At PAD_LINE auto-joypad has finished (it latches early in vblank and
    ; takes ~3 lines), so $4218.. hold this frame's pads. The doorbell write
    ; tells the coprocessor the block is complete rather than half-written.
    ;
    ; The cart bus is read-only EVERYWHERE ELSE; the coprocessor ignores
    ; writes outside the mailbox, so a stray store cannot corrupt staging
    ; or the kernel image.
    cpy #PAD_LINE
    bne @no_pads
    lda K_PAD_DONE
    bne @no_pads
    lda #$01
    sta K_PAD_DONE
    rep #$20
    .a16
    ; All four auto-joypad words. $4218/$421A are the primary data line of
    ; ports 1 and 2; $421C/$421E are each port's SECOND data line, which is
    ; where a multitap presents additional controllers. Reading all four is
    ; free, so do it unconditionally and let the host decide what is plugged
    ; in. (Note: a Super Multitap's full 5-controller protocol needs manual
    ; $4016 strobing — auto-joypad only ever yields these four words.)
    lda JOY1L                   ; port 1, data line 1 (BYsS udlr AXLR ....)
    sta f:HX_MB_JOYPADS_L
    lda JOY2L                   ; port 2, data line 1
    sta f:HX_MB_JOYPADS_L+2
    lda JOY3L                   ; port 1, data line 2 (multitap)
    sta f:HX_MB_JOYPADS_L+4
    lda JOY4L                   ; port 2, data line 2 (multitap)
    sta f:HX_MB_JOYPADS_L+6
    sep #$20
    .a8
    sta f:HX_MB_DOORBELL_L      ; any value — the ACCESS is the signal
@no_pads:

    ; --- once-per-frame prep at the V >= VIS_END crossing -------------
    ; When V reaches the bottom letterbox (V >= VIS_END) the visible region
    ; is done: latch the staged front buffer + arm the bulk DMA walk so the
    ; big blank window (bottom LB + vblank + top LB, ~70 lines) DMAs the
    ; frame, resident well before the unblank at V=top_lb. Gated to once
    ; per frame by K_FRAME_ARMED (cleared while V < VIS_END).
    lda K_VHI
    bne @ge_vis                 ; V >= 256 -> past VIS_END for sure
    lda K_VLO
    cmp K_VIS_END               ; carry set if V.lo >= VIS_END
    bcs @ge_vis
    stz K_FRAME_ARMED           ; V < VIS_END: visible/top region -> re-arm
    stz K_PAD_DONE              ; and re-arm the once-per-frame mailbox push
    bra @dispatch
@ge_vis:
    lda K_FRAME_ARMED
    bne @dispatch               ; already prepped this frame
    lda #$01
    sta K_FRAME_ARMED
    jsr frame_prep              ; latch front + cache descriptors + arm walk (keeps Y)
    ; per-frame siphon runtime reset (unconditional — even when frame_prep
    ; kept the old front): the first siphon line re-programs VMADD, and the
    ; running source cursor restarts at the cached base.
    stz K_SIPHON_STARTED
    rep #$20
    .a16
    lda K_SIPHON_SRC_BASE
    sta K_SIPHON_SRC
    sep #$20
    .a8

@dispatch:
    lda [K_FRONT_PTR],y         ; action = front action_table[V]
    cmp #ACT_UNBLANK
    beq @unblank
    cmp #ACT_BLANK
    beq @blank
    cmp #ACT_SIPHON
    beq @siphon
    bra @exit                   ; ACT_NONE / unknown

@unblank:
    lda #$0F
    sta INIDISP
    bra @exit

@blank:
    lda #$8F
    sta INIDISP                 ; opens the VRAM-DMA window
    lda K_BODY_PENDING
    beq @exit
    stz K_BODY_PENDING          ; fire the emitted DMA body once per frame
    jsr fire_dma_body           ; jsl into the front buffer's emitted body (window)

    ; --- report where the burst finished (mailbox) --------------------
    ; Latch V immediately after the body returns and hand it to the
    ; coprocessor, so it can MEASURE the blank it consumed instead of
    ; estimating. We have no cycle-budgeted chainer: an overrun is not
    ; deferred, it writes during active display and corrupts the picture
    ; with no other symptom. ~10 cycles, worth it.
    lda SLHV                    ; latch H/V
    lda STAT78                  ; reset the OPVCT read toggle
    lda OPVCT
    sta f:HX_MB_BURST_V_L
    lda OPVCT
    and #$01
    sta f:HX_MB_BURST_V_L+1

    lda f:HX_FRAME_DONE_L       ; strobe: DLL flips buffers + clears frame-ready
    bra @exit

@siphon:
    jsr siphon_line             ; per-line H-blank chunked VRAM push

@exit:
    rep #$30
    .a16
    .i16
    ply
    plx
    pla
    rti
.endproc

; ------------------------------------------------------------------
; fire_dma_body — jsl into the front buffer's coprocessor-emitted 65816
; DMA body in the served window (execute-from-window: no WRAM copy, no
; descriptor walk). The jsl's 24-bit operand lives at fire_dma_body+1 and
; is self-patched by frame_prep to $C0:<front buffer body>. The body fires
; all its baked DMA slots and returns via RTL. Entry/exit A8/I16.
; ------------------------------------------------------------------
.proc fire_dma_body
    jsl $000000                 ; operand (fire_dma_body+1..+3) patched per frame
    rts
.endproc

; ------------------------------------------------------------------
; frame_prep — once per frame, at the V>=VIS_END crossing. Read the
; frame-ready flag; if a new buffer is staged, latch it as the FRONT
; buffer (repoint the long pointers), cache its letterbox layout AND its
; H-blank siphon descriptor, reset the per-frame siphon runtime, and arm
; the bulk DMA walk (which then runs through the bottom-LB+vblank+top-LB
; blank window — resident long before the top-letterbox unblank).
; Entry/exit A8/I16; preserves Y (the caller's V). DBR=$00, DP=$0000.
; ------------------------------------------------------------------
.proc frame_prep
    .a8
    .i16
    phy
    lda f:HX_FRAME_READY_L
    bne :+
    jmp @done                   ; 0 -> nothing staged; keep front, no walk
:
    sec
    sbc #1
    and #$01                    ; A = staged buffer index (0/1)
    cmp K_FRONT_IDX
    beq @arm                    ; already front -> just (re)arm the walk
    sta K_FRONT_IDX             ; latch the new front buffer
    lda K_FRONT_IDX
    bne @buf1

    ; --- front = buffer 0 ($0000) ---
    stz K_FRONT_PTR+0
    stz K_FRONT_PTR+1
    lda #HX421_BANK
    sta K_FRONT_PTR+2
    bra @arm

@buf1:
    ; --- front = buffer 1 ($3000) ---
    lda #<HX_BUF1_BASE
    sta K_FRONT_PTR+0
    lda #>HX_BUF1_BASE
    sta K_FRONT_PTR+1
    lda #HX421_BANK
    sta K_FRONT_PTR+2

@arm:
    ; cache the letterbox layout from the front header
    ldy #HX_OFF_HDR_TOP_LB
    lda [K_FRONT_PTR],y
    sta K_TOP_LB
    ldy #HX_OFF_HDR_VIS_END
    lda [K_FRONT_PTR],y
    sta K_VIS_END
    ; cache the H-blank siphon descriptor (bytes/line, VRAM dst, source base)
    rep #$20
    .a16
    ldy #HX_OFF_SIP_BPL
    lda [K_FRONT_PTR],y
    sta K_SIPHON_BPL
    ldy #HX_OFF_SIP_VRAM
    lda [K_FRONT_PTR],y
    sta K_SIPHON_VRAM
    ldy #HX_OFF_SIP_SRC
    lda [K_FRONT_PTR],y
    sta K_SIPHON_SRC_BASE
    sep #$20
    .a8
    ; NOTE: the per-frame siphon runtime reset (K_SIPHON_STARTED=0 +
    ; K_SIPHON_SRC=base) runs at the VIS_END crossing in the ISR, NOT here,
    ; so it happens every frame even when no new frame is latched (a stale
    ; STARTED/cursor would otherwise siphon to the wrong VRAM word).
    ; --- patch the DMA-body jsl operand to the front buffer's emitted body,
    ;     then arm it to fire on this frame's first blank line. ---
    lda K_FRONT_IDX
    bne @body1
    lda #<(HX_BUF0_BASE + HX_OFF_DMABODY)
    sta fire_dma_body+1
    lda #>(HX_BUF0_BASE + HX_OFF_DMABODY)
    sta fire_dma_body+2
    bra @body_bank
@body1:
    lda #<(HX_BUF1_BASE + HX_OFF_DMABODY)
    sta fire_dma_body+1
    lda #>(HX_BUF1_BASE + HX_OFF_DMABODY)
    sta fire_dma_body+2
@body_bank:
    lda #HX421_BANK
    sta fire_dma_body+3
    lda #$01
    sta K_BODY_PENDING

@done:
    ply
    rts
.endproc

; ------------------------------------------------------------------
; siphon_line — one visible-line H-blank chunked VRAM push (ACT_SIPHON).
; NOT HDMA: a plain channel-0 GP-DMA to VMDATA, executed inside the
; right-edge H-blank ($4212 b6), which the SNES lets land in VRAM even
; while the display is live. Ported from microgarbage siphon_hblank_test.
; The free-running H-IRQ already re-fires next line, so — unlike the H+V
; version — there is NO per-line V re-arm here (that was the jitter risk).
; VMADD is programmed ONCE (first siphon line of the frame) and auto-
; increments after; the source is a running cursor advanced by bytes/line.
; Entry/exit A8/I16. DBR=$00, DP=$0000.
; ------------------------------------------------------------------
.proc siphon_line
    .a8
    .i16
    lda K_SIPHON_BPL+0
    ora K_SIPHON_BPL+1
    bne :+
    rts                         ; bytes/line == 0 -> siphon disabled
:
    ; channel 0: A-bus = window bank + running source; B-bus = VMDATAL,
    ; 2-reg (lo/hi) with A incrementing.
    lda #HX421_BANK
    sta A1B0
    lda #$01
    sta DMAP0
    lda #<VMDATAL
    sta BBAD0
    rep #$20
    .a16
    lda K_SIPHON_SRC
    sta A1T0L
    lda K_SIPHON_BPL
    sta DAS0L
    sep #$20
    .a8
    ; first siphon line this frame? set VMAIN + VMADD once (safe: we are
    ; about to force-blank inside H-blank, and it only auto-increments after)
    lda K_SIPHON_STARTED
    bne @dma
    lda #$01
    sta K_SIPHON_STARTED
    lda #$80
    sta VMAIN                   ; word step, increment after the high byte
    rep #$20
    .a16
    lda K_SIPHON_VRAM
    sta VMADDL
    sep #$20
    .a8
@dma:
    ; spin to the right-edge H-blank, then force-blank + DMA + unblank all
    ; inside it (off-screen, so no visible pixel is blanked).
@hbw:
    lda HVBJOY                  ; $4212
    and #$40                    ; b6 = H-blank
    beq @hbw
    lda #$8F
    sta INIDISP
    lda #$01
    sta MDMAEN                  ; the siphon burst (CPU stalls until done)
    lda #$0F
    sta INIDISP
    ; advance the running source cursor by bytes/line
    rep #$20
    .a16
    lda K_SIPHON_SRC
    clc
    adc K_SIPHON_BPL
    sta K_SIPHON_SRC
    sep #$20
    .a8
    rts
.endproc

; ------------------------------------------------------------------
; nmi_stub — NMI is disabled; park RAMVEC_NMI here so a stray NMI is
; harmless rather than a wild jump.
; ------------------------------------------------------------------
.proc nmi_stub
    rti
.endproc

; ==================================================================
;  Data: BG1 CHR (2 tiles) + palette (2 colours)
; ==================================================================

; tile 0 = empty (colour 0 backdrop); tile 1 = solid colour index 1.
; 4bpp tile = 16 bytes (planes 0/1 interleaved) + 16 bytes (planes 2/3).
; Solid colour 1: plane0 = $FF on every row, all other planes 0.
chr_data:
    .res 32, 0                  ; tile 0: empty
    .repeat 8
    .byte $FF, $00              ; tile 1: plane0=$FF, plane1=$00 (colour 1)
    .endrepeat
    .res 16, 0                  ; tile 1: planes 2/3 = 0
chr_data_end:
CHR_SIZE = chr_data_end - chr_data

; CGRAM (BGR555): 0 = dark-blue backdrop, 1 = bright-green bar.
pal_data:
    .word $4000                 ; 0: backdrop (blue)
    .word $03E0                 ; 1: bar (green)
pal_data_end:
PAL_SIZE = pal_data_end - pal_data
