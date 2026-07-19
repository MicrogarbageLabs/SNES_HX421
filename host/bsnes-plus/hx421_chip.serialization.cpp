// State lives inside hx421.dll; nothing meaningful to serialize on the
// bsnes side. The DLL's internal state (engine, audio ring, cart window)
// is NOT part of SNES save states — the cart runtime owns its own
// save/load, not the save-state stream.
#ifdef HX421_CPP
void Hx421::serialize(serializer &s) {
  // Save just the step-pacing counter so a serialize/deserialize
  // round-trip resumes cleanly.
  s.integer(samples_until_step);
}
#endif
