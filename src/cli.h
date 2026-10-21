/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef KD_CLI_H
#define KD_CLI_H

#include <stdbool.h>

/* The text modes that talk to UDisks2, without a window:
 *
 *   kidisks --info DISK        drive details and health
 *   kidisks --mount DEV        prints where it was mounted
 *   kidisks --unmount DEV
 *   kidisks --eject DISK
 *   kidisks --power-off DISK   "safely remove"
 *
 * Runs a GLib main loop until UDisks2 answers. Returns the exit code:
 * 0 done, 1 failed, 3 UDisks2 not available. */
int kd_cli_run(const char *action, const char *dev);

/* kidisks --scan DIR [--top N] [-b]: the total under DIR and its N biggest
 * entries. With bytes, lines are "BYTES<TAB>PATH" like `du -B1`, the total
 * first. Returns 0, or 1 if DIR could not be scanned. Unreadable folders
 * get a warning on stderr (the total is short), as with du. */
int kd_cli_scan(const char *path, int top, bool bytes);

#endif
