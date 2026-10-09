/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef KD_UI_UNITS_H
#define KD_UI_UNITS_H

#include <gtk/gtk.h>

#include "app.h"

/* The Units tab: one card per disk (model, bus, size, health) with a row
 * per partition (filesystem, label, mountpoint, usage bar, mount/open).
 * Read-only apart from mounting, unmounting, ejecting and powering off. */

typedef struct kd_units kd_units;

kd_units *kd_units_new(kd_app *app);
void kd_units_free(kd_units *u);
GtkWidget *kd_units_widget(kd_units *u);

/* app->disks changed. rebuild: the layout or the UDisks2 data changed;
 * otherwise only the used/free numbers are refreshed. */
void kd_units_update(kd_units *u, bool rebuild);

#endif
