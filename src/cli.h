/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef KD_CLI_H
#define KD_CLI_H

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

#endif
