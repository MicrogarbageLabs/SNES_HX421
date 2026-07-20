; ============================================================
;  hx421.inc — HX-421 SNES <-> coprocessor window map (milestone 1).
;
;  The HX-421 coprocessor presents a 64 KB window as the cart ROM.
;  bsnes' hx421 chip maps it HiROM-ish:
;      banks $00-3f,$80-bf : $8000-$FFFF  -> window $8000-$FFFF
;      banks $c0-ff        : $0000-$FFFF  -> window $0000-$FFFF (flat)
;  So the whole window is flat-addressable through bank $C0, clear of
;  WRAM/registers (which live in the low half of the low banks).
;
;  Milestone-1 scope: STANDALONE boot ROM. No VM, no cart-release
;  handshake, no per-frame data channel. The only coprocessor touch is
;  one boot-progress read-strobe the DLL logs (see below). $0000-$7FFF
;  is the DLL's mailbox/staging region and is otherwise left alone.
;
;  Public domain (CC0). No warranty.
; ============================================================

; HiROM full-window bank — a `lda f:<addr in $C0xxxx>` reads the cart
; window with DBR left at $00 (WRAM/regs live in banks $00-3f low half).
HX421_BANK          = $C0

; --- boot-progress read-strobe (milestone 1) -----------------------
; boot.s reads this ONE 24-bit address just before it jmp's into the
; WRAM kernel. The bus is read-only, so the *access itself* is the
; signal: the DLL watches for a read here and logs
;   "boot handoff — kernel staged in WRAM, jumping"
; which proves the reset stub executed all the way through the copy
; loop. Chosen inside the DLL staging region ($0000-$7FFF) but clear of
; the reserved mailbox bytes (status $7F00, frame-ready $7800,
; boot-runtime $7E00, joypad $7000-$77FF). Returned byte is don't-care.
HX421_BOOT_STROBE   = (HX421_BANK << 16) | $7FE0

; --- WRAM layout (bank $00 low RAM) --------------------------------
;   $0000-$00FF direct page   $0100-$01FF stack
;   $0200-$03FF kernel vars    $0400-$1FFF kernel code (copied here)
; The hardware NMI/IRQ vectors point at trampolines in the static top
; page that bounce through these WRAM pointers, so a RAM kernel can own
; the interrupts. Milestone 1 runs interrupts-OFF (static screen), but
; the trampolines still exist and the kernel points them at a safe RTI.
RAMVEC_NMI          = $0200
RAMVEC_IRQ          = $0202

; ============================================================
;  Milestone 2 — dynamic per-frame renderer WINDOW CONTRACT.
;
;  A free-running H-timer IRQ (NMITIMEN=$10, fires every scanline at
;  H=HTIME) reads a PER-LINE ACTION TABLE indexed by the live V counter
;  and drives the force-blank letterbox + the general-DMA frame push.
;  The DLL (playing the RISC-V) freewheels frame data into the served
;  window, DOUBLE-BUFFERED; the SNES reads the FRONT buffer, the DLL
;  writes the BACK buffer, and the swap is gated on a frame-ready flag.
;
;  Full prose: docs/window-contract.md.
;
;  --- per-line action codes (one byte per V line in the action table) ---
;      0 = nothing        (leave INIDISP as-is)
;      1 = force-blank     (INIDISP=$8F; opens VRAM DMA; letterbox+vblank)
;      2 = unblank/render  (INIDISP=$0F; the top-letterbox reveal line)
;      3 = siphon          (per-scanline H-blank chunked VRAM push; see the
;                           siphon descriptor + siphon_line in kernel.s)
; ============================================================
ACT_NONE            = 0
ACT_BLANK           = 1
ACT_UNBLANK         = 2
ACT_SIPHON          = 3

; --- double-buffered frame region (two buffers in the low window) -----
; Each buffer is self-contained: action table + a tiny frame header +
; the DMA descriptor list + the staged tilemap. The DLL owns whichever
; buffer is NOT currently front; the SNES latches front once per frame
; (at the V-wrap boundary) and reads only that buffer all frame, so a
; per-line action read can never see a half-written table.
HX_BUF0_BASE        = $0000
HX_BUF1_BASE        = $3000
HX_BUF_STRIDE       = $3000

; offsets inside a buffer
HX_OFF_ACTION       = $0000     ; 512 B: action[V], V = 0..261 (9-bit indexed)
HX_OFF_HDR          = $0200     ; frame header (see below)
HX_OFF_HDR_TOP_LB   = $0200     ;   +0 top-letterbox line count (unblank line)
HX_OFF_HDR_VIS_END  = $0201     ;   +1 first bottom-letterbox line (224 - bot_lb)
; --- H-blank siphon descriptor (the per-scanline chunked VRAM push) ----
; The siphon is NOT HDMA: it is a plain channel-0 GP-DMA to VMDATA fired
; inside each visible line's right-edge H-blank (dot ~274+, $4212 b6), which
; the SNES lets land in VRAM even while the display is live. Which lines run
; it is encoded in the action table (ACT_SIPHON=3). This descriptor gives
; the payload: bytes/line, the VRAM word dst (set ONCE on the first siphon
; line, auto-increments after), and the source base (a running cursor that
; advances by bytes/line each line). SIP_BPL=0 disables the siphon.
HX_OFF_SIP_BPL      = $0202     ;   +2 word: siphon bytes per line (0 = off)
HX_OFF_SIP_VRAM     = $0204     ;   +4 word: VRAM word dst (set once, auto-incr)
HX_OFF_SIP_SRC      = $0206     ;   +6 word: source base offset in bank $C0
HX_OFF_DMABODY      = $0208     ; RISC-V-emitted 65816 DMA body (baked immediates)
HX_OFF_TILEMAP      = $0400     ; 2048 B: 32x32 BG1 tilemap words (the frame)

; The DMA body at $0208 is 65816 MACHINE CODE the coprocessor (RISC-V) emits
; each frame: a straight run of baked-immediate DMA slots --
;   lda #bbus:sta BBAD0 ; lda #dmap:sta DMAP0 ; lda #src:sta A1T0L ;
;   lda #size:sta DAS0L ; (dest prep) ; lda #$01:sta MDMAEN
; -- with the A-bus bank ($4304) set once up front, ending in RTL. The kernel
; jsl's straight into it (execute-from-window, the freewheel win) instead of
; walking a descriptor list: no per-slot descriptor long-load, no runtime dest
; dispatch -- the lowest-overhead path, maximizing blank-window DMA bandwidth
; (the microgarbage mg_nmi / copro_r3d emitter technique). The coprocessor owns
; the FIT budget: total DMA must land inside the bottom-LB+vblank+top-LB window.
; Room from $0208 up to the tilemap at $0400 (~504 B, ~10 slots).

; --- control mailbox (single-instance; NOT double-buffered) -----------
; Kept clear of the two frame buffers ($0000-$5FFF) and the M1 boot
; strobe ($7FE0). Addresses carry read-side effects like the mgapi scheme.
HX_FRAME_READY      = $7800     ; DLL writes: 0 = none; else (buf_index + 1)
HX_FRAME_DONE       = $79C1     ; SNES read-strobe: walked the last DMA slot;
                                ; the DLL flips buffers + clears frame-ready.

; 24-bit (bank $C0) forms so `lda f:<sym>` reaches the window with DBR=$00.
HX_FRAME_READY_L    = (HX421_BANK << 16) | HX_FRAME_READY
HX_FRAME_DONE_L     = (HX421_BANK << 16) | HX_FRAME_DONE

; --- SNES -> cart mailbox (the ONE writable window) --------------------
; The cart bus is read-only by design everywhere else; the coprocessor
; ignores writes outside this range, so a stray store cannot corrupt the
; staging buffers or the kernel image at $8000+. Payloads (joypad state,
; command blocks) go here; pure signals stay read-strobes like FRAME_DONE.
; Writing DOORBELL means "payload complete" — so the coprocessor acts on a
; whole block instead of polling a half-written one.
HX_MB_BASE          = $7000     ; 256 B writable
HX_MB_JOYPADS       = $7000     ; 8 B: 4 ports x 16-bit pad word
HX_MB_DOORBELL      = $70FF     ; write = payload complete
HX_MB_JOYPADS_L     = (HX421_BANK << 16) | HX_MB_JOYPADS
HX_MB_DOORBELL_L    = (HX421_BANK << 16) | HX_MB_DOORBELL

; Scanline at which the kernel pushes the pads. Auto-joypad latches early in
; vblank (~225) and takes ~3 lines, so 230 is safely after it completes.
PAD_LINE            = 230

; ============================================================
;  Kernel WRAM working set (bank $00 low RAM)
; ============================================================
; --- direct-page long pointers (for `lda [dp],y` window reads) ---
; MUST live in $00xx (the direct page): the [dp],y addressing mode takes
; an 8-bit dp operand. Each is a 24-bit pointer (lo, hi, bank=$C0).
K_FRONT_PTR         = $00E0     ; -> front buffer base (action table @ +0)

; --- kernel vars ($02xx, above RAMVEC_*) ---
K_VLO               = $0210     ; latched V counter low  (K_VLO|K_VHI = 9-bit V)
K_VHI               = $0211     ; latched V counter high bit
K_FRONT_IDX         = $0212     ; current front buffer index (0/1; $FF = none yet)
K_TOP_LB            = $0213     ; cached top-letterbox line count (unblank line)
K_VIS_END           = $0214     ; cached first bottom-letterbox line
K_BODY_PENDING      = $0215     ; 1 = fire the emitted DMA body once this frame
K_FRAME_ARMED       = $0216     ; 1 once this frame's prep ran (V>=VIS_END crossing)

; --- H-blank siphon runtime state ---
K_SIPHON_STARTED    = $0217     ; 1 after the first siphon line set VMADD this frame
K_SIPHON_BPL        = $0218     ; 16-bit: cached bytes/line (0 = siphon off)
K_SIPHON_VRAM       = $021A     ; 16-bit: cached VRAM word dst (set once/frame)
K_SIPHON_SRC_BASE   = $021C     ; 16-bit: cached source base (re-seeds the cursor)
K_SIPHON_SRC        = $021E     ; 16-bit: running source cursor (advances per line)
K_PAD_DONE          = $0220     ; 1 once this frame's mailbox pad push ran
