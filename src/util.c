/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "util.h"

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>

char *kd_read_file(const char *path, size_t *len)
{
    FILE *f = fopen(path, "rb");
    if (!f)
        return NULL;

    char *buf = NULL;
    size_t size = 0, cap = 0;
    for (;;) {
        if (cap - size < 65536) {
            cap = cap ? cap * 2 : 1 << 20;
            char *nb = realloc(buf, cap + 1);
            if (!nb) {
                free(buf);
                fclose(f);
                errno = ENOMEM;
                return NULL;
            }
            buf = nb;
        }
        size_t n = fread(buf + size, 1, cap - size, f);
        size += n;
        if (n == 0)
            break;
    }
    if (ferror(f)) {
        free(buf);
        fclose(f);
        errno = EIO;
        return NULL;
    }
    fclose(f);
    buf[size] = '\0';
    if (len)
        *len = size;
    return buf;
}

bool kd_file_exists(const char *path)
{
    struct stat st;
    return stat(path, &st) == 0 && S_ISREG(st.st_mode);
}

bool kd_dir_exists(const char *path)
{
    struct stat st;
    return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

bool kd_pathf(char *out, size_t size, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(out, size, fmt, ap);
    va_end(ap);
    if (n < 0 || (size_t)n >= size) {
        if (size)
            out[0] = '\0';
        return false;
    }
    return true;
}

void kd_make_parent_dirs(const char *path)
{
    char buf[KD_PATH_MAX];
    if (!kd_pathf(buf, sizeof buf, "%s", path))
        return;
    for (char *p = buf + 1; *p; p++)
        if (*p == '/') {
            *p = '\0';
            mkdir(buf, 0755);
            *p = '/';
        }
}

bool kd_config_path(char *out, size_t size, const char *name)
{
    const char *xdg = getenv("XDG_CONFIG_HOME");
    if (xdg && xdg[0] == '/')
        return kd_pathf(out, size, "%s/kidisks/%s", xdg, name);
    const char *home = getenv("HOME");
    if (!home || !*home) {
        if (size)
            out[0] = '\0';
        return false;
    }
    return kd_pathf(out, size, "%s/.config/kidisks/%s", home, name);
}

/* whole_from: from this value up, no decimals. */
static void format_units(char *out, size_t size, uint64_t bytes, unsigned base,
                         const char *const units[], double whole_from)
{
    if (bytes < base) {
        snprintf(out, size, "%llu %s", (unsigned long long)bytes, units[0]);
        return;
    }
    double v = (double)bytes;
    int u = 0;
    while (v >= base && units[u + 1]) {
        v /= base;
        u++;
    }
    /* Decide on the rounded value: 46.98 prints "47", not "47.0". */
    double tenths = (double)(long long)(v * 10 + 0.5);
    if (v >= whole_from || (long long)tenths % 10 == 0)
        snprintf(out, size, "%.0f %s", v, units[u]);
    else
        snprintf(out, size, "%.1f %s", v, units[u]);
}

void kd_format_size(char *out, size_t size, uint64_t bytes)
{
    static const char *const units[] = {"B", "KiB", "MiB", "GiB", "TiB", "PiB", NULL};
    format_units(out, size, bytes, 1024, units, 1e300);
}

void kd_format_size_si(char *out, size_t size, uint64_t bytes)
{
    static const char *const units[] = {"B", "kB", "MB", "GB", "TB", "PB", NULL};
    format_units(out, size, bytes, 1000, units, 99.95);
}

void kd_format_disk_size(char *out, size_t size, uint64_t bytes)
{
    char bin[32], si[32];
    kd_format_size(bin, sizeof bin, bytes);
    kd_format_size_si(si, sizeof si, bytes);
    snprintf(out, size, "%s (%s)", bin, si);
}
