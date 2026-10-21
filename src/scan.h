/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef KD_SCAN_H
#define KD_SCAN_H

#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>

/* What takes up room under a folder, for the Space tab and `--scan`.
 *
 * The scan walks with openat + fdopendir + fstatat(AT_SYMLINK_NOFOLLOW), as
 * du does: sizes are the room really used on disk (st_blocks * 512), it
 * stays on the filesystem it started on (st_dev), symbolic links are never
 * followed, and a file with several hard links counts once. The total
 * matches `du -sxB1`.
 *
 * The tree is compact: nodes in one array, referred to by index; the names
 * in one arena; the children of each folder a run in the kids array,
 * biggest first. About 36 bytes per file plus its name.
 *
 * Node ids never change while the tree lives: kd_tree_graft and
 * kd_tree_remove only add nodes or drop them from their parent's run (the
 * dropped ones stay in the array, unreachable).
 *
 * kd_tree_scan is blocking and touches nothing global, so it can run in a
 * thread; it reports progress and checks for cancellation through
 * kd_scan_progress, which another thread may read and set. Everything else
 * here is for one thread at a time.
 *
 * Memory: kd_tree owns its arrays; kd_tree_free releases them. */

typedef uint32_t kd_node_id;
#define KD_NODE_NONE UINT32_MAX
/* The folder the scan started from. */
#define KD_TREE_ROOT 0u

enum {
    KD_NODE_DIR = 1u << 0,
    /* A folder that could not be read (whole or in part): its size is short. */
    KD_NODE_DENIED = 1u << 1,
    /* Another filesystem is mounted here: not scanned, size 0 (du -x). */
    KD_NODE_MOUNT = 1u << 2,
};

typedef struct {
    uint64_t size;  /* on disk; a folder counts itself and all inside it */
    uint32_t name;  /* offset in kd_tree.names */
    kd_node_id parent;
    uint32_t first; /* folders: their children are kids[first .. first+count) */
    uint32_t count;
    uint32_t files; /* files (anything but folders) inside, at any depth; 1 for a file */
    uint32_t flags;
} kd_node;

typedef struct {
    kd_node *nodes;
    uint32_t n, cap;
    kd_node_id *kids;
    uint32_t n_kids, cap_kids;
    char *names;
    uint32_t names_len, names_cap;
    /* Folders that could not be read (KD_NODE_DENIED). */
    uint32_t denied;
} kd_tree;

typedef struct {
    atomic_bool cancel;     /* set by anyone: the scan stops soon after */
    atomic_uint_fast64_t files;
    atomic_uint_fast64_t bytes;
} kd_scan_progress;

/* Scans the folder at path. The root node's name is the absolute path.
 * NULL with errno set: ECANCELED if cancelled, ENOTDIR if path is not a
 * folder, ENOMEM, or why the folder could not be opened. progress may be
 * NULL. */
kd_tree *kd_tree_scan(const char *path, kd_scan_progress *progress);
void kd_tree_free(kd_tree *t);

static inline const kd_node *kd_tree_node(const kd_tree *t, kd_node_id id)
{
    return &t->nodes[id];
}

static inline const char *kd_tree_name(const kd_tree *t, kd_node_id id)
{
    return t->names + t->nodes[id].name;
}

/* The i-th biggest child of a folder. */
static inline kd_node_id kd_tree_child(const kd_tree *t, kd_node_id id, uint32_t i)
{
    return t->kids[t->nodes[id].first + i];
}

/* The child named name, or KD_NODE_NONE. */
kd_node_id kd_tree_find_child(const kd_tree *t, kd_node_id id, const char *name);

/* The node's full path; the caller frees it with free(). NULL if out of
 * memory. */
char *kd_tree_path(const kd_tree *t, kd_node_id id);

/* Puts a new scan of a folder in place of what the tree had for it (the
 * "Refresh" of the Space tab): at's children become sub's, and the sizes
 * up to the root are fixed. sub is consumed (freed) either way. false if
 * out of memory (the tree is unchanged). Hard links shared between sub and
 * the rest of the tree may be counted twice. */
bool kd_tree_graft(kd_tree *t, kd_node_id at, kd_tree *sub);

/* Takes a node out of the tree (after it went to the trash or was
 * deleted): out of its parent's children and out of the sizes above it.
 * Not the root. */
void kd_tree_remove(kd_tree *t, kd_node_id id);

#endif
