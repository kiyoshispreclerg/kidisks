/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef KD_PREFS_H
#define KD_PREFS_H

#include <stdbool.h>

/* What the window remembers between runs (last tab, size...), kept in
 * ~/.config/kidisks/kidisks.ini. It is an ordinary INI the person may edit:
 * comments and unknown keys survive a save.
 *
 * kd_prefs owns its kd_ini and the path. Missing or unreadable files give
 * empty prefs (the getters then return the defaults); a broken line is
 * skipped with a warning. */

typedef struct kd_prefs kd_prefs;

/* path NULL = the default place (kd_config_path). Never returns NULL unless
 * out of memory. */
kd_prefs *kd_prefs_load(const char *path);
void kd_prefs_free(kd_prefs *p);

/* The default when absent or not a number, and clamped to [min, max]. */
int kd_prefs_get_int(const kd_prefs *p, const char *section, const char *key,
                     int def, int min, int max);
void kd_prefs_set_int(kd_prefs *p, const char *section, const char *key, int v);

/* Writes only if something changed. 0 or -1 (errno set). */
int kd_prefs_save(kd_prefs *p);

#endif
