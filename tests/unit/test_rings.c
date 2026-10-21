/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "../harness.h"
#include "rings.h"

#define TAU (2 * M_PI)

/* A tree built by hand, sizes in bytes:
 *
 *   root 100
 *   ├ A 50/   ├ A1 30   └ A2 20
 *   ├ B 25
 *   ├ C 20/   └ C1 20/  └ C1a 20
 *   └ s1..s5 1 each
 */
enum { ROOT, A, B, C, S1, S2, S3, S4, S5, A1, A2, C1, C1A, N };

static kd_node nodes[N];
static kd_node_id kids[N];
static char names[N * 8];
static kd_tree tree;

static void node(int id, int parent, const char *name, uint64_t size, bool dir)
{
    nodes[id] = (kd_node){.size = size, .parent = parent < 0 ? KD_NODE_NONE : (kd_node_id)parent,
                          .flags = dir ? KD_NODE_DIR : 0, .files = dir ? 0 : 1};
    nodes[id].name = (uint32_t)(id * 8);
    memcpy(names + id * 8, name, strlen(name) + 1);
}

static uint32_t n_kids;

static void children(int id, const int *ids, int n)
{
    nodes[id].first = n_kids;
    nodes[id].count = (uint32_t)n;
    for (int i = 0; i < n; i++)
        kids[n_kids++] = (kd_node_id)ids[i];
}

static void build(void)
{
    node(ROOT, -1, "/r", 100, true);
    node(A, ROOT, "A", 50, true);
    node(B, ROOT, "B", 25, false);
    node(C, ROOT, "C", 20, true);
    static const char *const s[] = {"s1", "s2", "s3", "s4", "s5"};
    for (int i = 0; i < 5; i++)
        node(S1 + i, ROOT, s[i], 1, false);
    node(A1, A, "A1", 30, false);
    node(A2, A, "A2", 20, false);
    node(C1, C, "C1", 20, true);
    node(C1A, C1, "C1a", 20, false);
    children(ROOT, (const int[]){A, B, C, S1, S2, S3, S4, S5}, 8);
    children(A, (const int[]){A1, A2}, 2);
    children(C, (const int[]){C1}, 1);
    children(C1, (const int[]){C1A}, 1);
    tree = (kd_tree){.nodes = nodes, .n = N, .kids = kids, .n_kids = n_kids, .names = names};
}

static bool near(double a, double b)
{
    return fabs(a - b) < 1e-9;
}

TEST(layout)
{
    kd_rings r = {0};
    /* 2%: the 1-byte files merge. */
    kd_rings_layout(&r, &tree, KD_TREE_ROOT, 5, TAU * 0.02);
    CHECK_INT_EQ(r.depth, 3);
    CHECK_INT_EQ(r.n, 8);
    /* Ring 0: A, B, C, then the five small ones as one. */
    CHECK_INT_EQ(r.segs[0].node, A);
    CHECK(near(r.segs[0].a0, 0) && near(r.segs[0].a1, TAU * 0.5));
    CHECK_INT_EQ(r.segs[1].node, B);
    CHECK(near(r.segs[1].a0, TAU * 0.5) && near(r.segs[1].a1, TAU * 0.75));
    CHECK_INT_EQ(r.segs[2].node, C);
    CHECK_INT_EQ(r.segs[3].node, KD_NODE_NONE);
    CHECK_INT_EQ(r.segs[3].n_small, 5);
    CHECK_INT_EQ(r.segs[3].size, 5);
    CHECK(near(r.segs[3].a0, TAU * 0.95) && near(r.segs[3].a1, TAU));
    /* Ring 1: A1, A2 under A; C1 under C. */
    CHECK_INT_EQ(r.segs[4].node, A1);
    CHECK_INT_EQ(r.segs[4].depth, 1);
    CHECK_INT_EQ(r.segs[4].parent, 0);
    CHECK(near(r.segs[4].a1, TAU * 0.3));
    CHECK(near(r.segs[4].hue, r.segs[0].hue));
    CHECK_INT_EQ(r.segs[6].node, C1);
    CHECK_INT_EQ(r.segs[6].parent, 2);
    CHECK(near(r.segs[6].a0, TAU * 0.75));
    /* Ring 2. */
    CHECK_INT_EQ(r.segs[7].node, C1A);
    CHECK_INT_EQ(r.segs[7].parent, 6);
    /* Colors go around: A's hue is a quarter turn (its middle). */
    CHECK(near(r.segs[0].hue, 0.25));
    kd_rings_clear(&r);
}

TEST(no_merge_and_depth_limit)
{
    kd_rings r = {0};
    kd_rings_layout(&r, &tree, KD_TREE_ROOT, 1, 0);
    CHECK_INT_EQ(r.depth, 1);
    CHECK_INT_EQ(r.n, 8);
    for (int i = 0; i < r.n; i++)
        CHECK(r.segs[i].node != KD_NODE_NONE);

    /* Centered on A: its two files fill the circle. */
    kd_rings_layout(&r, &tree, A, 5, 0);
    CHECK_INT_EQ(r.n, 2);
    CHECK(near(r.segs[1].a1, TAU));
    kd_rings_clear(&r);
}

TEST(hit)
{
    kd_rings r = {0};
    kd_rings_layout(&r, &tree, KD_TREE_ROOT, 5, TAU * 0.02);
    kd_rings_geom g;
    kd_rings_geom_for(&g, 400, 300, r.depth);
    CHECK(near(g.cx, 200) && near(g.cy, 150));
    CHECK(near(kd_rings_r0(&g, r.depth), 150 - 8));

    CHECK_INT_EQ(kd_rings_hit(&r, &g, 200, 150), KD_RINGS_CENTER);
    CHECK_INT_EQ(kd_rings_hit(&r, &g, 0, 0), KD_RINGS_NONE);
    /* Just right of 12 o'clock, middle of ring 0: A. */
    double rm = kd_rings_r0(&g, 0) + g.ring_w / 2;
    CHECK_INT_EQ(kd_rings_hit(&r, &g, 201, 150 - rm), 0);
    /* Just past straight down (half a turn) in ring 0: the start of B. */
    CHECK_INT_EQ(kd_rings_hit(&r, &g, 199, 150 + rm), 1);
    /* Just left of 12 o'clock: the small ones. */
    CHECK_INT_EQ(kd_rings_hit(&r, &g, 199, 150 - rm), 3);
    /* Ring 1 at 3 o'clock (a quarter turn): A1 (0 .. 0.3 turn). */
    double rm1 = kd_rings_r0(&g, 1) + g.ring_w / 2;
    CHECK_INT_EQ(kd_rings_hit(&r, &g, 200 + rm1, 150), 4);
    /* Ring 2 at 9 o'clock: C1a (0.75 .. 0.95). Ring 2 at 3 o'clock: nothing. */
    double rm2 = kd_rings_r0(&g, 2) + g.ring_w / 2;
    CHECK_INT_EQ(kd_rings_hit(&r, &g, 200 - rm2, 149), 7);
    CHECK_INT_EQ(kd_rings_hit(&r, &g, 200 + rm2, 150), KD_RINGS_NONE);
    kd_rings_clear(&r);
}

TEST(empty_folder)
{
    kd_rings r = {0};
    kd_rings_layout(&r, &tree, B, 5, 0);
    CHECK_INT_EQ(r.n, 0);
    CHECK_INT_EQ(r.depth, 0);
    kd_rings_geom g;
    kd_rings_geom_for(&g, 100, 100, r.depth);
    CHECK_INT_EQ(kd_rings_hit(&r, &g, 50, 50), KD_RINGS_CENTER);
    CHECK_INT_EQ(kd_rings_hit(&r, &g, 50, 5), KD_RINGS_NONE);
    kd_rings_clear(&r);
}

int main(void)
{
    build();
    RUN(layout);
    RUN(no_merge_and_depth_limit);
    RUN(hit);
    RUN(empty_folder);
    return harness_report();
}
