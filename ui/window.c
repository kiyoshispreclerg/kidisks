/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "window.h"

#include <errno.h>

#include <gdk/gdkkeysyms.h>
#include <glib-unix.h>
#include <gtk/gtk.h>
#include <signal.h>

#include "i18n.h"
#include "log.h"
#include "prefs.h"

typedef struct {
    GtkWidget *window;
    GtkWidget *notebook;
    GtkWidget *status;
    kd_prefs *prefs;
    /* Last unmaximized size, for the prefs. */
    int width, height;
    bool maximized;
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

static GtkWidget *icon_image(const char *const *names, GtkIconSize size)
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

    GtkWidget *img = icon_image(tabs[t].icons, GTK_ICON_SIZE_DIALOG);
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
    gtk_box_pack_start(GTK_BOX(box), icon_image(tabs[t].icons, GTK_ICON_SIZE_MENU),
                       FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), gtk_label_new_with_mnemonic(_(tabs[t].title)),
                       FALSE, FALSE, 0);
    gtk_widget_show_all(box);
    return box;
}

static void on_about(GtkMenuItem *item, gpointer data)
{
    (void)item;
    kd_window *w = data;
    static const char *const authors[] = {"Kiyoshi", NULL};
    gtk_show_about_dialog(
        GTK_WINDOW(w->window), "program-name", "Kidisks", "version", KD_VERSION,
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
    gtk_widget_destroy(w->window);
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
        gtk_notebook_set_current_page(GTK_NOTEBOOK(w->notebook), (int)(ev->keyval - GDK_1));
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
    kd_prefs_set_int(w->prefs, "window", "tab", (int)num);
}

static void on_destroy(GtkWidget *widget, gpointer data)
{
    (void)widget;
    kd_window *w = data;
    kd_prefs_set_int(w->prefs, "window", "width", w->width);
    kd_prefs_set_int(w->prefs, "window", "height", w->height);
    kd_prefs_set_int(w->prefs, "window", "maximized", w->maximized);
    gtk_main_quit();
}

/* SIGTERM (logout), SIGINT (Ctrl+C), SIGHUP: close as if by the window
 * button, so the settings are saved. */
static gboolean on_signal(gpointer data)
{
    kd_window *w = data;
    gtk_widget_destroy(w->window);
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
    w.prefs = kd_prefs_load(opts->prefs_path);
    if (!w.prefs) {
        kd_log_error(_("out of memory"));
        return 1;
    }

    gtk_window_set_default_icon_name("kidisks");
    w.window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(w.window), "Kidisks");
    gtk_window_set_wmclass(GTK_WINDOW(w.window), "kidisks", "Kidisks");
    w.width = kd_prefs_get_int(w.prefs, "window", "width", 860, 400, 10000);
    w.height = kd_prefs_get_int(w.prefs, "window", "height", 600, 300, 10000);
    gtk_window_set_default_size(GTK_WINDOW(w.window), w.width, w.height);
    if (kd_prefs_get_int(w.prefs, "window", "maximized", 0, 0, 1))
        gtk_window_maximize(GTK_WINDOW(w.window));

    GtkAccelGroup *accel = gtk_accel_group_new();
    gtk_window_add_accel_group(GTK_WINDOW(w.window), accel);

    GtkWidget *vbox = gtk_vbox_new(FALSE, 0);
    gtk_container_add(GTK_CONTAINER(w.window), vbox);
    gtk_box_pack_start(GTK_BOX(vbox), menu_bar(&w, accel), FALSE, FALSE, 0);

    w.notebook = gtk_notebook_new();
    for (int t = 0; t < KD_TAB_COUNT; t++)
        gtk_notebook_append_page(GTK_NOTEBOOK(w.notebook), placeholder_page(t), tab_label(t));
    gtk_box_pack_start(GTK_BOX(vbox), w.notebook, TRUE, TRUE, 0);

    w.status = gtk_statusbar_new();
    gtk_box_pack_start(GTK_BOX(vbox), w.status, FALSE, FALSE, 0);

    int tab = opts->tab >= 0 ? opts->tab
                             : kd_prefs_get_int(w.prefs, "window", "tab", KD_TAB_UNITS, 0,
                                                KD_TAB_COUNT - 1);
    gtk_widget_show_all(w.window);
    /* After show: GtkNotebook ignores set_current_page on hidden pages. */
    gtk_notebook_set_current_page(GTK_NOTEBOOK(w.notebook), tab);

    g_signal_connect(w.notebook, "switch-page", G_CALLBACK(on_switch_page), &w);
    g_signal_connect(w.window, "key-press-event", G_CALLBACK(on_key), &w);
    g_signal_connect(w.window, "configure-event", G_CALLBACK(on_configure), &w);
    g_signal_connect(w.window, "window-state-event", G_CALLBACK(on_state), &w);
    g_signal_connect(w.window, "destroy", G_CALLBACK(on_destroy), &w);
    first_expose_id = g_signal_connect_after(w.window, "expose-event",
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

    int rc = 0;
    if (opts->exit_after_map) {
        /* The window is still up: take it down without on_destroy. */
        g_signal_handlers_disconnect_by_data(w.window, &w);
        gtk_widget_destroy(w.window);
    } else if (kd_prefs_save(w.prefs) < 0) {
        kd_log_warn(_("could not save the settings: %s"), g_strerror(errno));
    }
    kd_prefs_free(w.prefs);
    return rc;
}
