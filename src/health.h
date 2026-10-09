/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef KD_HEALTH_H
#define KD_HEALTH_H

#include <stddef.h>
#include <stdint.h>

#include "udisks.h"

/* SMART in words, for the Units tab and `kidisks --info`. Translated. */

typedef enum {
    KD_HEALTH_UNKNOWN, /* no SMART, or not read yet */
    KD_HEALTH_GOOD,
    KD_HEALTH_WARNING, /* bad sectors, attributes failing now */
    KD_HEALTH_FAILING, /* the drive predicts its own failure */
} kd_health;

/* The verdict and one line about it ("Good", "Good, 3 bad sectors"...). */
kd_health kd_health_of(const kd_drive_info *info, char *out, size_t size);

/* "4 years and 11 months", "12 days", "7 hours"; the two largest units. */
void kd_format_duration(char *out, size_t size, uint64_t seconds);

#endif
