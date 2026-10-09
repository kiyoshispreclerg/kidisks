/* SPDX-License-Identifier: GPL-3.0-or-later */
/* The disk model, on tests/fixtures/sys1 (made by tools/gen_fixtures.py). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../harness.h"
#include "disks.h"

#define MIB (1024ULL * 1024ULL)
#define GIB (1024ULL * MIB)
#define FX KD_FIXTURES_DIR "/sys1"

/* Every mount: 1000 blocks of 4 KiB, 250 free, 200 available; /srv fails. */
static int fake_statvfs(const char *path, struct statvfs *out)
{
    if (strcmp(path, "/srv") == 0)
        return -1;
    memset(out, 0, sizeof *out);
    out->f_frsize = 4096;
    out->f_bsize = 4096;
    out->f_blocks = 1000;
    out->f_bfree = 250;
    out->f_bavail = 200;
    return 0;
}

static const kd_sources fixture = {
    .sys = FX "/sys",
    .udev_db = FX "/udev",
    .mountinfo = FX "/mountinfo",
    .swaps = FX "/swaps",
    .statvfs_fn = fake_statvfs,
};

static kd_disks *scan(void)
{
    return kd_disks_scan(&fixture);
}

TEST(lists_disks_in_natural_order_without_dm_or_unused_loops)
{
    kd_disks *d = scan();
    CHECK(d);
    const char *want[] = {"sda", "sdb", "sdc", "sdd", "sde", "loop0", "nvme0n1", "zram0"};
    CHECK_INT_EQ(d->n, 8);
    for (int i = 0; i < 8; i++)
        CHECK_STR_EQ(d->disks[i].name, want[i]);
    CHECK(kd_disks_find(d, "loop1") == NULL);
    CHECK(kd_disks_find(d, "dm-0") == NULL);
    kd_disks_free(d);
}

TEST(mbr_with_extended_logical_swap_and_gaps)
{
    kd_disks *d = scan();
    CHECK(d);
    const kd_disk *k = kd_disks_find(d, "sda");
    CHECK(k);
    CHECK_STR_EQ(k->model, "Fake SSD 64GB");
    CHECK_STR_EQ(k->serial, "FAKE0001");
    CHECK_STR_EQ(k->table, "dos");
    CHECK_INT_EQ(k->bus, KD_BUS_SATA);
    /* sysfs says rotational; the drive's RPM (0) wins. */
    CHECK_INT_EQ(k->media, KD_MEDIA_SSD);
    CHECK(k->system);
    CHECK_INT_EQ(k->size, 64 * GIB);

    /* sda1, sda2 (ext), sda5, free in ext, sda6, free at the end. */
    CHECK_INT_EQ(k->n_parts, 6);
    const kd_part *p = k->parts;
    CHECK_STR_EQ(p[0].name, "sda1");
    CHECK_INT_EQ(p[0].start, MIB);
    CHECK_INT_EQ(p[0].size, 30 * GIB);
    CHECK_STR_EQ(p[0].fs.type, "ext4");
    CHECK_STR_EQ(p[0].fs.label, "root");
    CHECK_STR_EQ(p[0].fs.mountpoint, "/");
    CHECK_STR_EQ(p[0].flags, "0x80");
    CHECK(p[0].fs.has_usage);
    CHECK_INT_EQ(p[0].fs.fs_size, 4096000);
    CHECK_INT_EQ(p[0].fs.fs_used, 3072000);
    CHECK_INT_EQ(p[0].fs.fs_avail, 819200);

    /* The extended one: its real extent, not sysfs's 1 KiB. */
    CHECK_INT_EQ(p[1].kind, KD_PART_EXTENDED);
    CHECK_STR_EQ(p[1].name, "sda2");
    CHECK_INT_EQ(p[1].start, MIB + 30 * GIB);
    CHECK_INT_EQ(p[1].size, 33 * GIB - MIB);
    CHECK_STR_EQ(p[1].fs.type, "");

    /* No 1 MiB "free" before a logical partition: that's its EBR. */
    CHECK_INT_EQ(p[2].kind, KD_PART_LOGICAL);
    CHECK_STR_EQ(p[2].name, "sda5");
    CHECK(p[2].in_extended);
    CHECK(p[2].fs.swap_active);
    CHECK(p[2].fs.has_usage);
    CHECK_INT_EQ(p[2].fs.fs_size, 2097148ULL * 1024);
    CHECK_INT_EQ(p[2].fs.fs_used, 1024ULL * 1024);

    CHECK_INT_EQ(p[3].kind, KD_PART_FREE);
    CHECK(p[3].in_extended);
    CHECK_INT_EQ(p[3].size, 100 * MIB);
    CHECK_STR_EQ(p[3].name, "");

    CHECK_STR_EQ(p[4].name, "sda6");
    CHECK_STR_EQ(p[4].fs.type, "ntfs");
    CHECK_STR_EQ(p[4].fs.label, "Meus Dados");
    CHECK_STR_EQ(p[4].fs.mountpoint, "");
    CHECK(!p[4].fs.has_usage);

    CHECK_INT_EQ(p[5].kind, KD_PART_FREE);
    CHECK(!p[5].in_extended);
    CHECK_INT_EQ(p[5].start, 63 * GIB);
    CHECK_INT_EQ(p[5].size, 1 * GIB);
    kd_disks_free(d);
}

TEST(gpt_with_luks_and_lvm_holders)
{
    kd_disks *d = scan();
    CHECK(d);
    const kd_disk *k = kd_disks_find(d, "nvme0n1");
    CHECK(k);
    CHECK_INT_EQ(k->bus, KD_BUS_NVME);
    CHECK_INT_EQ(k->media, KD_MEDIA_SSD);
    CHECK_STR_EQ(k->table, "gpt");
    CHECK(k->system); /* /boot/efi, and /home through the LUKS */
    CHECK_INT_EQ(k->n_parts, 4);

    const kd_part *p = k->parts;
    CHECK_STR_EQ(p[0].pname, "EFI system");
    CHECK_STR_EQ(p[0].fs.mountpoint, "/boot/efi");

    CHECK_STR_EQ(p[1].fs.type, "crypto_LUKS");
    CHECK_STR_EQ(p[1].fs.mountpoint, "");
    CHECK_INT_EQ(p[1].n_holders, 1);
    const kd_holder *h = &p[1].holders[0];
    CHECK_STR_EQ(h->name, "dm-0");
    CHECK_STR_EQ(h->dm_name, "luks-home");
    CHECK_INT_EQ(h->kind, KD_HOLDER_CRYPT);
    CHECK_STR_EQ(h->fs.type, "ext4");
    CHECK_STR_EQ(h->fs.mountpoint, "/home");

    /* Two LVs, sorted; btrfs found by its /dev/mapper source although
     * mountinfo gives it an anonymous 0:45. */
    CHECK_INT_EQ(p[2].n_holders, 2);
    h = p[2].holders;
    CHECK_STR_EQ(h[0].name, "dm-1");
    CHECK_INT_EQ(h[0].kind, KD_HOLDER_LVM);
    CHECK_STR_EQ(h[0].fs.type, "btrfs");
    CHECK_STR_EQ(h[0].fs.mountpoint, "/srv");
    CHECK_INT_EQ(h[0].fs.n_mounts, 2);
    CHECK(!h[0].fs.has_usage); /* statvfs failed */
    CHECK_STR_EQ(h[1].name, "dm-2");
    CHECK_STR_EQ(h[1].fs.mountpoint, "");
    CHECK_INT_EQ(h[1].size, 10 * GIB);

    /* GPT keeps 33 sectors at the end for its backup. */
    CHECK_INT_EQ(p[3].kind, KD_PART_FREE);
    uint64_t p3_end = MIB + 512 * MIB + 50 * GIB + 40 * GIB;
    CHECK_INT_EQ(p[3].start, p3_end);
    CHECK_INT_EQ(p[3].size, 100 * GIB - 33 * 512 - p3_end);
    kd_disks_free(d);
}

TEST(stick_blank_disk_and_empty_reader)
{
    kd_disks *d = scan();
    CHECK(d);
    const kd_disk *k = kd_disks_find(d, "sdb");
    CHECK(k);
    CHECK_STR_EQ(k->model, "Acme Stick"); /* vendor already in the model */
    CHECK_INT_EQ(k->bus, KD_BUS_USB);
    CHECK_INT_EQ(k->media, KD_MEDIA_UNKNOWN); /* USB "rotational" is not believed */
    CHECK(k->removable);
    CHECK(!k->system);
    CHECK_STR_EQ(k->table, "");
    CHECK_STR_EQ(k->fs.type, "vfat");
    CHECK_STR_EQ(k->fs.label, "MY STICK");
    CHECK_STR_EQ(k->fs.mountpoint, "/media/u/MY STICK");
    CHECK_INT_EQ(k->n_parts, 0);

    k = kd_disks_find(d, "sdc");
    CHECK(k);
    CHECK_INT_EQ(k->logical_sector, 4096);
    CHECK_INT_EQ(k->n_parts, 1);
    CHECK_INT_EQ(k->parts[0].kind, KD_PART_FREE);
    CHECK_INT_EQ(k->parts[0].size, 16 * GIB);

    k = kd_disks_find(d, "sdd");
    CHECK(k);
    CHECK(k->no_media);
    CHECK_INT_EQ(k->n_parts, 0);
    kd_disks_free(d);
}

TEST(gpt_on_4k_sectors)
{
    kd_disks *d = scan();
    CHECK(d);
    const kd_disk *k = kd_disks_find(d, "sde");
    CHECK(k);
    CHECK_INT_EQ(k->media, KD_MEDIA_HDD);
    CHECK_INT_EQ(k->n_parts, 2);
    CHECK_INT_EQ(k->parts[0].size, 18 * GIB - MIB);
    /* With 4 KiB sectors the backup GPT takes 5 sectors = 20 KiB. */
    CHECK_INT_EQ(k->parts[1].kind, KD_PART_FREE);
    CHECK_INT_EQ(k->parts[1].size, 2 * GIB - 5 * 4096);
    kd_disks_free(d);
}

TEST(loop_and_zram_are_hidden)
{
    kd_disks *d = scan();
    CHECK(d);
    const kd_disk *k = kd_disks_find(d, "loop0");
    CHECK(k);
    CHECK(k->hidden);
    CHECK_STR_EQ(k->loop_file, "/var/lib/snapd/snaps/core_1.snap");
    /* The real mount beats the earlier bind of a subfolder. */
    CHECK_STR_EQ(k->fs.mountpoint, "/snap/core/1");
    CHECK_INT_EQ(k->fs.n_mounts, 2);

    k = kd_disks_find(d, "zram0");
    CHECK(k);
    CHECK(k->hidden);
    CHECK_INT_EQ(k->bus, KD_BUS_RAM);
    CHECK(k->fs.swap_active);
    kd_disks_free(d);
}

static int other_statvfs(const char *path, struct statvfs *out)
{
    int r = fake_statvfs(path, out);
    if (r == 0)
        out->f_bfree = 10;
    return r;
}

TEST(layout_key_ignores_usage_numbers)
{
    kd_disks *a = scan();
    kd_sources src = fixture;
    src.statvfs_fn = other_statvfs;
    kd_disks *b = kd_disks_scan(&src);
    CHECK(a && b);
    char *ka = kd_disks_layout_key(a), *kb = kd_disks_layout_key(b);
    CHECK(ka && kb && *ka);
    CHECK_STR_EQ(ka, kb);
    free(ka);
    free(kb);

    /* Something mounted elsewhere: a different key. */
    src.mountinfo = FX "/swaps"; /* no mounts at all */
    kd_disks *c = kd_disks_scan(&src);
    CHECK(c);
    ka = kd_disks_layout_key(a);
    char *kc = kd_disks_layout_key(c);
    CHECK(strcmp(ka, kc) != 0);
    free(ka);
    free(kc);
    kd_disks_free(a);
    kd_disks_free(b);
    kd_disks_free(c);
}

TEST(find_fs_by_any_name)
{
    kd_disks *d = scan();
    CHECK(d);
    CHECK_STR_EQ(kd_disks_find_fs(d, "sdb")->label, "MY STICK");
    CHECK_STR_EQ(kd_disks_find_fs(d, "sda6")->type, "ntfs");
    CHECK_STR_EQ(kd_disks_find_fs(d, "dm-0")->mountpoint, "/home");
    CHECK_STR_EQ(kd_disks_find_fs(d, "dm-2")->type, "ext4");
    CHECK(kd_disks_find_fs(d, "sdq1") == NULL);
    CHECK(kd_disks_find_fs(d, "") == NULL); /* free gaps have no name */
    CHECK(kd_fs_mountable(kd_disks_find_fs(d, "sda6")));
    CHECK(!kd_fs_mountable(kd_disks_find_fs(d, "sda5")));      /* swap */
    CHECK(!kd_fs_mountable(kd_disks_find_fs(d, "nvme0n1p2"))); /* LUKS */
    CHECK(!kd_fs_mountable(kd_disks_find_fs(d, "sda2")));      /* extended */
    kd_disks_free(d);
}

TEST(missing_sysfs_gives_an_empty_list)
{
    kd_sources src = {.sys = "/nonexistent", .udev_db = "/nonexistent",
                      .mountinfo = "/nonexistent", .swaps = "/nonexistent"};
    kd_disks *d = kd_disks_scan(&src);
    CHECK(d);
    CHECK_INT_EQ(d->n, 0);
    kd_disks_free(d);
}

TEST(text_listing)
{
    kd_disks *d = scan();
    CHECK(d);
    char *text = NULL;
    size_t len = 0;
    FILE *f = open_memstream(&text, &len);
    CHECK(f);
    kd_disks_print(d, f, false, false);
    fclose(f);
    CHECK_STR_HAS(text, "sda  Fake SSD 64GB  [SATA, SSD]  64 GiB (68.7 GB)  MBR");
    CHECK_STR_HAS(text, "  sda1       30 GiB  ext4  \"root\"  /  2.9 MiB used of 3.9 MiB\n");
    CHECK_STR_HAS(text, "    sda5       2 GiB  swap  [swap on]");
    CHECK_STR_HAS(text, "    (free)     100 MiB\n");
    CHECK_STR_HAS(text, "  sda2       33 GiB  extended\n");
    CHECK_STR_HAS(text, "    -> dm-0 (luks-home)");
    CHECK_STR_HAS(text, "sdd  Card Reader  [USB]  (no media)\n");
    CHECK(strstr(text, "loop0") == NULL); /* hidden */
    free(text);

    f = open_memstream(&text, &len);
    CHECK(f);
    kd_disks_print(d, f, true, true);
    fclose(f);
    CHECK_STR_HAS(text, "  sda1       32212254720  @1048576  ext4");
    CHECK_STR_HAS(text, "loop0  ?  [loop]  104857600  <- /var/lib/snapd/snaps/core_1.snap");
    free(text);
    kd_disks_free(d);
}

int main(void)
{
    RUN(lists_disks_in_natural_order_without_dm_or_unused_loops);
    RUN(mbr_with_extended_logical_swap_and_gaps);
    RUN(gpt_with_luks_and_lvm_holders);
    RUN(stick_blank_disk_and_empty_reader);
    RUN(gpt_on_4k_sectors);
    RUN(loop_and_zram_are_hidden);
    RUN(layout_key_ignores_usage_numbers);
    RUN(find_fs_by_any_name);
    RUN(missing_sysfs_gives_an_empty_list);
    RUN(text_listing);
    return harness_report();
}
