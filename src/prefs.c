/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "prefs.h"

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "i18n.h"
#include "ini.h"
#include "log.h"
#include "util.h"

struct kd_prefs {
    kd_ini *ini;
    char path[KD_PATH_MAX];
    bool dirty;
};

static void warn_line(void *ud, int line, const char *msg)
{
    const kd_prefs *p = ud;
    kd_log_warn("%s:%d: %s", p->path, line, msg);
}

kd_prefs *kd_prefs_load(const char *path)
{
    kd_prefs *p = calloc(1, sizeof *p);
    if (!p)
        return NULL;
    if (path)
        kd_pathf(p->path, sizeof p->path, "%s", path);
    else
        kd_config_path(p->path, sizeof p->path, "kidisks.ini");

    char *text = p->path[0] ? kd_read_file(p->path, NULL) : NULL;
    if (!text && p->path[0] && errno != ENOENT)
        kd_log_warn(_("could not read %s"), p->path);
    p->ini = text ? kd_ini_parse(text, warn_line, p) : kd_ini_new();
    free(text);
    if (!p->ini) {
        free(p);
        return NULL;
    }
    return p;
}

void kd_prefs_free(kd_prefs *p)
{
    if (!p)
        return;
    kd_ini_free(p->ini);
    free(p);
}

int kd_prefs_get_int(const kd_prefs *p, const char *section, const char *key,
                     int def, int min, int max)
{
    const char *s = kd_ini_get(p->ini, section, key);
    if (!s || !*s)
        return def;
    char *end;
    errno = 0;
    long v = strtol(s, &end, 10);
    if (*end || errno)
        return def;
    if (v < min)
        return min;
    if (v > max)
        return max;
    return (int)v;
}

void kd_prefs_set_int(kd_prefs *p, const char *section, const char *key, int v)
{
    char buf[16];
    snprintf(buf, sizeof buf, "%d", v);
    const char *old = kd_ini_get(p->ini, section, key);
    if (old && strcmp(old, buf) == 0)
        return;
    if (kd_ini_set(p->ini, section, key, buf) == 0)
        p->dirty = true;
}

int kd_prefs_save(kd_prefs *p)
{
    if (!p->dirty)
        return 0;
    if (!p->path[0]) {
        errno = ENOENT;
        return -1;
    }
    if (kd_ini_save(p->ini, p->path) < 0)
        return -1;
    p->dirty = false;
    return 0;
}
