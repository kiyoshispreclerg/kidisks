/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef KD_DISKS_H
#define KD_DISKS_H

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/statvfs.h>

/* The disks of the machine, read without root:
 *
 *   /sys/block/<disk>/...          sizes, partitions (start/size), sectors,
 *                                  rotational, removable, holders (LUKS/LVM)
 *   /run/udev/data/b<maj>:<min>    filesystem, label, UUID, partition type,
 *                                  table type, model, serial, bus
 *   /proc/self/mountinfo           where each one is mounted
 *   /proc/swaps                    active swap
 *   statvfs(mountpoint)            used and free space
 *
 * The udev database is read directly (its "E:KEY=VALUE" lines), not through
 * libudev, so tests can point every source at fixtures (kd_sources).
 *
 * Every size and offset is in bytes. sysfs counts in 512-byte units whatever
 * the logical sector size; that is converted here.
 *
 * Memory: kd_disks owns everything in it (arrays and strings); one
 * kd_disks_free releases it all. Strings are never NULL ("" when absent)
 * unless the comment says so. */

typedef enum {
    KD_BUS_UNKNOWN,
    KD_BUS_SATA,   /* also PATA: ID_BUS=ata */
    KD_BUS_SCSI,
    KD_BUS_USB,
    KD_BUS_NVME,
    KD_BUS_MMC,    /* SD cards, eMMC */
    KD_BUS_VIRTIO,
    KD_BUS_LOOP,
    KD_BUS_RAID,   /* md */
    KD_BUS_OPTICAL,
    KD_BUS_RAM,    /* zram, ram */
} kd_bus;

/* Spinning or not. USB bridges claim to spin even for flash drives, so
 * only trusted sources decide (see disks.c). */
typedef enum {
    KD_MEDIA_UNKNOWN,
    KD_MEDIA_HDD,
    KD_MEDIA_SSD,
} kd_media;

/* A filesystem (or swap, or LUKS container...) and where it is in use. */
typedef struct {
    const char *type;       /* ID_FS_TYPE: "ext4", "vfat", "swap", "crypto_LUKS", "" */
    const char *label;
    const char *uuid;
    const char *version;
    const char *mountpoint; /* the main one; "" if not mounted */
    int n_mounts;           /* bind mounts and the like count too */
    bool swap_active;
    /* From statvfs while mounted, or /proc/swaps while swap is on. */
    bool has_usage;
    uint64_t fs_size, fs_used, fs_avail;
} kd_fs;

typedef enum {
    KD_HOLDER_CRYPT, /* an open LUKS (dm-crypt) */
    KD_HOLDER_LVM,   /* a logical volume on this PV */
    KD_HOLDER_RAID,  /* an md array */
    KD_HOLDER_OTHER,
} kd_holder_kind;

/* Something built on top of a partition or disk (/sys/.../holders). */
typedef struct {
    const char *name;    /* "dm-0", "md0" */
    const char *dm_name; /* "luks-a81f...", "vg-root"; "" if not dm */
    kd_holder_kind kind;
    uint64_t size;
    kd_fs fs;
} kd_holder;

typedef enum {
    KD_PART_NORMAL,
    KD_PART_EXTENDED, /* MBR container of the logical ones */
    KD_PART_LOGICAL,
    KD_PART_FREE,     /* unallocated gap (>= 1 MiB) */
} kd_part_kind;

typedef struct {
    kd_part_kind kind;
    const char *name; /* "sda1"; "" for free space */
    int number;       /* 0 for free space */
    bool in_extended; /* logical partitions, and free space between them */
    uint64_t start, size;
    const char *type;  /* ID_PART_ENTRY_TYPE: "0x83", a GPT GUID */
    const char *pname; /* GPT partition name */
    const char *puuid;
    const char *flags; /* ID_PART_ENTRY_FLAGS: "0x80"... */
    kd_fs fs;
    kd_holder *holders;
    int n_holders;
} kd_part;

typedef struct {
    const char *name; /* "sda" */
    unsigned major, minor;
    const char *model;  /* "SanDisk SSD PLUS 120 GB"; vendor prefixed when missing */
    const char *serial;
    const char *revision;
    kd_bus bus;
    kd_media media;
    bool removable;
    bool readonly;
    bool no_media;  /* a card reader slot or drive with nothing in it */
    bool hidden;    /* loop, zram, ram: shown only on request */
    bool system;    /* holds /, /home, /boot..., or active swap */
    uint64_t size;
    unsigned logical_sector, physical_sector;
    const char *table;      /* "dos", "gpt"; "" without a partition table */
    const char *table_uuid;
    const char *loop_file;  /* backing file of a loop device */
    kd_fs fs;               /* a filesystem straight on the disk */
    kd_holder *holders;
    int n_holders;
    kd_part *parts; /* by start, with the free gaps; logical after their extended */
    int n_parts;
} kd_disk;

typedef struct kd_disks kd_disks;
struct kd_disks {
    kd_disk *disks; /* sorted by name, naturally (sda, sdb, sdaa; nvme0n1, nvme1n1) */
    int n;
    struct kd_pool *pool;
};

/* Where to read from. Any NULL field means the real one. */
typedef struct {
    const char *sys;       /* "/sys" */
    const char *udev_db;   /* "/run/udev/data" */
    const char *mountinfo; /* "/proc/self/mountinfo" */
    const char *swaps;     /* "/proc/swaps" */
    int (*statvfs_fn)(const char *path, struct statvfs *out);
} kd_sources;

/* Reads everything. src NULL = the real system. NULL only if out of memory
 * (an unreadable /sys gives an empty list). */
kd_disks *kd_disks_scan(const kd_sources *src);
void kd_disks_free(kd_disks *d);

const kd_disk *kd_disks_find(const kd_disks *d, const char *name);

/* The filesystem of a disk, partition or holder by its name ("sda",
 * "sda1", "dm-0"); NULL if there is no such device. */
const kd_fs *kd_disks_find_fs(const kd_disks *d, const char *name);

/* Can a filesystem of this type be mounted? (Not swap, LUKS, LVM, RAID
 * members or nothing.) */
bool kd_fs_mountable(const kd_fs *fs);

/* /, /home, /boot, /boot/efi, /usr, /var: not offered for unmounting, and
 * what makes a disk a "system" one. */
bool kd_mount_is_system(const char *mountpoint);

/* "SATA", "USB", "NVMe"... ("" for unknown). Not translated: they are names. */
const char *kd_bus_name(kd_bus bus);

/* A string that changes whenever what the Units tab shows changes, except
 * the used/free numbers (and SMART, which comes from elsewhere). Lets the UI
 * update in place instead of rebuilding. The caller frees it. */
char *kd_disks_layout_key(const kd_disks *d);

/* The model as text, for `kidisks --list`. bytes: sizes as plain byte
 * counts (for scripts and for comparing with lsblk -b). */
void kd_disks_print(const kd_disks *d, FILE *f, bool bytes, bool show_hidden);

#endif
