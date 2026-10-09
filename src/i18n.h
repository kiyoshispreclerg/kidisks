/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef KD_I18N_H
#define KD_I18N_H

/* gettext. The msgids are the English texts; the translations live in po/.
 * Everything a person reads goes through _(): the window, the text modes
 * (--list...) and the log messages. */

#include <libintl.h>

#define KD_GETTEXT_DOMAIN "kidisks"

#define _(s) gettext(s)
/* Marks a string to be extracted now and translated later, where it is used. */
#define N_(s) (s)

#endif
