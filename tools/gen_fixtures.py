#!/usr/bin/env python3
"""Writes tests/fixtures/sys1: a fake /sys/block, udev database, mountinfo
and swaps with the cases tests/unit/test_disks.c checks. Names, serials and
UUIDs are made up; the file formats are the real ones.

Sizes in sysfs are in 512-byte units whatever the logical sector size, as
the kernel does. Run from the repository root; it replaces the folder."""
import os
import shutil

ROOT = "tests/fixtures/sys1"
MIB = 2048  # 512-byte sectors per MiB
GIB = 1024 * MIB


def write(path, text):
    full = os.path.join(ROOT, path)
    os.makedirs(os.path.dirname(full), exist_ok=True)
    with open(full, "w") as f:
        f.write(text)


def udev(major, minor, **props):
    lines = ["E:%s=%s" % kv for kv in props.items()]
    write("udev/b%d:%d" % (major, minor), "\n".join(lines) + "\n")


def disk(name, major, minor, size, removable=0, rotational=0, ls=512, ps=None,
         model=None, **props):
    base = "sys/block/%s/" % name
    write(base + "dev", "%d:%d\n" % (major, minor))
    write(base + "size", "%d\n" % size)
    write(base + "removable", "%d\n" % removable)
    write(base + "ro", "0\n")
    write(base + "queue/rotational", "%d\n" % rotational)
    write(base + "queue/logical_block_size", "%d\n" % ls)
    write(base + "queue/physical_block_size", "%d\n" % (ps or ls))
    os.makedirs(os.path.join(ROOT, base + "holders"), exist_ok=True)
    if model:
        write(base + "device/model", model + "\n")
    udev(major, minor, **props)


def part(dname, pname, major, minor, number, start, size, holders=(), **props):
    base = "sys/block/%s/%s/" % (dname, pname)
    write(base + "dev", "%d:%d\n" % (major, minor))
    write(base + "partition", "%d\n" % number)
    write(base + "start", "%d\n" % start)
    write(base + "size", "%d\n" % size)
    os.makedirs(os.path.join(ROOT, base + "holders"), exist_ok=True)
    for h in holders:
        write(base + "holders/" + h, "")
    udev(major, minor, **props)


def dm(name, minor, size, dm_name, uuid, **props):
    base = "sys/block/%s/" % name
    write(base + "dev", "253:%d\n" % minor)
    write(base + "size", "%d\n" % size)
    write(base + "removable", "0\n")
    write(base + "dm/name", dm_name + "\n")
    write(base + "dm/uuid", uuid + "\n")
    udev(253, minor, DM_NAME=dm_name, **props)


shutil.rmtree(ROOT, ignore_errors=True)

# sda: MBR, SATA SSD. 1: ext4 on /. 2: extended (sysfs shows 2 sectors),
# with 5: active swap and 6: unmounted NTFS whose label has a space; a
# 100 MiB gap inside the extended one and 1 GiB free at the end.
disk("sda", 8, 0, 64 * GIB, rotational=1, ID_BUS="ata", ID_ATA_ROTATION_RATE_RPM="0",
     ID_MODEL="Fake_SSD_64GB", ID_MODEL_ENC="Fake\\x20SSD\\x2064GB\\x20\\x20",
     ID_SERIAL_SHORT="FAKE0001", ID_REVISION="1.0", ID_PART_TABLE_TYPE="dos",
     ID_PART_TABLE_UUID="1234abcd")
part("sda", "sda1", 8, 1, 1, MIB, 30 * GIB, ID_FS_TYPE="ext4", ID_FS_LABEL="root",
     ID_FS_UUID="11111111-1111-1111-1111-111111111111", ID_PART_ENTRY_TYPE="0x83",
     ID_PART_ENTRY_FLAGS="0x80", ID_PART_ENTRY_OFFSET=str(MIB),
     ID_PART_ENTRY_SIZE=str(30 * GIB))
ext_start = MIB + 30 * GIB
ext_size = 33 * GIB - MIB
part("sda", "sda2", 8, 2, 2, ext_start, 2, ID_PART_ENTRY_TYPE="0x5",
     ID_PART_ENTRY_OFFSET=str(ext_start), ID_PART_ENTRY_SIZE=str(ext_size))
part("sda", "sda5", 8, 5, 5, ext_start + MIB, 2 * GIB, ID_FS_TYPE="swap",
     ID_FS_UUID="55555555-5555-5555-5555-555555555555", ID_PART_ENTRY_TYPE="0x82")
sda6_start = ext_start + MIB + 2 * GIB + 100 * MIB
part("sda", "sda6", 8, 6, 6, sda6_start, ext_start + ext_size - sda6_start,
     ID_FS_TYPE="ntfs", ID_FS_LABEL="Meus_Dados", ID_FS_LABEL_ENC="Meus\\x20Dados",
     ID_PART_ENTRY_TYPE="0x7")

# nvme0n1: GPT. p1: EFI on /boot/efi. p2: LUKS, open as dm-0, ext4 on /home
# (mountinfo names it by /dev/mapper). p3: LVM PV with two LVs: dm-1 btrfs
# on /srv (anonymous 0:45 in mountinfo), dm-2 unmounted ext4.
disk("nvme0n1", 259, 0, 100 * GIB, ID_MODEL="Fake NVMe 100", ID_SERIAL_SHORT="NV0001",
     ID_PART_TABLE_TYPE="gpt", ID_PART_TABLE_UUID="aaaaaaaa-0000-0000-0000-000000000000")
part("nvme0n1", "nvme0n1p1", 259, 1, 1, MIB, 512 * MIB, ID_FS_TYPE="vfat",
     ID_FS_UUID="ABCD-1234", ID_PART_ENTRY_TYPE="c12a7328-f81f-11d2-ba4b-00a0c93ec93b",
     ID_PART_ENTRY_NAME_ENC="EFI\\x20system")
part("nvme0n1", "nvme0n1p2", 259, 2, 2, MIB + 512 * MIB, 50 * GIB, holders=["dm-0"],
     ID_FS_TYPE="crypto_LUKS", ID_FS_VERSION="2",
     ID_PART_ENTRY_TYPE="0fc63daf-8483-4772-8e79-3d69d8477de4")
p3_start = MIB + 512 * MIB + 50 * GIB
part("nvme0n1", "nvme0n1p3", 259, 3, 3, p3_start, 40 * GIB, holders=["dm-1", "dm-2"],
     ID_FS_TYPE="LVM2_member", ID_PART_ENTRY_TYPE="e6d6d379-f507-44c2-a23c-238f2a3df928")
dm("dm-0", 0, 50 * GIB - 32768, "luks-home", "CRYPT-LUKS2-0000-luks-home",
   ID_FS_TYPE="ext4", ID_FS_LABEL="home")
dm("dm-1", 1, 30 * GIB, "vg-srv", "LVM-abcdef", ID_FS_TYPE="btrfs", ID_FS_LABEL="srv")
dm("dm-2", 2, 10 * GIB, "vg-spare", "LVM-ghijkl", ID_FS_TYPE="ext4")

# sdb: USB stick, no table, FAT straight on the disk; label and
# mountpoint with a space (mountinfo escapes it as \040).
disk("sdb", 8, 16, 8 * GIB, removable=1, rotational=1, ID_BUS="usb",
     ID_VENDOR="Acme", ID_MODEL="Acme_Stick", ID_SERIAL_SHORT="STICK42",
     ID_FS_TYPE="vfat", ID_FS_LABEL="MY_STICK", ID_FS_LABEL_ENC="MY\\x20STICK")

# sdc: a blank 4Kn disk: all free.
disk("sdc", 8, 32, 16 * GIB, ls=4096, ID_BUS="ata", ID_MODEL="Blank_4K")

# sdd: card reader with no card.
disk("sdd", 8, 48, 0, removable=1, rotational=1, ID_BUS="usb", ID_MODEL="Card_Reader")

# sde: 4Kn disk with GPT and one partition leaving 2 GiB at the end.
disk("sde", 8, 64, 20 * GIB, ls=4096, rotational=1, ID_BUS="ata",
     ID_ATA_ROTATION_RATE_RPM="5400", ID_MODEL="Big_4K_HDD", ID_PART_TABLE_TYPE="gpt")
part("sde", "sde1", 8, 65, 1, MIB, 18 * GIB - MIB, ID_FS_TYPE="xfs")

# loop0: a mounted snap. loop1: unused (size 0, skipped). zram0: swap.
disk("loop0", 7, 0, 100 * MIB, ID_FS_TYPE="squashfs")
write("sys/block/loop0/loop/backing_file", "/var/lib/snapd/snaps/core_1.snap\n")
disk("loop1", 7, 1, 0)
disk("zram0", 252, 0, 4 * GIB, ID_FS_TYPE="swap")

write("mountinfo", "\n".join([
    "22 1 0:21 / /proc rw,nosuid shared:12 - proc proc rw",
    "30 1 8:1 / / rw,noatime shared:1 - ext4 /dev/sda1 rw",
    "31 30 259:1 / /boot/efi rw shared:2 - vfat /dev/nvme0n1p1 rw",
    "32 30 253:0 / /home rw shared:3 - ext4 /dev/mapper/luks-home rw",
    # btrfs: anonymous device number, found by source
    "33 30 0:45 / /srv rw shared:4 - btrfs /dev/mapper/vg-srv rw",
    "34 30 0:45 /sub /mnt/sub rw shared:5 - btrfs /dev/mapper/vg-srv rw",
    "35 30 8:16 / /media/u/MY\\040STICK rw shared:6 - vfat /dev/sdb rw",
    # a bind of a subfolder before the real mount: not the main one
    "36 30 7:0 /etc /mnt/bind rw shared:7 - squashfs /dev/loop0 ro",
    "37 30 7:0 / /snap/core/1 ro shared:8 - squashfs /dev/loop0 ro",
]) + "\n")

write("swaps", "\n".join([
    "Filename\t\t\t\tType\t\tSize\t\tUsed\t\tPriority",
    "/dev/sda5                               partition\t2097148\t\t1024\t\t-2",
    "/dev/zram0                              partition\t4194300\t\t0\t\t100",
    "/swapfile                               file\t\t1048572\t\t0\t\t-3",
]) + "\n")
print("wrote", ROOT)
