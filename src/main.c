/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <locale.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

#include <gtk/gtk.h>

#include "i18n.h"
#include "log.h"
#include "util.h"
#include "window.h"

/* Folder of the executable, "" if unknown. */
static char self_dir[KD_PATH_MAX];

static void find_self_dir(void)
{
    ssize_t n = readlink("/proc/self/exe", self_dir, sizeof self_dir - 1);
    self_dir[n > 0 ? n : 0] = '\0';
    char *slash = strrchr(self_dir, '/');
    if (slash)
        *slash = '\0';
    else
        self_dir[0] = '\0';
}

/* Translations: locale/ next to the executable (a build tree), else the
 * installed ones. */
static void setup_gettext(void)
{
    char dir[KD_PATH_MAX];
    const char *localedir = LOCALEDIR;
    if (self_dir[0] && kd_pathf(dir, sizeof dir, "%s/locale", self_dir) && kd_dir_exists(dir))
        localedir = dir;
    bindtextdomain(KD_GETTEXT_DOMAIN, localedir);
    bind_textdomain_codeset(KD_GETTEXT_DOMAIN, "UTF-8");
    textdomain(KD_GETTEXT_DOMAIN);
}

static void usage(FILE *f)
{
    fprintf(f, _("Usage: kidisks [OPTION]...\n"
                 "Disks, partitions, space usage and cleanup.\n"
                 "\n"
                 "  -t, --tab TAB        open on TAB: units, partitions, space or cleanup\n"
                 "  -c, --config FILE    settings file (default ~/.config/kidisks/kidisks.ini)\n"
                 "  -v, --verbose        print what is going on\n"
                 "  -V, --version        print the version and exit\n"
                 "  -h, --help           print this help and exit\n"));
}

/* "units", "partitions"... or the 1-based number; -1 if neither. */
static int parse_tab(const char *s)
{
    static const char *const names[KD_TAB_COUNT] = {"units", "partitions", "space",
                                                    "cleanup"};
    for (int t = 0; t < KD_TAB_COUNT; t++)
        if (strcasecmp(s, names[t]) == 0)
            return t;
    if (s[0] >= '1' && s[0] < '1' + KD_TAB_COUNT && !s[1])
        return s[0] - '1';
    return -1;
}

int main(int argc, char **argv)
{
    kd_window_opts opts = {.tab = -1, .start_us = g_get_monotonic_time()};
    setlocale(LC_ALL, "");
    find_self_dir();
    setup_gettext();

    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        bool more = i + 1 < argc;
        if (strcmp(a, "-h") == 0 || strcmp(a, "--help") == 0) {
            usage(stdout);
            return 0;
        } else if (strcmp(a, "-V") == 0 || strcmp(a, "--version") == 0) {
            printf("kidisks %s\n", KD_VERSION);
            return 0;
        } else if (strcmp(a, "-v") == 0 || strcmp(a, "--verbose") == 0) {
            kd_log_set_verbose(true);
        } else if ((strcmp(a, "-t") == 0 || strcmp(a, "--tab") == 0) && more) {
            opts.tab = parse_tab(argv[++i]);
            if (opts.tab < 0) {
                kd_log_error(_("unknown tab: %s"), argv[i]);
                return 2;
            }
        } else if ((strcmp(a, "-c") == 0 || strcmp(a, "--config") == 0) && more) {
            opts.prefs_path = argv[++i];
        } else if (strcmp(a, "--exit-after-map") == 0) {
            /* Undocumented: for startup timing and tests. */
            opts.exit_after_map = true;
        } else {
            usage(stderr);
            return 2;
        }
    }

    /* The window runs as the person using it. Changing disks goes through
     * UDisks2, which asks for the password via polkit. */
    if (geteuid() == 0) {
        kd_log_error(_("do not run Kidisks as root: it asks for the administrator "
                       "password itself when a change needs it"));
        return 1;
    }

    if (!gtk_init_check(&argc, &argv)) {
        kd_log_error(_("cannot open the display"));
        return 1;
    }
    /* Run from a build tree: the icon is still in ../data. */
    char data[KD_PATH_MAX];
    if (self_dir[0] && kd_pathf(data, sizeof data, "%s/../data/kidisks.svg", self_dir) &&
        kd_file_exists(data)) {
        data[strlen(data) - strlen("/kidisks.svg")] = '\0';
        gtk_icon_theme_append_search_path(gtk_icon_theme_get_default(), data);
    }
    return kd_window_run(&opts);
}
