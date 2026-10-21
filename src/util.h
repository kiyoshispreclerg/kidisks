/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef KD_UTIL_H
#define KD_UTIL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define KD_PATH_MAX 4096

/* Reads a whole file into a NUL-terminated buffer; NULL on error (errno set).
 * The caller frees it. */
char *kd_read_file(const char *path, size_t *len);

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
