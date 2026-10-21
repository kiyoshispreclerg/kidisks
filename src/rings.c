/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "rings.h"

#include <math.h>
#include <stdlib.h>

#define TAU (2 * M_PI)

/* Gaps between rings and around the edge, in pixels. */
#define MARGIN 8.0

static int push(kd_rings *r, const kd_ring_seg *s)
{
    if (r->n == r->cap) {
        int cap = r->cap ? r->cap * 2 : 256;
        kd_ring_seg *p = realloc(r->segs, (size_t)cap * sizeof *p);
        if (!p)
            return -1;
        r->segs = p;
        r->cap = cap;
    }
    r->segs[r->n] = *s;
    if (s->depth + 1 > r->depth)
        r->depth = s->depth + 1;
    return r->n++;
}

/* The children of the folder behind segment parent (or the center when
 * parent is -1), placed from angle a0 in ring depth. scale: radians per
 * byte. */
static void place_children(kd_rings *r, const kd_tree *t, kd_node_id id, int parent, int depth,
                           double a0, double scale, double hue, double min_angle)
{
    const kd_node *n = kd_tree_node(t, id);
    double a = a0;
    for (uint32_t i = 0; i < n->count; i++) {
        kd_node_id c = kd_tree_child(t, id, i);
        uint64_t size = kd_tree_node(t, c)->size;
        double sweep = (double)size * scale;
        if (sweep < min_angle) {
            /* Children are biggest first: all from here on are small. */
            kd_ring_seg s = {.node = KD_NODE_NONE, .depth = depth, .a0 = a, .parent = parent};
            for (uint32_t j = i; j < n->count; j++) {
                uint64_t sz = kd_tree_node(t, kd_tree_child(t, id, j))->size;
                if (sz) {
                    s.size += sz;
                    s.n_small++;
                }
            }
            s.a1 = a + (double)s.size * scale;
            /* Still too thin to see or point at: leave it out. */
            if (s.n_small && s.a1 - s.a0 >= min_angle / 4) {
                s.hue = depth ? hue : (s.a0 + s.a1) / 2 / TAU;
                push(r, &s);
            }
            break;
        }
        kd_ring_seg s = {.node = c, .depth = depth, .a0 = a, .a1 = a + sweep, .parent = parent,
                         .size = size};
        s.hue = depth ? hue : (s.a0 + s.a1) / 2 / TAU;
        push(r, &s);
        a += sweep;
    }
}

void kd_rings_layout(kd_rings *r, const kd_tree *t, kd_node_id center, int max_depth,
                     double min_angle)
{
    r->n = 0;
    r->depth = 0;
    r->center = center;
    uint64_t total = kd_tree_node(t, center)->size;
    if (!total || max_depth < 1)
        return;
    double scale = TAU / (double)total;
    place_children(r, t, center, -1, 0, 0, scale, 0, min_angle);
    /* Ring by ring: the segments of ring d are [from, to). */
    int from = 0;
    for (int d = 1; d < max_depth; d++) {
        int to = r->n;
        for (int i = from; i < to; i++) {
            kd_ring_seg s = r->segs[i];
            if (s.node == KD_NODE_NONE)
                continue;
            const kd_node *n = kd_tree_node(t, s.node);
            if ((n->flags & KD_NODE_DIR) && n->count)
                place_children(r, t, s.node, i, d, s.a0, scale, s.hue, min_angle);
        }
        if (r->n == to)
            break;
        from = to;
    }
}

void kd_rings_clear(kd_rings *r)
{
    free(r->segs);
    r->segs = NULL;
    r->n = r->cap = r->depth = 0;
}

void kd_rings_geom_for(kd_rings_geom *g, double w, double h, int depth)
{
    g->cx = w / 2;
    g->cy = h / 2;
    double radius = fmin(w, h) / 2 - MARGIN;
    if (radius < 10)
        radius = 10;
    if (depth < 1)
        depth = 1;
    /* The center gets a ring and a half's room, but never less than a
     * fifth of the whole, so its text fits. */
    g->r_center = fmax(radius / (depth + 1.5) * 1.5, radius / 5);
    g->ring_w = (radius - g->r_center) / depth;
}

int kd_rings_hit(const kd_rings *r, const kd_rings_geom *g, double x, double y)
{
    double dx = x - g->cx, dy = y - g->cy;
    double dist = sqrt(dx * dx + dy * dy);
    if (dist < g->r_center)
        return KD_RINGS_CENTER;
    if (g->ring_w <= 0)
        return KD_RINGS_NONE;
    int depth = (int)((dist - g->r_center) / g->ring_w);
    if (depth >= r->depth)
        return KD_RINGS_NONE;
    /* 0 at 12 o'clock, clockwise (y grows downwards). */
    double a = atan2(dx, -dy);
    if (a < 0)
        a += TAU;
    for (int i = 0; i < r->n; i++) {
        const kd_ring_seg *s = &r->segs[i];
        if (s->depth == depth && a >= s->a0 && a < s->a1)
            return i;
    }
    return KD_RINGS_NONE;
}
