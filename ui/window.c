/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "window.h"

#include <errno.h>

#include <gdk/gdkkeysyms.h>
#include <glib-unix.h>
#include <gtk/gtk.h>
#include <signal.h>

#include <stdarg.h>
#include <string.h>

#include "app.h"
#include "i18n.h"
#include "log.h"
#include "prefs.h"
#include "units.h"

/* How often the used/free numbers are refreshed while the Units tab shows. */
#define TICK_S 5

typedef struct {
    kd_app app;
    /* Last unmaximized size, for the prefs. */
    int width, height;
    bool maximized;
    char *layout_key;
    guint udisks_timer, tick;
} kd_window;

/* Each tab, in kd_tab order. The icons fall back to a generic one on themes
 * that lack the first choice (drive-multipartition and disk-quota are
 * Breeze names; the last of each list is in every freedesktop theme). */
static const struct {
    const char *title;
    const char *what;
    const char *icons[3];
} tabs[KD_TAB_COUNT] = {
    {N_("_Units"),
     N_("Disks and partitions at a glance: model, type, health, temperature "
        "and how full each partition is."),
     {"drive-harddisk", NULL}},
    {N_("_Partitions"),
     N_("Create, delete, format and resize partitions."),
     {"drive-multipartition", "drive-harddisk", NULL}},
    {N_("_Space"),
     N_("What takes up room on a disk, in concentric rings."),
     {"disk-quota", "drive-harddisk", NULL}},
    {N_("_Cleanup"),
     N_("Remove caches, leftovers and traces that programs leave behind."),
     {"edit-clear-all", "edit-clear", NULL}},
};

GtkWidget *kd_icon_image(const char *const *names, GtkIconSize size)
{
    GtkIconTheme *theme = gtk_icon_theme_get_default();
    const char *name = names[0];
    for (int i = 0; names[i]; i++)
        if (gtk_icon_theme_has_icon(theme, names[i])) {
            name = names[i];
            break;
        }
    return gtk_image_new_from_icon_name(name, size);
}

/* Until each tab gets its phase: what it will do. */
static GtkWidget *placeholder_page(int t)
{
    GtkWidget *box = gtk_vbox_new(FALSE, 12);
    gtk_container_set_border_width(GTK_CONTAINER(box), 24);

    GtkWidget *img = kd_icon_image(tabs[t].icons, GTK_ICON_SIZE_DIALOG);
    gtk_box_pack_start(GTK_BOX(box), img, TRUE, TRUE, 0);
    gtk_misc_set_alignment(GTK_MISC(img), 0.5f, 1.0f);

    GtkWidget *what = gtk_label_new(_(tabs[t].what));
    gtk_label_set_line_wrap(GTK_LABEL(what), TRUE);
    gtk_label_set_justify(GTK_LABEL(what), GTK_JUSTIFY_CENTER);
    gtk_box_pack_start(GTK_BOX(box), what, FALSE, FALSE, 0);

    GtkWidget *soon = gtk_label_new(NULL);
    char *markup = g_markup_printf_escaped("<i>%s</i>", _("Coming in a future version."));
    gtk_label_set_markup(GTK_LABEL(soon), markup);
    g_free(markup);
    gtk_misc_set_alignment(GTK_MISC(soon), 0.5f, 0.0f);
    gtk_box_pack_start(GTK_BOX(box), soon, TRUE, TRUE, 0);
    return box;
}

static GtkWidget *tab_label(int t)
{
    GtkWidget *box = gtk_hbox_new(FALSE, 6);
    gtk_box_pack_start(GTK_BOX(box), kd_icon_image(tabs[t].icons, GTK_ICON_SIZE_MENU),
                       FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), gtk_label_new_with_mnemonic(_(tabs[t].title)),
                       FALSE, FALSE, 0);
    gtk_widget_show_all(box);
    return box;
}

void kd_app_status(kd_app *app, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    char *msg = g_strdup_vprintf(fmt, ap);
    va_end(ap);
    guint ctx = gtk_statusbar_get_context_id(GTK_STATUSBAR(app->status), "kd");
    gtk_statusbar_pop(GTK_STATUSBAR(app->status), ctx);
    gtk_statusbar_push(GTK_STATUSBAR(app->status), ctx, msg);
    g_free(msg);
}

void kd_app_error(kd_app *app, const char *title, const char *detail)
{
    GtkWidget *d = gtk_message_dialog_new(GTK_WINDOW(app->window), GTK_DIALOG_DESTROY_WITH_PARENT,
                                          GTK_MESSAGE_ERROR, GTK_BUTTONS_CLOSE, "%s", title);
    if (detail && *detail)
        gtk_message_dialog_format_secondary_text(GTK_MESSAGE_DIALOG(d), "%s", detail);
    g_signal_connect(d, "response", G_CALLBACK(gtk_widget_destroy), NULL);
    gtk_widget_show(d);
}

/* ---- keeping the disk model fresh --------------------------------------------- */

/* Scans again; force: rebuild the tabs even if the layout is the same. */
static void rescan(kd_window *w, bool force)
{
    kd_disks *d = kd_disks_scan(NULL);
    if (!d)
        return;
    char *key = kd_disks_layout_key(d);
    bool changed = force || !w->layout_key || strcmp(key, w->layout_key) != 0;
    g_free(w->layout_key);
    w->layout_key = key;
    kd_disks_free(w->app.disks);
    w->app.disks = d;
    kd_log_debug("rescan: %s", changed ? "rebuilding" : "numbers only");
    kd_units_update(w->app.units, changed);
}

static void on_watch(void *ud)
{
    rescan(ud, false);
}

static gboolean udisks_settled(gpointer data)
{
    kd_window *w = data;
    w->udisks_timer = 0;
    const char *err = kd_udisks_error(w->app.udisks);
    if (err)
        kd_app_status(&w->app, "%s", err);
    rescan(w, true);
    return G_SOURCE_REMOVE;
}

/* UDisks2 sends bursts of property changes (a mount touches several
 * objects): rebuild once they settle. */
static void on_udisks(void *ud)
{
    kd_window *w = ud;
    if (w->udisks_timer)
        g_source_remove(w->udisks_timer);
    w->udisks_timer = g_timeout_add(300, udisks_settled, w);
}

static gboolean on_tick(gpointer data)
{
    kd_window *w = data;
    GdkWindow *gw = gtk_widget_get_window(w->app.window);
    bool visible = gw && !(gdk_window_get_state(gw) & GDK_WINDOW_STATE_ICONIFIED);
    if (visible && gtk_notebook_get_current_page(GTK_NOTEBOOK(w->app.notebook)) == KD_TAB_UNITS)
        rescan(w, false);
    return G_SOURCE_CONTINUE;
}

static void on_refresh(GtkMenuItem *item, gpointer data)
{
    (void)item;
    rescan(data, true);
}

static void on_show_hidden(GtkCheckMenuItem *item, gpointer data)
{
    kd_window *w = data;
    w->app.show_hidden = gtk_check_menu_item_get_active(item);
    kd_prefs_set_int(w->app.prefs, "units", "show_hidden", w->app.show_hidden);
    kd_units_update(w->app.units, true);
}

static void on_about(GtkMenuItem *item, gpointer data)
{
    (void)item;
    kd_window *w = data;
    static const char *const authors[] = {"Kiyoshi", NULL};
    gtk_show_about_dialog(
        GTK_WINDOW(w->app.window), "program-name", "Kidisks", "version", KD_VERSION,
        "logo-icon-name", "kidisks", "authors", authors,
        "comments", _("Disks, partitions, space usage and cleanup in one light program."),
        "license",
        _("Kidisks is free software: you can redistribute it and/or modify it under "
          "the terms of the GNU General Public License as published by the Free "
          "Software Foundation, either version 3 of the License, or (at your "
          "option) any later version.\n\n"
          "Kidisks is distributed in the hope that it will be useful, but WITHOUT "
          "ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or "
          "FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License "
          "for more details."),
        "wrap-license", TRUE, NULL);
}

static void on_quit(GtkMenuItem *item, gpointer data)
{
    (void)item;
    kd_window *w = data;
    gtk_widget_destroy(w->app.window);
}

static GtkWidget *menu_item(GtkWidget *menu, const char *stock, GCallback cb,
                            kd_window *w)
{
    GtkWidget *item = gtk_image_menu_item_new_from_stock(stock, NULL);
    g_signal_connect(item, "activate", cb, w);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), item);
    return item;
}

static GtkWidget *menu_bar(kd_window *w, GtkAccelGroup *accel)
{
    GtkWidget *bar = gtk_menu_bar_new();

    GtkWidget *file = gtk_menu_item_new_with_mnemonic(_("_File"));
    GtkWidget *file_menu = gtk_menu_new();
    gtk_menu_set_accel_group(GTK_MENU(file_menu), accel);
    GtkWidget *quit = menu_item(file_menu, GTK_STOCK_QUIT, G_CALLBACK(on_quit), w);
    gtk_widget_add_accelerator(quit, "activate", accel, GDK_q, GDK_CONTROL_MASK,
                               GTK_ACCEL_VISIBLE);
    gtk_menu_item_set_submenu(GTK_MENU_ITEM(file), file_menu);
    gtk_menu_shell_append(GTK_MENU_SHELL(bar), file);

    GtkWidget *view = gtk_menu_item_new_with_mnemonic(_("_View"));
    GtkWidget *view_menu = gtk_menu_new();
    gtk_menu_set_accel_group(GTK_MENU(view_menu), accel);
    GtkWidget *refresh = menu_item(view_menu, GTK_STOCK_REFRESH, G_CALLBACK(on_refresh), w);
    gtk_widget_add_accelerator(refresh, "activate", accel, GDK_F5, 0, GTK_ACCEL_VISIBLE);
    GtkWidget *hidden =
        gtk_check_menu_item_new_with_mnemonic(_("Show _hidden devices (loop, zram)"));
    gtk_check_menu_item_set_active(GTK_CHECK_MENU_ITEM(hidden), w->app.show_hidden);
    g_signal_connect(hidden, "toggled", G_CALLBACK(on_show_hidden), w);
    gtk_menu_shell_append(GTK_MENU_SHELL(view_menu), hidden);
    gtk_menu_item_set_submenu(GTK_MENU_ITEM(view), view_menu);
    gtk_menu_shell_append(GTK_MENU_SHELL(bar), view);

    GtkWidget *help = gtk_menu_item_new_with_mnemonic(_("_Help"));
    GtkWidget *help_menu = gtk_menu_new();
    menu_item(help_menu, GTK_STOCK_ABOUT, G_CALLBACK(on_about), w);
    gtk_menu_item_set_submenu(GTK_MENU_ITEM(help), help_menu);
    gtk_menu_shell_append(GTK_MENU_SHELL(bar), help);
    return bar;
}

/* Alt+1..4 jump to a tab (the mnemonics in the tab titles need the tab
 * strip focused; these work from anywhere). */
static gboolean on_key(GtkWidget *widget, GdkEventKey *ev, gpointer data)
{
    (void)widget;
    kd_window *w = data;
    if ((ev->state & gtk_accelerator_get_default_mod_mask()) != GDK_MOD1_MASK)
        return FALSE;
    if (ev->keyval >= GDK_1 && ev->keyval < GDK_1 + KD_TAB_COUNT) {
        gtk_notebook_set_current_page(GTK_NOTEBOOK(w->app.notebook), (int)(ev->keyval - GDK_1));
        return TRUE;
    }
    return FALSE;
}

static gboolean on_configure(GtkWidget *widget, GdkEventConfigure *ev, gpointer data)
{
    (void)widget;
    kd_window *w = data;
    if (!w->maximized) {
        w->width = ev->width;
        w->height = ev->height;
    }
    return FALSE;
}

static gboolean on_state(GtkWidget *widget, GdkEventWindowState *ev, gpointer data)
{
    (void)widget;
    kd_window *w = data;
    w->maximized = (ev->new_window_state & GDK_WINDOW_STATE_MAXIMIZED) != 0;
    return FALSE;
}

static void on_switch_page(GtkNotebook *nb, gpointer page, guint num, gpointer data)
{
    (void)nb;
    (void)page;
    kd_window *w = data;
    kd_prefs_set_int(w->app.prefs, "window", "tab", (int)num);
}

static void on_destroy(GtkWidget *widget, gpointer data)
{
    (void)widget;
    kd_window *w = data;
    kd_prefs_set_int(w->app.prefs, "window", "width", w->width);
    kd_prefs_set_int(w->app.prefs, "window", "height", w->height);
    kd_prefs_set_int(w->app.prefs, "window", "maximized", w->maximized);
    gtk_main_quit();
}

/* SIGTERM (logout), SIGINT (Ctrl+C), SIGHUP: close as if by the window
 * button, so the settings are saved. */
static gboolean on_signal(gpointer data)
{
    kd_window *w = data;
    gtk_widget_destroy(w->app.window);
    return G_SOURCE_REMOVE;
}

static gboolean quit_idle(gpointer data)
{
    (void)data;
    gtk_main_quit();
    return FALSE;
}

static gulong first_expose_id;

static gboolean on_first_expose(GtkWidget *widget, GdkEventExpose *ev, gpointer data)
{
    (void)ev;
    const kd_window_opts *opts = data;
    kd_log_debug("window drawn %.1f ms after start",
                 (double)(g_get_monotonic_time() - opts->start_us) / 1000.0);
    g_signal_handler_disconnect(widget, first_expose_id);
    if (opts->exit_after_map)
        g_idle_add(quit_idle, NULL);
    return FALSE;
}

int kd_window_run(const kd_window_opts *opts)
{
    kd_window w = {0};
    w.app.prefs = kd_prefs_load(opts->prefs_path);
    if (!w.app.prefs) {
        kd_log_error(_("out of memory"));
        return 1;
    }

    w.app.show_hidden = kd_prefs_get_int(w.app.prefs, "units", "show_hidden", 0, 0, 1);
    kd_log_debug("%.1f ms: prefs", (double)(g_get_monotonic_time() - opts->start_us) / 1000.0);
    w.app.disks = kd_disks_scan(NULL);
    kd_log_debug("%.1f ms: disks scanned", (double)(g_get_monotonic_time() - opts->start_us) / 1000.0);
    w.layout_key = w.app.disks ? kd_disks_layout_key(w.app.disks) : NULL;

    gtk_window_set_default_icon_name("kidisks");
    w.app.window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(w.app.window), "Kidisks");
    gtk_window_set_wmclass(GTK_WINDOW(w.app.window), "kidisks", "Kidisks");
    w.width = kd_prefs_get_int(w.app.prefs, "window", "width", 860, 400, 10000);
    w.height = kd_prefs_get_int(w.app.prefs, "window", "height", 600, 300, 10000);
    gtk_window_set_default_size(GTK_WINDOW(w.app.window), w.width, w.height);
    if (kd_prefs_get_int(w.app.prefs, "window", "maximized", 0, 0, 1))
        gtk_window_maximize(GTK_WINDOW(w.app.window));

    GtkAccelGroup *accel = gtk_accel_group_new();
    gtk_window_add_accel_group(GTK_WINDOW(w.app.window), accel);

    GtkWidget *vbox = gtk_vbox_new(FALSE, 0);
    gtk_container_add(GTK_CONTAINER(w.app.window), vbox);
    gtk_box_pack_start(GTK_BOX(vbox), menu_bar(&w, accel), FALSE, FALSE, 0);

    w.app.notebook = gtk_notebook_new();
    w.app.status = gtk_statusbar_new();
    /* The tabs may report on the status bar from the start. */
    w.app.udisks = kd_udisks_new(on_udisks, &w);
    w.app.units = kd_units_new(&w.app);
    kd_log_debug("%.1f ms: units tab built",
                 (double)(g_get_monotonic_time() - opts->start_us) / 1000.0);
    gtk_notebook_append_page(GTK_NOTEBOOK(w.app.notebook), kd_units_widget(w.app.units),
                             tab_label(KD_TAB_UNITS));
    for (int t = KD_TAB_UNITS + 1; t < KD_TAB_COUNT; t++)
        gtk_notebook_append_page(GTK_NOTEBOOK(w.app.notebook), placeholder_page(t), tab_label(t));
    gtk_box_pack_start(GTK_BOX(vbox), w.app.notebook, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(vbox), w.app.status, FALSE, FALSE, 0);
    w.app.watch = kd_watch_new(on_watch, &w);
    w.tick = g_timeout_add_seconds(TICK_S, on_tick, &w);

    int tab = opts->tab >= 0 ? opts->tab
                             : kd_prefs_get_int(w.app.prefs, "window", "tab", KD_TAB_UNITS, 0,
                                                KD_TAB_COUNT - 1);
    gtk_widget_show_all(w.app.window);
    kd_log_debug("%.1f ms: shown", (double)(g_get_monotonic_time() - opts->start_us) / 1000.0);
    /* After show: GtkNotebook ignores set_current_page on hidden pages. */
    gtk_notebook_set_current_page(GTK_NOTEBOOK(w.app.notebook), tab);

    g_signal_connect(w.app.notebook, "switch-page", G_CALLBACK(on_switch_page), &w);
    g_signal_connect(w.app.window, "key-press-event", G_CALLBACK(on_key), &w);
    g_signal_connect(w.app.window, "configure-event", G_CALLBACK(on_configure), &w);
    g_signal_connect(w.app.window, "window-state-event", G_CALLBACK(on_state), &w);
    g_signal_connect(w.app.window, "destroy", G_CALLBACK(on_destroy), &w);
    first_expose_id = g_signal_connect_after(w.app.window, "expose-event",
                                             G_CALLBACK(on_first_expose), (gpointer)opts);

    guint sig_ids[3];
    static const int sigs[3] = {SIGTERM, SIGINT, SIGHUP};
    for (int i = 0; i < 3; i++)
        sig_ids[i] = g_unix_signal_add(sigs[i], on_signal, &w);

    gtk_main();

    /* Each fires once (G_SOURCE_REMOVE): only the others are left. */
    for (int i = 0; i < 3; i++) {
        GSource *src = g_main_context_find_source_by_id(NULL, sig_ids[i]);
        if (src)
            g_source_destroy(src);
    }

    g_source_remove(w.tick);
    if (w.udisks_timer)
        g_source_remove(w.udisks_timer);
    kd_watch_free(w.app.watch);
    kd_udisks_free(w.app.udisks);

    int rc = 0;
    if (opts->exit_after_map) {
        /* The window is still up: take it down without on_destroy. */
        g_signal_handlers_disconnect_by_data(w.app.window, &w);
        gtk_widget_destroy(w.app.window);
    } else if (kd_prefs_save(w.app.prefs) < 0) {
        kd_log_warn(_("could not save the settings: %s"), g_strerror(errno));
    }
    kd_units_free(w.app.units);
    kd_disks_free(w.app.disks);
    g_free(w.layout_key);
    kd_prefs_free(w.app.prefs);
    return rc;
}
