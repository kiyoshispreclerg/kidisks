/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef KD_UDISKS_H
#define KD_UDISKS_H

#include <stdbool.h>
#include <stdint.h>

/* UDisks2 over GDBus (GLib main loop): drive details and SMART, which need
 * no password to read, and the actions that change things (mount, unmount,
 * eject, power off), for which udisksd asks polkit, and polkit asks the
 * person. Kidisks itself never runs as root.
 *
 * Everything is asynchronous. Devices are named as in kd_disks ("sda",
 * "sda1", "dm-0"): the Block whose Device is /dev/<name>.
 *
 * With KIDISKS_ONLY_LOOP set in the environment, every action on anything
 * but a loop device is refused here, before reaching D-Bus (tests). */

typedef struct kd_udisks kd_udisks;

/* Called when the connection is ready (or failed: see kd_udisks_error), and
 * whenever something in UDisks2 changes (mounts, media, SMART...). Several
 * changes come in bursts: debounce. */
typedef void (*kd_udisks_changed_fn)(void *ud);

kd_udisks *kd_udisks_new(kd_udisks_changed_fn changed, void *ud);
/* Pending actions are cancelled; their done callbacks are not called. */
void kd_udisks_free(kd_udisks *u);

bool kd_udisks_ready(const kd_udisks *u);
/* Why UDisks2 is not available (translated); NULL while connecting or
 * when connected. */
const char *kd_udisks_error(const kd_udisks *u);

/* What UDisks2 knows about a drive. Fields not known keep the values noted. */
typedef struct {
    bool found;
    bool removable, ejectable, can_power_off;
    int rotation_rate;      /* -1 unknown, 0 solid state, else RPM */
    const char *bus;        /* "usb", "sdio", "" (static string) */
    bool smart_supported, smart_enabled;
    bool smart_failing;     /* the drive predicts its own failure */
    uint64_t smart_updated; /* Unix time of the last reading; 0 = never */
    bool has_temp;
    double temp_c;
    uint64_t power_on_s;    /* 0 unknown */
    int64_t bad_sectors;    /* -1 unknown */
    int attrs_failing;      /* attributes below their threshold now */
    int attrs_failed_past;
} kd_drive_info;

/* disk: "sda". false if UDisks2 has no drive for it (loop, not ready...). */
bool kd_udisks_drive_info(kd_udisks *u, const char *disk, kd_drive_info *out);

/* ok: it worked. error: translated message, or NULL when it failed only
 * because the person dismissed the password dialog. result: the mount
 * path for kd_udisks_mount, else NULL. Strings live during the call. */
typedef void (*kd_udisks_done_fn)(bool ok, const char *error, const char *result, void *ud);

void kd_udisks_mount(kd_udisks *u, const char *dev, kd_udisks_done_fn done, void *ud);
void kd_udisks_unmount(kd_udisks *u, const char *dev, kd_udisks_done_fn done, void *ud);
/* Optical drives and the like. */
void kd_udisks_eject(kd_udisks *u, const char *disk, kd_udisks_done_fn done, void *ud);
/* "Safely remove": unmounts every filesystem on the drive (also inside open
 * LUKS), locks the LUKS, then powers the drive off. */
void kd_udisks_power_off(kd_udisks *u, const char *disk, kd_udisks_done_fn done, void *ud);

/* The KIDISKS_ONLY_LOOP check, for the UI to grey things out early. */
bool kd_udisks_allowed(const char *dev);

#endif
