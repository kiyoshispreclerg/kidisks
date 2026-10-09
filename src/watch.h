/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef KD_WATCH_H
#define KD_WATCH_H

/* Tells when the disk model may have changed, in the GLib main loop:
 *
 *   - udev "block" events (a pendrive in or out, a partition created...);
 *   - /proc/self/mountinfo and /proc/swaps, which the kernel marks with
 *     POLLPRI on every mount, unmount, swapon and swapoff.
 *
 * Bursts (a disk brings its partitions along) are folded into one call,
 * 250 ms after the last event. Without libudev access the mount watch still
 * works, and the other way round. */

typedef struct kd_watch kd_watch;

kd_watch *kd_watch_new(void (*changed)(void *ud), void *ud);
void kd_watch_free(kd_watch *w);

#endif
