// ============================================================
//  hx421_chip.cpp — bsnes-plus side of the HX-421 cart runtime.
//
//  Loads hx421.dll via LoadLibrary, resolves its exports, and routes
//  the SNES cart bus + audio + reset through it. The DLL contains the
//  whole HX-421 runtime; this file is the bsnes-side adapter, modeled
//  on MSU1 / the mgapi cart class. HX-421's own code — no microgarbage
//  dependency; it speaks the hx421.h ABI only.
//
//  Public domain (CC0). No warranty.
// ============================================================
#include <snes.hpp>
#include <math.h>
#include <stdlib.h>   // HX421 cmd: getenv
#include <string.h>   // HX421 cmd: memset

#define HX421_CPP
namespace SNES {

Hx421 hx421;

#include "hx421_chip.serialization.cpp"

#if defined(_WIN32)
  #include <windows.h>
  typedef HMODULE DllHandle;
  static DllHandle dll_open(const char *path)          { return ::LoadLibraryA(path); }
  static void      dll_close(DllHandle h)              { ::FreeLibrary(h); }
  static void     *dll_sym(DllHandle h, const char *n) { return (void *)::GetProcAddress(h, n); }
#else
  #include <dlfcn.h>
  typedef void *DllHandle;
  static DllHandle dll_open(const char *path)          { return ::dlopen(path, RTLD_NOW); }
  static void      dll_close(DllHandle h)              { ::dlclose(h); }
  static void     *dll_sym(DllHandle h, const char *n) { return ::dlsym(h, n); }
#endif

template <typename F>
static bool resolve(DllHandle h, const char *name, F &out) {
  void *p = dll_sym(h, name);
  if(!p) {
    fprintf(stderr, "hx421: missing export '%s'\n", name);
    return false;
  }
  // Bounce through void* to keep strict-aliasing happy on MSVC + mingw.
  out = reinterpret_cast<F>(p);
  return true;
}

bool Hx421::try_load() {
  if(dll_handle) return true;
  DllHandle h = dll_open("hx421.dll");
  if(!h) {
#if defined(_WIN32)
    fprintf(stderr,
            "hx421: LoadLibrary(\"hx421.dll\") FAILED, GetLastError=%lu\n",
            (unsigned long)::GetLastError());
#else
    fprintf(stderr, "hx421: dlopen(\"hx421.dll\") FAILED: %s\n", dlerror());
#endif
    fprintf(stderr,
            "hx421: cart bus will fall through to the loaded .sfc bytes.\n"
            "  fix: make sure hx421.dll (+ any libgcc/libwinpthread runtime\n"
            "  DLLs it needs) are present alongside bsnes.exe.\n");
    fflush(stderr);
    return false;
  }

  bool ok =
    resolve(h, "hx421_init",              p_init)              &&
    resolve(h, "hx421_shutdown",          p_shutdown)          &&
    resolve(h, "hx421_version",           p_version)           &&
    resolve(h, "hx421_abi_version",       p_abi_version)       &&
    resolve(h, "hx421_cart_read",         p_cart_read)         &&
    resolve(h, "hx421_step",              p_step)              &&
    resolve(h, "hx421_audio_pull",        p_audio_pull)        &&
    resolve(h, "hx421_post_joypads",      p_post_joypads)      &&
    resolve(h, "hx421_cart_reset_begin",  p_cart_reset_begin)  &&
    resolve(h, "hx421_cart_reset_ready",  p_cart_reset_ready)  &&
    resolve(h, "hx421_cart_reset_end",    p_cart_reset_end);
  if(!ok) {
    dll_close(h);
    return false;
  }

  // Optional export (older DLLs may lack it): port-2 mouse forward.
  // Resolved directly so a missing symbol is silent, not a scary line.
  p_post_mouse = reinterpret_cast<decltype(p_post_mouse)>(dll_sym(h, "hx421_post_mouse"));

  // Optional export (ABI 1.1+): SNES -> cart mailbox writes. A DLL without it
  // simply leaves the cart read-only, which is the pre-1.1 behaviour.
  p_cart_write = reinterpret_cast<decltype(p_cart_write)>(dll_sym(h, "hx421_cart_write"));

  // HX421 cmd: optional audio command channel export (older DLLs lack it —
  // a missing symbol must NOT fail load). Gated at runtime on $HX421_CMD so
  // the interactive test only issues commands when explicitly asked.
  p_audio_command = reinterpret_cast<decltype(p_audio_command)>(dll_sym(h, "hx421_audio_command"));
  cmd_enabled = (getenv("HX421_CMD") != nullptr);

  // ABI handshake. Only the MAJOR must match: MINOR bumps are additive by
  // definition (new functions, new trailing fields), and every added export is
  // resolved optionally, so an older host runs happily against a newer DLL —
  // it simply does not use what it has never heard of.
  //
  // Checking strict equality here defeats the whole point of having a
  // major/minor split, and fails in a spectacularly confusing way: the chip
  // refuses to load, bsnes falls back to normal mapping, and the SNES boots
  // whatever trigger .sfc was passed as if it were an ordinary cart.
  uint32_t dll_abi   = p_abi_version();
  uint32_t dll_major = dll_abi >> 16, dll_minor = dll_abi & 0xFFFF;
  if(dll_major != HX421_ABI_VERSION_MAJOR) {
    fprintf(stderr,
            "hx421: ABI MAJOR mismatch — header %u.%u, dll %u.%u. refusing to load.\n",
            (unsigned)HX421_ABI_VERSION_MAJOR, (unsigned)HX421_ABI_VERSION_MINOR,
            (unsigned)dll_major, (unsigned)dll_minor);
    dll_close(h);
    return false;
  }
  if(dll_minor != HX421_ABI_VERSION_MINOR) {
    fprintf(stderr,
            "hx421: ABI minor differs — header %u.%u, dll %u.%u. continuing "
            "(additive only; unknown exports stay unused).\n",
            (unsigned)HX421_ABI_VERSION_MAJOR, (unsigned)HX421_ABI_VERSION_MINOR,
            (unsigned)dll_major, (unsigned)dll_minor);
  }

  Hx421Config cfg = {};
  cfg.abi_version       = HX421_ABI_VERSION;
  cfg.cart_window_size  = HX421_CART_WINDOW_BYTES;   // 65536
  cfg.audio_sample_rate = HX421_AUDIO_RATE_HZ;       // 44100 (fixed sink)
  cfg.audio_frames_max  = 4096;
  cfg.pad_count         = 2;
  cfg.rom_select        = HX421_ROM_SMOKE;           // self-generated 440 Hz tone
  cfg.flags             = 0;
  cfg.autostart_path    = nullptr;
  cfg.reset.hold_ms     = 50;                         // CIC-equivalent + restage

  int rc = p_init(&cfg);
  if(rc != 0) {
    fprintf(stderr, "hx421: hx421_init failed: %d\n", rc);
    dll_close(h);
    return false;
  }
  dll_handle = h;
  fprintf(stderr, "hx421: loaded (%s), abi=%08x\n", p_version(), (unsigned)dll_abi);
  fflush(stderr);
  return true;
}

// ----------------------------------------------------------------
//  Memory interface
// ----------------------------------------------------------------

uint8 Hx421::read(unsigned addr) {
  if(!dll_handle) return cpu.regs.mdr;   // open-bus when not loaded
  return p_cart_read(addr & 0xFFFFFF);
}

void Hx421::write(unsigned addr, uint8 data) {
  // Forward everything; the DLL accepts only its mailbox window and ignores
  // the rest, so the cart stays effectively read-only outside it. Filtering
  // here as well would duplicate the address map in two places.
  if(dll_handle && p_cart_write) p_cart_write(addr & 0xFFFFFF, data);
}

unsigned Hx421::size() const {
  // 16 MB virtual span (whole HiROM region) so map mirror arithmetic
  // doesn't wrap. Real backing is the 64 KB window inside hx421.dll.
  return 0x1000000;
}

// ----------------------------------------------------------------
//  Coprocessor + audio stream
// ----------------------------------------------------------------

void Hx421::Enter() { hx421.enter(); }

// Pack one port's joypad button bits into the SNES auto-joypad word
// shape (bit15=B ... bit4=R, bits3..0 = controller-type signature).
// Caller MUST run SNES::input.poll() first. Same path as the mgapi
// chip; see its rationale for why port_read is used over input_poll.
static uint16_t hx421_pack_joypad_port(bool port_index) {
  uint16_t w = 0;
  for(unsigned i = 0; i < 16; i++) {
    w = (uint16_t)((w << 1) | (SNES::input.port_read(port_index) & 1u));
  }
  return w;
}

void Hx421::enter() {
  while(true) {
    scheduler.synchronize();

    int16 left = 0, right = 0;
    if(dll_handle) {
      // REAL pull: drain one stereo frame from hx421.dll's mixer and
      // feed it straight into bsnes's Stream. Underrun → silence.
      int16_t buf[2] = {0, 0};
      uint32_t got = p_audio_pull(buf, 1);
      if(got > 0) { left = buf[0]; right = buf[1]; }

      // Diagnostics: confirm the launch is non-silent from stderr.
      dbg_frames += 1;
      dbg_sqsum  += (double)left * (double)left + (double)right * (double)right;

      // Advance the runtime once per 60 Hz frame's worth of audio
      // (735 ≈ 44100/60). Drives the VM scheduler / audio render;
      // joypad post happens here so its cadence matches step().
      if(samples_until_step == 0) {
        SNES::input.poll();
        uint16_t pads[4] = {0, 0, 0, 0};
        pads[0] = hx421_pack_joypad_port(false);   // port 1
        // Port 2 is read as a SNES Mouse below, not packed as a pad.
        p_post_joypads(pads);

        // HX421 cmd: edge-triggered audio command channel. On each 0->1
        // button transition on port 1, submit an Hx421AudioCmd to the DLL.
        // A/B/X/Y trigger SFX slots 0..3; Start toggles music; Select stops.
        if(cmd_enabled && p_audio_command) {
          uint16_t now = pads[0];
          uint16_t pressed = (uint16_t)(now & ~prev_pad0);   // 0->1 this frame
          prev_pad0 = now;
          Hx421AudioCmd c;
          // SNES auto-joypad word bits (from hx421_pack_joypad_port):
          //   B=0x8000 Y=0x4000 Select=0x2000 Start=0x1000  A=0x0080 X=0x0040 L=0x0020 R=0x0010
          if(pressed & 0x0080){ memset(&c,0,sizeof c); c.opcode=HX421_ACMD_TRIGGER; c.slot=0; p_audio_command(&c); } // A
          if(pressed & 0x8000){ memset(&c,0,sizeof c); c.opcode=HX421_ACMD_TRIGGER; c.slot=1; p_audio_command(&c); } // B
          if(pressed & 0x0040){ memset(&c,0,sizeof c); c.opcode=HX421_ACMD_TRIGGER; c.slot=2; p_audio_command(&c); } // X
          if(pressed & 0x4000){ memset(&c,0,sizeof c); c.opcode=HX421_ACMD_TRIGGER; c.slot=3; p_audio_command(&c); } // Y
          if(pressed & 0x1000){ memset(&c,0,sizeof c); c.opcode = music_on?HX421_ACMD_STOP_ALL:HX421_ACMD_PLAY_MUSIC; p_audio_command(&c); music_on=!music_on; } // Start toggles music
          if(pressed & 0x2000){ memset(&c,0,sizeof c); c.opcode=HX421_ACMD_STOP_ALL; p_audio_command(&c); music_on=false; } // Select = stop all
        }

        if(p_post_mouse) {
          unsigned b[32];
          for(unsigned i = 0; i < 32; i++) b[i] = SNES::input.port_read(true) & 1u;
          int xmag = (b[25]<<6)|(b[26]<<5)|(b[27]<<4)|(b[28]<<3)|(b[29]<<2)|(b[30]<<1)|b[31];
          int ymag = (b[17]<<6)|(b[18]<<5)|(b[19]<<4)|(b[20]<<3)|(b[21]<<2)|(b[22]<<1)|b[23];
          int dx = b[24] ? -xmag : xmag;
          int dy = b[16] ? -ymag : ymag;
          unsigned btn = (b[9] ? 1u : 0u) | (b[8] ? 2u : 0u);
          p_post_mouse(dx, dy, btn);
        }

        p_step(16666666ull);
        samples_until_step = 735;

        // Once/sec: print pulled-frame count + RMS so a live launch is
        // provably producing audio (RMS > 0 == the tone is flowing).
        if(++dbg_steps >= 60) {
          double rms = dbg_frames ? sqrt(dbg_sqsum / (double)(dbg_frames * 2)) : 0.0;
          fprintf(stderr, "hx421: pulled %llu frames, rms=%.1f\n",
                  (unsigned long long)dbg_frames, rms);
          fflush(stderr);
          dbg_steps = 0;
        }
      } else {
        samples_until_step--;
      }
    }

    sample(left, right);
    step(1);
    synchronize_cpu();
  }
}

// ----------------------------------------------------------------
//  Lifecycle (matches MSU1 / mgapi hook points)
// ----------------------------------------------------------------

void Hx421::init() {
  // Nothing — try_load does the real work; init runs even for non-hx421
  // carts, so it stays inert.
}

void Hx421::enable() {
  audio.add_stream(this);
  audio_frequency(44100.0);
  // Cart owns all audio; the SNES DSP stays reset-muted (SPC700 never
  // programmed), so opt this stream out of the DSP-mute zeroing.
  ignore_dsp_mute = true;
}

void Hx421::unload() {
  if(dll_handle) {
    p_shutdown();
    dll_close((DllHandle)dll_handle);
    dll_handle = nullptr;
  }
}

void Hx421::power() { reset(); }

void Hx421::reset() {
  create(Hx421::Enter, 44100);
  samples_until_step = 0;
  prev_pad0 = 0;        // HX421 cmd
  music_on  = false;    // HX421 cmd
  dbg_frames = 0;
  dbg_sqsum  = 0.0;
  dbg_steps  = 0;

  if(dll_handle) {
    // Real-hardware-style reset: hold the cart bus while the runtime
    // restages the window and the CIC-equivalent timer elapses.
    p_cart_reset_begin();
    while(!p_cart_reset_ready()) {
      p_step(1000000ull);   // 1 ms of runtime per spin
    }
    p_cart_reset_end();
  }
}

}  // namespace SNES
