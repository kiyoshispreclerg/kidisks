/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef KD_UI_SPACE_H
#define KD_UI_SPACE_H

#include <gtk/gtk.h>

#include "app.h"

/* The Space tab (like Filelight): what takes up room under a folder, in
 * concentric rings. Buttons for the mounted filesystems, the home folder
 * or any folder; the scan runs in a thread (kd_tree_scan) and the rings are
 * laid out by kd_rings and drawn here with Cairo. Click a folder to go in,
 * the center to go back; right click to open, copy the path, move to the
 * trash or delete.
 *
 * The tab owns the tree and at most one job (a scan, a trash or a delete)
 * running in a thread. Only this thread (the GTK one) touches the tree; a
 * job hands back its result and the tab polls for it. */

typedef struct kd_space kd_space;

kd_space *kd_space_new(kd_app *app);
/* Cancels a scan in progress and waits for it. */
void kd_space_free(kd_space *s);
GtkWidget *kd_space_widget(kd_space *s);

/* app->disks changed what is mounted: refresh the buttons. */
void kd_space_update(kd_space *s);

/* Scans path and shows it (the Units tab's "See space"). */
void kd_space_open(kd_space *s, const char *path);

#endif
