/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef KD_UTIL_H
#define KD_UTIL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdint.h>

#define KD_PATH_MAX 4096

/* Reads a whole file into a NUL-terminated buffer; NULL on error (errno set).
 * The caller frees it. */
char *kd_read_file(const char *path, size_t *len);

/* Decides whether a file's contents are whole. text is NUL-terminated at len.
 * The same function serves the backup (only a sound file becomes the
 * .backup) and the reading (a damaged file gives way to the .backup). */
typedef bool (*kd_check_fn)(const char *text, size_t len);

/* What a power cut leaves behind is an empty or zeroed file: not empty, no
 * NUL bytes. Enough to read a lenient format. */
bool kd_check_usable(const char *text, size_t len);
/* Usable and ending in '\n' (a line file cut in the middle is not whole). */
bool kd_check_lines(const char *text, size_t len);
/* Usable and with a "-----BEGIN " and an "-----END " line. */
bool kd_check_pem(const char *text, size_t len);

/* fflush + fsync: call before fclose, or the new file may come back empty
 * after a power cut even when the rename went through. 0, or -1 with errno. */
int kd_flush_sync(FILE *f);

/* Copies path to <path>.backup (via .backup.tmp, fsync, rename), with the
 * same permissions as path. Missing path: nothing to do, 0. If ok is set and
 * rejects the file, the previous backup is kept (warning, -1). Other
 * failures also warn and give -1: callers write the new file anyway. */
int kd_backup_file(const char *path, kd_check_fn ok);

/* kd_read_file, but if path is missing or fails ok, tries <path>.backup
 * (with a warning saying so). If the backup is no good either, the result
 * (and errno) is exactly that of kd_read_file(path). Not for files the user
 * picked to import. */
char *kd_read_file_or_backup(const char *path, kd_check_fn ok, size_t *len);

bool kd_file_exists(const char *path);
bool kd_dir_exists(const char *path);

/* mkdir -p for the directory part of path (errors are left to whoever
 * then creates the file). */
void kd_make_parent_dirs(const char *path);

/* rm -r that stays put: never follows a symbolic link (a link is removed,
 * not what it points to) and never goes into another filesystem mounted
 * inside (EXDEV). Goes on past errors and removes all it can. 0, or -1 with
 * errno set to the first error. */
int kd_remove_tree(const char *path);

/* snprintf for paths: false (and an empty string) if it would not fit. */
bool kd_pathf(char *out, size_t size, const char *fmt, ...)
    __attribute__((format(printf, 3, 4)));

/* $XDG_CONFIG_HOME/kidisks/<name> (or ~/.config/...); false without $HOME
 * or if it does not fit. */
bool kd_config_path(char *out, size_t size, const char *name);

/* Sizes as people read them. Binary units (KiB, MiB, GiB, TiB), rounded as
 * lsblk does: one decimal, left out when it is zero ("46.8 GiB", "290 MiB",
 * "512 B"). The decimal separator follows LC_NUMERIC. */
void kd_format_size(char *out, size_t size, uint64_t bytes);
/* Decimal units (kB, MB, GB, TB), as drive makers label disks: one decimal
 * below 100, none from there up ("120 GB", "1.5 kB"). */
void kd_format_size_si(char *out, size_t size, uint64_t bytes);
/* Whole disks: "111.8 GiB (120 GB)". */
void kd_format_disk_size(char *out, size_t size, uint64_t bytes);

#endif
