/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "units.h"

#include <math.h>
#include <string.h>

#include "health.h"
#include "i18n.h"
#include "util.h"

/* Unallocated gaps smaller than this are left out of the overview (they
 * are in the Partitions tab). */
#define MIN_FREE_SHOWN (64ULL * 1024 * 1024)

struct kd_units {
    kd_app *app;
    GtkWidget *scroll;
    GtkWidget *box;  /* the cards */
    GPtrArray *bars; /* usage bars now on screen (not owned) */
    /* The same column is as wide in every card (all but the bar's). */
    GtkSizeGroup *cols[7];
};

/* ---- usage bar --------------------------------------------------------------- */

typedef struct {
    char dev[64];
    kd_fs fs; /* only the numbers and flags are used: the strings may be stale */
} bar_data;

static void bar_text(const bar_data *b, char *out, size_t size, int *pct)
{
    char used[32], total[32];
    kd_format_size(used, sizeof used, b->fs.fs_used);
    kd_format_size(total, sizeof total, b->fs.fs_size);
    *pct = b->fs.fs_size ? (int)lround(100.0 * (double)b->fs.fs_used / (double)b->fs.fs_size)
                         : 0;
    /* TRANSLATORS: usage bar: "41 GiB of 45.5 GiB (90%)" */
    snprintf(out, size, _("%s of %s (%d%%)"), used, total, *pct);
}

static gboolean on_bar_expose(GtkWidget *w, GdkEventExpose *ev, gpointer data)
{
    (void)ev;
    const bar_data *b = data;
    GtkAllocation a;
    gtk_widget_get_allocation(w, &a);
    GtkStyle *st = gtk_widget_get_style(w);
    /* A drawing area has its own GdkWindow: draw from 0,0. */
    cairo_t *cr = gdk_cairo_create(gtk_widget_get_window(w));

    double h = a.height, wd = a.width, r = 3;
    /* The track, in the theme's entry background with a thin border. */
    cairo_new_sub_path(cr);
    cairo_arc(cr, wd - r - 0.5, r + 0.5, r, -M_PI / 2, 0);
    cairo_arc(cr, wd - r - 0.5, h - r - 0.5, r, 0, M_PI / 2);
    cairo_arc(cr, r + 0.5, h - r - 0.5, r, M_PI / 2, M_PI);
    cairo_arc(cr, r + 0.5, r + 0.5, r, M_PI, 3 * M_PI / 2);
    cairo_close_path(cr);
    gdk_cairo_set_source_color(cr, &st->base[GTK_STATE_NORMAL]);
    cairo_fill_preserve(cr);
    cairo_save(cr);
    cairo_clip_preserve(cr);

    char text[128];
    int pct;
    bar_text(b, text, sizeof text, &pct);
    double frac = b->fs.fs_size ? (double)b->fs.fs_used / (double)b->fs.fs_size : 0;
    if (frac > 1)
        frac = 1;
    /* Green, amber from 75%, red from 90%; swap is always blue. */
    if (b->fs.swap_active)
        cairo_set_source_rgb(cr, 0.45, 0.62, 0.85);
    else if (pct >= 90)
        cairo_set_source_rgb(cr, 0.87, 0.36, 0.33);
    else if (pct >= 75)
        cairo_set_source_rgb(cr, 0.93, 0.68, 0.25);
    else
        cairo_set_source_rgb(cr, 0.47, 0.75, 0.40);
    cairo_rectangle(cr, 0, 0, wd * frac, h);
    cairo_fill(cr);
    cairo_restore(cr);
    gdk_cairo_set_source_color(cr, &st->dark[GTK_STATE_NORMAL]);
    cairo_set_line_width(cr, 1);
    cairo_stroke(cr);

    PangoLayout *pl = gtk_widget_create_pango_layout(w, text);
    int tw, th;
    pango_layout_get_pixel_size(pl, &tw, &th);
    gdk_cairo_set_source_color(cr, &st->text[GTK_STATE_NORMAL]);
    cairo_move_to(cr, (wd - tw) / 2, (h - th) / 2);
    pango_cairo_show_layout(cr, pl);
    g_object_unref(pl);
    cairo_destroy(cr);
    return TRUE;
}

static void bar_set(GtkWidget *w, bar_data *b, const kd_fs *fs)
{
    b->fs = *fs;
    char used[32], avail[32], total[32];
    kd_format_size(used, sizeof used, fs->fs_used);
    kd_format_size(avail, sizeof avail, fs->fs_avail);
    kd_format_size(total, sizeof total, fs->fs_size);
    char *tip = g_strdup_printf("%s: %s\n%s: %s\n%s: %s", _("Used"), used, _("Free"), avail,
                                _("Total"), total);
    gtk_widget_set_tooltip_text(w, tip);
    g_free(tip);
    gtk_widget_queue_draw(w);
}

static GtkWidget *bar_new(kd_units *u, const char *dev, const kd_fs *fs)
{
    GtkWidget *w = gtk_drawing_area_new();
    bar_data *b = g_new0(bar_data, 1);
    g_strlcpy(b->dev, dev, sizeof b->dev);
    g_object_set_data_full(G_OBJECT(w), "kd-bar", b, g_free);
    g_signal_connect(w, "expose-event", G_CALLBACK(on_bar_expose), b);
    /* Tall enough for the text, wide enough to read it. */
    PangoLayout *pl = gtk_widget_create_pango_layout(w, "8");
    int th;
    pango_layout_get_pixel_size(pl, NULL, &th);
    g_object_unref(pl);
    gtk_widget_set_size_request(w, 200, th + 6);
    bar_set(w, b, fs);
    g_ptr_array_add(u->bars, w);
    return w;
}

/* ---- actions --------------------------------------------------------------------- */

typedef struct {
    kd_app *app;
    char dev[64];
    char what[16]; /* "mount", "unmount", "power-off", "eject" */
} pending;

static void on_action_done(bool ok, const char *error, const char *result, void *ud)
{
    pending *p = ud;
    if (ok) {
        if (strcmp(p->what, "mount") == 0)
            kd_app_status(p->app, _("%s mounted at %s"), p->dev, result ? result : "?");
        else if (strcmp(p->what, "unmount") == 0)
            kd_app_status(p->app, _("%s unmounted"), p->dev);
        else if (strcmp(p->what, "power-off") == 0)
            kd_app_status(p->app, _("%s is off: it can be unplugged now"), p->dev);
        else
            kd_app_status(p->app, _("%s ejected"), p->dev);
    } else if (error) {
        char *title = g_strdup_printf(_("Could not change %s"), p->dev);
        kd_app_error(p->app, title, error);
        g_free(title);
        kd_app_status(p->app, "%s", "");
    } else {
        kd_app_status(p->app, "%s", "");
    }
    g_free(p);
}

static void on_action(GtkButton *btn, gpointer data)
{
    kd_units *u = data;
    const char *dev = g_object_get_data(G_OBJECT(btn), "kd-dev");
    const char *what = g_object_get_data(G_OBJECT(btn), "kd-what");
    pending *p = g_new0(pending, 1);
    p->app = u->app;
    g_strlcpy(p->dev, dev, sizeof p->dev);
    g_strlcpy(p->what, what, sizeof p->what);
    gtk_widget_set_sensitive(GTK_WIDGET(btn), FALSE);
    if (strcmp(what, "mount") == 0) {
        kd_app_status(u->app, _("Mounting %s…"), dev);
        kd_udisks_mount(u->app->udisks, dev, on_action_done, p);
    } else if (strcmp(what, "unmount") == 0) {
        kd_app_status(u->app, _("Unmounting %s…"), dev);
        kd_udisks_unmount(u->app->udisks, dev, on_action_done, p);
    } else if (strcmp(what, "power-off") == 0) {
        kd_app_status(u->app, _("Preparing %s to be removed…"), dev);
        kd_udisks_power_off(u->app->udisks, dev, on_action_done, p);
    } else {
        kd_app_status(u->app, _("Ejecting %s…"), dev);
        kd_udisks_eject(u->app->udisks, dev, on_action_done, p);
    }
}

static void on_open(GtkButton *btn, gpointer data)
{
    kd_units *u = data;
    const char *path = g_object_get_data(G_OBJECT(btn), "kd-path");
    GError *err = NULL;
    char *uri = g_filename_to_uri(path, NULL, &err);
    if (uri && !g_app_info_launch_default_for_uri(uri, NULL, &err)) {
        kd_app_error(u->app, _("Could not open the folder"), err->message);
    } else if (!uri) {
        kd_app_error(u->app, _("Could not open the folder"), err->message);
    }
    if (err)
        g_error_free(err);
    g_free(uri);
}

static void on_space(GtkButton *btn, gpointer data)
{
    kd_units *u = data;
    kd_app_show_space(u->app, g_object_get_data(G_OBJECT(btn), "kd-path"));
}

static GtkWidget *action_button(kd_units *u, const char *label, const char *icon,
                                const char *dev, const char *what)
{
    GtkWidget *b = gtk_button_new_with_label(label);
    if (icon)
        gtk_button_set_image(GTK_BUTTON(b), gtk_image_new_from_icon_name(icon, GTK_ICON_SIZE_BUTTON));
    g_object_set_data_full(G_OBJECT(b), "kd-dev", g_strdup(dev), g_free);
    g_object_set_data(G_OBJECT(b), "kd-what", (gpointer)what);
    g_signal_connect(b, "clicked", G_CALLBACK(on_action), u);
    const char *why = NULL;
    if (!kd_udisks_ready(u->app->udisks))
        why = kd_udisks_error(u->app->udisks) ? kd_udisks_error(u->app->udisks)
                                               : _("UDisks2 is not ready yet.");
    else if (!kd_udisks_allowed(dev))
        why = _("KIDISKS_ONLY_LOOP is set: only loop devices may be changed.");
    if (why) {
        gtk_widget_set_sensitive(b, FALSE);
        gtk_widget_set_tooltip_text(b, why);
    }
    return b;
}

/* ---- rows ------------------------------------------------------------------------- */

/* "ext4", "FAT32", "NTFS", "LUKS"... */
static const char *fs_display(const kd_fs *fs)
{
    static const struct {
        const char *type, *name;
    } names[] = {{"crypto_LUKS", "LUKS"}, {"LVM2_member", "LVM"}, {"linux_raid_member", "RAID"},
                 {"ntfs", "NTFS"},        {"exfat", "exFAT"},    {"xfs", "XFS"},
                 {"iso9660", "ISO 9660"}, {"squashfs", "SquashFS"}};
    if (strcmp(fs->type, "vfat") == 0)
        return *fs->version ? fs->version : "FAT";
    for (size_t i = 0; i < G_N_ELEMENTS(names); i++)
        if (strcmp(fs->type, names[i].type) == 0)
            return names[i].name;
    return fs->type;
}

/* Plain text; dim = grey (for "not mounted", free space...). Few font
 * variants on purpose: each new one costs a fontconfig sort over every
 * installed font. */
static GtkWidget *cell_label(const char *text, bool dim)
{
    GtkWidget *l = gtk_label_new(NULL);
    if (dim) {
        char *m = g_markup_printf_escaped("<span foreground=\"#888888\">%s</span>", text);
        gtk_label_set_markup(GTK_LABEL(l), m);
        g_free(m);
    } else {
        gtk_label_set_text(GTK_LABEL(l), text);
    }
    gtk_misc_set_alignment(GTK_MISC(l), 0, 0.5f);
    return l;
}

typedef struct {
    kd_units *u;
    GtkWidget *table;
    guint row;
} rows;

static void attach(rows *r, GtkWidget *w, guint col, bool expand)
{
    if (r->u->cols[col])
        gtk_size_group_add_widget(r->u->cols[col], w);
    gtk_table_attach(GTK_TABLE(r->table), w, col, col + 1, r->row, r->row + 1,
                     expand ? GTK_EXPAND | GTK_FILL : GTK_FILL, GTK_FILL, 0, 0);
}

/* What goes in the mountpoint column when there is no mountpoint. */
static const char *not_mounted_text(const kd_fs *fs, int n_holders, const kd_holder *hs)
{
    if (strcmp(fs->type, "crypto_LUKS") == 0)
        return n_holders ? _("unlocked") : _("locked");
    if (strcmp(fs->type, "LVM2_member") == 0)
        return n_holders ? _("in use by LVM") : _("LVM, inactive");
    if (n_holders && hs[0].kind == KD_HOLDER_RAID)
        return _("in a RAID array");
    if (strcmp(fs->type, "swap") == 0)
        return _("swap, off");
    if (kd_fs_mountable(fs))
        return _("not mounted");
    return "";
}

static void add_fs_row(rows *r, int indent, const char *name, const char *tip, const char *dev,
                       const kd_fs *fs, uint64_t size, int n_holders, const kd_holder *hs)
{
    kd_units *u = r->u;
    GtkWidget *nl = cell_label(name, false);
    gtk_misc_set_padding(GTK_MISC(nl), 16 * indent, 0);
    gtk_label_set_ellipsize(GTK_LABEL(nl), PANGO_ELLIPSIZE_MIDDLE);
    gtk_label_set_max_width_chars(GTK_LABEL(nl), 14);
    if (tip)
        gtk_widget_set_tooltip_text(nl, tip);
    attach(r, nl, 0, false);
    attach(r, cell_label(fs_display(fs), false), 1, false);
    GtkWidget *ll = cell_label(fs->label, false);
    gtk_label_set_ellipsize(GTK_LABEL(ll), PANGO_ELLIPSIZE_END);
    gtk_label_set_max_width_chars(GTK_LABEL(ll), 16);
    attach(r, ll, 2, false);

    GtkWidget *ml;
    if (*fs->mountpoint) {
        ml = cell_label(fs->mountpoint, false);
        gtk_label_set_ellipsize(GTK_LABEL(ml), PANGO_ELLIPSIZE_MIDDLE);
        gtk_label_set_max_width_chars(GTK_LABEL(ml), 22);
        gtk_widget_set_tooltip_text(ml, fs->mountpoint);
    } else if (fs->swap_active) {
        ml = cell_label(_("swap, in use"), false);
    } else {
        ml = cell_label(not_mounted_text(fs, n_holders, hs), true);
    }
    attach(r, ml, 3, false);

    if (fs->has_usage)
        attach(r, bar_new(u, dev, fs), 4, true);

    char sz[32];
    kd_format_size(sz, sizeof sz, size);
    GtkWidget *sl = cell_label(sz, false);
    gtk_misc_set_alignment(GTK_MISC(sl), 1, 0.5f);
    attach(r, sl, 5, false);

    GtkWidget *btns = gtk_hbox_new(FALSE, 4);
    if (kd_fs_mountable(fs)) {
        if (!*fs->mountpoint)
            gtk_box_pack_start(GTK_BOX(btns),
                               action_button(u, _("Mount"), "media-mount", dev, "mount"),
                               FALSE, FALSE, 0);
        else if (!kd_mount_is_system(fs->mountpoint))
            gtk_box_pack_start(GTK_BOX(btns),
                               action_button(u, _("Unmount"), "media-eject", dev, "unmount"),
                               FALSE, FALSE, 0);
        if (*fs->mountpoint) {
            GtkWidget *open = gtk_button_new();
            gtk_button_set_image(GTK_BUTTON(open),
                                 gtk_image_new_from_icon_name("folder-open", GTK_ICON_SIZE_BUTTON));
            gtk_widget_set_tooltip_text(open, _("Open in the file manager"));
            g_object_set_data_full(G_OBJECT(open), "kd-path", g_strdup(fs->mountpoint), g_free);
            g_signal_connect(open, "clicked", G_CALLBACK(on_open), u);
            gtk_box_pack_start(GTK_BOX(btns), open, FALSE, FALSE, 0);

            static const char *const space_icons[] = {"disk-quota", "drive-harddisk", NULL};
            GtkWidget *space = gtk_button_new();
            gtk_button_set_image(GTK_BUTTON(space),
                                 kd_icon_image(space_icons, GTK_ICON_SIZE_BUTTON));
            gtk_widget_set_tooltip_text(space, _("See what takes up room here"));
            g_object_set_data_full(G_OBJECT(space), "kd-path", g_strdup(fs->mountpoint), g_free);
            g_signal_connect(space, "clicked", G_CALLBACK(on_space), u);
            gtk_box_pack_start(GTK_BOX(btns), space, FALSE, FALSE, 0);
        }
    }
    attach(r, btns, 6, false);
    r->row++;
}

static void add_holder_rows(rows *r, int indent, const kd_holder *hs, int n)
{
    for (int i = 0; i < n; i++) {
        const kd_holder *h = &hs[i];
        char *name = g_strdup(*h->dm_name ? h->dm_name : h->name);
        char *tip = g_strdup_printf("/dev/%s%s%s", h->name, *h->dm_name ? "\n/dev/mapper/" : "",
                                    h->dm_name);
        add_fs_row(r, indent, name, tip, h->name, &h->fs, h->size, 0, NULL);
        g_free(name);
        g_free(tip);
    }
}

static void add_plain_row(rows *r, int indent, const char *name, const char *what, uint64_t size)
{
    GtkWidget *nl = cell_label(name, true);
    gtk_misc_set_padding(GTK_MISC(nl), 16 * indent, 0);
    attach(r, nl, 0, false);
    gtk_table_attach(GTK_TABLE(r->table), cell_label(what, true), 1, 5, r->row, r->row + 1,
                     GTK_FILL, GTK_FILL, 0, 0);
    char sz[32];
    kd_format_size(sz, sizeof sz, size);
    GtkWidget *sl = cell_label(sz, true);
    gtk_misc_set_alignment(GTK_MISC(sl), 1, 0.5f);
    attach(r, sl, 5, false);
    r->row++;
}

/* ---- cards ------------------------------------------------------------------------- */

static GtkWidget *disk_icon(const kd_disk *k, kd_media media)
{
    static const char *const usb[] = {"drive-removable-media-usb", "drive-removable-media",
                                      "drive-harddisk", NULL};
    static const char *const mmc[] = {"media-flash", "drive-removable-media", "drive-harddisk",
                                      NULL};
    static const char *const optical[] = {"drive-optical", "drive-harddisk", NULL};
    static const char *const ssd[] = {"drive-harddisk-solidstate", "drive-harddisk", NULL};
    static const char *const hdd[] = {"drive-harddisk", NULL};
    const char *const *names = hdd;
    if (k->bus == KD_BUS_USB)
        names = usb;
    else if (k->bus == KD_BUS_MMC)
        names = mmc;
    else if (k->bus == KD_BUS_OPTICAL)
        names = optical;
    else if (media == KD_MEDIA_SSD)
        names = ssd;
    return kd_icon_image(names, GTK_ICON_SIZE_DND);
}

static void append_part(GString *s, const char *part)
{
    if (!*part)
        return;
    if (s->len)
        g_string_append(s, "  ·  ");
    g_string_append(s, part);
}

static GtkWidget *health_label(const kd_drive_info *info)
{
    char verdict[256];
    kd_health h = kd_health_of(info, verdict, sizeof verdict);
    static const char *const color[] = {"#888888", "#3a9a3a", "#d08a10", "#d03030"};
    GString *s = g_string_new(NULL);
    char *esc = g_markup_escape_text(verdict, -1);
    if (h == KD_HEALTH_FAILING)
        g_string_printf(s, "<span foreground=\"%s\"><b>● %s</b></span>", color[h], esc);
    else
        g_string_printf(s, "<span foreground=\"%s\">●</span> %s", color[h], esc);
    g_free(esc);
    if (info->has_temp)
        g_string_append_printf(s, "  ·  %.0f °C", info->temp_c);
    if (info->power_on_s) {
        char dur[96];
        kd_format_duration(dur, sizeof dur, info->power_on_s);
        esc = g_markup_printf_escaped(_("powered on for %s"), dur);
        g_string_append_printf(s, "  ·  %s", esc);
        g_free(esc);
    }
    GtkWidget *l = gtk_label_new(NULL);
    gtk_label_set_markup(GTK_LABEL(l), s->str);
    gtk_misc_set_alignment(GTK_MISC(l), 0, 0.5f);
    g_string_free(s, TRUE);
    return l;
}

static GtkWidget *disk_card(kd_units *u, const kd_disk *k)
{
    kd_drive_info info;
    kd_udisks_drive_info(u->app->udisks, k->name, &info);
    kd_media media = k->media;
    if (media == KD_MEDIA_UNKNOWN && info.rotation_rate >= 0 && k->bus != KD_BUS_OPTICAL)
        media = info.rotation_rate ? KD_MEDIA_HDD : KD_MEDIA_SSD;

    GtkWidget *frame = gtk_frame_new(NULL);
    gtk_frame_set_shadow_type(GTK_FRAME(frame), GTK_SHADOW_ETCHED_IN);
    GtkWidget *vbox = gtk_vbox_new(FALSE, 6);
    gtk_container_set_border_width(GTK_CONTAINER(vbox), 10);
    gtk_container_add(GTK_CONTAINER(frame), vbox);

    /* Header: icon, model, details, buttons. */
    GtkWidget *head = gtk_hbox_new(FALSE, 10);
    gtk_box_pack_start(GTK_BOX(vbox), head, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(head), disk_icon(k, media), FALSE, FALSE, 0);
    GtkWidget *titles = gtk_vbox_new(FALSE, 2);
    gtk_box_pack_start(GTK_BOX(head), titles, TRUE, TRUE, 0);

    char *m = g_markup_printf_escaped("<b><big>%s</big></b>", *k->model ? k->model : k->name);
    GtkWidget *title = gtk_label_new(NULL);
    gtk_label_set_markup(GTK_LABEL(title), m);
    g_free(m);
    gtk_misc_set_alignment(GTK_MISC(title), 0, 0.5f);
    gtk_label_set_ellipsize(GTK_LABEL(title), PANGO_ELLIPSIZE_END);
    gtk_box_pack_start(GTK_BOX(titles), title, FALSE, FALSE, 0);

    GString *sub = g_string_new(NULL);
    char buf[256];
    snprintf(buf, sizeof buf, "/dev/%s", k->name);
    append_part(sub, buf);
    append_part(sub, kd_bus_name(k->bus));
    append_part(sub, media == KD_MEDIA_HDD ? _("HDD") : media == KD_MEDIA_SSD ? _("SSD") : "");
    if (!k->no_media) {
        kd_format_disk_size(buf, sizeof buf, k->size);
        append_part(sub, buf);
    }
    if (*k->table)
        append_part(sub, strcmp(k->table, "dos") == 0 ? "MBR" : strcmp(k->table, "gpt") == 0
                                                                    ? "GPT"
                                                                    : k->table);
    if (*k->serial) {
        snprintf(buf, sizeof buf, _("serial %s"), k->serial);
        append_part(sub, buf);
    }
    if (*k->loop_file)
        append_part(sub, k->loop_file);
    if (k->system)
        append_part(sub, _("system disk"));
    GtkWidget *subl = gtk_label_new(NULL);
    m = g_markup_printf_escaped("<small>%s</small>", sub->str);
    gtk_label_set_markup(GTK_LABEL(subl), m);
    g_free(m);
    g_string_free(sub, TRUE);
    gtk_misc_set_alignment(GTK_MISC(subl), 0, 0.5f);
    gtk_label_set_ellipsize(GTK_LABEL(subl), PANGO_ELLIPSIZE_END);
    gtk_box_pack_start(GTK_BOX(titles), subl, FALSE, FALSE, 0);

    GtkWidget *hbtn = gtk_hbox_new(FALSE, 4);
    if (info.found && info.can_power_off && !k->system && !k->no_media)
        gtk_box_pack_start(GTK_BOX(hbtn),
                           action_button(u, _("Safely remove"), "media-eject", k->name,
                                         "power-off"),
                           FALSE, FALSE, 0);
    else if (info.found && info.ejectable && !k->system && !k->no_media)
        gtk_box_pack_start(GTK_BOX(hbtn), action_button(u, _("Eject"), "media-eject", k->name,
                                                        "eject"),
                           FALSE, FALSE, 0);
    GtkWidget *halign = gtk_alignment_new(1, 0.5f, 0, 0);
    gtk_container_add(GTK_CONTAINER(halign), hbtn);
    gtk_box_pack_end(GTK_BOX(head), halign, FALSE, FALSE, 0);

    if (info.found && k->bus != KD_BUS_OPTICAL)
        gtk_box_pack_start(GTK_BOX(vbox), health_label(&info), FALSE, FALSE, 0);

    if (k->no_media) {
        gtk_box_pack_start(GTK_BOX(vbox), cell_label(_("No media"), true), FALSE, FALSE, 0);
        return frame;
    }

    rows r = {u, gtk_table_new(1, 7, FALSE), 0};
    gtk_table_set_col_spacings(GTK_TABLE(r.table), 12);
    gtk_table_set_row_spacings(GTK_TABLE(r.table), 4);

    if (*k->fs.type)
        add_fs_row(&r, 0, k->name, NULL, k->name, &k->fs, k->size, k->n_holders, k->holders);
    add_holder_rows(&r, 1, k->holders, k->n_holders);
    for (int i = 0; i < k->n_parts; i++) {
        const kd_part *p = &k->parts[i];
        int indent = p->in_extended ? 1 : 0;
        if (p->kind == KD_PART_FREE) {
            if (p->size >= MIN_FREE_SHOWN)
                add_plain_row(&r, indent, "—", _("unallocated"), p->size);
        } else if (p->kind == KD_PART_EXTENDED) {
            add_plain_row(&r, indent, p->name, _("extended partition"), p->size);
        } else {
            add_fs_row(&r, indent, p->name, *p->pname ? p->pname : NULL, p->name, &p->fs,
                       p->size, p->n_holders, p->holders);
            add_holder_rows(&r, indent + 1, p->holders, p->n_holders);
        }
    }
    if (r.row)
        gtk_box_pack_start(GTK_BOX(vbox), r.table, FALSE, FALSE, 4);
    else
        gtk_widget_destroy(r.table);
    return frame;
}

/* ---- the tab ----------------------------------------------------------------------- */

static void rebuild(kd_units *u)
{
    g_ptr_array_set_size(u->bars, 0);
    GList *kids = gtk_container_get_children(GTK_CONTAINER(u->box));
    for (GList *l = kids; l; l = l->next)
        gtk_widget_destroy(l->data);
    g_list_free(kids);

    int shown = 0;
    for (int i = 0; u->app->disks && i < u->app->disks->n; i++) {
        const kd_disk *k = &u->app->disks->disks[i];
        if (k->hidden && !u->app->show_hidden)
            continue;
        gtk_box_pack_start(GTK_BOX(u->box), disk_card(u, k), FALSE, FALSE, 0);
        shown++;
    }
    if (!shown)
        gtk_box_pack_start(GTK_BOX(u->box), cell_label(_("No disks found."), true), FALSE,
                           FALSE, 0);
    gtk_widget_show_all(u->box);
}

static void refresh_numbers(kd_units *u)
{
    for (guint i = 0; i < u->bars->len; i++) {
        GtkWidget *w = g_ptr_array_index(u->bars, i);
        bar_data *b = g_object_get_data(G_OBJECT(w), "kd-bar");
        const kd_fs *fs = kd_disks_find_fs(u->app->disks, b->dev);
        if (fs && fs->has_usage)
            bar_set(w, b, fs);
    }
}

void kd_units_update(kd_units *u, bool full)
{
    if (full)
        rebuild(u);
    else
        refresh_numbers(u);
}

kd_units *kd_units_new(kd_app *app)
{
    kd_units *u = g_new0(kd_units, 1);
    u->app = app;
    u->bars = g_ptr_array_new();
    for (int i = 0; i < 7; i++)
        if (i != 4)
            u->cols[i] = gtk_size_group_new(GTK_SIZE_GROUP_HORIZONTAL);
    u->box = gtk_vbox_new(FALSE, 12);
    gtk_container_set_border_width(GTK_CONTAINER(u->box), 12);
    u->scroll = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(u->scroll), GTK_POLICY_NEVER,
                                   GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_add_with_viewport(GTK_SCROLLED_WINDOW(u->scroll), u->box);
    gtk_viewport_set_shadow_type(GTK_VIEWPORT(gtk_bin_get_child(GTK_BIN(u->scroll))),
                                 GTK_SHADOW_NONE);
    rebuild(u);
    return u;
}

void kd_units_free(kd_units *u)
{
    if (!u)
        return;
    g_ptr_array_free(u->bars, TRUE);
    for (int i = 0; i < 7; i++)
        if (u->cols[i])
            g_object_unref(u->cols[i]);
    g_free(u);
}

GtkWidget *kd_units_widget(kd_units *u)
{
    return u->scroll;
}
