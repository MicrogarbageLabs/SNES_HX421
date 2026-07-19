// ============================================================
//  hx421_chip.hpp — HX-421 coprocessor cart runtime, bsnes-plus side.
//
//  Loads hx421.dll (the HX-421 runtime: engine + audio mixer + SNES
//  cart-window decode) and routes the SNES cart bus + audio + reset
//  through it. Modeled on the proven mgapi cart class, but this is
//  HX-421's OWN code with NO microgarbage dependency: it speaks the
//  hx421.h ABI (see SNES_HX_421/include/hx421.h).
//
//  Opt-in: only instantiated when $HX421_ENABLE is set (checked in
//  cartridge.cpp), so a normal ROM still uses mgapi / normal mapping.
//  The two chips coexist; this one never regresses mgapi.
//
//  IMPROVEMENT over mgapi: our hx421.dll implements a REAL audio pull
//  (mgapi's DLL pushed to its own WASAPI sink and returned silence).
//  So enter() feeds bsnes's own Stream directly from hx421_audio_pull
//  — real audio through bsnes's pipeline (the MSU1 model).
//
//  Public domain (CC0). No warranty.
// ============================================================

// Neutralize the ABI header's linkage decorations: we resolve every
// entry point via GetProcAddress into function pointers, so we must
// NOT pull in __declspec(dllimport) prototypes. HX421_STATIC makes
// HX421_API expand to nothing; we only use the header's Config struct,
// constants, and enums (never its function declarations).
#ifndef HX421_STATIC
#  define HX421_STATIC
#endif
#include "hx421.h"

class Hx421 : public Memory, public Coprocessor, public Stream {
public:
  static void Enter();
  void enter();

  // Lifecycle (parallel to MSU1 / mgapi hook points). init() runs once;
  // enable() after cart load when has_hx421 is true; power()/reset()
  // bracket resets; unload() at cart eject.
  void init();
  void enable();
  void power();
  void reset();
  void unload();

  // Try to load hx421.dll from the bsnes binary's working dir. Returns
  // true on success; the DLL then stays resident for the process life.
  bool try_load();
  bool is_loaded() const { return dll_handle != nullptr; }

  // Memory interface — the bus calls this for any address in our mapped
  // HiROM ranges. Cart bus is read-only; write is a no-op.
  uint8 read(unsigned addr);
  void  write(unsigned addr, uint8 data);
  unsigned size() const;

  void serialize(serializer&);

private:
  void *dll_handle;

  // hx421.dll function pointers (resolved at try_load time). Signatures
  // mirror hx421.h exactly.
  int         (*p_init)(const Hx421Config *);
  void        (*p_shutdown)();
  const char *(*p_version)();
  uint32_t    (*p_abi_version)();
  uint8_t     (*p_cart_read)(uint32_t);
  void        (*p_step)(uint64_t);
  uint32_t    (*p_audio_pull)(int16_t *, uint32_t);
  void        (*p_post_joypads)(const uint16_t *);
  void        (*p_post_mouse)(int, int, unsigned);   // optional export
  int32_t     (*p_audio_command)(const Hx421AudioCmd *);   // HX421 cmd (optional export)
  void        (*p_cart_reset_begin)();
  int         (*p_cart_reset_ready)();
  void        (*p_cart_reset_end)();

  // Step pacing: we call hx421_step once per "frame" (16.67 ms) so the
  // runtime advances at SNES speed. The coprocessor enter() loop runs
  // at 44100 Hz, so 735 audio frames = one 60 Hz frame; we count down.
  unsigned samples_until_step;

  // HX421 cmd: edge-triggered audio command channel state. prev_pad0 holds
  // last frame's port-1 button word (for 0->1 edge detect); music_on tracks
  // the Start-toggle; cmd_enabled gates the whole thing on $HX421_CMD.
  uint16_t prev_pad0;
  bool     music_on;
  bool     cmd_enabled;

  // Diagnostics (behind the enable flag): pulled-frame count + running
  // sum-of-squares so a launch can be confirmed non-silent from stderr.
  uint64_t dbg_frames;
  double   dbg_sqsum;
  unsigned dbg_steps;
};

extern Hx421 hx421;
