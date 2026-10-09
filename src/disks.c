/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "disks.h"

#include <ctype.h>
#include <dirent.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "i18n.h"
#include "util.h"

#define MIB (1024ULL * 1024ULL)
/* Gaps up to this are alignment (and the EBR before each logical
 * partition), not free space. */
#define MIN_GAP MIB

/* ---- string pool --------------------------------------------------------- */

typedef struct kd_pool_chunk {
    struct kd_pool_chunk *next;
    size_t used, cap;
    max_align_t data[];
} kd_pool_chunk;

struct kd_pool {
    kd_pool_chunk *head;
};

static void *pool_alloc(struct kd_pool *p, size_t n)
{
    n = (n + sizeof(max_align_t) - 1) / sizeof(max_align_t) * sizeof(max_align_t);
    kd_pool_chunk *c = p->head;
    if (!c || c->cap - c->used < n) {
        size_t cap = n > 16384 ? n : 16384;
        c = malloc(sizeof *c + cap);
        if (!c)
            return NULL;
        c->next = p->head;
        c->used = 0;
        c->cap = cap;
        p->head = c;
    }
    void *r = (char *)c->data + c->used;
    c->used += n;
    memset(r, 0, n);
    return r;
}

static const char *pool_strdup(struct kd_pool *p, const char *s)
{
    if (!s || !*s)
        return "";
    size_t n = strlen(s) + 1;
    char *r = pool_alloc(p, n);
    if (!r)
        return "";
    memcpy(r, s, n);
    return r;
}

static void pool_free(struct kd_pool *p)
{
    if (!p)
        return;
    for (kd_pool_chunk *c = p->head, *next; c; c = next) {
        next = c->next;
        free(c);
    }
    free(p);
}

/* ---- small readers ------------------------------------------------------- */

static void trim(char *s)
{
    size_t n = strlen(s);
    while (n && isspace((unsigned char)s[n - 1]))
        s[--n] = '\0';
    size_t i = 0;
    while (isspace((unsigned char)s[i]))
        i++;
    if (i)
        memmove(s, s + i, n - i + 1);
}

/* First line of a sysfs-style file, trimmed. false (and "") if unreadable. */
static bool read_line(char *out, size_t size, const char *fmt, ...)
    __attribute__((format(printf, 3, 4)));

static bool read_line(char *out, size_t size, const char *fmt, ...)
{
    char path[KD_PATH_MAX];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(path, sizeof path, fmt, ap);
    va_end(ap);
    out[0] = '\0';
    if (n < 0 || (size_t)n >= sizeof path)
        return false;
    FILE *f = fopen(path, "r");
    if (!f)
        return false;
    bool ok = fgets(out, (int)size, f) != NULL;
    fclose(f);
    if (!ok) {
        out[0] = '\0';
        return false;
    }
    trim(out);
    return true;
}

static uint64_t read_u64(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

static uint64_t read_u64(const char *fmt, ...)
{
    char path[KD_PATH_MAX], buf[64];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(path, sizeof path, fmt, ap);
    va_end(ap);
    if (n < 0 || (size_t)n >= sizeof path || !read_line(buf, sizeof buf, "%s", path))
        return 0;
    return strtoull(buf, NULL, 10);
}

/* ---- udev database ------------------------------------------------------- */

typedef struct {
    char *text; /* the whole file; NULL if there is none */
} uprops;

static uprops uprops_load(const kd_sources *src, unsigned major, unsigned minor)
{
    char path[KD_PATH_MAX];
    uprops p = {NULL};
    if (kd_pathf(path, sizeof path, "%s/b%u:%u", src->udev_db, major, minor))
        p.text = kd_read_file(path, NULL);
    return p;
}

/* Value of E:KEY into out ("" if absent). */
static bool uprops_get(const uprops *p, const char *key, char *out, size_t size)
{
    out[0] = '\0';
    if (!p->text)
        return false;
    size_t klen = strlen(key);
    for (const char *line = p->text; *line;) {
        const char *eol = strchr(line, '\n');
        size_t len = eol ? (size_t)(eol - line) : strlen(line);
        if (len > klen + 2 && line[0] == 'E' && line[1] == ':' &&
            memcmp(line + 2, key, klen) == 0 && line[2 + klen] == '=') {
            size_t vlen = len - klen - 3;
            if (vlen >= size)
                vlen = size - 1;
            memcpy(out, line + klen + 3, vlen);
            out[vlen] = '\0';
            return true;
        }
        if (!eol)
            break;
        line = eol + 1;
    }
    return false;
}

/* The *_ENC values escape spaces and odd bytes as \xHH. */
static void decode_enc(char *s)
{
    char *w = s;
    for (const char *r = s; *r;) {
        if (r[0] == '\\' && r[1] == 'x' && isxdigit((unsigned char)r[2]) &&
            isxdigit((unsigned char)r[3])) {
            char hex[3] = {r[2], r[3], 0};
            *w++ = (char)strtol(hex, NULL, 16);
            r += 4;
        } else {
            *w++ = *r++;
        }
    }
    *w = '\0';
}

/* KEY_ENC decoded, else KEY with "_" for spaces undone (only when asked). */
static void uprops_get_enc(const uprops *p, const char *key, char *out, size_t size,
                           bool underscores)
{
    char enc[256];
    if (kd_pathf(enc, sizeof enc, "%s_ENC", key) && uprops_get(p, enc, out, size) && *out) {
        decode_enc(out);
    } else if (uprops_get(p, key, out, size) && underscores) {
        for (char *c = out; *c; c++)
            if (*c == '_')
                *c = ' ';
    }
    trim(out);
}

/* ---- mountinfo and swaps ------------------------------------------------- */

typedef struct {
    unsigned major, minor;
    bool root;        /* the whole filesystem, not a bind of a subfolder */
    char *mountpoint; /* unescaped */
    char *source;
} mount_entry;

typedef struct {
    char *path;
    uint64_t size, used; /* bytes */
} swap_entry;

typedef struct {
    mount_entry *mounts;
    int n_mounts;
    swap_entry *swaps;
    int n_swaps;
    char *mount_text, *swap_text; /* the entries point in here */
} sys_state;

/* \040 and friends, in place. */
static void unescape_octal(char *s)
{
    char *w = s;
    for (const char *r = s; *r;) {
        if (r[0] == '\\' && r[1] >= '0' && r[1] <= '3' && r[2] >= '0' && r[2] <= '7' &&
            r[3] >= '0' && r[3] <= '7') {
            *w++ = (char)((r[1] - '0') * 64 + (r[2] - '0') * 8 + (r[3] - '0'));
            r += 4;
        } else {
            *w++ = *r++;
        }
    }
    *w = '\0';
}

/* Splits on spaces in place; returns how many fields (at most max). */
static int split_fields(char *line, char **fields, int max)
{
    int n = 0;
    char *save = NULL;
    for (char *tok = strtok_r(line, " \t", &save); tok && n < max;
         tok = strtok_r(NULL, " \t", &save))
        fields[n++] = tok;
    return n;
}

static void load_mounts(sys_state *st, const char *path)
{
    st->mount_text = kd_read_file(path, NULL);
    if (!st->mount_text)
        return;
    int cap = 0;
    char *save = NULL;
    for (char *line = strtok_r(st->mount_text, "\n", &save); line;
         line = strtok_r(NULL, "\n", &save)) {
        /* id parent maj:min root mountpoint opts [optional...] - fstype source superopts */
        char *f[32];
        int n = split_fields(line, f, 32);
        int dash = -1;
        for (int i = 6; i < n; i++)
            if (strcmp(f[i], "-") == 0) {
                dash = i;
                break;
            }
        unsigned ma, mi;
        if (n < 6 || dash < 0 || dash + 2 >= n || sscanf(f[2], "%u:%u", &ma, &mi) != 2)
            continue;
        if (st->n_mounts == cap) {
            cap = cap ? cap * 2 : 64;
            mount_entry *nm = realloc(st->mounts, (size_t)cap * sizeof *nm);
            if (!nm)
                return;
            st->mounts = nm;
        }
        mount_entry *m = &st->mounts[st->n_mounts++];
        m->major = ma;
        m->minor = mi;
        unescape_octal(f[3]);
        m->root = strcmp(f[3], "/") == 0;
        m->mountpoint = f[4];
        unescape_octal(m->mountpoint);
        m->source = f[dash + 2];
        unescape_octal(m->source);
    }
}

static void load_swaps(sys_state *st, const char *path)
{
    st->swap_text = kd_read_file(path, NULL);
    if (!st->swap_text)
        return;
    int cap = 0;
    char *save = NULL;
    bool header = true;
    for (char *line = strtok_r(st->swap_text, "\n", &save); line;
         line = strtok_r(NULL, "\n", &save)) {
        if (header) { /* "Filename Type Size Used Priority" */
            header = false;
            continue;
        }
        char *f[8];
        if (split_fields(line, f, 8) < 4)
            continue;
        if (st->n_swaps == cap) {
            cap = cap ? cap * 2 : 4;
            swap_entry *ns = realloc(st->swaps, (size_t)cap * sizeof *ns);
            if (!ns)
                return;
            st->swaps = ns;
        }
        swap_entry *s = &st->swaps[st->n_swaps++];
        unescape_octal(f[0]);
        s->path = f[0];
        s->size = strtoull(f[2], NULL, 10) * 1024;
        s->used = strtoull(f[3], NULL, 10) * 1024;
    }
}

static void sys_state_free(sys_state *st)
{
    free(st->mounts);
    free(st->swaps);
    free(st->mount_text);
    free(st->swap_text);
}

/* ---- filesystems ---------------------------------------------------------- */

typedef struct {
    const kd_sources *src;
    struct kd_pool *pool;
    sys_state st;
} scan_ctx;

/* Does a mount source or swap path name this device? */
static bool path_is_dev(const char *path, const char *name, const char *dm_name)
{
    if (strncmp(path, "/dev/", 5) != 0)
        return false;
    if (strcmp(path + 5, name) == 0)
        return true;
    return dm_name && *dm_name && strncmp(path + 5, "mapper/", 7) == 0 &&
           strcmp(path + 12, dm_name) == 0;
}

static void fill_fs(scan_ctx *c, kd_fs *fs, const uprops *up, const char *name,
                    const char *dm_name, unsigned major, unsigned minor)
{
    char buf[512];
    uprops_get(up, "ID_FS_TYPE", buf, sizeof buf);
    fs->type = pool_strdup(c->pool, buf);
    uprops_get_enc(up, "ID_FS_LABEL", buf, sizeof buf, false);
    fs->label = pool_strdup(c->pool, buf);
    uprops_get(up, "ID_FS_UUID", buf, sizeof buf);
    fs->uuid = pool_strdup(c->pool, buf);
    uprops_get(up, "ID_FS_VERSION", buf, sizeof buf);
    fs->version = pool_strdup(c->pool, buf);
    fs->mountpoint = "";

    /* The main mountpoint: the first that mounts the whole filesystem (a
     * bind of a subfolder only if there is nothing else). btrfs mounts show
     * an anonymous maj:min, hence the source check. */
    const mount_entry *best = NULL;
    for (int i = 0; i < c->st.n_mounts; i++) {
        const mount_entry *m = &c->st.mounts[i];
        if (!((m->major == major && m->minor == minor) || path_is_dev(m->source, name, dm_name)))
            continue;
        fs->n_mounts++;
        if (!best || (!best->root && m->root))
            best = m;
    }
    if (best) {
        fs->mountpoint = pool_strdup(c->pool, best->mountpoint);
        struct statvfs sv;
        int (*sfn)(const char *, struct statvfs *) =
            c->src->statvfs_fn ? c->src->statvfs_fn : statvfs;
        if (sfn(best->mountpoint, &sv) == 0 && sv.f_blocks) {
            uint64_t fr = sv.f_frsize ? sv.f_frsize : sv.f_bsize;
            fs->has_usage = true;
            fs->fs_size = (uint64_t)sv.f_blocks * fr;
            fs->fs_used = (uint64_t)(sv.f_blocks - sv.f_bfree) * fr;
            fs->fs_avail = (uint64_t)sv.f_bavail * fr;
        }
    }

    for (int i = 0; i < c->st.n_swaps; i++) {
        const swap_entry *s = &c->st.swaps[i];
        if (path_is_dev(s->path, name, dm_name)) {
            fs->swap_active = true;
            fs->has_usage = true;
            fs->fs_size = s->size;
            fs->fs_used = s->used;
            fs->fs_avail = s->size - s->used;
        }
    }
}

/* major:minor from <sys>/block/<path>/dev. */
static bool read_devnum(const scan_ctx *c, const char *path, unsigned *ma, unsigned *mi)
{
    char buf[32];
    return read_line(buf, sizeof buf, "%s/block/%s/dev", c->src->sys, path) &&
           sscanf(buf, "%u:%u", ma, mi) == 2;
}

/* ---- holders --------------------------------------------------------------- */

static int count_dir(const char *path)
{
    DIR *d = opendir(path);
    if (!d)
        return 0;
    int n = 0;
    for (struct dirent *e; (e = readdir(d));)
        if (e->d_name[0] != '.')
            n++;
    closedir(d);
    return n;
}

static int cmp_holder(const void *a, const void *b)
{
    return strcmp(((const kd_holder *)a)->name, ((const kd_holder *)b)->name);
}

/* path: "sda/sda1" or "sda", relative to <sys>/block. */
static void fill_holders(scan_ctx *c, const char *path, kd_holder **out, int *n_out)
{
    char dir[KD_PATH_MAX];
    *out = NULL;
    *n_out = 0;
    if (!kd_pathf(dir, sizeof dir, "%s/block/%s/holders", c->src->sys, path))
        return;
    int n = count_dir(dir);
    if (!n)
        return;
    DIR *d = opendir(dir);
    if (!d)
        return;
    kd_holder *hs = pool_alloc(c->pool, (size_t)n * sizeof *hs);
    int k = 0;
    for (struct dirent *e; hs && k < n && (e = readdir(d));) {
        if (e->d_name[0] == '.')
            continue;
        kd_holder *h = &hs[k];
        h->name = pool_strdup(c->pool, e->d_name);
        char buf[512];
        read_line(buf, sizeof buf, "%s/block/%s/dm/name", c->src->sys, e->d_name);
        h->dm_name = pool_strdup(c->pool, buf);
        read_line(buf, sizeof buf, "%s/block/%s/dm/uuid", c->src->sys, e->d_name);
        if (strncmp(buf, "CRYPT-", 6) == 0)
            h->kind = KD_HOLDER_CRYPT;
        else if (strncmp(buf, "LVM-", 4) == 0)
            h->kind = KD_HOLDER_LVM;
        else if (strncmp(e->d_name, "md", 2) == 0)
            h->kind = KD_HOLDER_RAID;
        else
            h->kind = KD_HOLDER_OTHER;
        h->size = read_u64("%s/block/%s/size", c->src->sys, e->d_name) * 512;
        unsigned ma = 0, mi = 0;
        read_devnum(c, e->d_name, &ma, &mi);
        uprops up = uprops_load(c->src, ma, mi);
        fill_fs(c, &h->fs, &up, e->d_name, h->dm_name, ma, mi);
        free(up.text);
        k++;
    }
    closedir(d);
    if (k > 1)
        qsort(hs, (size_t)k, sizeof *hs, cmp_holder);
    *out = hs;
    *n_out = k;
}

/* ---- partitions ------------------------------------------------------------ */

static bool is_extended_type(const char *type)
{
    return strcmp(type, "0x5") == 0 || strcmp(type, "0xf") == 0 || strcmp(type, "0x85") == 0;
}

static int cmp_part(const void *a, const void *b)
{
    const kd_part *pa = a, *pb = b;
    if (pa->start != pb->start)
        return pa->start < pb->start ? -1 : 1;
    /* An extended partition and its first gap can't share a start; keep the
     * container first anyway. */
    return (int)pa->in_extended - (int)pb->in_extended;
}

typedef struct {
    kd_part *v;
    int n, cap;
} part_vec;

static kd_part *part_push(part_vec *pv)
{
    if (pv->n == pv->cap) {
        int cap = pv->cap ? pv->cap * 2 : 8;
        kd_part *nv = realloc(pv->v, (size_t)cap * sizeof *nv);
        if (!nv)
            return NULL;
        pv->v = nv;
        pv->cap = cap;
    }
    kd_part *p = &pv->v[pv->n++];
    memset(p, 0, sizeof *p);
    return p;
}

static void add_gap(part_vec *pv, uint64_t from, uint64_t to, bool in_ext)
{
    if (to <= from || to - from <= MIN_GAP)
        return;
    kd_part *p = part_push(pv);
    if (!p)
        return;
    p->kind = KD_PART_FREE;
    p->name = p->type = p->pname = p->puuid = p->flags = "";
    p->fs.type = p->fs.label = p->fs.uuid = p->fs.version = p->fs.mountpoint = "";
    p->start = from;
    p->size = to - from;
    p->in_extended = in_ext;
}

/* Gaps between the partitions: top level over the usable area of the
 * table, and inside the extended one. pv is sorted on entry. */
static void add_free_space(part_vec *pv, const kd_disk *disk)
{
    uint64_t ls = disk->logical_sector ? disk->logical_sector : 512;
    uint64_t first = ls, last = disk->size; /* MBR: sector 0 */
    if (strcmp(disk->table, "gpt") == 0) {
        /* Header and 16 KiB of entries at the start, a copy at the end. */
        first = (2 + 16384 / ls) * ls;
        uint64_t tail = (1 + 16384 / ls) * ls;
        last = disk->size > tail ? disk->size - tail : 0;
    }

    int n = pv->n;
    uint64_t pos = first;
    const kd_part *ext = NULL;
    for (int i = 0; i < n; i++) {
        const kd_part *p = &pv->v[i];
        if (p->in_extended)
            continue;
        add_gap(pv, pos, p->start, false);
        if (p->start + p->size > pos)
            pos = p->start + p->size;
        if (p->kind == KD_PART_EXTENDED)
            ext = p;
    }
    add_gap(pv, pos, last, false);

    if (ext) {
        /* Each logical partition has its EBR right before it: those
         * sectors are below MIN_GAP and vanish. */
        uint64_t epos = ext->start, eend = ext->start + ext->size;
        for (int i = 0; i < n; i++) {
            const kd_part *p = &pv->v[i];
            if (!p->in_extended)
                continue;
            add_gap(pv, epos, p->start, true);
            if (p->start + p->size > epos)
                epos = p->start + p->size;
        }
        add_gap(pv, epos, eend, true);
    }
}

static void scan_parts(scan_ctx *c, kd_disk *disk)
{
    char dir[KD_PATH_MAX];
    if (!kd_pathf(dir, sizeof dir, "%s/block/%s", c->src->sys, disk->name))
        return;
    DIR *d = opendir(dir);
    if (!d)
        return;
    part_vec pv = {0};
    bool dos = strcmp(disk->table, "dos") == 0;
    for (struct dirent *e; (e = readdir(d));) {
        if (e->d_name[0] == '.')
            continue;
        char pnum[32];
        if (!read_line(pnum, sizeof pnum, "%s/%s/partition", dir, e->d_name))
            continue;
        char rel[KD_PATH_MAX];
        if (!kd_pathf(rel, sizeof rel, "%s/%s", disk->name, e->d_name))
            continue;
        kd_part *p = part_push(&pv);
        if (!p)
            break;
        p->name = pool_strdup(c->pool, e->d_name);
        p->number = atoi(pnum);
        p->start = read_u64("%s/%s/start", dir, e->d_name) * 512;
        p->size = read_u64("%s/%s/size", dir, e->d_name) * 512;

        unsigned ma = 0, mi = 0;
        read_devnum(c, rel, &ma, &mi);
        uprops up = uprops_load(c->src, ma, mi);
        char buf[512];
        uprops_get(&up, "ID_PART_ENTRY_TYPE", buf, sizeof buf);
        p->type = pool_strdup(c->pool, buf);
        uprops_get_enc(&up, "ID_PART_ENTRY_NAME", buf, sizeof buf, false);
        p->pname = pool_strdup(c->pool, buf);
        uprops_get(&up, "ID_PART_ENTRY_UUID", buf, sizeof buf);
        p->puuid = pool_strdup(c->pool, buf);
        uprops_get(&up, "ID_PART_ENTRY_FLAGS", buf, sizeof buf);
        p->flags = pool_strdup(c->pool, buf);

        if (dos && p->number <= 4 && is_extended_type(p->type)) {
            p->kind = KD_PART_EXTENDED;
            /* sysfs gives the 1 KiB the kernel exposes; the real extent is
             * in the partition entry. */
            if (uprops_get(&up, "ID_PART_ENTRY_SIZE", buf, sizeof buf))
                p->size = strtoull(buf, NULL, 10) * 512;
        } else if (dos && p->number >= 5) {
            p->kind = KD_PART_LOGICAL;
            p->in_extended = true;
        }
        if (p->kind == KD_PART_EXTENDED) {
            /* Its "filesystem" is the partition table inside it: not shown. */
            p->fs.type = p->fs.label = p->fs.uuid = p->fs.version = p->fs.mountpoint = "";
        } else {
            fill_fs(c, &p->fs, &up, e->d_name, NULL, ma, mi);
        }
        free(up.text);
        fill_holders(c, rel, &p->holders, &p->n_holders);
    }
    closedir(d);

    /* qsort wants a valid pointer even for 0 elements. */
    if (*disk->table) {
        if (pv.n > 1)
            qsort(pv.v, (size_t)pv.n, sizeof *pv.v, cmp_part);
        add_free_space(&pv, disk);
    }
    if (pv.n > 1)
        qsort(pv.v, (size_t)pv.n, sizeof *pv.v, cmp_part);

    if (pv.n) {
        disk->parts = pool_alloc(c->pool, (size_t)pv.n * sizeof *disk->parts);
        if (disk->parts) {
            memcpy(disk->parts, pv.v, (size_t)pv.n * sizeof *pv.v);
            disk->n_parts = pv.n;
        }
    }
    free(pv.v);
}

/* ---- disks ------------------------------------------------------------------- */

static kd_bus guess_bus(const char *name, const uprops *up)
{
    char buf[64];
    if (strncmp(name, "nvme", 4) == 0)
        return KD_BUS_NVME;
    if (strncmp(name, "mmcblk", 6) == 0)
        return KD_BUS_MMC;
    if (strncmp(name, "loop", 4) == 0)
        return KD_BUS_LOOP;
    if (strncmp(name, "md", 2) == 0)
        return KD_BUS_RAID;
    if (strncmp(name, "zram", 4) == 0 || strncmp(name, "ram", 3) == 0)
        return KD_BUS_RAM;
    if (strncmp(name, "vd", 2) == 0)
        return KD_BUS_VIRTIO;
    if (strncmp(name, "sr", 2) == 0 || (uprops_get(up, "ID_CDROM", buf, sizeof buf) &&
                                        strcmp(buf, "1") == 0))
        return KD_BUS_OPTICAL;
    uprops_get(up, "ID_BUS", buf, sizeof buf);
    if (strcmp(buf, "usb") == 0)
        return KD_BUS_USB;
    if (strcmp(buf, "ata") == 0)
        return KD_BUS_SATA;
    if (strcmp(buf, "scsi") == 0)
        return KD_BUS_SCSI;
    return KD_BUS_UNKNOWN;
}

static kd_media guess_media(kd_bus bus, bool rotational, const uprops *up)
{
    char rpm[32];
    switch (bus) {
    case KD_BUS_NVME:
    case KD_BUS_MMC:
        return KD_MEDIA_SSD;
    case KD_BUS_SATA:
        /* The drive's own IDENTIFY data: 0 = solid state. */
        if (uprops_get(up, "ID_ATA_ROTATION_RATE_RPM", rpm, sizeof rpm) && *rpm)
            return atoi(rpm) > 0 ? KD_MEDIA_HDD : KD_MEDIA_SSD;
        return rotational ? KD_MEDIA_HDD : KD_MEDIA_SSD;
    case KD_BUS_SCSI:
    case KD_BUS_VIRTIO:
    case KD_BUS_UNKNOWN:
        /* Only "not rotational" is worth believing here. */
        return rotational ? KD_MEDIA_UNKNOWN : KD_MEDIA_SSD;
    case KD_BUS_USB: /* bridges say "rotational" even for flash */
    case KD_BUS_LOOP:
    case KD_BUS_RAID:
    case KD_BUS_OPTICAL:
    case KD_BUS_RAM:
        break;
    }
    return KD_MEDIA_UNKNOWN;
}

const char *kd_bus_name(kd_bus bus)
{
    switch (bus) {
    case KD_BUS_SATA: return "SATA";
    case KD_BUS_SCSI: return "SCSI";
    case KD_BUS_USB: return "USB";
    case KD_BUS_NVME: return "NVMe";
    case KD_BUS_MMC: return "SD/MMC";
    case KD_BUS_VIRTIO: return "VirtIO";
    case KD_BUS_LOOP: return "loop";
    case KD_BUS_RAID: return "RAID";
    case KD_BUS_OPTICAL: return "CD/DVD";
    case KD_BUS_RAM: return "RAM";
    case KD_BUS_UNKNOWN: break;
    }
    return "";
}

static bool starts_with_ci(const char *s, const char *prefix)
{
    size_t n = strlen(prefix);
    return n && strncasecmp(s, prefix, n) == 0;
}

static void fill_model(scan_ctx *c, kd_disk *disk, const uprops *up)
{
    char model[256], vendor[128];
    uprops_get_enc(up, "ID_MODEL", model, sizeof model, true);
    if (!*model)
        read_line(model, sizeof model, "%s/block/%s/device/model", c->src->sys, disk->name);
    uprops_get_enc(up, "ID_VENDOR", vendor, sizeof vendor, true);
    if (!*vendor)
        read_line(vendor, sizeof vendor, "%s/block/%s/device/vendor", c->src->sys, disk->name);
    /* "ATA" is the bus, not who made it. */
    if (strcmp(vendor, "ATA") == 0 || starts_with_ci(model, vendor))
        vendor[0] = '\0';
    char full[400];
    snprintf(full, sizeof full, "%s%s%s", vendor, *vendor && *model ? " " : "", model);
    disk->model = pool_strdup(c->pool, full);

    char buf[256];
    uprops_get(up, "ID_SERIAL_SHORT", buf, sizeof buf);
    if (!*buf)
        read_line(buf, sizeof buf, "%s/block/%s/device/serial", c->src->sys, disk->name);
    disk->serial = pool_strdup(c->pool, buf);
    uprops_get(up, "ID_REVISION", buf, sizeof buf);
    if (!*buf)
        read_line(buf, sizeof buf, "%s/block/%s/device/firmware_rev", c->src->sys, disk->name);
    disk->revision = pool_strdup(c->pool, buf);
}

bool kd_mount_is_system(const char *mp)
{
    static const char *const sys[] = {"/", "/home", "/boot", "/boot/efi", "/usr", "/var", NULL};
    for (int i = 0; sys[i]; i++)
        if (strcmp(mp, sys[i]) == 0)
            return true;
    return false;
}

static bool fs_is_system(const kd_fs *fs, const kd_holder *hs, int nh)
{
    if (kd_mount_is_system(fs->mountpoint) || fs->swap_active)
        return true;
    for (int i = 0; i < nh; i++)
        if (kd_mount_is_system(hs[i].fs.mountpoint) || hs[i].fs.swap_active)
            return true;
    return false;
}

/* false = not a disk to list (dm, partitions, empty loops...). */
static bool scan_disk(scan_ctx *c, const char *name, kd_disk *disk)
{
    if (strncmp(name, "dm-", 3) == 0) /* shown under what they sit on */
        return false;
    unsigned ma, mi;
    if (!read_devnum(c, name, &ma, &mi))
        return false;
    disk->name = pool_strdup(c->pool, name);
    disk->major = ma;
    disk->minor = mi;
    disk->size = read_u64("%s/block/%s/size", c->src->sys, name) * 512;
    disk->removable = read_u64("%s/block/%s/removable", c->src->sys, name) != 0;
    disk->readonly = read_u64("%s/block/%s/ro", c->src->sys, name) != 0;
    bool rotational = read_u64("%s/block/%s/queue/rotational", c->src->sys, name) != 0;
    disk->logical_sector =
        (unsigned)read_u64("%s/block/%s/queue/logical_block_size", c->src->sys, name);
    disk->physical_sector =
        (unsigned)read_u64("%s/block/%s/queue/physical_block_size", c->src->sys, name);
    if (!disk->logical_sector)
        disk->logical_sector = 512;
    if (!disk->physical_sector)
        disk->physical_sector = disk->logical_sector;

    uprops up = uprops_load(c->src, ma, mi);
    disk->bus = guess_bus(name, &up);
    if (disk->size == 0 && !disk->removable && disk->bus != KD_BUS_OPTICAL) {
        free(up.text); /* unused loop, nbd... */
        return false;
    }
    disk->no_media = disk->size == 0;
    disk->hidden = disk->bus == KD_BUS_LOOP || disk->bus == KD_BUS_RAM;
    disk->media = guess_media(disk->bus, rotational, &up);
    fill_model(c, disk, &up);

    char buf[KD_PATH_MAX];
    uprops_get(&up, "ID_PART_TABLE_TYPE", buf, sizeof buf);
    disk->table = pool_strdup(c->pool, buf);
    uprops_get(&up, "ID_PART_TABLE_UUID", buf, sizeof buf);
    disk->table_uuid = pool_strdup(c->pool, buf);
    read_line(buf, sizeof buf, "%s/block/%s/loop/backing_file", c->src->sys, name);
    disk->loop_file = pool_strdup(c->pool, buf);
    fill_fs(c, &disk->fs, &up, name, NULL, ma, mi);
    free(up.text);
    /* A table hides any stray filesystem signature blkid may also see. */
    if (*disk->table)
        disk->fs.type = "";

    fill_holders(c, name, &disk->holders, &disk->n_holders);
    scan_parts(c, disk);

    /* A blank disk: all of it is free. */
    if (!*disk->table && !*disk->fs.type && !disk->n_holders && !disk->n_parts &&
        !disk->no_media && disk->bus != KD_BUS_OPTICAL) {
        part_vec pv = {0};
        add_gap(&pv, 0, disk->size, false);
        if (pv.n) {
            disk->parts = pool_alloc(c->pool, sizeof *disk->parts);
            if (disk->parts) {
                disk->parts[0] = pv.v[0];
                disk->n_parts = 1;
            }
        }
        free(pv.v);
    }

    disk->system = fs_is_system(&disk->fs, disk->holders, disk->n_holders);
    for (int i = 0; i < disk->n_parts && !disk->system; i++)
        disk->system = fs_is_system(&disk->parts[i].fs, disk->parts[i].holders,
                                    disk->parts[i].n_holders);
    return true;
}

/* Natural order: letter runs by length then alphabetically (sdz < sdaa, as
 * the kernel names them), digit runs by value (loop2 < loop10). */
static int natural_cmp(const char *a, const char *b)
{
    while (*a && *b) {
        bool da = isdigit((unsigned char)*a), db = isdigit((unsigned char)*b);
        size_t la = 0, lb = 0;
        while (a[la] && (bool)isdigit((unsigned char)a[la]) == da)
            la++;
        while (b[lb] && (bool)isdigit((unsigned char)b[lb]) == db)
            lb++;
        if (da && db) {
            unsigned long long va = strtoull(a, NULL, 10), vb = strtoull(b, NULL, 10);
            if (va != vb)
                return va < vb ? -1 : 1;
        } else if (da != db) {
            return da ? -1 : 1;
        } else {
            if (la != lb)
                return la < lb ? -1 : 1;
            int r = strncmp(a, b, la);
            if (r)
                return r;
        }
        a += la;
        b += lb;
    }
    return (unsigned char)*a - (unsigned char)*b;
}

static int cmp_disk(const void *a, const void *b)
{
    return natural_cmp(((const kd_disk *)a)->name, ((const kd_disk *)b)->name);
}

kd_disks *kd_disks_scan(const kd_sources *in)
{
    kd_sources src = {"/sys", "/run/udev/data", "/proc/self/mountinfo", "/proc/swaps", NULL};
    if (in) {
        if (in->sys)
            src.sys = in->sys;
        if (in->udev_db)
            src.udev_db = in->udev_db;
        if (in->mountinfo)
            src.mountinfo = in->mountinfo;
        if (in->swaps)
            src.swaps = in->swaps;
        src.statvfs_fn = in->statvfs_fn;
    }

    kd_disks *d = calloc(1, sizeof *d);
    if (!d)
        return NULL;
    d->pool = calloc(1, sizeof *d->pool);
    if (!d->pool) {
        free(d);
        return NULL;
    }
    scan_ctx c = {.src = &src, .pool = d->pool};
    load_mounts(&c.st, src.mountinfo);
    load_swaps(&c.st, src.swaps);

    char dir[KD_PATH_MAX];
    DIR *dd = kd_pathf(dir, sizeof dir, "%s/block", src.sys) ? opendir(dir) : NULL;
    int cap = 0;
    for (struct dirent *e; dd && (e = readdir(dd));) {
        if (e->d_name[0] == '.')
            continue;
        if (d->n == cap) {
            cap = cap ? cap * 2 : 16;
            kd_disk *nd = realloc(d->disks, (size_t)cap * sizeof *nd);
            if (!nd)
                break;
            d->disks = nd;
        }
        kd_disk *disk = &d->disks[d->n];
        memset(disk, 0, sizeof *disk);
        if (scan_disk(&c, e->d_name, disk))
            d->n++;
    }
    if (dd)
        closedir(dd);
    sys_state_free(&c.st);
    if (d->n)
        qsort(d->disks, (size_t)d->n, sizeof *d->disks, cmp_disk);
    return d;
}

void kd_disks_free(kd_disks *d)
{
    if (!d)
        return;
    free(d->disks);
    pool_free(d->pool);
    free(d);
}

const kd_disk *kd_disks_find(const kd_disks *d, const char *name)
{
    for (int i = 0; d && i < d->n; i++)
        if (strcmp(d->disks[i].name, name) == 0)
            return &d->disks[i];
    return NULL;
}

static const kd_fs *find_in_holders(const kd_holder *hs, int n, const char *name)
{
    for (int i = 0; i < n; i++)
        if (strcmp(hs[i].name, name) == 0)
            return &hs[i].fs;
    return NULL;
}

const kd_fs *kd_disks_find_fs(const kd_disks *d, const char *name)
{
    for (int i = 0; d && i < d->n; i++) {
        const kd_disk *k = &d->disks[i];
        if (strcmp(k->name, name) == 0)
            return &k->fs;
        const kd_fs *fs = find_in_holders(k->holders, k->n_holders, name);
        if (fs)
            return fs;
        for (int j = 0; j < k->n_parts; j++) {
            const kd_part *p = &k->parts[j];
            if (p->kind != KD_PART_FREE && strcmp(p->name, name) == 0)
                return &p->fs;
            fs = find_in_holders(p->holders, p->n_holders, name);
            if (fs)
                return fs;
        }
    }
    return NULL;
}

bool kd_fs_mountable(const kd_fs *fs)
{
    static const char *const no[] = {"swap", "crypto_LUKS", "LVM2_member", "linux_raid_member",
                                     "zfs_member", "bcache", NULL};
    if (!*fs->type)
        return false;
    for (int i = 0; no[i]; i++)
        if (strcmp(fs->type, no[i]) == 0)
            return false;
    return true;
}

/* ---- layout key -------------------------------------------------------------- */

typedef struct {
    char *s;
    size_t len, cap;
    bool failed;
} sbuf;

static void sb_printf(sbuf *b, const char *fmt, ...) __attribute__((format(printf, 2, 3)));

static void sb_printf(sbuf *b, const char *fmt, ...)
{
    if (b->failed)
        return;
    for (;;) {
        va_list ap;
        va_start(ap, fmt);
        int n = vsnprintf(b->s ? b->s + b->len : NULL, b->s ? b->cap - b->len : 0, fmt, ap);
        va_end(ap);
        if (n < 0) {
            b->failed = true;
            return;
        }
        if (b->s && b->len + (size_t)n < b->cap) {
            b->len += (size_t)n;
            return;
        }
        size_t cap = (b->cap + (size_t)n + 1) * 2;
        char *ns = realloc(b->s, cap);
        if (!ns) {
            b->failed = true;
            return;
        }
        b->s = ns;
        b->cap = cap;
    }
}

static void key_fs(sbuf *b, const kd_fs *fs)
{
    sb_printf(b, "|%s|%s|%s|%d|%d", fs->type, fs->label, fs->mountpoint, fs->swap_active,
              fs->has_usage);
}

static void key_holders(sbuf *b, const kd_holder *hs, int n)
{
    for (int i = 0; i < n; i++) {
        sb_printf(b, "<%s|%s", hs[i].name, hs[i].dm_name);
        key_fs(b, &hs[i].fs);
        sb_printf(b, ">");
    }
}

char *kd_disks_layout_key(const kd_disks *d)
{
    sbuf b = {0};
    for (int i = 0; i < d->n; i++) {
        const kd_disk *k = &d->disks[i];
        sb_printf(&b, "[%s|%s|%llu|%s|%d|%d", k->name, k->model, (unsigned long long)k->size,
                  k->table, k->no_media, k->media);
        key_fs(&b, &k->fs);
        key_holders(&b, k->holders, k->n_holders);
        for (int j = 0; j < k->n_parts; j++) {
            const kd_part *p = &k->parts[j];
            sb_printf(&b, "{%d|%s|%llu|%llu", p->kind, p->name, (unsigned long long)p->start,
                      (unsigned long long)p->size);
            key_fs(&b, &p->fs);
            key_holders(&b, p->holders, p->n_holders);
            sb_printf(&b, "}");
        }
        sb_printf(&b, "]");
    }
    if (b.failed || !b.s) {
        free(b.s);
        return calloc(1, 1);
    }
    return b.s;
}

/* ---- text output --------------------------------------------------------------- */

static void fmt_size(char *out, size_t n, uint64_t v, bool bytes)
{
    if (bytes)
        snprintf(out, n, "%llu", (unsigned long long)v);
    else
        kd_format_size(out, n, v);
}

static void print_fs(FILE *f, const kd_fs *fs, bool bytes)
{
    if (*fs->type)
        fprintf(f, "  %s", fs->type);
    if (*fs->label)
        fprintf(f, "  \"%s\"", fs->label);
    if (*fs->mountpoint)
        fprintf(f, "  %s", fs->mountpoint);
    if (fs->swap_active)
        fprintf(f, "  %s", _("[swap on]"));
    if (fs->has_usage) {
        char used[32], total[32];
        fmt_size(used, sizeof used, fs->fs_used, bytes);
        fmt_size(total, sizeof total, fs->fs_size, bytes);
        fputs("  ", f);
        /* TRANSLATORS: space used in a filesystem: "31.2 GiB used of 45.9 GiB" */
        fprintf(f, _("%s used of %s"), used, total);
    }
}

static void print_holders(FILE *f, const kd_holder *hs, int n, const char *indent, bool bytes)
{
    for (int i = 0; i < n; i++) {
        char size[32];
        fmt_size(size, sizeof size, hs[i].size, bytes);
        fprintf(f, "%s-> %s%s%s%s  %s", indent, hs[i].name, *hs[i].dm_name ? " (" : "",
                hs[i].dm_name, *hs[i].dm_name ? ")" : "", size);
        print_fs(f, &hs[i].fs, bytes);
        fputc('\n', f);
    }
}

void kd_disks_print(const kd_disks *d, FILE *f, bool bytes, bool show_hidden)
{
    for (int i = 0; i < d->n; i++) {
        const kd_disk *k = &d->disks[i];
        if (k->hidden && !show_hidden)
            continue;
        char size[64];
        if (bytes)
            fmt_size(size, sizeof size, k->size, true);
        else
            kd_format_disk_size(size, sizeof size, k->size);
        fprintf(f, "%s  %s", k->name, *k->model ? k->model : "?");
        const char *bus = kd_bus_name(k->bus);
        const char *media = k->media == KD_MEDIA_HDD   ? _("HDD")
                            : k->media == KD_MEDIA_SSD ? _("SSD")
                                                       : "";
        fprintf(f, "  [%s%s%s]", bus, *bus && *media ? ", " : "", media);
        if (!k->no_media)
            fprintf(f, "  %s", size);
        if (*k->table)
            fprintf(f, "  %s", strcmp(k->table, "dos") == 0 ? "MBR" : k->table);
        if (k->logical_sector != 512 || k->physical_sector != 512)
            fprintf(f, "  %u/%u B", k->logical_sector, k->physical_sector);
        if (*k->serial)
            fprintf(f, "  %s %s", _("serial"), k->serial);
        if (*k->loop_file)
            fprintf(f, "  <- %s", k->loop_file);
        if (k->no_media)
            fprintf(f, "  %s", _("(no media)"));
        if (k->system)
            fprintf(f, "  %s", _("[system]"));
        print_fs(f, &k->fs, bytes);
        fputc('\n', f);
        print_holders(f, k->holders, k->n_holders, "  ", bytes);

        for (int j = 0; j < k->n_parts; j++) {
            const kd_part *p = &k->parts[j];
            const char *indent = p->in_extended ? "    " : "  ";
            char psize[32], pstart[32];
            fmt_size(psize, sizeof psize, p->size, bytes);
            if (p->kind == KD_PART_FREE) {
                fprintf(f, "%s%-10s %s", indent, _("(free)"), psize);
            } else {
                fprintf(f, "%s%-10s %s", indent, p->name, psize);
                if (p->kind == KD_PART_EXTENDED)
                    fprintf(f, "  %s", _("extended"));
            }
            if (bytes) {
                fmt_size(pstart, sizeof pstart, p->start, true);
                fprintf(f, "  @%s", pstart);
            }
            print_fs(f, &p->fs, bytes);
            fputc('\n', f);
            print_holders(f, p->holders, p->n_holders, p->in_extended ? "      " : "    ", bytes);
        }
    }
}
