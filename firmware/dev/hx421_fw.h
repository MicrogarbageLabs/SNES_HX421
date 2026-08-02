/* ============================================================
 *  hx421_fw.h — the firmware-side glue: backend adapters over the stock
 *  sd2snes services (FatFs, the debug UART) + the load-and-run entry that ties
 *  sysimpl + the streaming loader together. This is the file that lives INSIDE
 *  the firmware fork. See docs/dev-mode.md.
 *
 *  Public domain (CC0). No warranty.
 * ============================================================ */

#ifndef HX421_FW_H
#define HX421_FW_H

/* Load a game .hxg from the SD card into the game region and run it. `hxg_path`
 * is the absolute path to the packed game; `sandbox_prefix` is the directory the
 * game's own file paths are confined to. Returns 0 on success, or the negated
 * Hx421LoadResult on a load failure. Blocks until the game returns. */
int hx421_fw_run_game(const char *hxg_path, const char *sandbox_prefix);

#endif /* HX421_FW_H */
