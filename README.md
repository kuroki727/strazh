# strazh

A minimal, dependency-free PID 1 and service supervisor for Linux, written in C++17.

## About

strazh is a minimal init system for Linux: a PID 1 that mounts what's
needed to boot, reaps zombies, and hands off to a lightweight service
supervisor. Services are plain directories with an executable `run`
file — no config language to parse, no shell required — in the
spirit of runit and OpenRC, but written from scratch in dependency-
free C++17.

strazh is not trying to replace systemd feature-for-feature. It has
no unit files, no cgroups integration, no socket activation (yet).
It aims to do one thing — start, supervise, and restart your
services — reliably and legibly, with a codebase small enough to
read in an afternoon.

## Repository layout

```
strazh/
├── main.cpp        # PID 1: signalfd + epoll loop, zombie reaping, spawns stage1
├── stage1.cpp       # mounts proc/sysfs/tmpfs, sets hostname
├── CMakeLists.txt
└── LICENSE
```

> **Note:** `CMakeLists.txt` currently points at `src/main.cpp` and
> `src/stage1.cpp`, but the sources live at the repo root — `cmake
> --build` will fail until one of these is fixed: either move the
> `.cpp` files into `src/`, or drop the `src/` prefix in
> `CMakeLists.txt`.

## Features

- Minimal PID 1 (`main.cpp`): `signalfd` + `epoll` event loop,
  zombie reaping via `waitpid(WNOHANG)`, tracks `stage1`'s exit
  status separately from other reaped children
- Defensive error handling — file descriptors are closed on every
  error path (`epoll_create1`, `epoll_ctl`, `spawn_stage1` failures)
  so PID 1 doesn't leak fds while limping along
- Portable header includes — `__has_include` fallbacks
  (`<cstdio>`/`<stdio.h>`, `<csignal>`/`<signal.h>`, etc.) for
  libcs where the C++-wrapped headers aren't available
- `stage1`: real `mount(2)` for `proc`/`sysfs`/`tmpfs`,
  `sethostname(2)`, guarded with `#error` on non-Linux
- Service supervisor — directory-based, no config parser (planned)
- Dependency ordering between services (planned)
- Control socket + `svctl`/`initctl` CLIs for start/stop/status/reboot (planned)
- Zero external dependencies — pure C++17 and Linux syscalls, nothing else

## Status

Early / work in progress, not bootable yet.

- [x] PID 1 skeleton — `signalfd`/`epoll` loop, zombie reaping
- [x] `stage1` — mounts `proc`, `sysfs`, `tmpfs`; sets hostname
- [ ] Fix `CMakeLists.txt` source paths (see note above)
- [ ] Real shutdown — `reboot(2)` with `RB_AUTOBOOT`/`RB_POWER_OFF`
      (currently `SIGTERM`/`SIGINT` just stop the main loop)
- [ ] Service supervisor — spawn/respawn from `/etc/sv/<name>/run`
- [ ] Dependency ordering (`after` file, topological sort)
- [ ] Control socket + `svctl`
- [ ] `initctl` — reboot/halt/reexec
- [ ] Logging subsystem (`svlogd`-style, per-service log pipe)
- [ ] `devtmpfs` + udev integration (currently `tmpfs` stub for `/dev`)

## Design principles

- **Directories as config.** A service is a directory with an
  executable `run` file. No unit-file parser to write or maintain.
- **Zero external dependencies.** Only libc and POSIX/Linux syscalls
  — no third-party libraries.
- **Small and legible over feature-complete.** Not a systemd
  replacement: no cgroups, no socket activation (yet), no unit
  language. The goal is a codebase readable end-to-end in an
  afternoon.

## Building

Requires a C++17 compiler, CMake ≥ 3.16, and Linux (the code uses
`signalfd`, `epoll`, and raw `mount(2)`, so it won't build or run
anywhere else).

```sh
git clone https://github.com/kuroki727/strazh
cd strazh
cmake -B build
cmake --build build
```

This produces `build/myinit` and `build/stage1` — once the
`src/` path mismatch above is resolved.

## Testing

strazh is meant to run as real PID 1, so it is **not** something you
run on your host machine to test — a broken build as your actual
init means an unbootable system. Development and testing happens in
QEMU against a disposable rootfs:

```sh
qemu-system-x86_64 \
  -kernel bzImage \
  -initrd rootfs.cpio \
  -append "init=/myinit console=ttyS0"
```

Individual components can be smoke-tested in isolation before that.
`stage1` accepts an optional root path so its `mount()` calls can be
pointed at a scratch directory instead of the real `/proc`, `/sys`,
`/dev`:

```sh
mkdir -p /tmp/scratch-root/proc /tmp/scratch-root/sys /tmp/scratch-root/dev
./stage1 /tmp/scratch-root
mount | grep scratch-root   # verify
```

`main.cpp` (as `myinit`) can similarly be run as a regular process
and sent signals directly (`kill -TERM <pid>`) to check the
`signalfd`/`epoll` loop and reap logic without needing to be PID 1.

## Service directory format (planned)

Modeled on runit — a service is a directory, not a config file to parse:

```
/etc/sv/<name>/
  run       # executable, exec'd directly — no shell wrapper required
  finish    # optional, run once the service stops
  after     # optional, newline-separated list of services to start first
```

## License

MIT. See [LICENSE](LICENSE).

## Contributing

Not yet accepting external contributions — still a solo learning
project in its early stages. Issues and discussion are welcome once
there's a first tagged release.
