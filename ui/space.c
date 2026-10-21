/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "space.h"

#include <errno.h>
#include <gdk/gdkkeysyms.h>
#include <math.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

#include "i18n.h"
#include "log.h"
#include "rings.h"
#include "scan.h"
#include "util.h"

#define TAU (2 * M_PI)
#define MAX_RINGS 5
/* Slices thinner than this merge into "N small items". */
#define MIN_ANGLE (TAU / 360)
/* Beside the rings: names of the ring-0 slices of at least this sweep. */
#define LABEL_ROOM 150
#define LABEL_MIN_ANGLE (TAU / 72)
#define MAX_LABELS 24
/* At most this many folders in the path bar after the root. */
#define MAX_CRUMBS 6

enum { VIEW_HINT, VIEW_PROGRESS, VIEW_CHART };

typedef enum { JOB_SCAN, JOB_TRASH, JOB_DELETE } job_kind;

/* Work for the thread. It only reads path and writes the results; done
 * tells the tab they are there. */
typedef struct {
    job_kind kind;
    char *path;
    /* JOB_SCAN: graft the result here (KD_NODE_NONE: a new tree). Trash and
     * delete: the node that goes. */
    kd_node_id target;
    kd_scan_progress progress;
    kd_tree *result;
    int err;
    char *error; /* g_file_trash's message */
    atomic_bool done;
} job;

struct kd_space {
    kd_app *app;
    GtkWidget *page;
    GtkWidget *roots;  /* the places to scan */
    GtkWidget *crumbs; /* scan root → folder shown */
    GtkWidget *up, *refresh;
    GtkWidget *views;  /* hint, progress, chart (a notebook without tabs) */
    GtkWidget *progress_title, *progress_count, *progress_bar;
    GtkWidget *area;
    GtkWidget *info;   /* what the mouse is on */

    kd_tree *tree;
    unsigned gen; /* bumped whenever tree is replaced: node ids change meaning */
    kd_node_id cur;
    kd_rings rings;
    kd_rings_geom geom;
    bool layout_dirty;
    cairo_surface_t *cache; /* the rings without the highlight */
    int cache_w, cache_h;
    int hover;

    GtkWidget *menu, *mi_open, *mi_term, *mi_copy, *mi_trash, *mi_delete;
    kd_node_id menu_node;
    unsigned menu_gen;

    job *job;
    GThread *thread;
    guint poll;
};

/* ---- names and numbers ------------------------------------------------------------ */

/* What to call a node on screen (UTF-8, whatever bytes the name has). The
 * caller g_free()s it. */
static char *display_name(const kd_tree *t, kd_node_id id)
{
    return g_filename_display_name(kd_tree_name(t, id));
}

/* The last part of a path, for messages: "Downloads". */
static char *short_name(const char *path)
{
    char *base = g_path_get_basename(path);
    char *disp = g_filename_display_name(base);
    g_free(base);
    return disp;
}

/* The path of id from the folder `from` above it: "oxygen/base/64x64". */
static char *relative_name(const kd_tree *t, kd_node_id from, kd_node_id id)
{
    GPtrArray *parts = g_ptr_array_new_with_free_func(g_free);
    for (kd_node_id a = id; a != from && a != KD_NODE_NONE; a = kd_tree_node(t, a)->parent)
        g_ptr_array_insert(parts, 0, display_name(t, a));
    g_ptr_array_add(parts, NULL);
    char *out = g_strjoinv("/", (char **)parts->pdata);
    g_ptr_array_free(parts, TRUE);
    return out;
}

static char *node_path(const kd_tree *t, kd_node_id id)
{
    char *p = kd_tree_path(t, id);
    char *g = g_strdup(p ? p : "");
    free(p);
    return g;
}

/* "12 MiB in 345 files" */
static char *size_and_files(uint64_t size, uint32_t files)
{
    char sz[32];
    kd_format_size(sz, sizeof sz, size);
    return g_strdup_printf(ngettext("%s in %u file", "%s in %u files", files), sz,
                           (unsigned)files);
}

/* ---- colors ------------------------------------------------------------------------ */

static void seg_color(const kd_tree *t, const kd_ring_seg *s, double *r, double *g, double *b)
{
    double sat, val;
    if (s->node == KD_NODE_NONE) {
        sat = 0;
        val = 0.80;
    } else if (!(kd_tree_node(t, s->node)->flags & KD_NODE_DIR)) {
        /* Files: a grey with a hint of the slice's color. */
        sat = 0.10;
        val = 0.86;
    } else {
        /* Folders: strong near the center, paler outwards. */
        sat = 0.62 - 0.08 * s->depth;
        val = 0.88 + 0.02 * s->depth;
    }
    gtk_hsv_to_rgb(s->hue, sat, val, r, g, b);
}

/* ---- drawing ----------------------------------------------------------------------- */

static void sector_path(cairo_t *cr, const kd_rings_geom *g, int depth, double a0, double a1)
{
    double r0 = kd_rings_r0(g, depth), r1 = r0 + g->ring_w;
    cairo_new_path(cr);
    cairo_arc(cr, g->cx, g->cy, r1, a0 - M_PI / 2, a1 - M_PI / 2);
    cairo_arc_negative(cr, g->cx, g->cy, r0, a1 - M_PI / 2, a0 - M_PI / 2);
    cairo_close_path(cr);
}

/* The names of the big ring-0 slices, outside the rings at the left and
 * right, each with a line to its slice (as Filelight does): inside, the
 * rings are too thin for text. */
typedef struct {
    double ax, ay; /* on the slice */
    double ex, ey; /* just outside the rings */
    double y;      /* the text's middle, after making room */
    bool right;
    int seg;
} name_tag;

static int by_y(const void *a, const void *b)
{
    double ya = ((const name_tag *)a)->ey, yb = ((const name_tag *)b)->ey;
    return ya < yb ? -1 : ya > yb;
}

/* Spreads one side's labels (sorted by y) so they don't overlap and stay
 * in [top, bottom]. */
static void spread(name_tag *l, int n, double step, double top, double bottom)
{
    for (int i = 0; i < n; i++) {
        l[i].y = l[i].ey;
        if (l[i].y < top)
            l[i].y = top;
        if (i && l[i].y < l[i - 1].y + step)
            l[i].y = l[i - 1].y + step;
    }
    for (int i = n - 1; i >= 0; i--) {
        double max = i == n - 1 ? bottom : l[i + 1].y - step;
        if (l[i].y > max)
            l[i].y = max;
    }
}

static void draw_labels(cairo_t *cr, GtkWidget *w, const kd_space *s, int width, int height)
{
    const kd_rings_geom *g = &s->geom;
    double outer = kd_rings_r0(g, s->rings.depth);
    name_tag l[MAX_LABELS];
    int n = 0;
    for (int i = 0; i < s->rings.n && n < MAX_LABELS; i++) {
        const kd_ring_seg *seg = &s->rings.segs[i];
        if (seg->depth || seg->node == KD_NODE_NONE || seg->a1 - seg->a0 < LABEL_MIN_ANGLE)
            continue;
        double phi = (seg->a0 + seg->a1) / 2 - M_PI / 2;
        double rm = kd_rings_r0(g, 0) + g->ring_w / 2;
        l[n] = (name_tag){.ax = g->cx + rm * cos(phi), .ay = g->cy + rm * sin(phi),
                       .ex = g->cx + (outer + 8) * cos(phi), .ey = g->cy + (outer + 8) * sin(phi),
                       .right = cos(phi) >= 0, .seg = i};
        n++;
    }
    if (!n)
        return;
    qsort(l, (size_t)n, sizeof *l, by_y);
    /* Right side first in the array, then the left. */
    name_tag sorted[MAX_LABELS];
    int nr = 0;
    for (int i = 0; i < n; i++)
        if (l[i].right)
            sorted[nr++] = l[i];
    int k = nr;
    for (int i = 0; i < n; i++)
        if (!l[i].right)
            sorted[k++] = l[i];

    PangoLayout *pl = gtk_widget_create_pango_layout(w, "Xg");
    int th;
    pango_layout_get_pixel_size(pl, NULL, &th);
    double step = th + 2, top = th / 2.0 + 2, bottom = height - th / 2.0 - 2;
    spread(sorted, nr, step, top, bottom);
    spread(sorted + nr, n - nr, step, top, bottom);

    GtkStyle *st = gtk_widget_get_style(w);
    double col_r = g->cx + outer + 18, col_l = g->cx - outer - 18;
    pango_layout_set_ellipsize(pl, PANGO_ELLIPSIZE_END);
    for (int i = 0; i < n; i++) {
        const name_tag *b = &sorted[i];
        double col = b->right ? col_r : col_l;
        double room = b->right ? width - col - 8 : col - 8;
        if (room < 30)
            continue;
        gdk_cairo_set_source_color(cr, &st->fg[GTK_STATE_NORMAL]);
        cairo_arc(cr, b->ax, b->ay, 2, 0, TAU);
        cairo_fill(cr);
        cairo_move_to(cr, b->ax, b->ay);
        cairo_line_to(cr, b->ex, b->ey);
        cairo_line_to(cr, col, b->y);
        cairo_set_line_width(cr, 1);
        cairo_stroke(cr);

        char *name = display_name(s->tree, s->rings.segs[b->seg].node);
        pango_layout_set_text(pl, name, -1);
        g_free(name);
        pango_layout_set_width(pl, (int)room * PANGO_SCALE);
        int tw;
        pango_layout_get_pixel_size(pl, &tw, NULL);
        cairo_move_to(cr, b->right ? col + 4 : col - 4 - tw, b->y - th / 2.0);
        pango_cairo_show_layout(cr, pl);
    }
    g_object_unref(pl);
}

static void draw_center(cairo_t *cr, GtkWidget *w, const kd_space *s)
{
    const kd_rings_geom *g = &s->geom;
    GtkStyle *st = gtk_widget_get_style(w);
    cairo_new_path(cr);
    cairo_arc(cr, g->cx, g->cy, g->r_center - 1, 0, TAU);
    gdk_cairo_set_source_color(cr, &st->base[GTK_STATE_NORMAL]);
    cairo_fill_preserve(cr);
    gdk_cairo_set_source_color(cr, &st->dark[GTK_STATE_NORMAL]);
    cairo_set_line_width(cr, 1);
    cairo_stroke(cr);

    const kd_node *n = kd_tree_node(s->tree, s->cur);
    /* The root's name is its whole path, which the path bar already shows. */
    char *base = g_path_get_basename(kd_tree_name(s->tree, s->cur));
    char *name = g_filename_display_name(base);
    g_free(base);
    char sz[32];
    kd_format_size(sz, sizeof sz, n->size);
    char *files = g_strdup_printf(ngettext("%u file", "%u files", n->files), (unsigned)n->files);
    char *m = g_markup_printf_escaped("<b>%s</b>\n%s\n%s", name, sz, files);
    g_free(files);
    g_free(name);
    PangoLayout *pl = gtk_widget_create_pango_layout(w, NULL);
    pango_layout_set_markup(pl, m, -1);
    g_free(m);
    pango_layout_set_alignment(pl, PANGO_ALIGN_CENTER);
    pango_layout_set_width(pl, (int)(g->r_center * 1.6) * PANGO_SCALE);
    pango_layout_set_ellipsize(pl, PANGO_ELLIPSIZE_MIDDLE);
    int tw, th;
    pango_layout_get_pixel_size(pl, &tw, &th);
    gdk_cairo_set_source_color(cr, &st->text[GTK_STATE_NORMAL]);
    cairo_move_to(cr, g->cx - tw / 2.0, g->cy - th / 2.0);
    pango_cairo_show_layout(cr, pl);
    g_object_unref(pl);
}

/* Everything but the highlight, into s->cache. */
static void render(kd_space *s, GtkWidget *w, int width, int height)
{
    if (s->cache)
        cairo_surface_destroy(s->cache);
    s->cache = gdk_window_create_similar_surface(gtk_widget_get_window(w), CAIRO_CONTENT_COLOR,
                                                 width, height);
    s->cache_w = width;
    s->cache_h = height;
    cairo_t *cr = cairo_create(s->cache);
    GtkStyle *st = gtk_widget_get_style(w);
    gdk_cairo_set_source_color(cr, &st->bg[GTK_STATE_NORMAL]);
    cairo_paint(cr);

    /* Room at the sides for the names, if the area is wide enough. */
    int room = width >= 3 * LABEL_ROOM ? LABEL_ROOM : 0;
    kd_rings_geom_for(&s->geom, width - 2 * room, height, s->rings.depth);
    s->geom.cx = width / 2.0;
    cairo_set_line_width(cr, 1);
    for (int i = 0; i < s->rings.n; i++) {
        const kd_ring_seg *seg = &s->rings.segs[i];
        double r, gr, b;
        seg_color(s->tree, seg, &r, &gr, &b);
        sector_path(cr, &s->geom, seg->depth, seg->a0, seg->a1);
        cairo_set_source_rgb(cr, r, gr, b);
        cairo_fill_preserve(cr);
        gdk_cairo_set_source_color(cr, &st->bg[GTK_STATE_NORMAL]);
        cairo_stroke(cr);
    }
    if (room)
        draw_labels(cr, w, s, width, height);
    draw_center(cr, w, s);
    cairo_destroy(cr);
}

static gboolean on_expose(GtkWidget *w, GdkEventExpose *ev, gpointer data)
{
    (void)ev;
    kd_space *s = data;
    if (!s->tree)
        return FALSE;
    GtkAllocation a;
    gtk_widget_get_allocation(w, &a);
    gint64 t0 = g_get_monotonic_time();
    if (s->layout_dirty) {
        kd_rings_layout(&s->rings, s->tree, s->cur, MAX_RINGS, MIN_ANGLE);
        kd_log_debug("rings: %d segments laid out in %.2f ms", s->rings.n,
                     (double)(g_get_monotonic_time() - t0) / 1000.0);
        s->layout_dirty = false;
        if (s->cache) {
            cairo_surface_destroy(s->cache);
            s->cache = NULL;
        }
    }
    if (!s->cache || s->cache_w != a.width || s->cache_h != a.height) {
        gint64 t1 = g_get_monotonic_time();
        render(s, w, a.width, a.height);
        kd_log_debug("rings: drawn in %.2f ms", (double)(g_get_monotonic_time() - t1) / 1000.0);
    }
    gint64 t2 = g_get_monotonic_time();

    cairo_t *cr = gdk_cairo_create(gtk_widget_get_window(w));
    cairo_set_source_surface(cr, s->cache, 0, 0);
    cairo_paint(cr);
    if (s->hover >= 0) {
        const kd_ring_seg *seg = &s->rings.segs[s->hover];
        sector_path(cr, &s->geom, seg->depth, seg->a0, seg->a1);
        cairo_set_source_rgba(cr, 1, 1, 1, 0.35);
        cairo_fill_preserve(cr);
        cairo_set_source_rgba(cr, 0, 0, 0, 0.6);
        cairo_set_line_width(cr, 2);
        cairo_stroke(cr);
    } else if (s->hover == KD_RINGS_CENTER && s->cur != KD_TREE_ROOT) {
        cairo_arc(cr, s->geom.cx, s->geom.cy, s->geom.r_center - 1, 0, TAU);
        cairo_set_source_rgba(cr, 0, 0, 0, 0.6);
        cairo_set_line_width(cr, 2);
        cairo_stroke(cr);
    }
    cairo_destroy(cr);
    kd_log_debug("rings: shown in %.2f ms", (double)(g_get_monotonic_time() - t2) / 1000.0);
    return TRUE;
}

static void on_style_set(GtkWidget *w, GtkStyle *prev, gpointer data)
{
    (void)prev;
    kd_space *s = data;
    if (s->cache) {
        cairo_surface_destroy(s->cache);
        s->cache = NULL;
    }
    gtk_widget_queue_draw(w);
}

/* ---- what the mouse is on ------------------------------------------------------------ */

static void show_info(kd_space *s)
{
    char *text = NULL;
    if (!s->tree) {
        text = g_strdup("");
    } else if (s->hover >= 0) {
        const kd_ring_seg *seg = &s->rings.segs[s->hover];
        uint64_t total = kd_tree_node(s->tree, s->cur)->size;
        int pct = total ? (int)lround(100.0 * (double)seg->size / (double)total) : 0;
        char sz[32];
        kd_format_size(sz, sizeof sz, seg->size);
        if (seg->node == KD_NODE_NONE) {
            /* TRANSLATORS: a merged slice: "37 small items: 12 MiB (1%)" */
            text = g_strdup_printf(ngettext("%u small item: %s (%d%%)", "%u small items: %s (%d%%)",
                                            seg->n_small),
                                   (unsigned)seg->n_small, sz, pct);
        } else {
            const kd_node *n = kd_tree_node(s->tree, seg->node);
            char *name = relative_name(s->tree, s->cur, seg->node);
            if (n->flags & KD_NODE_DIR) {
                char *sf = size_and_files(n->size, n->files);
                /* TRANSLATORS: "Downloads/: 12 GiB in 345 files (40%)" */
                text = g_strdup_printf(_("%s/: %s (%d%%)"), name, sf, pct);
                g_free(sf);
            } else {
                text = g_strdup_printf("%s: %s (%d%%)", name, sz, pct);
            }
            g_free(name);
            if (n->flags & KD_NODE_DENIED) {
                char *t = g_strdup_printf("%s · %s", text, _("could not be read in full"));
                g_free(text);
                text = t;
            }
        }
    } else {
        const kd_node *n = kd_tree_node(s->tree, s->cur);
        char *path = node_path(s->tree, s->cur);
        char *disp = g_filename_display_name(path);
        char *sf = size_and_files(n->size, n->files);
        text = g_strdup_printf("%s: %s", disp, sf);
        g_free(sf);
        g_free(disp);
        g_free(path);
        if (s->tree->denied) {
            char *t = g_strdup_printf(
                ngettext("%s · %u folder could not be read", "%s · %u folders could not be read",
                         s->tree->denied),
                text, (unsigned)s->tree->denied);
            g_free(text);
            text = t;
        }
    }
    gtk_label_set_text(GTK_LABEL(s->info), text);
    g_free(text);
}

static void set_hover(kd_space *s, int hover)
{
    if (hover == s->hover)
        return;
    s->hover = hover;
    show_info(s);
    gtk_widget_queue_draw(s->area);
}

static gboolean on_motion(GtkWidget *w, GdkEventMotion *ev, gpointer data)
{
    (void)w;
    kd_space *s = data;
    if (s->tree && s->cache && !s->layout_dirty)
        set_hover(s, kd_rings_hit(&s->rings, &s->geom, ev->x, ev->y));
    return FALSE;
}

static gboolean on_leave(GtkWidget *w, GdkEventCrossing *ev, gpointer data)
{
    (void)w;
    (void)ev;
    set_hover(data, KD_RINGS_NONE);
    return FALSE;
}

/* ---- navigation ---------------------------------------------------------------------- */

static void go_to(kd_space *s, kd_node_id id);

static void on_crumb(GtkButton *b, gpointer data)
{
    kd_space *s = data;
    go_to(s, (kd_node_id)GPOINTER_TO_UINT(g_object_get_data(G_OBJECT(b), "kd-node")));
}

static void add_crumb(kd_space *s, kd_node_id id, const char *text)
{
    GtkWidget *b = gtk_button_new_with_label(text);
    gtk_button_set_relief(GTK_BUTTON(b), GTK_RELIEF_NONE);
    gtk_button_set_focus_on_click(GTK_BUTTON(b), FALSE);
    GtkWidget *l = gtk_bin_get_child(GTK_BIN(b));
    gtk_label_set_ellipsize(GTK_LABEL(l), PANGO_ELLIPSIZE_MIDDLE);
    gtk_label_set_max_width_chars(GTK_LABEL(l), 30);
    gtk_widget_set_sensitive(b, id != s->cur);
    g_object_set_data(G_OBJECT(b), "kd-node", GUINT_TO_POINTER(id));
    g_signal_connect(b, "clicked", G_CALLBACK(on_crumb), s);
    gtk_box_pack_start(GTK_BOX(s->crumbs), b, FALSE, FALSE, 0);
}

static void rebuild_crumbs(kd_space *s)
{
    GList *kids = gtk_container_get_children(GTK_CONTAINER(s->crumbs));
    for (GList *l = kids; l; l = l->next)
        gtk_widget_destroy(l->data);
    g_list_free(kids);
    if (!s->tree)
        return;

    kd_node_id chain[MAX_CRUMBS];
    int n = 0;
    bool cut = false;
    for (kd_node_id a = s->cur; a != KD_TREE_ROOT; a = kd_tree_node(s->tree, a)->parent) {
        if (n == MAX_CRUMBS) {
            cut = true;
            memmove(chain, chain + 1, (MAX_CRUMBS - 1) * sizeof *chain);
            n--;
        }
        chain[n++] = a;
    }
    char *root = display_name(s->tree, KD_TREE_ROOT);
    add_crumb(s, KD_TREE_ROOT, root);
    g_free(root);
    if (cut)
        gtk_box_pack_start(GTK_BOX(s->crumbs), gtk_label_new("› …"), FALSE, FALSE, 0);
    for (int i = n - 1; i >= 0; i--) {
        gtk_box_pack_start(GTK_BOX(s->crumbs), gtk_label_new("›"), FALSE, FALSE, 0);
        char *name = display_name(s->tree, chain[i]);
        add_crumb(s, chain[i], name);
        g_free(name);
    }
    gtk_widget_show_all(s->crumbs);
}

static void go_to(kd_space *s, kd_node_id id)
{
    s->cur = id;
    s->layout_dirty = true;
    s->hover = KD_RINGS_NONE;
    rebuild_crumbs(s);
    gtk_widget_set_sensitive(s->up, s->tree && id != KD_TREE_ROOT);
    gtk_widget_set_sensitive(s->refresh, s->tree != NULL);
    show_info(s);
    gtk_widget_queue_draw(s->area);
}

static void go_up(kd_space *s)
{
    if (s->tree && s->cur != KD_TREE_ROOT)
        go_to(s, kd_tree_node(s->tree, s->cur)->parent);
}

static void on_up(GtkButton *b, gpointer data)
{
    (void)b;
    go_up(data);
}

/* ---- jobs ---------------------------------------------------------------------------- */

static gpointer job_run(gpointer data)
{
    job *j = data;
    if (j->kind == JOB_SCAN) {
        j->result = kd_tree_scan(j->path, &j->progress);
        j->err = j->result ? 0 : errno;
    } else if (j->kind == JOB_TRASH) {
        GFile *f = g_file_new_for_path(j->path);
        GError *e = NULL;
        if (!g_file_trash(f, NULL, &e)) {
            j->error = g_strdup(e->message);
            g_error_free(e);
        }
        g_object_unref(f);
    } else {
        j->err = kd_remove_tree(j->path) < 0 ? errno : 0;
    }
    atomic_store(&j->done, true);
    return NULL;
}

static void job_free(job *j)
{
    if (!j)
        return;
    kd_tree_free(j->result);
    g_free(j->path);
    g_free(j->error);
    g_free(j);
}

static void show_view(kd_space *s, int view)
{
    gtk_notebook_set_current_page(GTK_NOTEBOOK(s->views), view);
}

static void start_job(kd_space *s, job_kind kind, const char *path, kd_node_id target);

static void finish_job(kd_space *s, job *j)
{
    char *disp = short_name(j->path);
    if (j->kind == JOB_SCAN) {
        if (j->result && j->target == KD_NODE_NONE) {
            kd_tree_free(s->tree);
            s->tree = j->result;
            s->gen++;
            j->result = NULL;
            go_to(s, KD_TREE_ROOT);
        } else if (j->result) {
            if (!kd_tree_graft(s->tree, j->target, j->result))
                kd_app_error(s->app, _("Out of memory"), NULL);
            j->result = NULL;
            go_to(s, s->cur);
        } else if (j->err != ECANCELED) {
            char *title = g_strdup_printf(_("Could not look into “%s”"), disp);
            kd_app_error(s->app, title, g_strerror(j->err));
            g_free(title);
        }
        show_view(s, s->tree ? VIEW_CHART : VIEW_HINT);
        if (s->tree && s->tree->denied)
            kd_app_status(s->app,
                          ngettext("%u folder could not be read (no permission): the sizes "
                                   "above it are short",
                                   "%u folders could not be read (no permission): the sizes "
                                   "above them are short",
                                   s->tree->denied),
                          (unsigned)s->tree->denied);
        else
            kd_app_status(s->app, "%s", "");
    } else {
        const char *err = j->error ? j->error : j->err ? g_strerror(j->err) : NULL;
        kd_node_id parent = kd_tree_node(s->tree, j->target)->parent;
        if (!err) {
            /* Showing what went (or something inside it)? Show its folder. */
            bool inside = false;
            for (kd_node_id a = s->cur; a != KD_NODE_NONE; a = kd_tree_node(s->tree, a)->parent)
                inside |= a == j->target;
            kd_tree_remove(s->tree, j->target);
            go_to(s, inside ? parent : s->cur);
            kd_app_status(s->app, j->kind == JOB_TRASH ? _("“%s” moved to the trash")
                                                       : _("“%s” deleted"),
                          disp);
        } else {
            char *title = g_strdup_printf(j->kind == JOB_TRASH
                                              ? _("Could not move “%s” to the trash")
                                              : _("Could not delete “%s”"),
                                          disp);
            kd_app_error(s->app, title, err);
            g_free(title);
            kd_app_status(s->app, "%s", "");
            /* Part of it may be gone: look at the folder again. */
            if (j->kind == JOB_DELETE) {
                char *p = node_path(s->tree, parent);
                start_job(s, JOB_SCAN, p, parent);
                g_free(p);
            }
        }
    }
    g_free(disp);
}

static gboolean poll_job(gpointer data)
{
    kd_space *s = data;
    job *j = s->job;
    if (!atomic_load(&j->done)) {
        if (j->kind == JOB_SCAN) {
            char sz[32];
            kd_format_size(sz, sizeof sz, atomic_load(&j->progress.bytes));
            unsigned long long files = atomic_load(&j->progress.files);
            char *t = g_strdup_printf(ngettext("%llu file, %s", "%llu files, %s", files), files, sz);
            gtk_label_set_text(GTK_LABEL(s->progress_count), t);
            g_free(t);
            gtk_progress_bar_pulse(GTK_PROGRESS_BAR(s->progress_bar));
        }
        return G_SOURCE_CONTINUE;
    }
    g_thread_join(s->thread);
    s->thread = NULL;
    s->job = NULL;
    s->poll = 0;
    finish_job(s, j);
    job_free(j);
    return G_SOURCE_REMOVE;
}

static void start_job(kd_space *s, job_kind kind, const char *path, kd_node_id target)
{
    job *j = g_new0(job, 1);
    j->kind = kind;
    j->path = g_strdup(path);
    j->target = target;
    s->job = j;
    if (kind == JOB_SCAN) {
        char *disp = g_filename_display_name(path);
        char *m = g_markup_printf_escaped(_("<b>Looking into %s…</b>"), disp);
        gtk_label_set_markup(GTK_LABEL(s->progress_title), m);
        g_free(m);
        g_free(disp);
        gtk_label_set_text(GTK_LABEL(s->progress_count), "");
        show_view(s, VIEW_PROGRESS);
        set_hover(s, KD_RINGS_NONE);
    } else {
        char *disp = short_name(path);
        kd_app_status(s->app, kind == JOB_TRASH ? _("Moving “%s” to the trash…") : _("Deleting “%s”…"),
                      disp);
        g_free(disp);
    }
    s->thread = g_thread_new("kd-space", job_run, j);
    s->poll = g_timeout_add(100, poll_job, s);
}

/* Cancels a scan and waits for it. false if a trash or delete is running:
 * those cannot be stopped halfway. */
static bool stop_job(kd_space *s)
{
    if (!s->job)
        return true;
    if (s->job->kind != JOB_SCAN)
        return false;
    atomic_store(&s->job->progress.cancel, true);
    g_thread_join(s->thread);
    g_source_remove(s->poll);
    job_free(s->job);
    s->job = NULL;
    s->thread = NULL;
    s->poll = 0;
    return true;
}

static void busy(kd_space *s)
{
    kd_app_status(s->app, "%s", _("Wait for the file operation to finish."));
}

void kd_space_open(kd_space *s, const char *path)
{
    if (!stop_job(s)) {
        busy(s);
        return;
    }
    start_job(s, JOB_SCAN, path, KD_NODE_NONE);
}

static void on_cancel(GtkButton *b, gpointer data)
{
    (void)b;
    kd_space *s = data;
    if (s->job && s->job->kind == JOB_SCAN)
        atomic_store(&s->job->progress.cancel, true);
}

static void on_refresh(GtkButton *b, gpointer data)
{
    (void)b;
    kd_space *s = data;
    if (!s->tree)
        return;
    if (!stop_job(s)) {
        busy(s);
        return;
    }
    char *p = node_path(s->tree, s->cur);
    start_job(s, JOB_SCAN, p, s->cur);
    g_free(p);
}

/* ---- context menu ---------------------------------------------------------------------- */

static void open_uri_of(kd_space *s, const char *path)
{
    GError *err = NULL;
    char *uri = g_filename_to_uri(path, NULL, &err);
    if (!uri || !g_app_info_launch_default_for_uri(uri, NULL, &err))
        kd_app_error(s->app, _("Could not open the folder"), err->message);
    if (err)
        g_error_free(err);
    g_free(uri);
}

/* For the answer, which may come after the tab is gone. */
typedef struct {
    char *folder;
} show_items;

static void on_show_items(GObject *src, GAsyncResult *res, gpointer data)
{
    show_items *si = data;
    GError *err = NULL;
    GVariant *v = g_dbus_connection_call_finish(G_DBUS_CONNECTION(src), res, &err);
    if (v) {
        g_variant_unref(v);
    } else {
        /* No file manager that selects files: open the folder. */
        g_error_free(err);
        char *uri = g_filename_to_uri(si->folder, NULL, NULL);
        if (uri)
            g_app_info_launch_default_for_uri(uri, NULL, NULL);
        g_free(uri);
    }
    g_free(si->folder);
    g_free(si);
}

/* A file: its folder, with it selected (org.freedesktop.FileManager1, which
 * Dolphin, Nautilus and others answer). */
static void show_file(kd_space *s, const char *path)
{
    GDBusConnection *bus = g_bus_get_sync(G_BUS_TYPE_SESSION, NULL, NULL);
    char *folder = g_path_get_dirname(path);
    char *uri = g_filename_to_uri(path, NULL, NULL);
    if (!bus || !uri) {
        open_uri_of(s, folder);
    } else {
        show_items *si = g_new0(show_items, 1);
        si->folder = g_strdup(folder);
        const char *uris[] = {uri, NULL};
        g_dbus_connection_call(bus, "org.freedesktop.FileManager1", "/org/freedesktop/FileManager1",
                               "org.freedesktop.FileManager1", "ShowItems",
                               g_variant_new("(^ass)", uris, ""), NULL, G_DBUS_CALL_FLAGS_NONE,
                               3000, NULL, on_show_items, si);
    }
    if (bus)
        g_object_unref(bus);
    g_free(uri);
    g_free(folder);
}

/* The menu stays up while a scan may end and replace the tree. */
static bool menu_stale(const kd_space *s)
{
    return !s->tree || s->menu_gen != s->gen;
}

static void on_menu_open(GtkMenuItem *mi, gpointer data)
{
    (void)mi;
    kd_space *s = data;
    if (menu_stale(s))
        return;
    char *p = node_path(s->tree, s->menu_node);
    if (kd_tree_node(s->tree, s->menu_node)->flags & KD_NODE_DIR)
        open_uri_of(s, p);
    else
        show_file(s, p);
    g_free(p);
}

static void on_menu_term(GtkMenuItem *mi, gpointer data)
{
    (void)mi;
    kd_space *s = data;
    if (menu_stale(s))
        return;
    kd_node_id id = s->menu_node;
    if (!(kd_tree_node(s->tree, id)->flags & KD_NODE_DIR))
        id = kd_tree_node(s->tree, id)->parent;
    char *dir = node_path(s->tree, id);
    static const char *const terms[] = {"x-terminal-emulator", "konsole", "gnome-terminal",
                                        "xfce4-terminal", "mate-terminal", "lxterminal", "xterm"};
    char *prog = NULL;
    for (size_t i = 0; i < G_N_ELEMENTS(terms) && !prog; i++)
        prog = g_find_program_in_path(terms[i]);
    GError *err = NULL;
    if (!prog) {
        kd_app_error(s->app, _("No terminal program found"), NULL);
    } else {
        char *argv[] = {prog, NULL};
        if (!g_spawn_async(dir, argv, NULL, G_SPAWN_DEFAULT, NULL, NULL, NULL, &err)) {
            kd_app_error(s->app, _("Could not open a terminal"), err->message);
            g_error_free(err);
        }
    }
    g_free(prog);
    g_free(dir);
}

static void on_menu_copy(GtkMenuItem *mi, gpointer data)
{
    (void)mi;
    kd_space *s = data;
    if (menu_stale(s))
        return;
    char *p = node_path(s->tree, s->menu_node);
    char *disp = g_filename_display_name(p);
    gtk_clipboard_set_text(gtk_clipboard_get(GDK_SELECTION_CLIPBOARD), disp, -1);
    gtk_clipboard_set_text(gtk_clipboard_get(GDK_SELECTION_PRIMARY), disp, -1);
    g_free(disp);
    g_free(p);
}

static void on_menu_trash(GtkMenuItem *mi, gpointer data)
{
    (void)mi;
    kd_space *s = data;
    if (menu_stale(s))
        return;
    if (s->job) {
        busy(s);
        return;
    }
    char *p = node_path(s->tree, s->menu_node);
    start_job(s, JOB_TRASH, p, s->menu_node);
    g_free(p);
}

static void on_menu_delete(GtkMenuItem *mi, gpointer data)
{
    (void)mi;
    kd_space *s = data;
    if (menu_stale(s))
        return;
    if (s->job) {
        busy(s);
        return;
    }
    kd_node_id id = s->menu_node;
    const kd_node *n = kd_tree_node(s->tree, id);
    char *name = display_name(s->tree, id);
    GtkWidget *d = gtk_message_dialog_new(GTK_WINDOW(s->app->window), GTK_DIALOG_MODAL,
                                          GTK_MESSAGE_WARNING, GTK_BUTTONS_NONE,
                                          _("Delete “%s” for good?"), name);
    g_free(name);
    char sz[32];
    kd_format_size(sz, sizeof sz, n->size);
    if (n->flags & KD_NODE_DIR) {
        char *sf = size_and_files(n->size, n->files);
        gtk_message_dialog_format_secondary_text(
            GTK_MESSAGE_DIALOG(d), _("The folder and all inside it (%s) will be gone, with no "
                                     "way back. To be able to undo, move it to the trash "
                                     "instead."),
            sf);
        g_free(sf);
    } else {
        gtk_message_dialog_format_secondary_text(
            GTK_MESSAGE_DIALOG(d), _("The file (%s) will be gone, with no way back. To be able "
                                     "to undo, move it to the trash instead."),
            sz);
    }
    gtk_dialog_add_buttons(GTK_DIALOG(d), GTK_STOCK_CANCEL, GTK_RESPONSE_CANCEL, GTK_STOCK_DELETE,
                           GTK_RESPONSE_ACCEPT, NULL);
    gtk_dialog_set_default_response(GTK_DIALOG(d), GTK_RESPONSE_CANCEL);
    int r = gtk_dialog_run(GTK_DIALOG(d));
    gtk_widget_destroy(d);
    /* The tree may have changed while the dialog was up (a scan ended). */
    if (r != GTK_RESPONSE_ACCEPT || s->job || menu_stale(s))
        return;
    char *p = node_path(s->tree, id);
    start_job(s, JOB_DELETE, p, id);
    g_free(p);
}

static GtkWidget *menu_add(kd_space *s, const char *label, const char *icon, GCallback cb)
{
    GtkWidget *mi = gtk_image_menu_item_new_with_mnemonic(label);
    if (icon)
        gtk_image_menu_item_set_image(GTK_IMAGE_MENU_ITEM(mi),
                                      gtk_image_new_from_icon_name(icon, GTK_ICON_SIZE_MENU));
    g_signal_connect(mi, "activate", cb, s);
    gtk_menu_shell_append(GTK_MENU_SHELL(s->menu), mi);
    return mi;
}

static void build_menu(kd_space *s)
{
    s->menu = gtk_menu_new();
    s->mi_open = menu_add(s, _("_Open in the file manager"), "folder-open", G_CALLBACK(on_menu_open));
    s->mi_term = menu_add(s, _("Open a _terminal here"), "utilities-terminal",
                          G_CALLBACK(on_menu_term));
    s->mi_copy = menu_add(s, _("_Copy the path"), "edit-copy", G_CALLBACK(on_menu_copy));
    gtk_menu_shell_append(GTK_MENU_SHELL(s->menu), gtk_separator_menu_item_new());
    s->mi_trash = menu_add(s, _("Move to the t_rash"), "user-trash", G_CALLBACK(on_menu_trash));
    s->mi_delete = menu_add(s, _("_Delete…"), "edit-delete", G_CALLBACK(on_menu_delete));
    gtk_widget_show_all(s->menu);
}

static void popup_menu(kd_space *s, kd_node_id id, GdkEventButton *ev)
{
    s->menu_node = id;
    s->menu_gen = s->gen;
    const kd_node *n = kd_tree_node(s->tree, id);
    /* Never the scan root, nor another filesystem mounted inside. */
    bool removable = id != KD_TREE_ROOT && !(n->flags & KD_NODE_MOUNT) && !s->job;
    gtk_widget_set_sensitive(s->mi_trash, removable);
    gtk_widget_set_sensitive(s->mi_delete, removable);
    gtk_menu_popup(GTK_MENU(s->menu), NULL, NULL, NULL, NULL, ev->button, ev->time);
}

/* ---- clicks ---------------------------------------------------------------------------- */

static gboolean on_press(GtkWidget *w, GdkEventButton *ev, gpointer data)
{
    kd_space *s = data;
    gtk_widget_grab_focus(w);
    if (!s->tree || !s->cache || s->layout_dirty || ev->type != GDK_BUTTON_PRESS)
        return FALSE;
    int hit = kd_rings_hit(&s->rings, &s->geom, ev->x, ev->y);
    kd_node_id node = hit >= 0 ? s->rings.segs[hit].node : hit == KD_RINGS_CENTER ? s->cur
                                                                                   : KD_NODE_NONE;
    if (ev->button == 1) {
        if (hit == KD_RINGS_CENTER)
            go_up(s);
        else if (node != KD_NODE_NONE && (kd_tree_node(s->tree, node)->flags & KD_NODE_DIR) &&
                 kd_tree_node(s->tree, node)->count)
            go_to(s, node);
        return TRUE;
    }
    if (ev->button == 3 && node != KD_NODE_NONE) {
        popup_menu(s, node, ev);
        return TRUE;
    }
    /* The "back" button of the mouse. */
    if (ev->button == 8) {
        go_up(s);
        return TRUE;
    }
    return FALSE;
}

static gboolean on_key(GtkWidget *w, GdkEventKey *ev, gpointer data)
{
    (void)w;
    kd_space *s = data;
    guint mods = ev->state & gtk_accelerator_get_default_mod_mask();
    if ((ev->keyval == GDK_BackSpace && !mods) ||
        (ev->keyval == GDK_Up && mods == GDK_MOD1_MASK)) {
        go_up(s);
        return TRUE;
    }
    return FALSE;
}

/* ---- places to scan ----------------------------------------------------------------- */

static void on_place(GtkButton *b, gpointer data)
{
    kd_space_open(data, g_object_get_data(G_OBJECT(b), "kd-path"));
}

static void on_choose(GtkButton *b, gpointer data)
{
    (void)b;
    kd_space *s = data;
    GtkWidget *d = gtk_file_chooser_dialog_new(
        _("Choose a folder to look into"), GTK_WINDOW(s->app->window),
        GTK_FILE_CHOOSER_ACTION_SELECT_FOLDER, GTK_STOCK_CANCEL, GTK_RESPONSE_CANCEL,
        GTK_STOCK_OPEN, GTK_RESPONSE_ACCEPT, NULL);
    gtk_file_chooser_set_local_only(GTK_FILE_CHOOSER(d), TRUE);
    if (gtk_dialog_run(GTK_DIALOG(d)) == GTK_RESPONSE_ACCEPT) {
        char *path = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(d));
        if (path)
            kd_space_open(s, path);
        g_free(path);
    }
    gtk_widget_destroy(d);
}

static GtkWidget *place_button(kd_space *s, const char *label, const char *const *icons,
                               const char *path, const char *tip)
{
    /* Built by hand: a GtkButton's own label can't be kept short. */
    GtkWidget *b = gtk_button_new();
    GtkWidget *box = gtk_hbox_new(FALSE, 4);
    gtk_box_pack_start(GTK_BOX(box), kd_icon_image(icons, GTK_ICON_SIZE_BUTTON), FALSE, FALSE, 0);
    GtkWidget *l = gtk_label_new(label);
    gtk_label_set_ellipsize(GTK_LABEL(l), PANGO_ELLIPSIZE_END);
    gtk_label_set_max_width_chars(GTK_LABEL(l), 16);
    gtk_box_pack_start(GTK_BOX(box), l, FALSE, FALSE, 0);
    gtk_container_add(GTK_CONTAINER(b), box);
    gtk_button_set_focus_on_click(GTK_BUTTON(b), FALSE);
    if (tip)
        gtk_widget_set_tooltip_text(b, tip);
    if (path) {
        g_object_set_data_full(G_OBJECT(b), "kd-path", g_strdup(path), g_free);
        g_signal_connect(b, "clicked", G_CALLBACK(on_place), s);
    }
    gtk_box_pack_start(GTK_BOX(s->roots), b, FALSE, FALSE, 0);
    return b;
}

static void add_mount(kd_space *s, GPtrArray *seen, const char *dev, const kd_fs *fs)
{
    if (!*fs->mountpoint || !kd_fs_mountable(fs))
        return;
    for (guint i = 0; i < seen->len; i++)
        if (strcmp(g_ptr_array_index(seen, i), fs->mountpoint) == 0)
            return;
    g_ptr_array_add(seen, (gpointer)fs->mountpoint);
    static const char *const icons[] = {"drive-harddisk", NULL};
    char *base = *fs->label ? g_strdup(fs->label) : g_path_get_basename(fs->mountpoint);
    GString *tip = g_string_new(NULL);
    g_string_printf(tip, "%s\n/dev/%s · %s", fs->mountpoint, dev, fs->type);
    if (fs->has_usage) {
        char used[32], total[32];
        kd_format_size(used, sizeof used, fs->fs_used);
        kd_format_size(total, sizeof total, fs->fs_size);
        g_string_append(tip, " · ");
        g_string_append_printf(tip, _("%s of %s used"), used, total);
    }
    place_button(s, base, icons, fs->mountpoint, tip->str);
    g_string_free(tip, TRUE);
    g_free(base);
}

void kd_space_update(kd_space *s)
{
    GList *kids = gtk_container_get_children(GTK_CONTAINER(s->roots));
    for (GList *l = kids; l; l = l->next)
        gtk_widget_destroy(l->data);
    g_list_free(kids);

    static const char *const home_icons[] = {"user-home", "folder", NULL};
    place_button(s, _("Home folder"), home_icons, g_get_home_dir(), g_get_home_dir());
    GPtrArray *seen = g_ptr_array_new();
    const kd_disks *d = s->app->disks;
    for (int i = 0; d && i < d->n; i++) {
        const kd_disk *k = &d->disks[i];
        if (k->hidden && !s->app->show_hidden)
            continue;
        add_mount(s, seen, k->name, &k->fs);
        for (int h = 0; h < k->n_holders; h++)
            add_mount(s, seen, k->holders[h].name, &k->holders[h].fs);
        for (int p = 0; p < k->n_parts; p++) {
            const kd_part *pt = &k->parts[p];
            add_mount(s, seen, pt->name, &pt->fs);
            for (int h = 0; h < pt->n_holders; h++)
                add_mount(s, seen, pt->holders[h].name, &pt->holders[h].fs);
        }
    }
    g_ptr_array_free(seen, TRUE);
    static const char *const choose_icons[] = {"folder-open", NULL};
    GtkWidget *b = place_button(s, _("Choose a folder…"), choose_icons, NULL, NULL);
    g_signal_connect(b, "clicked", G_CALLBACK(on_choose), s);
    gtk_widget_show_all(s->roots);
}

/* ---- the tab --------------------------------------------------------------------------- */

static GtkWidget *hint_view(void)
{
    GtkWidget *box = gtk_vbox_new(FALSE, 12);
    static const char *const icons[] = {"disk-quota", "drive-harddisk", NULL};
    GtkWidget *img = kd_icon_image(icons, GTK_ICON_SIZE_DIALOG);
    gtk_misc_set_alignment(GTK_MISC(img), 0.5f, 1.0f);
    gtk_box_pack_start(GTK_BOX(box), img, TRUE, TRUE, 0);
    GtkWidget *l = gtk_label_new(_("Choose what to look into: a disk, your home folder or any "
                                   "folder. Each slice of the rings is a folder or file, as "
                                   "big as the room it takes."));
    gtk_label_set_line_wrap(GTK_LABEL(l), TRUE);
    gtk_label_set_justify(GTK_LABEL(l), GTK_JUSTIFY_CENTER);
    gtk_misc_set_alignment(GTK_MISC(l), 0.5f, 0.0f);
    gtk_box_pack_start(GTK_BOX(box), l, TRUE, TRUE, 0);
    return box;
}

static GtkWidget *progress_view(kd_space *s)
{
    GtkWidget *box = gtk_vbox_new(FALSE, 8);
    GtkWidget *top = gtk_alignment_new(0.5f, 0.5f, 0, 0);
    gtk_box_pack_start(GTK_BOX(box), top, TRUE, TRUE, 0);
    GtkWidget *inner = gtk_vbox_new(FALSE, 8);
    gtk_container_add(GTK_CONTAINER(top), inner);
    s->progress_title = gtk_label_new(NULL);
    gtk_label_set_ellipsize(GTK_LABEL(s->progress_title), PANGO_ELLIPSIZE_MIDDLE);
    gtk_label_set_max_width_chars(GTK_LABEL(s->progress_title), 60);
    gtk_box_pack_start(GTK_BOX(inner), s->progress_title, FALSE, FALSE, 0);
    s->progress_bar = gtk_progress_bar_new();
    gtk_progress_bar_set_pulse_step(GTK_PROGRESS_BAR(s->progress_bar), 0.05);
    gtk_widget_set_size_request(s->progress_bar, 320, -1);
    gtk_box_pack_start(GTK_BOX(inner), s->progress_bar, FALSE, FALSE, 0);
    s->progress_count = gtk_label_new(NULL);
    gtk_box_pack_start(GTK_BOX(inner), s->progress_count, FALSE, FALSE, 0);
    GtkWidget *cancel = gtk_button_new_from_stock(GTK_STOCK_CANCEL);
    g_signal_connect(cancel, "clicked", G_CALLBACK(on_cancel), s);
    GtkWidget *ca = gtk_alignment_new(0.5f, 0, 0, 0);
    gtk_container_add(GTK_CONTAINER(ca), cancel);
    gtk_box_pack_start(GTK_BOX(inner), ca, FALSE, FALSE, 4);
    return box;
}

static GtkWidget *tool_button(const char *const *icons, const char *tip, GCallback cb,
                              kd_space *s)
{
    GtkWidget *b = gtk_button_new();
    gtk_button_set_image(GTK_BUTTON(b), kd_icon_image(icons, GTK_ICON_SIZE_BUTTON));
    gtk_button_set_focus_on_click(GTK_BUTTON(b), FALSE);
    gtk_widget_set_tooltip_text(b, tip);
    gtk_widget_set_sensitive(b, FALSE);
    g_signal_connect(b, "clicked", cb, s);
    return b;
}

kd_space *kd_space_new(kd_app *app)
{
    kd_space *s = g_new0(kd_space, 1);
    s->app = app;
    s->hover = KD_RINGS_NONE;
    s->page = gtk_vbox_new(FALSE, 6);
    gtk_container_set_border_width(GTK_CONTAINER(s->page), 8);

    GtkWidget *bar = gtk_hbox_new(FALSE, 6);
    gtk_box_pack_start(GTK_BOX(s->page), bar, FALSE, FALSE, 0);
    s->roots = gtk_hbox_new(FALSE, 4);
    gtk_box_pack_start(GTK_BOX(bar), s->roots, TRUE, TRUE, 0);
    static const char *const up_icons[] = {"go-up", NULL};
    static const char *const refresh_icons[] = {"view-refresh", NULL};
    s->refresh = tool_button(refresh_icons, _("Look into this folder again"),
                             G_CALLBACK(on_refresh), s);
    gtk_box_pack_end(GTK_BOX(bar), s->refresh, FALSE, FALSE, 0);
    s->up = tool_button(up_icons, _("Up one folder (Backspace)"), G_CALLBACK(on_up), s);
    gtk_box_pack_end(GTK_BOX(bar), s->up, FALSE, FALSE, 0);

    s->crumbs = gtk_hbox_new(FALSE, 0);
    gtk_box_pack_start(GTK_BOX(s->page), s->crumbs, FALSE, FALSE, 0);

    s->views = gtk_notebook_new();
    gtk_notebook_set_show_tabs(GTK_NOTEBOOK(s->views), FALSE);
    gtk_notebook_set_show_border(GTK_NOTEBOOK(s->views), FALSE);
    gtk_notebook_append_page(GTK_NOTEBOOK(s->views), hint_view(), NULL);
    gtk_notebook_append_page(GTK_NOTEBOOK(s->views), progress_view(s), NULL);
    s->area = gtk_drawing_area_new();
    gtk_widget_set_can_focus(s->area, TRUE);
    gtk_widget_add_events(s->area, GDK_POINTER_MOTION_MASK | GDK_BUTTON_PRESS_MASK |
                                       GDK_LEAVE_NOTIFY_MASK | GDK_KEY_PRESS_MASK);
    g_signal_connect(s->area, "expose-event", G_CALLBACK(on_expose), s);
    g_signal_connect(s->area, "style-set", G_CALLBACK(on_style_set), s);
    g_signal_connect(s->area, "motion-notify-event", G_CALLBACK(on_motion), s);
    g_signal_connect(s->area, "leave-notify-event", G_CALLBACK(on_leave), s);
    g_signal_connect(s->area, "button-press-event", G_CALLBACK(on_press), s);
    g_signal_connect(s->area, "key-press-event", G_CALLBACK(on_key), s);
    gtk_notebook_append_page(GTK_NOTEBOOK(s->views), s->area, NULL);
    gtk_box_pack_start(GTK_BOX(s->page), s->views, TRUE, TRUE, 0);

    s->info = gtk_label_new(NULL);
    gtk_misc_set_alignment(GTK_MISC(s->info), 0, 0.5f);
    gtk_label_set_ellipsize(GTK_LABEL(s->info), PANGO_ELLIPSIZE_MIDDLE);
    gtk_box_pack_start(GTK_BOX(s->page), s->info, FALSE, FALSE, 0);

    build_menu(s);
    kd_space_update(s);
    return s;
}

void kd_space_free(kd_space *s)
{
    if (!s)
        return;
    if (s->job) {
        /* A trash or delete is let finish: stopping halfway is worse. */
        atomic_store(&s->job->progress.cancel, true);
        g_thread_join(s->thread);
        g_source_remove(s->poll);
        job_free(s->job);
    }
    gtk_widget_destroy(s->menu);
    if (s->cache)
        cairo_surface_destroy(s->cache);
    kd_rings_clear(&s->rings);
    kd_tree_free(s->tree);
    g_free(s);
}

GtkWidget *kd_space_widget(kd_space *s)
{
    return s->page;
}
