/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef KD_INI_H
#define KD_INI_H

#include <stdbool.h>
#include <stddef.h>

/* A small INI document that can be read and edited in place:
 *
 *   # comment            ; comment too
 *   [section]
 *   key = value
 *
 * Every line is kept as written, so a file edited by kidisks keeps
 * the user's comments, order and blank lines; only the touched lines change.
 * Section and key names are case-insensitive. Values are trimmed; there is
 * no quoting or escaping (a value may contain "=", "#" and ";"). */

typedef struct kd_ini kd_ini;

/* Empty document. */
kd_ini *kd_ini_new(void);
/* Parses text. Malformed lines are kept (and skipped by the getters); each
 * one is reported to warn with its 1-based line number, if warn is set. */
kd_ini *kd_ini_parse(const char *text,
                     void (*warn)(void *ud, int line, const char *msg),
                     void *ud);
void kd_ini_free(kd_ini *ini);

/* NULL if absent. With repeated keys, the last one wins. */
const char *kd_ini_get(const kd_ini *ini, const char *section, const char *key);
/* Line number of that key (0 if absent), for messages. */
int kd_ini_line(const kd_ini *ini, const char *section, const char *key);

/* Iteration in file order. Returns false at the end. *section is "" for keys
 * before the first header. Pass i = 0 first, then what it leaves in *i. */
bool kd_ini_next(const kd_ini *ini, int *i, const char **section,
                 const char **key, const char **value, int *line);

/* Sections in file order, each once; same protocol as kd_ini_next. */
bool kd_ini_next_section(const kd_ini *ini, int *i, const char **section);

/* Replaces the value in place, or appends the key to its section (created at
 * the end if missing). */
int kd_ini_set(kd_ini *ini, const char *section, const char *key,
               const char *value);
/* Removes every line with that key. */
void kd_ini_unset(kd_ini *ini, const char *section, const char *key);
/* Removes the section header and its keys (comments inside go too). */
void kd_ini_remove_section(kd_ini *ini, const char *section);

/* The whole document as text; the caller frees it. */
char *kd_ini_dump(const kd_ini *ini);
/* For kd_backup_file: usable and read without a single warning. Stricter
 * than what loading needs, so a typo does not replace the good backup. */
bool kd_ini_check_clean(const char *text, size_t len);

/* Writes atomically (temp file + fsync + rename), creating the parent
 * folders. The file being replaced is first copied to <path>.backup, if it
 * is sound; a failed backup is only a warning. */

int kd_ini_save(const kd_ini *ini, const char *path);

#endif
