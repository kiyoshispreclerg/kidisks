/* SPDX-License-Identifier: GPL-3.0-or-later */
/* SMART in words (C locale: the msgids). */
#include <string.h>

#include "../harness.h"
#include "health.h"

static kd_drive_info healthy(void)
{
    kd_drive_info i;
    memset(&i, 0, sizeof i);
    i.found = i.smart_supported = i.smart_enabled = true;
    i.smart_updated = 1700000000;
    i.bad_sectors = 0;
    i.rotation_rate = 0;
    return i;
}

TEST(verdicts)
{
    char s[256];
    kd_drive_info i = healthy();
    CHECK_INT_EQ(kd_health_of(&i, s, sizeof s), KD_HEALTH_GOOD);
    CHECK_STR_EQ(s, "Good");

    i.attrs_failed_past = 1;
    CHECK_INT_EQ(kd_health_of(&i, s, sizeof s), KD_HEALTH_GOOD);
    CHECK_STR_EQ(s, "Good (1 attribute failed in the past)");

    i.bad_sectors = 3;
    CHECK_INT_EQ(kd_health_of(&i, s, sizeof s), KD_HEALTH_WARNING);
    CHECK_STR_EQ(s, "Warning: 3 bad sectors");

    /* Failing beats everything, even "not read yet". */
    i.smart_failing = true;
    i.smart_updated = 0;
    CHECK_INT_EQ(kd_health_of(&i, s, sizeof s), KD_HEALTH_FAILING);
    CHECK_STR_HAS(s, "FAILING");

    i = healthy();
    i.smart_updated = 0;
    CHECK_INT_EQ(kd_health_of(&i, s, sizeof s), KD_HEALTH_UNKNOWN);
    i = healthy();
    i.smart_supported = false;
    CHECK_INT_EQ(kd_health_of(&i, s, sizeof s), KD_HEALTH_UNKNOWN);
    CHECK_STR_EQ(s, "No health data (SMART)");
    i = healthy();
    i.found = false;
    CHECK_INT_EQ(kd_health_of(&i, s, sizeof s), KD_HEALTH_UNKNOWN);
}

TEST(durations)
{
    char s[128];
    kd_format_duration(s, sizeof s, 0);
    CHECK_STR_EQ(s, "less than an hour");
    kd_format_duration(s, sizeof s, 3600);
    CHECK_STR_EQ(s, "1 hour");
    kd_format_duration(s, sizeof s, 86400 + 7200);
    CHECK_STR_EQ(s, "1 day and 2 hours");
    kd_format_duration(s, sizeof s, 12 * 86400);
    CHECK_STR_EQ(s, "12 days");
    /* sda here: 156178800 s of SMART power-on time. */
    kd_format_duration(s, sizeof s, 156178800);
    CHECK_STR_EQ(s, "4 years and 11 months");
    /* Whole years leave the months out instead of "and 0 months". */
    kd_format_duration(s, sizeof s, 2 * 31557600ULL);
    CHECK_STR_EQ(s, "2 years");
}

int main(void)
{
    RUN(verdicts);
    RUN(durations);
    return harness_report();
}
