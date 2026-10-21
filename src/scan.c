/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "scan.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* Inodes already counted (files with st_nlink > 1). Open addressing; the
 * slots hold ino + 1 so that 0 means empty. One filesystem per scan, so
 * the inode alone is the key. */
typedef struct {
    uint64_t *slots;
    size_t cap, n; /* cap is a power of two */
} ino_set;

/* true if ino was already there; false if it was added (or out of memory:
 * then it counts again, the lesser evil). */
static bool ino_seen(ino_set *s, uint64_t ino)
{
    if (s->n * 2 >= s->cap) {
        size_t cap = s->cap ? s->cap * 2 : 1024;
        uint64_t *slots = calloc(cap, sizeof *slots);
        if (!slots)
            return false;
        for (size_t i = 0; i < s->cap; i++) {
            uint64_t k = s->slots[i];
            if (!k)
                continue;
            size_t j = (size_t)(k * 0x9E3779B97F4A7C15ULL) & (cap - 1);
            while (slots[j])
                j = (j + 1) & (cap - 1);
            slots[j] = k;
        }
        free(s->slots);
        s->slots = slots;
        s->cap = cap;
    }
    uint64_t k = ino + 1;
    size_t j = (size_t)(k * 0x9E3779B97F4A7C15ULL) & (s->cap - 1);
    while (s->slots[j]) {
        if (s->slots[j] == k)
            return true;
        j = (j + 1) & (s->cap - 1);
    }
    s->slots[j] = k;
    s->n++;
    return false;
}

/* ---- growing the arrays ---------------------------------------------------------- */

static bool grow(void **arr, uint32_t *cap, uint32_t need, size_t elem)
{
    if (need <= *cap)
        return true;
    uint64_t c = *cap ? *cap : 256;
    while (c < need)
        c *= 2;
    if (c > UINT32_MAX)
        c = UINT32_MAX;
    if (c < need || c > SIZE_MAX / elem)
        return false;
    void *p = realloc(*arr, (size_t)c * elem);
    if (!p)
        return false;
    *arr = p;
    *cap = (uint32_t)c;
    return true;
}

static bool reserve(kd_tree *t, uint32_t nodes, uint32_t kids, uint64_t names)
{
    if (names > UINT32_MAX || (uint64_t)t->n + nodes >= KD_NODE_NONE ||
        (uint64_t)t->n_kids + kids > UINT32_MAX)
        return false;
    return grow((void **)&t->nodes, &t->cap, t->n + nodes, sizeof *t->nodes) &&
           grow((void **)&t->kids, &t->cap_kids, t->n_kids + kids, sizeof *t->kids) &&
           grow((void **)&t->names, &t->names_cap, (uint32_t)names, 1);
}

/* Appends a node named name; KD_NODE_NONE if out of memory. */
static kd_node_id add_node(kd_tree *t, const char *name, kd_node_id parent)
{
    size_t len = strlen(name) + 1;
    if (!reserve(t, 1, 0, (uint64_t)t->names_len + len))
        return KD_NODE_NONE;
    kd_node_id id = t->n++;
    kd_node *n = &t->nodes[id];
    memset(n, 0, sizeof *n);
    n->name = t->names_len;
    n->parent = parent;
    memcpy(t->names + t->names_len, name, len);
    t->names_len += (uint32_t)len;
    return id;
}

/* ---- order of the children -------------------------------------------------------- */

/* qsort has no user pointer in C11; each thread sorts its own tree. */
static _Thread_local const kd_tree *sort_tree;

static int by_size(const void *a, const void *b)
{
    const kd_node *x = &sort_tree->nodes[*(const kd_node_id *)a];
    const kd_node *y = &sort_tree->nodes[*(const kd_node_id *)b];
    if (x->size != y->size)
        return x->size > y->size ? -1 : 1;
    return strcmp(sort_tree->names + x->name, sort_tree->names + y->name);
}

static void sort_kids(kd_tree *t, kd_node_id id)
{
    const kd_node *n = &t->nodes[id];
    if (n->count < 2)
        return;
    sort_tree = t;
    qsort(t->kids + n->first, n->count, sizeof *t->kids, by_size);
    sort_tree = NULL;
}

/* ---- the walk ------------------------------------------------------------------------- */

typedef struct {
    kd_tree *t;
    dev_t dev;
    kd_scan_progress *progress;
    ino_set inos;
    bool nomem;
} walk;

static bool stopped(walk *w)
{
    return w->nomem || (w->progress && atomic_load_explicit(&w->progress->cancel,
                                                            memory_order_relaxed));
}

static void deny(walk *w, kd_node_id id)
{
    if (!(w->t->nodes[id].flags & KD_NODE_DENIED)) {
        w->t->nodes[id].flags |= KD_NODE_DENIED;
        w->t->denied++;
    }
}

/* Reads the folder open at fd (taken over) into node id, then each folder
 * in it. Every child of one folder is read before going into any of them,
 * so they are a single run in kids. */
static void walk_dir(walk *w, int fd, kd_node_id id)
{
    kd_tree *t = w->t;
    DIR *d = fdopendir(fd);
    if (!d) {
        close(fd);
        deny(w, id);
        return;
    }
    int dfd = dirfd(d);
    uint32_t first = t->n_kids;
    uint64_t size = 0, files = 0;
    unsigned since_check = 0;

    for (;;) {
        errno = 0;
        struct dirent *e = readdir(d);
        if (!e) {
            if (errno)
                deny(w, id);
            break;
        }
        const char *nm = e->d_name;
        if (nm[0] == '.' && (!nm[1] || (nm[1] == '.' && !nm[2])))
            continue;
        if (++since_check == 4096) {
            since_check = 0;
            if (stopped(w))
                break;
        }
        struct stat st;
        if (fstatat(dfd, nm, &st, AT_SYMLINK_NOFOLLOW) < 0) {
            /* Gone since readdir (ENOENT), or a folder we may list but not
             * enter (EACCES). */
            if (errno != ENOENT)
                deny(w, id);
            continue;
        }
        kd_node_id c = add_node(t, nm, id);
        if (c == KD_NODE_NONE || !reserve(t, 0, 1, t->names_len)) {
            w->nomem = true;
            break;
        }
        t->kids[t->n_kids++] = c;
        kd_node *n = &t->nodes[c];
        uint64_t bytes = (uint64_t)st.st_blocks * 512;
        if (S_ISDIR(st.st_mode)) {
            n->flags = KD_NODE_DIR;
            if (st.st_dev != w->dev) {
                n->flags |= KD_NODE_MOUNT;
                bytes = 0;
            }
        } else {
            n->files = 1;
            if (st.st_nlink > 1 && ino_seen(&w->inos, (uint64_t)st.st_ino))
                bytes = 0;
        }
        n->size = bytes;
        size += bytes;
        files += n->files;
        if (w->progress) {
            atomic_fetch_add_explicit(&w->progress->files, n->files, memory_order_relaxed);
            atomic_fetch_add_explicit(&w->progress->bytes, bytes, memory_order_relaxed);
        }
    }
    t->nodes[id].first = first;
    t->nodes[id].count = t->n_kids - first;

    for (uint32_t i = first; i < first + t->nodes[id].count && !stopped(w); i++) {
        kd_node_id c = t->kids[i];
        if ((t->nodes[c].flags & (KD_NODE_DIR | KD_NODE_MOUNT)) != KD_NODE_DIR)
            continue;
        int cfd = openat(dfd, t->names + t->nodes[c].name,
                         O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        if (cfd < 0) {
            deny(w, c);
            continue;
        }
        uint64_t before = t->nodes[c].size;
        walk_dir(w, cfd, c);
        size += t->nodes[c].size - before;
        files += t->nodes[c].files;
    }
    closedir(d);
    t->nodes[id].size += size;
    t->nodes[id].files += (uint32_t)files;
    sort_kids(t, id);
}

kd_tree *kd_tree_scan(const char *path, kd_scan_progress *progress)
{
    char *abs = realpath(path, NULL);
    if (!abs)
        return NULL;
    int fd = open(abs, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    struct stat st;
    if (fd < 0 || fstat(fd, &st) < 0) {
        int e = errno;
        if (fd >= 0)
            close(fd);
        free(abs);
        errno = e;
        return NULL;
    }

    walk w = {.progress = progress};
    w.t = calloc(1, sizeof *w.t);
    kd_node_id root = w.t ? add_node(w.t, abs, KD_NODE_NONE) : KD_NODE_NONE;
    free(abs);
    if (root == KD_NODE_NONE) {
        close(fd);
        kd_tree_free(w.t);
        errno = ENOMEM;
        return NULL;
    }
    w.dev = st.st_dev;
    w.t->nodes[root].flags = KD_NODE_DIR;
    w.t->nodes[root].size = (uint64_t)st.st_blocks * 512;
    walk_dir(&w, fd, root);
    free(w.inos.slots);

    if (stopped(&w)) {
        kd_tree_free(w.t);
        errno = w.nomem ? ENOMEM : ECANCELED;
        return NULL;
    }
    /* Give back what the doubling left unused (often a third). */
    kd_tree *t = w.t;
    void *p = realloc(t->nodes, (size_t)t->n * sizeof *t->nodes);
    if (p) {
        t->nodes = p;
        t->cap = t->n;
    }
    p = t->n_kids ? realloc(t->kids, (size_t)t->n_kids * sizeof *t->kids) : NULL;
    if (p) {
        t->kids = p;
        t->cap_kids = t->n_kids;
    }
    p = realloc(t->names, t->names_len);
    if (p) {
        t->names = p;
        t->names_cap = t->names_len;
    }
    return t;
}

void kd_tree_free(kd_tree *t)
{
    if (!t)
        return;
    free(t->nodes);
    free(t->kids);
    free(t->names);
    free(t);
}

kd_node_id kd_tree_find_child(const kd_tree *t, kd_node_id id, const char *name)
{
    const kd_node *n = &t->nodes[id];
    for (uint32_t i = 0; i < n->count; i++) {
        kd_node_id c = t->kids[n->first + i];
        if (strcmp(kd_tree_name(t, c), name) == 0)
            return c;
    }
    return KD_NODE_NONE;
}

char *kd_tree_path(const kd_tree *t, kd_node_id id)
{
    size_t len = 0;
    for (kd_node_id a = id; a != KD_NODE_NONE; a = t->nodes[a].parent)
        len += strlen(kd_tree_name(t, a)) + 1;
    char *out = malloc(len + 1);
    if (!out)
        return NULL;
    /* Built from the end. */
    char *p = out + len;
    *p = '\0';
    for (kd_node_id a = id; a != KD_NODE_NONE; a = t->nodes[a].parent) {
        const char *nm = kd_tree_name(t, a);
        size_t l = strlen(nm);
        p -= l;
        memcpy(p, nm, l);
        if (t->nodes[a].parent != KD_NODE_NONE)
            *--p = '/';
    }
    /* The root "/" gives "//usr": drop the doubled slash. */
    if (p[0] == '/' && p[1] == '/')
        p++;
    memmove(out, p, strlen(p) + 1);
    return out;
}

/* Denied folders in the subtree of id, id included. */
static uint32_t count_denied(const kd_tree *t, kd_node_id id)
{
    const kd_node *n = &t->nodes[id];
    uint32_t c = (n->flags & KD_NODE_DENIED) ? 1 : 0;
    for (uint32_t i = 0; i < n->count; i++)
        c += count_denied(t, t->kids[n->first + i]);
    return c;
}

/* id changed size by (new - old): fix the folders above it and their order. */
static void resize_up(kd_tree *t, kd_node_id id, uint64_t old_size, uint64_t new_size,
                      uint32_t old_files, uint32_t new_files)
{
    for (kd_node_id a = t->nodes[id].parent; a != KD_NODE_NONE; a = t->nodes[a].parent) {
        t->nodes[a].size = t->nodes[a].size - old_size + new_size;
        t->nodes[a].files = t->nodes[a].files - old_files + new_files;
    }
    for (kd_node_id a = t->nodes[id].parent; a != KD_NODE_NONE; a = t->nodes[a].parent)
        sort_kids(t, a);
}

bool kd_tree_graft(kd_tree *t, kd_node_id at, kd_tree *sub)
{
    if (!reserve(t, sub->n - 1, sub->n_kids, (uint64_t)t->names_len + sub->names_len)) {
        kd_tree_free(sub);
        return false;
    }
    /* sub's node i (i >= 1) becomes off + i; its root becomes at. */
    uint32_t off = t->n - 1, off_kid = t->n_kids, off_name = t->names_len;
    memcpy(t->names + off_name, sub->names, sub->names_len);
    t->names_len += sub->names_len;
    for (uint32_t i = 1; i < sub->n; i++) {
        kd_node n = sub->nodes[i];
        n.name += off_name;
        n.parent = n.parent == KD_TREE_ROOT ? at : n.parent + off;
        n.first += off_kid;
        t->nodes[t->n++] = n;
    }
    for (uint32_t k = 0; k < sub->n_kids; k++)
        t->kids[t->n_kids++] = sub->kids[k] + off;

    const kd_node *r = &sub->nodes[KD_TREE_ROOT];
    kd_node *a = &t->nodes[at];
    uint64_t old_size = a->size;
    uint32_t old_files = a->files;
    t->denied = t->denied - count_denied(t, at) + sub->denied;
    a->first = r->first + off_kid;
    a->count = r->count;
    a->size = r->size;
    a->files = r->files;
    a->flags = (a->flags & ~(uint32_t)KD_NODE_DENIED) | (r->flags & KD_NODE_DENIED);
    resize_up(t, at, old_size, r->size, old_files, r->files);
    kd_tree_free(sub);
    return true;
}

void kd_tree_remove(kd_tree *t, kd_node_id id)
{
    kd_node_id p = t->nodes[id].parent;
    if (p == KD_NODE_NONE)
        return;
    kd_node *pn = &t->nodes[p];
    for (uint32_t i = 0; i < pn->count; i++) {
        if (t->kids[pn->first + i] != id)
            continue;
        memmove(&t->kids[pn->first + i], &t->kids[pn->first + i + 1],
                (pn->count - i - 1) * sizeof *t->kids);
        pn->count--;
        break;
    }
    t->denied -= count_denied(t, id);
    resize_up(t, id, t->nodes[id].size, 0, t->nodes[id].files, 0);
}
