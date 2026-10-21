/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "util.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

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

/* Removes name (in the folder open at dfd) and, if it is a folder,
 * everything in it. Returns the first errno met, 0 if none. */
static int remove_at(int dfd, const char *name, dev_t dev)
{
    struct stat st;
    if (fstatat(dfd, name, &st, AT_SYMLINK_NOFOLLOW) < 0)
        return errno == ENOENT ? 0 : errno;
    if (!S_ISDIR(st.st_mode))
        return unlinkat(dfd, name, 0) < 0 ? errno : 0;
    /* A filesystem mounted inside is not ours to empty. */
    if (st.st_dev != dev)
        return EXDEV;
    int fd = openat(dfd, name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0)
        return errno;
    DIR *d = fdopendir(fd);
    if (!d) {
        int e = errno;
        close(fd);
        return e;
    }
    int err = 0;
    struct dirent *e;
    while ((e = readdir(d))) {
        const char *nm = e->d_name;
        if (nm[0] == '.' && (!nm[1] || (nm[1] == '.' && !nm[2])))
            continue;
        int r = remove_at(dirfd(d), nm, dev);
        if (r && !err)
            err = r;
    }
    closedir(d);
    if (unlinkat(dfd, name, AT_REMOVEDIR) < 0 && !err)
        err = errno;
    return err;
}

int kd_remove_tree(const char *path)
{
    char buf[KD_PATH_MAX];
    if (!kd_pathf(buf, sizeof buf, "%s", path)) {
        errno = ENAMETOOLONG;
        return -1;
    }
    size_t len = strlen(buf);
    while (len > 1 && buf[len - 1] == '/')
        buf[--len] = '\0';
    char *slash = strrchr(buf, '/');
    const char *name = slash ? slash + 1 : buf;
    if (!*name || strcmp(name, ".") == 0 || strcmp(name, "..") == 0) {
        errno = EINVAL;
        return -1;
    }
    const char *parent = ".";
    if (slash == buf) {
        parent = "/";
    } else if (slash) {
        *slash = '\0';
        parent = buf;
    }
    int dfd = open(parent, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    struct stat st;
    if (dfd < 0)
        return -1;
    if (fstatat(dfd, name, &st, AT_SYMLINK_NOFOLLOW) < 0) {
        int e = errno;
        close(dfd);
        errno = e;
        return -1;
    }
    int err = remove_at(dfd, name, st.st_dev);
    close(dfd);
    if (err) {
        errno = err;
        return -1;
    }
    return 0;
}
