/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "watch.h"

#include <fcntl.h>
#include <glib-unix.h>
#include <glib.h>
#include <libudev.h>
#include <unistd.h>

#include "log.h"

#define DEBOUNCE_MS 250

struct kd_watch {
    void (*changed)(void *ud);
    void *ud;
    struct udev *udev;
    struct udev_monitor *mon;
    guint mon_src;
    int fds[2]; /* mountinfo, swaps */
    guint fd_src[2];
    guint timer;
};

static gboolean fire(gpointer data)
{
    kd_watch *w = data;
    w->timer = 0;
    w->changed(w->ud);
    return G_SOURCE_REMOVE;
}

static void poke(kd_watch *w)
{
    if (w->timer)
        g_source_remove(w->timer);
    w->timer = g_timeout_add(DEBOUNCE_MS, fire, w);
}

static gboolean on_udev(gint fd, GIOCondition cond, gpointer data)
{
    (void)fd;
    (void)cond;
    kd_watch *w = data;
    /* Drain: one wakeup can carry many events. */
    for (struct udev_device *dev; (dev = udev_monitor_receive_device(w->mon));) {
        kd_log_debug("udev: %s %s", udev_device_get_action(dev),
                     udev_device_get_sysname(dev));
        udev_device_unref(dev);
    }
    poke(w);
    return G_SOURCE_CONTINUE;
}

static gboolean on_proc(gint fd, GIOCondition cond, gpointer data)
{
    (void)cond;
    kd_watch *w = data;
    /* The flag stays up until the file is read again from the start. */
    char buf[4096];
    lseek(fd, 0, SEEK_SET);
    while (read(fd, buf, sizeof buf) > 0)
        ;
    kd_log_debug("mounts or swaps changed");
    poke(w);
    return G_SOURCE_CONTINUE;
}

kd_watch *kd_watch_new(void (*changed)(void *ud), void *ud)
{
    kd_watch *w = g_new0(kd_watch, 1);
    w->changed = changed;
    w->ud = ud;

    w->udev = udev_new();
    if (w->udev)
        w->mon = udev_monitor_new_from_netlink(w->udev, "udev");
    if (w->mon && udev_monitor_filter_add_match_subsystem_devtype(w->mon, "block", NULL) >= 0 &&
        udev_monitor_enable_receiving(w->mon) >= 0) {
        w->mon_src = g_unix_fd_add(udev_monitor_get_fd(w->mon), G_IO_IN, on_udev, w);
    } else {
        kd_log_debug("udev monitor not available");
    }

    static const char *const files[2] = {"/proc/self/mountinfo", "/proc/swaps"};
    for (int i = 0; i < 2; i++) {
        w->fds[i] = open(files[i], O_RDONLY | O_CLOEXEC);
        if (w->fds[i] >= 0) {
            char buf[4096];
            while (read(w->fds[i], buf, sizeof buf) > 0)
                ;
            w->fd_src[i] = g_unix_fd_add(w->fds[i], G_IO_PRI | G_IO_ERR, on_proc, w);
        }
    }
    return w;
}

void kd_watch_free(kd_watch *w)
{
    if (!w)
        return;
    if (w->timer)
        g_source_remove(w->timer);
    if (w->mon_src)
        g_source_remove(w->mon_src);
    for (int i = 0; i < 2; i++) {
        if (w->fd_src[i])
            g_source_remove(w->fd_src[i]);
        if (w->fds[i] >= 0)
            close(w->fds[i]);
    }
    if (w->mon)
        udev_monitor_unref(w->mon);
    if (w->udev)
        udev_unref(w->udev);
    g_free(w);
}
