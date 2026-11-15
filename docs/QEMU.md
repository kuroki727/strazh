# Disposable QEMU boot testing

The provided fixture creates an initramfs archive without root privileges,
mounting, extracting device nodes, or touching the host's init configuration.
Device nodes exist only as archive records. The VM gets no disks, network or
host directory shares. The runner terminates only the QEMU process it launches.

## Inputs

Provide an x86-64 Linux `bzImage` and a **statically linked BusyBox**. Pin and
record their versions/configuration and SHA-256 hashes for reproducibility.
There are no implicit downloads. BusyBox needs `sh`, `sleep` (fractional sleep),
`grep`, and `kill`. Python 3.8+, binutils `readelf`, and `qemu-system-x86_64` are
host-side test tools, not Strazh runtime dependencies.

The kernel needs initramfs/gzip, ELF execution, procfs, sysfs, tmpfs, devtmpfs,
signalfd, epoll, Unix sockets and a serial console. Use Linux 5.8+ for mount-ID
inspection. Relevant configuration includes CONFIG_BLK_DEV_INITRD,
CONFIG_RD_GZIP, CONFIG_BINFMT_ELF, CONFIG_BINFMT_SCRIPT, CONFIG_PROC_FS,
CONFIG_SYSFS, CONFIG_TMPFS, CONFIG_DEVTMPFS, CONFIG_SIGNALFD, CONFIG_EPOLL,
CONFIG_UNIX, CONFIG_TTY, CONFIG_SERIAL_8250 and CONFIG_SERIAL_8250_CONSOLE.

## Build and run

```sh
cmake -S . -B build-static -DCMAKE_BUILD_TYPE=Release \
  -DSTRAZH_WERROR=ON -DCMAKE_EXE_LINKER_FLAGS=-static
cmake --build build-static -j2
python3 tools/make-qemu-rootfs.py build-static /path/to/busybox-static rootfs.cpio.gz
sha256sum /path/to/bzImage /path/to/busybox-static rootfs.cpio.gz
python3 tools/qemu-smoke.py /path/to/bzImage rootfs.cpio.gz
```

All four Strazh executables and BusyBox must have no ELF interpreter segment;
the packer rejects dynamic binaries. Use a toolchain with static libc/libstdc++
archives. Do not combine this static build with sanitizer flags.

With identical binaries and Python/zlib implementation, the packer fixes archive
ordering, timestamps, ownership, inode numbers and gzip metadata. Preserve the
inputs and hashes; the scripts do not promise a reproducible compiler toolchain.

The VM starts `/etc/myinit/myinit` as PID 1. Stage1 mounts the four filesystems.
Services `alpha`, `beta` and `probe` exercise dependencies. The probe verifies
mount types and stop/start/restart convergence, emits `STRAZH_QEMU_PASS`, and
signals its VM's PID 1 to shut down. The runner requires both the PASS marker and
PID 1's successful reap-only shutdown message. It fails on a FAIL marker, kernel
panic, premature QEMU exit, or a 120-second timeout. It then terminates QEMU
because Strazh does not yet issue a physical poweroff syscall.

## Manual fault testing

For deeper boot testing, edit only a disposable copy of the generated service
fixture or its packer:

- Make stage1 fail or hang; verify no supervisor starts and PID 1 stays alive.
- Make a service ignore TERM; verify KILL after the five-second grace.
- Kill the supervisor inside the VM; verify PID 1 cleans adopted children, does
  not restart duplicate services, and reports runtime failure.
- Add a dependency cycle or missing dependency; verify startup fails closed.
- Run rapid child churn while clients connect and disconnect; verify service
  status, reaping and shutdown progress.
- Boot with an unexpected filesystem already mounted at a required target;
  verify stage1 refuses to cover it rather than hiding it.

These scenarios require further VM instrumentation for unattended assertions;
the smoke runner covers the basic successful boot path. A successful smoke test
alone is not evidence of production readiness or complete hardware support.
