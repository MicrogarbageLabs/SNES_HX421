/* hx421.h — HX-421 coprocessor cartridge-seam contract (public ABI)
 *
 * The single, platform-neutral header that defines how a host drives the HX-421
 * coprocessor runtime. TWO hosts implement the SNES-bus side against this ABI:
 *   - ares (PC):  a forked coprocessor board + DLL shim (dev; cycle-accurate)
 *   - FPGA + M3 (hardware): the real cart
 * The runtime (engine/ + a thin wrapper) is IDENTICAL across both; only the seam
 * implementation differs. Flat C, extern "C", dual-target: a Windows DLL for the
 * ares host, or a static lib linked into the M3 firmware.
 *
 * Full rationale: docs/emulation-seam.md. Derived from the mgapi ABI (reference
 * template only — no dependency on microgarbage).
 *
 * Per-frame cadence (host):
 *   once:            hx421_init(&cfg)
 *   each frame:      hx421_post_joypads()/post_mouse(); hx421_step(elapsed_ns)
 *   during CPU exec: hx421_cart_read(addr) per cart-bus byte the SNES fetches
 *   audio cadence:   hx421_audio_pull(dst, frames)   (host owns the sink)
 *   reset:           reset_begin() -> loop step() while !reset_ready() -> reset_end()
 */
#ifndef HX421_H
#define HX421_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- Linkage (dual-target) --------------------------------------------- */
#if defined(HX421_STATIC)            /* linked into firmware / static test build */
#  define HX421_API
#elif defined(HX421_BUILDING_DLL)    /* building the PC DLL */
#  if defined(_WIN32)
#    define HX421_API __declspec(dllexport)
#  else
#    define HX421_API __attribute__((visibility("default")))
#  endif
#else                                /* host importing the PC DLL */
#  if defined(_WIN32)
#    define HX421_API __declspec(dllimport)
#  else
#    define HX421_API
#  endif
#endif

/* ---- ABI version (explicit — not a reserved-pad zero-check) ------------- */
#define HX421_ABI_VERSION_MAJOR 1u
#define HX421_ABI_VERSION_MINOR 0u   /* bump MINOR for additive fields/functions */
#define HX421_ABI_VERSION \
    (((uint32_t)HX421_ABI_VERSION_MAJOR << 16) | (uint32_t)HX421_ABI_VERSION_MINOR)

/* ---- Fixed constants --------------------------------------------------- */
#define HX421_CART_WINDOW_BYTES     (64u * 1024u) /* SNES-visible ROM window */
#define HX421_AUDIO_RATE_HZ         44100u        /* master/487; mixer pinned here */
#define HX421_AUDIO_BYTES_PER_FRAME 4u            /* interleaved stereo int16 */
#define HX421_MAX_PADS              4u

/* rom_select */
enum {
    HX421_ROM_SMOKE = 0,  /* built-in smoke/menu */
    HX421_ROM_BOOT  = 1,  /* boot banner / autostart */
    HX421_ROM_NONE  = 2
};

/* ---- Mailbox: magic addresses inside the 64 KB window (read-only pull) ---
 * The host never writes the window; it reads these via hx421_cart_read. Reads
 * carry address-keyed side effects (latch an index, advance a one-shot). v1
 * keeps the proven mgapi scheme; revisit for a dedicated mailbox later. */
#define HX421_MB_JOYPAD_BASE   0x7000u  /* 0x7000-0x77FF: joypad-mailbox readback */
#define HX421_MB_FRAME_READY   0x7800u  /* frame-ready byte */
#define HX421_MB_DMA_DESC_BASE 0x7808u  /* 0x7808-0x7847: DMA descriptor list */
#define HX421_MB_BOOT_RUNTIME  0x7E00u  /* read once: status boot -> runtime */
#define HX421_MB_STATUS        0x7F00u  /* status byte */

/* ---- Init config ------------------------------------------------------- */
typedef struct Hx421ResetConfig {
    uint32_t hold_ms;               /* 0 => default (~50 ms; models CIC + restage) */
} Hx421ResetConfig;

typedef struct Hx421Config {
    uint32_t abi_version;           /* MUST == HX421_ABI_VERSION */
    uint32_t cart_window_size;      /* MUST == HX421_CART_WINDOW_BYTES */
    uint32_t audio_sample_rate;     /* 0 = auto (host queries device); MCU passes
                                       HX421_AUDIO_RATE_HZ. (!=0 && <8000) => -EINVAL */
    uint32_t audio_frames_max;      /* upper bound per audio_pull; sizes the ring. !=0 */
    uint8_t  pad_count;             /* pads the SNES polls; <= HX421_MAX_PADS */
    uint8_t  rom_select;            /* HX421_ROM_* */
    uint8_t  flags;                 /* reserved flags; 0 for now */
    uint8_t  _reserved0;
    const char *autostart_path;     /* NULL = none */
    Hx421ResetConfig reset;
    uint32_t _reserved[4];          /* MUST be zero (future growth) */
} Hx421Config;

/* ---- Lifecycle --------------------------------------------------------- */
HX421_API int         hx421_init(const Hx421Config *cfg); /* 0, or negative errno */
HX421_API void        hx421_shutdown(void);               /* re-init allowed after */
HX421_API const char *hx421_version(void);                /* build string, non-NULL */
HX421_API uint32_t    hx421_abi_version(void);            /* the build's HX421_ABI_VERSION */

/* ---- Cart bus (read-only pull) ----------------------------------------- */
/* Serve ONE cart-bus byte. addr = full 24-bit SNES address (window mirror
 * applied internally). Never blocks. May have address-keyed side effects. */
HX421_API uint8_t     hx421_cart_read(uint32_t addr);

/* ---- Tick -------------------------------------------------------------- */
/* Advance the runtime by a wall-clock delta. Returns immediately (signals a
 * worker); the host thread never blocks on VM/audio. 0 => default quantum. */
HX421_API void        hx421_step(uint64_t elapsed_ns);

/* ---- Audio (real pull; host owns the sink) ----------------------------- */
/* Drain up to `frames` mixed frames into dst (interleaved stereo int16 @
 * HX421_AUDIO_RATE_HZ). Returns frames actually written (short on underrun).
 * The cumulative pull count is the SNES master-clock reference that drives the
 * drift PLL (see docs/emulation-seam.md). */
HX421_API uint32_t    hx421_audio_pull(int16_t *dst_stereo, uint32_t frames);

/* ---- Audio command channel (game logic -> mixer) ----------------------- */
/* Audio commands come from the game/player logic — the RISC-V soft core on
 * hardware, a host driver on the emulator — NOT over the SNES bus. On hardware
 * the RISC-V fills a ring of Hx421AudioCmd and the M3 drains it; this function
 * IS that drain/dispatch, callable directly. Additive to the ABI (an older host
 * simply never calls it). */
enum {
    HX421_ACMD_NONE = 0,
    HX421_ACMD_LOAD_SFX,        /* slot = sfx slot; arg = built-in asset id      */
    HX421_ACMD_TRIGGER,         /* slot = sfx slot; gain/pan q15 -> voice handle */
    HX421_ACMD_PLAY_MUSIC,      /* arg = music asset (0 = configured WAV) -> voice*/
    HX421_ACMD_STOP,            /* arg = voice handle                            */
    HX421_ACMD_STOP_ALL,
    HX421_ACMD_SET_MASTER_VOL   /* gain q15 (reserved; v1 stub)                  */
};

typedef struct Hx421AudioCmd {
    uint16_t opcode;            /* HX421_ACMD_*                                   */
    uint16_t slot;             /* LOAD_SFX / TRIGGER: sfx slot index             */
    int16_t  gain;             /* q15 (TRIGGER, SET_MASTER_VOL); 0 => unity       */
    int16_t  pan;              /* q15 (TRIGGER)                                   */
    uint32_t arg;              /* LOAD_SFX/PLAY_MUSIC asset id; STOP voice handle */
} Hx421AudioCmd;

/* Submit one audio command. Returns a voice handle for TRIGGER/PLAY_MUSIC
 * (>=0), 0 for other successful commands, or negative on error. */
HX421_API int32_t     hx421_audio_command(const Hx421AudioCmd *cmd);

/* ---- Input ------------------------------------------------------------- */
/* SNES auto-joypad word layout: B Y Sel Start Up Dn Lt Rt A X L R (+ type sig). */
HX421_API void        hx421_post_joypads(const uint16_t pads[HX421_MAX_PADS]);
HX421_API void        hx421_post_mouse(int dx, int dy, unsigned buttons); /* bit0 L, bit1 R */

/* ---- Cart reset marshalling -------------------------------------------- */
HX421_API void        hx421_cart_reset_begin(void); /* hold; restage window */
HX421_API int         hx421_cart_reset_ready(void); /* 1 once hold_ms elapsed */
HX421_API void        hx421_cart_reset_end(void);   /* ack release */

#ifdef __cplusplus
} /* extern "C" */
#endif
#endif /* HX421_H */
