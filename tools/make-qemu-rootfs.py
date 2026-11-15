#!/usr/bin/env python3
"""Create a deterministic newc initramfs, including device nodes in the archive only.
Requires four statically linked Strazh binaries and a static BusyBox with sh,
sleep, grep, and kill applets. Never mounts or changes the host filesystem.
"""
import gzip
import io
from pathlib import Path
import stat
import subprocess
import sys


def newc(entries):
    output = io.BytesIO()
    for inode, (name, mode, data, rdevmajor, rdevminor) in enumerate(entries, 1):
        encoded = name.encode() + b'\0'
        fields = [inode, mode, 0, 0, 1, 0, len(data), 0, 0,
                  rdevmajor, rdevminor, len(encoded), 0]
        output.write(b'070701' + b''.join(('%08x' % field).encode() for field in fields))
        output.write(encoded)
        output.write(b'\0' * (-output.tell() % 4))
        output.write(data)
        output.write(b'\0' * (-output.tell() % 4))
    return output.getvalue()


def static_binary(path):
    result = subprocess.run(['readelf', '-l', str(path)], check=True, capture_output=True, text=True)
    if 'INTERP' in result.stdout:
        raise ValueError('%s is dynamically linked; rebuild with -static' % path)
    return path.read_bytes()


PROBE = b'''#!/bin/sh
set -eu
fail() { echo STRAZH_QEMU_FAIL; exit 1; }
wait_state() {
    i=0
    until /bin/svctl status | /bin/grep -q "^beta: $1"; do
        i=$((i + 1))
        [ "$i" -lt 50 ] || fail
        /bin/sleep 0.1
    done
}
/bin/grep -q ' /proc proc ' /proc/mounts || fail
/bin/grep -q ' /sys sysfs ' /proc/mounts || fail
/bin/grep -q ' /dev devtmpfs ' /proc/mounts || fail
/bin/grep -q ' /run tmpfs ' /proc/mounts || fail
wait_state running
/bin/svctl stop beta || fail
wait_state disabled
/bin/svctl start beta || fail
wait_state running
/bin/svctl restart beta || fail
/bin/sleep 0.2
wait_state running
echo STRAZH_QEMU_PASS
/bin/kill -TERM 1
exec /bin/sleep 3600
'''


def main():
    if len(sys.argv) != 4:
        sys.exit('usage: make-qemu-rootfs.py BUILD_DIR STATIC_BUSYBOX OUTPUT.cpio.gz')
    build, busybox, output = map(Path, sys.argv[1:])
    entries = []
    for directory in ['bin', 'etc', 'etc/myinit', 'etc/sv', 'etc/sv/alpha',
                      'etc/sv/beta', 'etc/sv/probe', 'dev', 'proc', 'sys', 'run', 'tmp']:
        entries.append((directory, stat.S_IFDIR | (0o1777 if directory == 'tmp' else 0o755), b'', 0, 0))
    entries.append(('bin/busybox', stat.S_IFREG | 0o755, static_binary(busybox), 0, 0))
    for applet in ['sh', 'sleep', 'grep', 'kill']:
        entries.append(('bin/' + applet, stat.S_IFLNK | 0o777, b'busybox', 0, 0))
    for binary in ['myinit', 'stage1', 'supervisor']:
        entries.append(('etc/myinit/' + binary, stat.S_IFREG | 0o755, static_binary(build / binary), 0, 0))
    entries.append(('bin/svctl', stat.S_IFREG | 0o755, static_binary(build / 'svctl'), 0, 0))
    entries.append(('init', stat.S_IFLNK | 0o777, b'etc/myinit/myinit', 0, 0))
    entries.append(('dev/console', stat.S_IFCHR | 0o600, b'', 5, 1))
    entries.append(('dev/null', stat.S_IFCHR | 0o666, b'', 1, 3))
    for name in ['alpha', 'beta']:
        entries.append(('etc/sv/' + name + '/run', stat.S_IFREG | 0o755,
                        b'#!/bin/sh\nexec /bin/sleep 3600\n', 0, 0))
    entries.append(('etc/sv/beta/after', stat.S_IFREG | 0o644, b'alpha\n', 0, 0))
    entries.append(('etc/sv/probe/after', stat.S_IFREG | 0o644, b'beta\n', 0, 0))
    entries.append(('etc/sv/probe/run', stat.S_IFREG | 0o755, PROBE, 0, 0))
    entries.sort(key=lambda entry: entry[0])
    entries.append(('TRAILER!!!', 0, b'', 0, 0))
    with output.open('wb') as file:
        with gzip.GzipFile(fileobj=file, mode='wb', filename='', mtime=0) as archive:
            archive.write(newc(entries))
    print(output)


if __name__ == '__main__':
    main()
