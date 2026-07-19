; ============================================================
;  snes.inc — SNES hardware registers this boot ROM touches.
;  A focused subset (PPU $21xx + CPU/DMA $42xx/$43xx). Standard
;  hardware equates — no HX-421 / coprocessor specifics here.
;  Public domain (CC0). No warranty.
; ============================================================

; --- PPU ($21xx) ---
INIDISP   = $2100      ; b7 force-blank, b0-3 brightness
OBSEL     = $2101      ; OBJ size/base
BGMODE    = $2105      ; BG mode + tile-size bits
MOSAIC    = $2106      ; mosaic enable + size
BG1SC     = $2107      ; BG1 tilemap base + screen size
BG2SC     = $2108      ; BG2 tilemap base + screen size
BG3SC     = $2109      ; BG3 tilemap base + screen size
BG4SC     = $210A      ; BG4 tilemap base + screen size
BG12NBA   = $210B      ; BG1/BG2 char base
BG34NBA   = $210C      ; BG3/BG4 char base
BG1HOFS   = $210D      ; BG1 horizontal scroll (write-twice)
BG1VOFS   = $210E      ; BG1 vertical scroll
BG2HOFS   = $210F
BG2VOFS   = $2110
BG3HOFS   = $2111
BG3VOFS   = $2112
VMAIN     = $2115      ; VRAM addr increment mode
VMADDL    = $2116      ; VRAM word address low
VMADDH    = $2117      ; VRAM word address high
VMDATAL   = $2118      ; VRAM data write low
VMDATAH   = $2119      ; VRAM data write high
CGADD     = $2121      ; CGRAM word address
CGDATA    = $2122      ; CGRAM data write
; window mask + colour-math ($2123-$212B, $212E-$2133) — boot zeroes these
; so the PPU starts from a clean baseline (no stray clip / tint / interlace).
W12SEL    = $2123
W34SEL    = $2124
WOBJSEL   = $2125
WH0       = $2126
WH1       = $2127
WH2       = $2128
WH3       = $2129
WBGLOG    = $212A
WOBJLOG   = $212B
TM        = $212C      ; main-screen layer enable
TS        = $212D      ; sub-screen layer enable
TMW       = $212E
TSW       = $212F
CGWSEL    = $2130
CGADSUB   = $2131
COLDATA   = $2132
SETINI    = $2133      ; screen mode / interlace
; --- H/V counter read-back (M2 action-table kernel) ---
SLHV      = $2137      ; software latch of H/V counters (read to latch)
OPVCT     = $213D      ; V counter (read low then high; bit8 in high byte)
STAT78    = $213F      ; PPU2 status (read resets OPHCT/OPVCT read toggle)

; --- CPU / DMA ($42xx, $43xx) ---
NMITIMEN  = $4200      ; b7 NMI enable, b0 auto-joypad, b4 HIRQ, b5 VIRQ
HTIMEL    = $4207      ; H-counter IRQ target low byte
HTIMEH    = $4208      ; H-counter IRQ target high byte (bit 0)
VTIMEL    = $4209      ; V-counter IRQ target low byte
VTIMEH    = $420A      ; V-counter IRQ target high byte (bit 0)
RDNMI     = $4210      ; read to ack NMI (b7 = vblank flag)
TIMEUP    = $4211      ; read to acknowledge an H/V-timer IRQ
HVBJOY    = $4212      ; b7 vblank, b0 auto-joypad busy
MDMAEN    = $420B      ; general-DMA enable (1 bit per channel)
HDMAEN    = $420C      ; HDMA enable (1 bit per channel)

; DMA channel 0 registers
DMAP0     = $4300      ; transfer pattern + direction
BBAD0     = $4301      ; B-bus address ($21xx low byte)
A1T0L     = $4302      ; A-bus address low
A1T0H     = $4303      ; A-bus address high
A1B0      = $4304      ; A-bus bank
DAS0L     = $4305      ; byte count low
DAS0H     = $4306      ; byte count high
