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

/* Override the stash directory. A NULL or empty argument restores the
 * default. The string is copied. */
void SnesQuickStateSetDir(const char *dir);
const char *SnesQuickStateDir(void);

/* Save a snapshot into the stash under a fresh sortable name, then prune the
 * stash to SNES_QUICKSTATE_MAX files (oldest first). Logs one line to stderr
 * and a host breadcrumb. Returns true on success; out_path receives the file
 * written and out_bytes its total size (header included) when non-NULL. */
bool SnesQuickStateSave(char *out_path, size_t out_path_size, size_t *out_bytes);

/* Load the newest ct_*.l3 in the stash. Logs the file it loaded. Returns true
 * on success; out_path receives the path. */
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
