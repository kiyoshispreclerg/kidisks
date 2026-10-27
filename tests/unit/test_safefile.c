/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "../harness.h"
#include "ini.h"
#include "util.h"

static char dir[] = "/tmp/kd-test-XXXXXX";
static char path[KD_PATH_MAX], bak[KD_PATH_MAX];

static void put(const char *file, const char *text, size_t len)
{
    FILE *f = fopen(file, "wb");
    fwrite(text, 1, len, f);
    fclose(f);
}

static void put_str(const char *file, const char *text)
{
    put(file, text, strlen(text));
}

static void reset(void)
{
    remove(path);
    remove(bak);
}

static mode_t mode_of(const char *file)
{
    struct stat st;
    return stat(file, &st) == 0 ? st.st_mode & 07777 : 0;
}

static bool is_text(const char *file, const char *want)
{
    char *t = kd_read_file(file, NULL);
    bool same = t && strcmp(t, want) == 0;
    free(t);
    return same;
}

TEST(first_save_makes_no_backup)
{
    reset();
    kd_ini *ini = kd_ini_new();
    CHECK(kd_ini_set(ini, "a", "k", "1") == 0);
    CHECK_INT_EQ(kd_ini_save(ini, path), 0);
    kd_ini_free(ini);
    CHECK(kd_file_exists(path));
    CHECK(!kd_file_exists(bak));
}

TEST(second_save_keeps_previous_with_same_mode)
{
    reset();
    put_str(path, "[a]\nk = old\n");
    CHECK(chmod(path, 0600) == 0);
    kd_ini *ini = kd_ini_parse("[a]\nk = old\n", NULL, NULL);
    CHECK(kd_ini_set(ini, "a", "k", "new") == 0);
    CHECK_INT_EQ(kd_ini_save(ini, path), 0);
    kd_ini_free(ini);
    CHECK(is_text(path, "[a]\nk = new\n"));
    CHECK(is_text(bak, "[a]\nk = old\n"));
    CHECK_INT_EQ((int)mode_of(bak), 0600);
}

TEST(empty_or_zeroed_main_does_not_replace_good_backup)
{
    reset();
    put_str(bak, "[a]\nk = good\n");
    kd_ini *ini = kd_ini_new();
    CHECK(kd_ini_set(ini, "a", "k", "new") == 0);

    put(path, "", 0);
    CHECK_INT_EQ(kd_ini_save(ini, path), 0);
    CHECK(is_text(bak, "[a]\nk = good\n"));
    CHECK(is_text(path, "[a]\nk = new\n"));

    put(path, "\0\0\0\0", 4);
    CHECK_INT_EQ(kd_ini_save(ini, path), 0);
    CHECK(is_text(bak, "[a]\nk = good\n"));
    kd_ini_free(ini);
}

TEST(typo_in_ini_does_not_become_backup)
{
    reset();
    put_str(bak, "[a]\nk = good\n");
    put_str(path, "[a]\nk = typo\nthis is not a pair\n");
    kd_ini *ini = kd_ini_new();
    CHECK(kd_ini_set(ini, "a", "k", "new") == 0);
    CHECK_INT_EQ(kd_ini_save(ini, path), 0);
    kd_ini_free(ini);
    CHECK(is_text(bak, "[a]\nk = good\n"));
}

TEST(backup_failure_does_not_stop_the_save)
{
    reset();
    put_str(path, "[a]\nk = old\n");
    /* A directory where the backup should go: rename onto it fails. */
    CHECK(mkdir(bak, 0700) == 0);
    kd_ini *ini = kd_ini_parse("[a]\nk = old\n", NULL, NULL);
    CHECK(kd_ini_set(ini, "a", "k", "new") == 0);
    CHECK_INT_EQ(kd_ini_save(ini, path), 0);
    kd_ini_free(ini);
    CHECK(is_text(path, "[a]\nk = new\n"));
    rmdir(bak);
}

TEST(read_prefers_a_sound_main)
{
    reset();
    put_str(path, "main\n");
    put_str(bak, "backup\n");
    size_t len = 0;
    char *t = kd_read_file_or_backup(path, kd_check_usable, &len);
    CHECK(t);
    CHECK_STR_EQ(t, "main\n");
    CHECK_INT_EQ((int)len, 5);
    free(t);
}

TEST(read_falls_back_when_main_missing_or_empty_or_zeroed)
{
    reset();
    put_str(bak, "backup\n");
    size_t len = 0;
    char *t = kd_read_file_or_backup(path, kd_check_usable, &len);
    CHECK(t);
    CHECK_STR_EQ(t, "backup\n");
    CHECK_INT_EQ((int)len, 7);
    free(t);

    put(path, "", 0);
    t = kd_read_file_or_backup(path, kd_check_usable, NULL);
    CHECK(t);
    CHECK_STR_EQ(t, "backup\n");
    free(t);

    put(path, "\0\0\0", 3);
    t = kd_read_file_or_backup(path, kd_check_usable, NULL);
    CHECK(t);
    CHECK_STR_EQ(t, "backup\n");
    free(t);
}

TEST(read_without_a_usable_backup_acts_as_before)
{
    reset();
    errno = 0;
    CHECK(kd_read_file_or_backup(path, kd_check_usable, NULL) == NULL);
    CHECK_INT_EQ(errno, ENOENT);

    put(path, "", 0);
    size_t len = 99;
    char *t = kd_read_file_or_backup(path, kd_check_usable, &len);
    CHECK(t);
    CHECK_INT_EQ((int)len, 0);
    free(t);

    put(bak, "", 0); /* an empty backup is no better */
    t = kd_read_file_or_backup(path, kd_check_usable, &len);
    CHECK(t);
    CHECK_STR_EQ(t, "");
    free(t);
}

TEST(line_file_without_final_newline_is_not_backed_up)
{
    reset();
    put_str(bak, "a\nb\n");
    put_str(path, "a\nb\nc");
    CHECK(kd_backup_file(path, kd_check_lines) < 0);
    CHECK(is_text(bak, "a\nb\n"));
    put_str(path, "a\nb\nc\n");
    CHECK_INT_EQ(kd_backup_file(path, kd_check_lines), 0);
    CHECK(is_text(bak, "a\nb\nc\n"));
}

TEST(checkers)
{
    CHECK(!kd_check_usable("", 0));
    CHECK(!kd_check_usable("a\0b", 3));
    CHECK(kd_check_usable("x", 1));
    CHECK(!kd_check_pem("-----BEGIN X-----\n", 18));
    CHECK(kd_check_pem("-----BEGIN X-----\n-----END X-----\n", 34));
    CHECK(kd_ini_check_clean("[a]\nk = 1\n", 10));
    CHECK(!kd_ini_check_clean("[a]\nnonsense\n", 13));
}

int main(void)
{
    if (!mkdtemp(dir))
        return 1;
    kd_pathf(path, sizeof path, "%s/file", dir);
    kd_pathf(bak, sizeof bak, "%s/file.backup", dir);
    RUN(first_save_makes_no_backup);
    RUN(second_save_keeps_previous_with_same_mode);
    RUN(empty_or_zeroed_main_does_not_replace_good_backup);
    RUN(typo_in_ini_does_not_become_backup);
    RUN(backup_failure_does_not_stop_the_save);
    RUN(read_prefers_a_sound_main);
    RUN(read_falls_back_when_main_missing_or_empty_or_zeroed);
    RUN(read_without_a_usable_backup_acts_as_before);
    RUN(line_file_without_final_newline_is_not_backed_up);
    RUN(checkers);
    int rc = harness_report();
    kd_remove_tree(dir);
    return rc;
}
