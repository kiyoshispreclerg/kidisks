/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <stdlib.h>
#include <string.h>

#include "../harness.h"
#include "util.h"

TEST(pathf_fails_instead_of_truncating)
{
    char buf[8];
    CHECK(kd_pathf(buf, sizeof buf, "%s", "1234567"));
    CHECK_STR_EQ(buf, "1234567");
    CHECK(!kd_pathf(buf, sizeof buf, "%s", "12345678"));
    CHECK_STR_EQ(buf, "");
}

TEST(sizes_in_binary_units)
{
    char s[32];
    kd_format_size(s, sizeof s, 0);
    CHECK_STR_EQ(s, "0 B");
    kd_format_size(s, sizeof s, 1023);
    CHECK_STR_EQ(s, "1023 B");
    kd_format_size(s, sizeof s, 1024);
    CHECK_STR_EQ(s, "1 KiB");
    kd_format_size(s, sizeof s, 1536);
    CHECK_STR_EQ(s, "1.5 KiB");
    kd_format_size(s, sizeof s, 290ULL << 20);
    CHECK_STR_EQ(s, "290 MiB");
    /* sda1 here: 98086912 sectors. */
    kd_format_size(s, sizeof s, 98086912ULL * 512);
    CHECK_STR_EQ(s, "46.8 GiB");
    kd_format_size(s, sizeof s, 234441648ULL * 512);
    CHECK_STR_EQ(s, "111.8 GiB");
    /* 99.97 GiB rounds to 100: no "100.0". */
    kd_format_size(s, sizeof s, (uint64_t)(99.97 * (1 << 30)));
    CHECK_STR_EQ(s, "100 GiB");
    kd_format_size(s, sizeof s, 2ULL << 40);
    CHECK_STR_EQ(s, "2 TiB");
    /* Past the last unit it keeps counting in PiB. */
    kd_format_size(s, sizeof s, UINT64_MAX);
    CHECK_STR_EQ(s, "16384 PiB");
}

TEST(disk_size_shows_both)
{
    char s[64];
    /* A "120 GB" SSD: 234441648 sectors. */
    kd_format_disk_size(s, sizeof s, 234441648ULL * 512);
    CHECK_STR_EQ(s, "111.8 GiB (120 GB)");
    kd_format_size_si(s, sizeof s, 500107862016ULL);
    CHECK_STR_EQ(s, "500 GB");
    kd_format_size_si(s, sizeof s, 999);
    CHECK_STR_EQ(s, "999 B");
    kd_format_size_si(s, sizeof s, 1500);
    CHECK_STR_EQ(s, "1.5 kB");
}

TEST(config_path_follows_xdg)
{
    char p[KD_PATH_MAX];
    const char *old_xdg = getenv("XDG_CONFIG_HOME");
    char *saved = old_xdg ? strdup(old_xdg) : NULL;

    setenv("XDG_CONFIG_HOME", "/x/cfg", 1);
    CHECK(kd_config_path(p, sizeof p, "kidisks.ini"));
    CHECK_STR_EQ(p, "/x/cfg/kidisks/kidisks.ini");
    /* Relative XDG paths are invalid per the spec: fall back to $HOME. */
    setenv("XDG_CONFIG_HOME", "rel", 1);
    setenv("HOME", "/h", 1);
    CHECK(kd_config_path(p, sizeof p, "kidisks.ini"));
    CHECK_STR_EQ(p, "/h/.config/kidisks/kidisks.ini");

    if (saved)
        setenv("XDG_CONFIG_HOME", saved, 1);
    else
        unsetenv("XDG_CONFIG_HOME");
    free(saved);
}

int main(void)
{
    RUN(pathf_fails_instead_of_truncating);
    RUN(sizes_in_binary_units);
    RUN(disk_size_shows_both);
    RUN(config_path_follows_xdg);
    return harness_report();
}
