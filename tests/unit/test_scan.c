/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "../harness.h"
#include "scan.h"
#include "util.h"

static char dir[] = "/tmp/kd-scan-XXXXXX";
static char outside[] = "/tmp/kd-scan-out-XXXXXX";
static bool as_root;

/* A file of n real bytes (not sparse, so it takes room on disk). */
static void write_file(const char *rel, size_t n)
{
    char path[KD_PATH_MAX];
    kd_pathf(path, sizeof path, "%s/%s", dir, rel);
    FILE *f = fopen(path, "wb");
    if (!f) {
        perror(path);
        exit(1);
    }
    static char buf[4096];
    memset(buf, 'x', sizeof buf);
    for (size_t done = 0; done < n;) {
        size_t k = n - done < sizeof buf ? n - done : sizeof buf;
        fwrite(buf, 1, k, f);
        done += k;
    }
    fclose(f);
}

static void make_dir(const char *rel)
{
    char path[KD_PATH_MAX];
    kd_pathf(path, sizeof path, "%s/%s", dir, rel);
    mkdir(path, 0755);
}

static void link_rel(const char *from, const char *to)
{
    char a[KD_PATH_MAX], b[KD_PATH_MAX];
    kd_pathf(a, sizeof a, "%s/%s", dir, from);
    kd_pathf(b, sizeof b, "%s/%s", dir, to);
    if (link(a, b) < 0)
        perror("link");
}

/* `du -sxB1 path`: what the scan must agree with. */
static unsigned long long du_total(const char *path)
{
    char cmd[KD_PATH_MAX + 64];
    snprintf(cmd, sizeof cmd, "du -sxB1 '%s' 2>/dev/null", path);
    FILE *p = popen(cmd, "r");
    unsigned long long v = 0;
    if (p) {
        if (fscanf(p, "%llu", &v) != 1)
            v = 0;
        pclose(p);
    }
    return v;
}

static void build_tree(void)
{
    /* A 200 KiB file with a second name elsewhere, a symbolic link to a
     * folder full of data outside, an empty file, a name that is not
     * UTF-8, a folder nobody may read. */
    make_dir("a");
    make_dir("a/sub");
    make_dir("empty");
    write_file("a/big", 100 * 1024);
    write_file("a/sub/x", 10 * 1024);
    write_file("b", 50 * 1024);
    write_file("hl1", 200 * 1024);
    link_rel("hl1", "a/hl2");
    write_file("zero", 0);
    write_file("bad-\xff\xfe.bin", 4096);

    char path[KD_PATH_MAX], target[KD_PATH_MAX];
    kd_pathf(target, sizeof target, "%s/data", outside);
    FILE *f = fopen(target, "wb");
    static char buf[64 * 1024];
    fwrite(buf, 1, sizeof buf, f);
    fclose(f);
    kd_pathf(path, sizeof path, "%s/to-outside", dir);
    if (symlink(outside, path) < 0)
        perror("symlink");

    make_dir("locked");
    write_file("locked/secret", 8192);
    kd_pathf(path, sizeof path, "%s/locked", dir);
    chmod(path, 0);
}

TEST(total_matches_du)
{
    kd_tree *t = kd_tree_scan(dir, NULL);
    CHECK(t);
    unsigned long long du = du_total(dir);
    CHECK(du > 0);
    CHECK_INT_EQ(kd_tree_node(t, KD_TREE_ROOT)->size, du);
    kd_tree_free(t);
}

TEST(tree_shape)
{
    kd_tree *t = kd_tree_scan(dir, NULL);
    CHECK(t);
    char *real = realpath(dir, NULL);
    CHECK_STR_EQ(kd_tree_name(t, KD_TREE_ROOT), real);
    free(real);

    /* Biggest first. */
    const kd_node *root = kd_tree_node(t, KD_TREE_ROOT);
    CHECK_INT_EQ(root->count, 8);
    for (uint32_t i = 1; i < root->count; i++)
        CHECK(kd_tree_node(t, kd_tree_child(t, KD_TREE_ROOT, i - 1))->size >=
              kd_tree_node(t, kd_tree_child(t, KD_TREE_ROOT, i))->size);

    /* The hard link counts once: hl1 is found first (root before a/). */
    kd_node_id a = kd_tree_find_child(t, KD_TREE_ROOT, "a");
    kd_node_id hl1 = kd_tree_find_child(t, KD_TREE_ROOT, "hl1");
    CHECK(a != KD_NODE_NONE && hl1 != KD_NODE_NONE);
    kd_node_id hl2 = kd_tree_find_child(t, a, "hl2");
    CHECK(hl2 != KD_NODE_NONE);
    CHECK(kd_tree_node(t, hl1)->size >= 200 * 1024);
    CHECK_INT_EQ(kd_tree_node(t, hl2)->size, 0);
    CHECK(kd_tree_node(t, a)->flags & KD_NODE_DIR);

    /* The link is a link, not the 64 KiB behind it. */
    kd_node_id l = kd_tree_find_child(t, KD_TREE_ROOT, "to-outside");
    CHECK(l != KD_NODE_NONE);
    CHECK(!(kd_tree_node(t, l)->flags & KD_NODE_DIR));
    CHECK(kd_tree_node(t, l)->size < 64 * 1024);

    /* The name keeps its bytes. */
    CHECK(kd_tree_find_child(t, KD_TREE_ROOT, "bad-\xff\xfe.bin") != KD_NODE_NONE);

    /* Files: big, x, b, hl1, hl2, zero, bad, to-outside (not the locked one). */
    CHECK_INT_EQ(root->files, as_root ? 9 : 8);

    kd_node_id locked = kd_tree_find_child(t, KD_TREE_ROOT, "locked");
    CHECK(locked != KD_NODE_NONE);
    if (!as_root) {
        CHECK(kd_tree_node(t, locked)->flags & KD_NODE_DENIED);
        CHECK_INT_EQ(t->denied, 1);
    }

    kd_node_id sub = kd_tree_find_child(t, a, "sub");
    kd_node_id x = kd_tree_find_child(t, sub, "x");
    CHECK(x != KD_NODE_NONE);
    char *p = kd_tree_path(t, x), expect[KD_PATH_MAX];
    char *real2 = realpath(dir, NULL);
    kd_pathf(expect, sizeof expect, "%s/a/sub/x", real2);
    CHECK_STR_EQ(p, expect);
    free(p);
    free(real2);
    kd_tree_free(t);
}

TEST(path_under_slash)
{
    /* Not a scan of /: build the two nodes by hand. */
    kd_tree t = {0};
    kd_node nodes[2] = {{.name = 0, .parent = KD_NODE_NONE, .flags = KD_NODE_DIR},
                        {.name = 2, .parent = 0}};
    char names[] = "/\0usr";
    t.nodes = nodes;
    t.n = 2;
    t.names = names;
    char *p = kd_tree_path(&t, 1);
    CHECK_STR_EQ(p, "/usr");
    free(p);
    p = kd_tree_path(&t, 0);
    CHECK_STR_EQ(p, "/");
    free(p);
}

TEST(graft_after_change)
{
    kd_tree *t = kd_tree_scan(dir, NULL);
    CHECK(t);
    kd_node_id a = kd_tree_find_child(t, KD_TREE_ROOT, "a");
    write_file("a/sub/new", 300 * 1024);

    char *pa = kd_tree_path(t, a);
    kd_tree *sub = kd_tree_scan(pa, NULL);
    free(pa);
    CHECK(sub);
    uint32_t files_before = kd_tree_node(t, KD_TREE_ROOT)->files;
    CHECK(kd_tree_graft(t, a, sub));
    /* hl2 in a/ now counts too: it was hl1's second name in the full scan. */
    uint64_t hl = kd_tree_node(t, kd_tree_find_child(t, KD_TREE_ROOT, "hl1"))->size;
    CHECK_INT_EQ(kd_tree_node(t, KD_TREE_ROOT)->size, du_total(dir) + hl);
    CHECK_INT_EQ(kd_tree_node(t, KD_TREE_ROOT)->files, files_before + 1);
    /* a grew past everything else: first now. */
    CHECK_INT_EQ(kd_tree_child(t, KD_TREE_ROOT, 0), a);
    kd_node_id s = kd_tree_find_child(t, a, "sub");
    CHECK(kd_tree_find_child(t, s, "new") != KD_NODE_NONE);
    CHECK_INT_EQ(kd_tree_node(t, kd_tree_find_child(t, s, "new"))->parent, s);

    char path[KD_PATH_MAX];
    kd_pathf(path, sizeof path, "%s/a/sub/new", dir);
    unlink(path);
    kd_tree_free(t);
}

TEST(remove_node)
{
    kd_tree *t = kd_tree_scan(dir, NULL);
    CHECK(t);
    kd_node_id a = kd_tree_find_child(t, KD_TREE_ROOT, "a");
    kd_node_id sub = kd_tree_find_child(t, a, "sub");
    uint64_t total = kd_tree_node(t, KD_TREE_ROOT)->size;
    uint64_t a_size = kd_tree_node(t, a)->size;
    uint64_t s = kd_tree_node(t, sub)->size;
    uint32_t count = kd_tree_node(t, a)->count;
    kd_tree_remove(t, sub);
    CHECK_INT_EQ(kd_tree_node(t, KD_TREE_ROOT)->size, total - s);
    CHECK_INT_EQ(kd_tree_node(t, a)->size, a_size - s);
    CHECK_INT_EQ(kd_tree_node(t, a)->count, count - 1);
    CHECK(kd_tree_find_child(t, a, "sub") == KD_NODE_NONE);

    if (!as_root) {
        kd_tree_remove(t, kd_tree_find_child(t, KD_TREE_ROOT, "locked"));
        CHECK_INT_EQ(t->denied, 0);
    }
    kd_tree_free(t);
}

TEST(errors)
{
    kd_scan_progress p = {0};
    atomic_store(&p.cancel, true);
    errno = 0;
    CHECK(kd_tree_scan(dir, &p) == NULL);
    CHECK_INT_EQ(errno, ECANCELED);

    char path[KD_PATH_MAX];
    kd_pathf(path, sizeof path, "%s/b", dir);
    CHECK(kd_tree_scan(path, NULL) == NULL);
    CHECK_INT_EQ(errno, ENOTDIR);
    kd_pathf(path, sizeof path, "%s/nope", dir);
    CHECK(kd_tree_scan(path, NULL) == NULL);
    CHECK_INT_EQ(errno, ENOENT);
}

TEST(progress_counts)
{
    kd_scan_progress p = {0};
    kd_tree *t = kd_tree_scan(dir, &p);
    CHECK(t);
    CHECK_INT_EQ(atomic_load(&p.files), kd_tree_node(t, KD_TREE_ROOT)->files);
    CHECK(atomic_load(&p.bytes) > 0);
    kd_tree_free(t);
}

TEST(remove_tree_stays_put)
{
    /* The link to the outside goes; what it points to stays. */
    char path[KD_PATH_MAX], keep[KD_PATH_MAX];
    kd_pathf(keep, sizeof keep, "%s/data", outside);
    make_dir("gone");
    make_dir("gone/deep");
    write_file("gone/deep/f", 1000);
    kd_pathf(path, sizeof path, "%s/gone/link", dir);
    CHECK(symlink(outside, path) == 0);
    kd_pathf(path, sizeof path, "%s/gone/", dir);
    CHECK_INT_EQ(kd_remove_tree(path), 0);
    kd_pathf(path, sizeof path, "%s/gone", dir);
    struct stat st;
    CHECK(lstat(path, &st) < 0 && errno == ENOENT);
    CHECK(kd_file_exists(keep));

    CHECK_INT_EQ(kd_remove_tree("/tmp/.."), -1);
    CHECK_INT_EQ(errno, EINVAL);
}

int main(void)
{
    as_root = geteuid() == 0;
    if (!mkdtemp(dir) || !mkdtemp(outside)) {
        perror("mkdtemp");
        return 1;
    }
    build_tree();
    RUN(total_matches_du);
    RUN(tree_shape);
    RUN(path_under_slash);
    RUN(graft_after_change);
    RUN(remove_node);
    RUN(errors);
    RUN(progress_counts);
    RUN(remove_tree_stays_put);

    char path[KD_PATH_MAX];
    kd_pathf(path, sizeof path, "%s/locked", dir);
    chmod(path, 0755);
    kd_remove_tree(dir);
    kd_remove_tree(outside);
    return harness_report();
}
