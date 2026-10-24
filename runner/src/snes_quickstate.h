#ifndef SNESRECOMP_SNES_QUICKSTATE_H
#define SNESRECOMP_SNES_QUICKSTATE_H

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Quick save / quick load — a rolling stash of full-machine snapshots.
 *
 * The payload is the SAME rollback snapshot the debug server's
 * save_state / load_state commands write (see debug_server.c): a 12-byte
 * header ("L3SN", version 2, payload length) followed by
 * RtlRollbackSaveToMemory()'s blob. The v2 tail carries the recompiled
 * CpuState, fiber position and interpreter residue, which is what makes a
 * loaded state resume execution instead of merely painting a picture.
 * Keeping the bytes identical means a quick save is readable by
 * scripts/verify_aot.py / ct_cheats.py and vice versa.
 *
 * The stash is a directory (default "saves/states", exe-relative because the
 * desktop host anchors the cwd there) holding at most SNES_QUICKSTATE_MAX
 * files named
 *
 *     ct_<YYYYmmddTHHMMSS>_<seq>.l3
 *
 * Lexicographic order is chronological order, so "newest" is the last name.
 * Pruning only ever deletes files matching ct_*.l3 — never anything else the
 * player keeps in the same directory.
 */

#define SNES_QUICKSTATE_MAX 20
#define SNES_QUICKSTATE_DEFAULT_DIR "saves/states"
#define SNES_QUICKSTATE_PATH_MAX 1024
/* Room for "State 20/20  2026-10-24 11:40:34  (newest)" plus slack. */
#define SNES_QUICKSTATE_OSD_MAX 128

/* Override the stash directory. A NULL or empty argument restores the
 * default. The string is copied. */
void SnesQuickStateSetDir(const char *dir);
const char *SnesQuickStateDir(void);

/* Save a snapshot into the stash under a fresh sortable name, then prune the
 * stash to SNES_QUICKSTATE_MAX files (oldest first). A successful save also
 * becomes the selected state. Logs one line to stderr and a host breadcrumb.
 * Returns true on success; out_path receives the file written and out_bytes
 * its total size (header included) when non-NULL. */
bool SnesQuickStateSave(char *out_path, size_t out_path_size, size_t *out_bytes);

/* ── selection over the stash ──────────────────────────────────────────────
 *
 * The stash is listed oldest-first (index 0) and the selection is tracked by
 * file name, not by index, so pruning or a concurrently added state cannot
 * silently point it at a different file. Until the player explicitly steps,
 * the selection is the newest state: R2/L2 keep their original semantics for
 * anyone who never touches L4/R4. */

/* Number of ct_*.l3 files currently in the stash (0 when the directory is
 * missing). The listing is re-scanned on every call. */
int SnesQuickStateCount(void);

/* Full path of the state at `index` (0 = oldest, count-1 = newest). False if
 * the index is out of range. */
bool SnesQuickStatePathAt(int index, char *out_path, size_t out_path_size);

/* Index of the selected state, or -1 when the stash is empty. When nothing
 * has been selected the result is the newest index (count-1). */
int SnesQuickStateSelectedIndex(void);

/* Select the state at `index`. False when out of range. */
bool SnesQuickStateSelectIndex(int index);

/* Move the selection `delta` entries through the sorted stash, wrapping at
 * both ends (+1 = newer, -1 = older). An empty stash leaves the selection
 * alone and writes "No saved states" to out_msg. On success out_msg receives
 * the player-facing line, e.g.
 *   "State 7/12  2026-10-24 11:40:34  (newest)".
 * Returns true when the selection moved. */
bool SnesQuickStateStep(int delta, char *out_msg, size_t out_msg_size);

/* The same player-facing line for an arbitrary index. */
bool SnesQuickStateDescribeIndex(int index, char *out_msg, size_t out_msg_size);

/* Load the state at `index` (0 = oldest). Same L3SN v2 payload and post-load
 * reconciliation as SnesQuickStateLoadFrom(). A successful load also selects
 * that index, so the next L2 press is the state the caller just chose. */
bool SnesQuickStateLoadIndex(int index, char *out_path, size_t out_path_size);

/* Load the selected state, or the newest one when no selection was made.
 * Logs the file it loaded. Returns true on success; out_path receives the
 * path. */
bool SnesQuickStateLoad(char *out_path, size_t out_path_size);

/* Explicit-path variants in the same v2 format, for --save-state /
 * --load-state and the debug server's quick_save / quick_load. The save
 * variant creates the parent directory if needed. */
bool SnesQuickStateSaveTo(const char *path, size_t *out_bytes);
bool SnesQuickStateLoadFrom(const char *path);

#ifdef __cplusplus
}
#endif

#endif /* SNESRECOMP_SNES_QUICKSTATE_H */
