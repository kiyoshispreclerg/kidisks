/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef KD_UI_APP_H
#define KD_UI_APP_H

#include <gtk/gtk.h>
#include <stdbool.h>

#include "disks.h"
#include "prefs.h"
#include "udisks.h"
#include "watch.h"

/* What the tabs share. Owned by the window (window.c), which keeps the disk
 * model fresh and tells the tabs:
 *
 *   - udev, mount or swap events (kd_watch) → a new scan; if what is shown
 *     changed (kd_disks_layout_key), the tabs rebuild, else they update the
 *     numbers in place;
 *   - every few seconds while visible → the same, for the used/free numbers;
 *   - UDisks2 changes (SMART, drive capabilities) → rebuild.
 *
 * The tabs must not keep pointers into app->disks across these calls: it is
 * replaced on every scan. */
typedef struct kd_app {
    GtkWidget *window;
    GtkWidget *notebook;
    GtkWidget *status;
    kd_prefs *prefs;
    kd_disks *disks;
    kd_udisks *udisks;
    kd_watch *watch;
    bool show_hidden; /* loop, zram... (View menu) */
    struct kd_units *units;
} kd_app;

/* A message in the status bar (replaces the previous one). */
void kd_app_status(kd_app *app, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
/* An error dialog over the window. */
void kd_app_error(kd_app *app, const char *title, const char *detail);

/* An icon from the theme: the first of names (NULL-terminated) the theme
 * has, else the first one. */
GtkWidget *kd_icon_image(const char *const *names, GtkIconSize size);

#endif
