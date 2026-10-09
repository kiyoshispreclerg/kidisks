# Kidisks

*[Português](README.md)*

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

**Status:** in development (phase 0: the window with empty tabs). See
[PLANO.md](PLANO.md) (in Portuguese).

## Building

Dependencies (Debian/Ubuntu): `build-essential pkg-config libgtk2.0-dev
gettext`.

```sh
make
build/kidisks
sudo make install      # into /usr/local; PREFIX=/usr to change
```

## License

GPL-3.0-or-later. See [LICENSE](LICENSE).
