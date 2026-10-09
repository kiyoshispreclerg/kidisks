/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "health.h"

#include <libintl.h>
#include <stdio.h>

#include "i18n.h"

kd_health kd_health_of(const kd_drive_info *info, char *out, size_t size)
{
    if (!info->found || !info->smart_supported) {
        snprintf(out, size, "%s", _("No health data (SMART)"));
        return KD_HEALTH_UNKNOWN;
    }
    if (!info->smart_enabled) {
        snprintf(out, size, "%s", _("SMART is turned off on this drive"));
        return KD_HEALTH_UNKNOWN;
    }
    if (info->smart_failing) {
        snprintf(out, size, "%s",
                 _("FAILING: the drive says it is about to fail. Back up its data now."));
        return KD_HEALTH_FAILING;
    }
    if (!info->smart_updated) {
        snprintf(out, size, "%s", _("Health not read yet"));
        return KD_HEALTH_UNKNOWN;
    }
    if (info->bad_sectors > 0) {
        snprintf(out, size,
                 ngettext("Warning: %lld bad sector", "Warning: %lld bad sectors",
                          (unsigned long)info->bad_sectors),
                 (long long)info->bad_sectors);
        return KD_HEALTH_WARNING;
    }
    if (info->attrs_failing > 0) {
        snprintf(out, size,
                 ngettext("Warning: %d attribute is failing", "Warning: %d attributes are failing",
                          (unsigned long)info->attrs_failing),
                 info->attrs_failing);
        return KD_HEALTH_WARNING;
    }
    if (info->attrs_failed_past > 0) {
        snprintf(out, size,
                 ngettext("Good (%d attribute failed in the past)",
                          "Good (%d attributes failed in the past)",
                          (unsigned long)info->attrs_failed_past),
                 info->attrs_failed_past);
        return KD_HEALTH_GOOD;
    }
    snprintf(out, size, "%s", _("Good"));
    return KD_HEALTH_GOOD;
}

/* One unit with its number: "4 years". */
static void unit(char *out, size_t size, unsigned long n, int which)
{
    switch (which) {
    case 0: snprintf(out, size, ngettext("%lu year", "%lu years", n), n); break;
    case 1: snprintf(out, size, ngettext("%lu month", "%lu months", n), n); break;
    case 2: snprintf(out, size, ngettext("%lu day", "%lu days", n), n); break;
    default: snprintf(out, size, ngettext("%lu hour", "%lu hours", n), n); break;
    }
}

void kd_format_duration(char *out, size_t size, uint64_t s)
{
    /* Average lengths: good enough for "how long has it been on". */
    static const uint64_t len[4] = {31557600, 2629800, 86400, 3600};
    unsigned long n[4];
    uint64_t rest = s;
    for (int i = 0; i < 4; i++) {
        n[i] = (unsigned long)(rest / len[i]);
        rest %= len[i];
    }
    int first = 0;
    while (first < 4 && !n[first])
        first++;
    if (first == 4) {
        snprintf(out, size, "%s", _("less than an hour"));
        return;
    }
    char a[64], b[64];
    unit(a, sizeof a, n[first], first);
    if (first == 3 || !n[first + 1]) {
        snprintf(out, size, "%s", a);
        return;
    }
    unit(b, sizeof b, n[first + 1], first + 1);
    /* TRANSLATORS: two amounts of time: "4 years and 11 months" */
    snprintf(out, size, _("%s and %s"), a, b);
}
