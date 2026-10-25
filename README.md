# Kidisks

*[Português](README.pt-BR.md)*

A lightweight disk manager for Linux, in C and GTK2. One program that does
what GParted, Filelight and BleachBit do:

- **Units:** every disk and partition at a glance: model, type, health,
  temperature and how full each partition is.
- **Partitions:** create, delete, format and resize, with a queue of
  operations applied only when you say so.
- **Space:** what takes up room on each disk, in concentric rings.
- **Cleanup:** removes caches, leftovers and traces programs leave behind,
  always with a preview of what will go.

Kidisks runs as your user. When a change needs root, UDisks2 asks for the
administrator password through polkit.

**Status:** in development. Done: the Units tab (disks, partitions, LUKS
and LVM, usage, health and temperature from SMART, mount, unmount and safe
removal) and the Space tab (Filelight-like rings, the same totals as
`du`, trash and delete). See [PLANO.md](PLANO.md) (in Portuguese).

Also from the command line:

```sh
kidisks --list            # disks and partitions
kidisks --scan ~ --top 10 # what takes up the most room in your home folder
kidisks --space ~/Downloads  # opens the Space tab on that folder
kidisks --info sda        # health, temperature, power-on time
kidisks --mount sdb1      # mounts it (and says where)
kidisks --power-off sdb   # unmounts everything and powers the disk off
```

## Installing

The easiest way is the `.deb` package from the
[releases page](https://github.com/kiyoshispreclerg/kidisks/releases). Each
release has one for Ubuntu 22.04, Ubuntu 24.04, Debian 12 and Debian 13; pick
the one that matches your system. They are named
`kidisks_VERSION-1_SYSTEM_ARCH.deb`, and each one was installed and run on a
clean system of its own kind before being published. Releases of `v0.x.x` (and
tags with a hyphen, such as `v1.0.0-rc1`) are marked as pre-releases.

```sh
sudo apt install ./kidisks_0.1.0-1_ubuntu24.04_amd64.deb   # use the file you downloaded
sudo apt remove kidisks
```

`apt` pulls in the libraries it needs, and `udisks2` (used to mount, eject and
read SMART) comes along as a recommended package. To install from source
instead, see [Building](#building).

## Building

Dependencies (Debian/Ubuntu): `build-essential pkg-config libgtk2.0-dev
libudev-dev gettext`; to run: `udisks2`.

```sh
make
build/kidisks
sudo make install      # into /usr/local; PREFIX=/usr to change
```

## License

GPL-3.0-or-later. See [LICENSE](LICENSE).
