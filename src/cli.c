/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "cli.h"

#include <errno.h>
#include <glib.h>
#include <stdio.h>
#include <string.h>

#include "disks.h"
#include "health.h"
#include "i18n.h"
#include "log.h"
#include "scan.h"
#include "udisks.h"
#include "util.h"

typedef struct {
    GMainLoop *loop;
    kd_udisks *u;
    const char *action, *dev;
    bool started;
    guint info_timer;
    int rc;
} cli;

static void print_info(cli *c)
{
    kd_drive_info info;
    if (!kd_udisks_drive_info(c->u, c->dev, &info)) {
        kd_log_error(_("UDisks2 has no drive for %s"), c->dev);
        c->rc = 1;
        return;
    }
    char buf[256];
    kd_health_of(&info, buf, sizeof buf);
    printf("%s: %s\n", _("Health"), buf);
    if (info.has_temp)
        printf("%s: %.0f °C\n", _("Temperature"), info.temp_c);
    if (info.power_on_s) {
        kd_format_duration(buf, sizeof buf, info.power_on_s);
        printf("%s: %s\n", _("Powered on for"), buf);
    }
    if (info.bad_sectors >= 0)
        printf("%s: %lld\n", _("Bad sectors"), (long long)info.bad_sectors);
    if (info.rotation_rate > 0)
        printf("%s: %d RPM\n", _("Rotation"), info.rotation_rate);
    else if (info.rotation_rate == 0)
        printf("%s: %s\n", _("Rotation"), _("none (solid state)"));
    printf("%s: %s\n", _("Removable"), info.removable ? _("yes") : _("no"));
    printf("%s: %s\n", _("Can be powered off"), info.can_power_off ? _("yes") : _("no"));
}

static void on_done(bool ok, const char *error, const char *result, void *ud)
{
    cli *c = ud;
    if (ok) {
        if (result)
            printf("%s\n", result);
    } else {
        kd_log_error("%s", error ? error : _("cancelled"));
        c->rc = 1;
    }
    g_main_loop_quit(c->loop);
}

static gboolean info_now(gpointer data)
{
    cli *c = data;
    c->info_timer = 0;
    print_info(c);
    g_main_loop_quit(c->loop);
    return G_SOURCE_REMOVE;
}

static void on_changed(void *ud)
{
    cli *c = ud;
    if (c->started) {
        /* --info: the SMART attributes (true temperature) have arrived. */
        if (c->info_timer) {
            g_source_remove(c->info_timer);
            info_now(c);
        }
        return;
    }
    c->started = true;
    if (!kd_udisks_ready(c->u)) {
        kd_log_error("%s", kd_udisks_error(c->u));
        c->rc = 3;
        g_main_loop_quit(c->loop);
        return;
    }
    if (strcmp(c->action, "info") == 0) {
        /* The first look asks for the SMART attributes; print when they
         * come, or after 2 s without them. */
        kd_drive_info info;
        kd_udisks_drive_info(c->u, c->dev, &info);
        c->info_timer = g_timeout_add(info.smart_updated ? 2000 : 0, info_now, c);
    } else if (strcmp(c->action, "mount") == 0) {
        kd_udisks_mount(c->u, c->dev, on_done, c);
    } else if (strcmp(c->action, "unmount") == 0) {
        kd_udisks_unmount(c->u, c->dev, on_done, c);
    } else if (strcmp(c->action, "eject") == 0) {
        kd_udisks_eject(c->u, c->dev, on_done, c);
    } else {
        kd_udisks_power_off(c->u, c->dev, on_done, c);
    }
}

int kd_cli_run(const char *action, const char *dev)
{
    /* "/dev/sda1" and "sda1" both work. */
    if (strncmp(dev, "/dev/", 5) == 0)
        dev += 5;
    cli c = {.action = action, .dev = dev};
    c.loop = g_main_loop_new(NULL, FALSE);
    c.u = kd_udisks_new(on_changed, &c);
    g_main_loop_run(c.loop);
    kd_udisks_free(c.u);
    g_main_loop_unref(c.loop);
    return c.rc;
}

int kd_cli_scan(const char *path, int top, bool bytes)
{
    kd_tree *t = kd_tree_scan(path, NULL);
    if (!t) {
        kd_log_error(_("cannot scan %s: %s"), path, g_strerror(errno));
        return 1;
    }
    const kd_node *root = kd_tree_node(t, KD_TREE_ROOT);
    const char *rname = kd_tree_name(t, KD_TREE_ROOT);
    const char *sep = g_str_has_suffix(rname, "/") ? "" : "/";
    char sz[32];
    if (bytes) {
        printf("%llu\t%s\n", (unsigned long long)root->size, rname);
    } else {
        kd_format_size(sz, sizeof sz, root->size);
        printf(ngettext("%s: %s in %u file\n", "%s: %s in %u files\n", root->files), rname, sz,
               (unsigned)root->files);
    }
    for (uint32_t i = 0; i < root->count && (top < 0 || i < (uint32_t)top); i++) {
        kd_node_id c = kd_tree_child(t, KD_TREE_ROOT, i);
        const kd_node *n = kd_tree_node(t, c);
        bool dir = n->flags & KD_NODE_DIR;
        if (bytes) {
            printf("%llu\t%s%s%s\n", (unsigned long long)n->size, rname, sep,
                   kd_tree_name(t, c));
        } else {
            kd_format_size(sz, sizeof sz, n->size);
            printf("%10s  %s%s%s\n", sz, kd_tree_name(t, c), dir ? "/" : "",
                   (n->flags & KD_NODE_MOUNT) ? _("  (another filesystem, not counted)") : "");
        }
    }
    if (t->denied)
        kd_log_warn(ngettext("%u folder could not be read: the total is short",
                             "%u folders could not be read: the total is short", t->denied),
                    (unsigned)t->denied);
    kd_tree_free(t);
    return 0;
}
