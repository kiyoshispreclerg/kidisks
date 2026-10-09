/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef KD_UI_WINDOW_H
#define KD_UI_WINDOW_H

#include <stdbool.h>

/* The main window: one notebook with the four tabs (Units, Partitions,
 * Space, Cleanup), a menu bar and a status bar. It remembers the last tab
 * and its size in kd_prefs. */

/* The tabs, in the order they appear. Values go to kidisks.ini: append
 * only. */
typedef enum {
    KD_TAB_UNITS,
    KD_TAB_PARTITIONS,
    KD_TAB_SPACE,
    KD_TAB_CLEANUP,
    KD_TAB_COUNT
} kd_tab;

typedef struct {
    /* -1 = the one remembered from last time. */
    int tab;
    /* Quits right after the window is first drawn (startup timing, tests). */
    bool exit_after_map;
    /* NULL = the default ~/.config/kidisks/kidisks.ini. */
    const char *prefs_path;
    /* g_get_monotonic_time() when main started, for the -v startup time. */
    long long start_us;
} kd_window_opts;

/* gtk_init must have been called. Runs the main loop; returns the process
 * exit code. */
int kd_window_run(const kd_window_opts *opts);

#endif
