/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef KD_RINGS_H
#define KD_RINGS_H

#include <stdint.h>

#include "scan.h"

/* The concentric rings of the Space tab, as geometry only (no GTK, no
 * Cairo), so they can be tested: which slice goes where, and which slice is
 * under the mouse.
 *
 * The folder shown sits in the center disc. Ring 0 holds its children,
 * ring 1 their children, and so on. Angles are in radians, 0 at 12 o'clock
 * and growing clockwise, each slice's sweep proportional to its size
 * against the center folder. Slices narrower than min_angle, and everything
 * smaller after them, merge into one "small items" slice per folder.
 *
 * Memory: kd_rings owns its segment array; kd_rings_clear frees it. The
 * segments refer to tree nodes by id, so they stay valid until the tree is
 * changed or freed. */

typedef struct {
    kd_node_id node;  /* KD_NODE_NONE for a merged "small items" slice */
    int depth;        /* ring: 0 = right around the center */
    double a0, a1;    /* start and end angle */
    double hue;       /* 0..1: from the ring-0 slice it sits under */
    int parent;       /* segment one ring in, -1 in ring 0 */
    uint32_t n_small; /* merged slice: how many items */
    uint64_t size;    /* the node's size, or the merged items' */
} kd_ring_seg;

typedef struct {
    kd_ring_seg *segs;
    int n, cap;
    int depth;        /* rings in use (the deepest segment's depth + 1) */
    kd_node_id center;
} kd_rings;

/* Lays out center's subtree over at most max_depth rings. Replaces what r
 * had. Segments come ring 0 first in angle order, then each ring after the
 * other (so a slice's parent always comes before it). */
void kd_rings_layout(kd_rings *r, const kd_tree *t, kd_node_id center, int max_depth,
                     double min_angle);
void kd_rings_clear(kd_rings *r);

/* Where the rings go in a w × h area. */
typedef struct {
    double cx, cy;
    double r_center; /* radius of the center disc */
    double ring_w;   /* width of each ring */
} kd_rings_geom;

void kd_rings_geom_for(kd_rings_geom *g, double w, double h, int depth);

/* Radii of a ring. */
static inline double kd_rings_r0(const kd_rings_geom *g, int depth)
{
    return g->r_center + depth * g->ring_w;
}

/* What is at (x, y): a segment index, KD_RINGS_CENTER or KD_RINGS_NONE. */
#define KD_RINGS_CENTER (-1)
#define KD_RINGS_NONE (-2)
int kd_rings_hit(const kd_rings *r, const kd_rings_geom *g, double x, double y);

#endif
