/*
 * Quick save / quick load — see snes_quickstate.h for the contract.
 *
 * The snapshot bytes are deliberately the debug server's L3SN v2 format
 * (cmd_save_state / cmd_load_state in debug_server.c), so a state written by
 * R2 is loadable by scripts/verify_aot.py and ct_cheats.py, and an existing
 * fixture is loadable by L2.
 */
#include "snes_quickstate.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "common_cpu_infra.h"
#include "common_rtl.h"
#include "host_report.h"
#include "snes/ppu.h"
#include "snes/snes.h"

#if defined(_WIN32)
#include <direct.h>
#include <windows.h>
#else
#include <dirent.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#endif

/* Same header as debug_server.c's save_state / load_state. */
#define QS_MAGIC 0x4c33534eu   /* "L3SN" */
#define QS_VERSION 2u
#define QS_HEADER_BYTES 12u

static char s_state_dir[SNES_QUICKSTATE_PATH_MAX] = SNES_QUICKSTATE_DEFAULT_DIR;

void SnesQuickStateSetDir(const char *dir) {
  if (!dir || !dir[0]) {
    snprintf(s_state_dir, sizeof(s_state_dir), "%s", SNES_QUICKSTATE_DEFAULT_DIR);
    return;
  }
  snprintf(s_state_dir, sizeof(s_state_dir), "%s", dir);
}

const char *SnesQuickStateDir(void) { return s_state_dir; }

/* ── filesystem helpers ──────────────────────────────────────────────────── */

static void qs_mkdir_one(const char *path) {
#if defined(_WIN32)
  _mkdir(path);
#else
  mkdir(path, 0755);
#endif
}

/* mkdir -p for the stash / a --save-state parent. Best effort: an existing
 * directory is success and a failure is reported by the fopen that follows. */
static void qs_mkdir_p(const char *path) {
  char tmp[SNES_QUICKSTATE_PATH_MAX];
  size_t n = strlen(path);
  if (n == 0 || n >= sizeof(tmp)) return;
  memcpy(tmp, path, n + 1);
  for (char *p = tmp + 1; *p; p++) {
    if (*p == '/' || *p == '\\') {
      char sep = *p;
      *p = 0;
      qs_mkdir_one(tmp);
      *p = sep;
    }
  }
  qs_mkdir_one(tmp);
}

/* Create the directory holding `file` (path up to the last separator). */
static void qs_mkdir_parent(const char *file) {
  char dir[SNES_QUICKSTATE_PATH_MAX];
  size_t n = strlen(file);
  if (n == 0 || n >= sizeof(dir)) return;
  memcpy(dir, file, n + 1);
  char *slash = strrchr(dir, '/');
#if defined(_WIN32)
  char *bslash = strrchr(dir, '\\');
  if (!slash || (bslash && bslash > slash)) slash = bslash;
#endif
  if (!slash) return;
  if (slash == dir) slash[1] = 0;   /* root */
  else *slash = 0;
  qs_mkdir_p(dir);
}

/* Only ever touch files this feature owns: ct_*.l3. */
static bool qs_is_stash_name(const char *name) {
  size_t n = strlen(name);
  if (n < 8) return false;
  if (strncmp(name, "ct_", 3) != 0) return false;
  return strcmp(name + n - 3, ".l3") == 0;
}

static char *qs_strdup(const char *s) {
  size_t n = strlen(s) + 1;
  char *p = (char *)malloc(n);
  if (p) memcpy(p, s, n);
  return p;
}

static void qs_free_names(char **names, int count) {
  if (!names) return;
  for (int i = 0; i < count; i++) free(names[i]);
  free(names);
}

/* Names (not paths) of the stash files in `dir`, unsorted. Returns the count
 * and stores a malloc'd array in *out_names (NULL when empty). */
static int qs_scan(const char *dir, char ***out_names) {
  char **names = NULL;
  int count = 0, cap = 0;

#if defined(_WIN32)
  char pattern[SNES_QUICKSTATE_PATH_MAX];
  snprintf(pattern, sizeof(pattern), "%s\\*", dir);
  WIN32_FIND_DATAA fd;
  HANDLE h = FindFirstFileA(pattern, &fd);
  if (h != INVALID_HANDLE_VALUE) {
    do {
      if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
      if (!qs_is_stash_name(fd.cFileName)) continue;
      if (count == cap) {
        int next = cap ? cap * 2 : 16;
        char **grown = (char **)realloc(names, (size_t)next * sizeof(char *));
        if (!grown) break;
        names = grown;
        cap = next;
      }
      char *dup = qs_strdup(fd.cFileName);
      if (!dup) break;
      names[count++] = dup;
    } while (FindNextFileA(h, &fd));
    FindClose(h);
  }
#else
  DIR *d = opendir(dir);
  if (d) {
    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
      if (!qs_is_stash_name(ent->d_name)) continue;
      if (count == cap) {
        int next = cap ? cap * 2 : 16;
        char **grown = (char **)realloc(names, (size_t)next * sizeof(char *));
        if (!grown) break;
        names = grown;
        cap = next;
      }
      char *dup = qs_strdup(ent->d_name);
      if (!dup) break;
      names[count++] = dup;
    }
    closedir(d);
  }
#endif

  *out_names = names;
  return count;
}

static int qs_name_cmp(const void *a, const void *b) {
  return strcmp(*(char *const *)a, *(char *const *)b);
}

/* Names of the stash files, sorted oldest-first (lexicographic order is
 * chronological order for ct_<stamp>_<seq>.l3). */
static int qs_list(const char *dir, char ***out_names) {
  int n = qs_scan(dir, out_names);
  if (n > 1) qsort(*out_names, (size_t)n, sizeof(char *), qs_name_cmp);
  return n;
}

/* ── selection ───────────────────────────────────────────────────────────── */

/* Tracked by file name: index arithmetic would silently retarget the
 * selection whenever a newer state is saved or the oldest one is pruned. */
static char s_selected_name[128];
static bool s_selected_set;

static int qs_find_name(char **names, int n, const char *name) {
  if (!name || !name[0]) return -1;
  for (int i = 0; i < n; i++)
    if (strcmp(names[i], name) == 0) return i;
  return -1;
}

/* Index the player means: the selected file when it still exists, otherwise
 * the newest state (the pre-selection L2/R2 behaviour). -1 when empty. */
static int qs_current_index(char **names, int n) {
  if (n <= 0) return -1;
  if (s_selected_set) {
    int i = qs_find_name(names, n, s_selected_name);
    if (i >= 0) return i;
  }
  return n - 1;
}

static void qs_set_selected(const char *name) {
  if (!name || !name[0]) {
    s_selected_set = false;
    s_selected_name[0] = 0;
    return;
  }
  snprintf(s_selected_name, sizeof(s_selected_name), "%s", name);
  s_selected_set = true;
}

/* "ct_20261024T114031_0001.l3" -> "2026-10-24 11:40:31". False when the name
 * does not carry the expected stamp (a hand-renamed file). */
static bool qs_parse_stamp(const char *name, char *out, size_t out_size) {
  int Y, mo, d, h, mi, s;
  if (sscanf(name, "ct_%4d%2d%2dT%2d%2d%2d", &Y, &mo, &d, &h, &mi, &s) != 6)
    return false;
  if (mo < 1 || mo > 12 || d < 1 || d > 31 || h > 23 || mi > 59 || s > 60)
    return false;
  snprintf(out, out_size, "%04d-%02d-%02d %02d:%02d:%02d", Y, mo, d, h, mi, s);
  return true;
}

/* Player-facing line for a sorted-list index: index/total, the timestamp
 * embedded in the file name, and a marker on the newest state. */
static bool qs_describe(char **names, int n, int index, char *out,
                        size_t out_size) {
  if (index < 0 || index >= n) return false;
  char when[40];
  if (!qs_parse_stamp(names[index], when, sizeof(when)))
    snprintf(when, sizeof(when), "(unknown time)");
  snprintf(out, out_size, "State %d/%d  %s%s", index + 1, n, when,
           index == n - 1 ? "  (newest)" : "");
  return true;
}

/* Sort ascending (oldest first) and delete the oldest until at most
 * SNES_QUICKSTATE_MAX remain. `keep` (a basename, may be NULL) is never
 * deleted, even if a backwards clock made it sort oldest; the selected state
 * is protected the same way. After the deletions the selection is clamped to
 * the nearest surviving entry if something outside this module removed it. */
static void qs_prune(const char *dir, const char *keep) {
  char **names = NULL;
  int n = qs_list(dir, &names);
  if (n <= SNES_QUICKSTATE_MAX) {
    qs_free_names(names, n);
    return;
  }
  int selected_before = qs_current_index(names, n);
  int to_remove = n - SNES_QUICKSTATE_MAX;
  for (int i = 0; i < n && to_remove > 0; i++) {
    if (keep && strcmp(names[i], keep) == 0) continue;
    if (s_selected_set && strcmp(names[i], s_selected_name) == 0) continue;
    char path[SNES_QUICKSTATE_PATH_MAX];
    snprintf(path, sizeof(path), "%s/%s", dir, names[i]);
    if (remove(path) == 0) {
      fprintf(stderr, "[quickstate] pruned %s\n", names[i]);
      to_remove--;
    } else {
      fprintf(stderr, "[quickstate] could not prune %s\n", path);
    }
  }
  qs_free_names(names, n);

  char **after = NULL;
  int na = qs_list(dir, &after);
  if (na == 0) {
    qs_set_selected(NULL);
  } else if (s_selected_set &&
             qs_find_name(after, na, s_selected_name) < 0) {
    int clamp = selected_before < 0 ? na - 1 : selected_before;
    if (clamp >= na) clamp = na - 1;
    if (clamp < 0) clamp = 0;
    qs_set_selected(after[clamp]);
    fprintf(stderr, "[quickstate] selection clamped to %s\n", after[clamp]);
  }
  qs_free_names(after, na);
}

/* ── snapshot file I/O (L3SN v2, byte-identical to debug_server.c) ───────── */

static bool qs_write_v2(const char *path, size_t *out_bytes) {
  size_t bound = RtlRollbackSnapshotBound();
  if (bound == 0) return false;
  unsigned char *buf = (unsigned char *)malloc(bound);
  if (!buf) {
    fprintf(stderr, "[quickstate] out of memory (snapshot bound %zu)\n", bound);
    return false;
  }
  size_t n = RtlRollbackSaveToMemory(buf, bound);
  if (n == 0) {
    free(buf);
    fprintf(stderr, "[quickstate] snapshot failed (no execution state yet?)\n");
    return false;
  }

  FILE *f = fopen(path, "wb");
  if (!f) {
    free(buf);
    fprintf(stderr, "[quickstate] fopen failed: %s\n", path);
    return false;
  }
  uint32_t magic = QS_MAGIC, version = QS_VERSION, payload = (uint32_t)n;
  bool ok = fwrite(&magic, 4, 1, f) == 1 &&
            fwrite(&version, 4, 1, f) == 1 &&
            fwrite(&payload, 4, 1, f) == 1 &&
            fwrite(buf, 1, n, f) == n;
  free(buf);
  if (fclose(f) != 0) ok = false;
  if (!ok) {
    fprintf(stderr, "[quickstate] write failed: %s\n", path);
    remove(path);
    return false;
  }
  if (out_bytes) *out_bytes = n + QS_HEADER_BYTES;
  return true;
}

static bool qs_read_v2(const char *path) {
  FILE *f = fopen(path, "rb");
  if (!f) {
    fprintf(stderr, "[quickstate] cannot open %s\n", path);
    return false;
  }
  uint32_t magic = 0, version = 0, payload = 0;
  if (fread(&magic, 4, 1, f) != 1 || magic != QS_MAGIC) {
    fclose(f);
    fprintf(stderr, "[quickstate] %s: bad magic (expected L3SN)\n", path);
    return false;
  }
  if (fread(&version, 4, 1, f) != 1 || version != QS_VERSION) {
    fclose(f);
    fprintf(stderr,
            "[quickstate] %s: unsupported version %u (quick load needs the "
            "v2 rollback tail; v1 is guest-only and cannot resume)\n",
            path, version);
    return false;
  }
  if (fread(&payload, 4, 1, f) != 1 || payload == 0 || payload > (1u << 24)) {
    fclose(f);
    fprintf(stderr, "[quickstate] %s: bad v2 payload length\n", path);
    return false;
  }
  unsigned char *buf = (unsigned char *)malloc(payload);
  if (!buf) {
    fclose(f);
    fprintf(stderr, "[quickstate] out of memory (payload %u)\n", payload);
    return false;
  }
  size_t got = fread(buf, 1, payload, f);
  fclose(f);
  if (got != payload) {
    free(buf);
    fprintf(stderr, "[quickstate] %s: short read (%zu/%u)\n", path, got, payload);
    return false;
  }
  bool ok = RtlRollbackLoadFromMemory(buf, payload);
  free(buf);
  if (!ok) {
    fprintf(stderr, "[quickstate] %s: rollback load rejected\n", path);
    return false;
  }
  /* Same post-load reconciliation as the debug server's load_state. */
  PpuResetWidescreenOamHistory(g_snes->ppu);
  return true;
}

/* ── public API ─────────────────────────────────────────────────────────── */

static void qs_timestamp(char *buf, size_t size) {
  time_t t = time(NULL);
  struct tm tmv;
#if defined(_WIN32)
  localtime_s(&tmv, &t);
#else
  localtime_r(&t, &tmv);
#endif
  if (strftime(buf, size, "%Y%m%dT%H%M%S", &tmv) == 0) snprintf(buf, size, "00000000T000000");
}

/* The stash only gets a new file once per second at most in practice, but a
 * held key or a script can ask for several in the same second; pick the next
 * unused sequence so names stay unique and sortable. */
static unsigned qs_next_seq(const char *dir, const char *prefix) {
  char **names = NULL;
  int n = qs_scan(dir, &names);
  unsigned max_seq = 0;
  size_t plen = strlen(prefix);
  for (int i = 0; i < n; i++) {
    if (strncmp(names[i], prefix, plen) != 0) continue;
    unsigned v = 0;
    if (sscanf(names[i] + plen, "%u", &v) == 1 && v > max_seq) max_seq = v;
  }
  qs_free_names(names, n);
  return max_seq + 1;
}

bool SnesQuickStateSave(char *out_path, size_t out_path_size, size_t *out_bytes) {
  const char *dir = s_state_dir;
  qs_mkdir_p(dir);

  char stamp[32];
  qs_timestamp(stamp, sizeof(stamp));
  char prefix[64];
  snprintf(prefix, sizeof(prefix), "ct_%s_", stamp);
  unsigned seq = qs_next_seq(dir, prefix);

  char path[SNES_QUICKSTATE_PATH_MAX];
  char name[128];
  snprintf(name, sizeof(name), "%s%04u.l3", prefix, seq);
  snprintf(path, sizeof(path), "%s/%s", dir, name);

  size_t bytes = 0;
  if (!qs_write_v2(path, &bytes)) return false;

  fprintf(stderr, "[quickstate] saved %s (%zu bytes)\n", name, bytes);
  host_report_breadcrumb("quick save: %s (%zu bytes)", path, bytes);

  qs_prune(dir, name);
  /* R2 selects what it just wrote, so L2 loads it without any further
   * stepping (the newest state is the selected one). */
  qs_set_selected(name);
  fprintf(stderr, "[quickstate] selected newest %s\n", name);

  if (out_path && out_path_size) snprintf(out_path, out_path_size, "%s", path);
  if (out_bytes) *out_bytes = bytes;
  return true;
}

int SnesQuickStateCount(void) {
  char **names = NULL;
  int n = qs_list(s_state_dir, &names);
  qs_free_names(names, n);
  return n;
}

bool SnesQuickStatePathAt(int index, char *out_path, size_t out_path_size) {
  char **names = NULL;
  int n = qs_list(s_state_dir, &names);
  bool ok = index >= 0 && index < n;
  if (ok && out_path && out_path_size)
    snprintf(out_path, out_path_size, "%s/%s", s_state_dir, names[index]);
  qs_free_names(names, n);
  return ok;
}

int SnesQuickStateSelectedIndex(void) {
  char **names = NULL;
  int n = qs_list(s_state_dir, &names);
  int idx = qs_current_index(names, n);
  qs_free_names(names, n);
  return idx;
}

bool SnesQuickStateSelectIndex(int index) {
  char **names = NULL;
  int n = qs_list(s_state_dir, &names);
  bool ok = index >= 0 && index < n;
  if (ok) qs_set_selected(names[index]);
  qs_free_names(names, n);
  return ok;
}

bool SnesQuickStateDescribeIndex(int index, char *out_msg,
                                 size_t out_msg_size) {
  char **names = NULL;
  int n = qs_list(s_state_dir, &names);
  bool ok = qs_describe(names, n, index, out_msg, out_msg_size);
  qs_free_names(names, n);
  return ok;
}

bool SnesQuickStateStep(int delta, char *out_msg, size_t out_msg_size) {
  char **names = NULL;
  int n = qs_list(s_state_dir, &names);
  if (n == 0) {
    qs_free_names(names, n);
    if (out_msg && out_msg_size) snprintf(out_msg, out_msg_size, "No saved states");
    fprintf(stderr, "[quickstate] no states in %s\n", s_state_dir);
    return false;
  }
  int cur = qs_current_index(names, n);
  int next = cur + delta;
  next %= n;
  if (next < 0) next += n;
  qs_set_selected(names[next]);
  if (out_msg && out_msg_size) {
    qs_describe(names, n, next, out_msg, out_msg_size);
    fprintf(stderr, "[quickstate] %s\n", out_msg);
  }
  qs_free_names(names, n);
  return true;
}

bool SnesQuickStateLoadIndex(int index, char *out_path, size_t out_path_size) {
  char **names = NULL;
  int n = qs_list(s_state_dir, &names);
  if (index < 0 || index >= n) {
    qs_free_names(names, n);
    fprintf(stderr, "[quickstate] load index %d out of range (%d states)\n",
            index, n);
    return false;
  }
  char name[128];
  snprintf(name, sizeof(name), "%s", names[index]);
  char path[SNES_QUICKSTATE_PATH_MAX];
  snprintf(path, sizeof(path), "%s/%s", s_state_dir, name);
  qs_free_names(names, n);

  fprintf(stderr, "[quickstate] loading index %d/%d %s\n", index + 1, n, name);
  if (!qs_read_v2(path)) return false;
  /* Loading an explicit index also selects it: the next L2 press is the
   * state the player just chose, not whatever was selected before. */
  qs_set_selected(name);
  fprintf(stderr, "[quickstate] loaded %s\n", path);
  host_report_breadcrumb("quick load index %d: %s", index, path);
  if (out_path && out_path_size) snprintf(out_path, out_path_size, "%s", path);
  return true;
}

bool SnesQuickStateLoad(char *out_path, size_t out_path_size) {
  const char *dir = s_state_dir;
  char **names = NULL;
  int n = qs_list(dir, &names);
  if (n == 0) {
    qs_free_names(names, n);
    fprintf(stderr, "[quickstate] no ct_*.l3 in %s\n", dir);
    return false;
  }
  int idx = qs_current_index(names, n);
  bool explicit_selection = s_selected_set &&
                            qs_find_name(names, n, s_selected_name) >= 0;
  char name[128];
  snprintf(name, sizeof(name), "%s", names[idx]);
  char path[SNES_QUICKSTATE_PATH_MAX];
  snprintf(path, sizeof(path), "%s/%s", dir, name);
  fprintf(stderr, "[quickstate] loading %s %d/%d %s\n",
          explicit_selection ? "selected" : "newest", idx + 1, n, name);
  qs_free_names(names, n);

  if (!qs_read_v2(path)) return false;
  fprintf(stderr, "[quickstate] loaded %s\n", path);
  host_report_breadcrumb("quick load: %s", path);
  if (out_path && out_path_size) snprintf(out_path, out_path_size, "%s", path);
  return true;
}

bool SnesQuickStateSaveTo(const char *path, size_t *out_bytes) {
  if (!path || !path[0]) return false;
  qs_mkdir_parent(path);
  size_t bytes = 0;
  if (!qs_write_v2(path, &bytes)) return false;
  fprintf(stderr, "[quickstate] saved %s (%zu bytes)\n", path, bytes);
  host_report_breadcrumb("save state: %s (%zu bytes)", path, bytes);
  if (out_bytes) *out_bytes = bytes;
  return true;
}

bool SnesQuickStateLoadFrom(const char *path) {
  if (!path || !path[0]) return false;
  if (!qs_read_v2(path)) return false;
  fprintf(stderr, "[quickstate] loaded %s\n", path);
  host_report_breadcrumb("load state: %s", path);
  return true;
}
