#!/usr/bin/env python3
"""Run only a diskless, networkless disposable VM; terminate QEMU after shutdown."""
import os
import selectors
import shutil
import subprocess
import sys
import time


def main():
    if len(sys.argv) != 3:
        sys.exit('usage: qemu-smoke.py bzImage rootfs.cpio.gz')
    qemu = shutil.which('qemu-system-x86_64')
    if not qemu:
        sys.exit('qemu-system-x86_64 is required')
    args = [qemu, '-accel', 'tcg', '-m', '256', '-display', 'none', '-monitor', 'none',
            '-serial', 'stdio', '-nic', 'none', '-no-reboot', '-no-shutdown',
            '-kernel', sys.argv[1], '-initrd', sys.argv[2],
            '-append', 'rdinit=/etc/myinit/myinit console=ttyS0 panic=-1']
    process = subprocess.Popen(args, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    selector = selectors.DefaultSelector()
    selector.register(process.stdout, selectors.EVENT_READ)
    deadline = time.monotonic() + 120
    log = b''
    passed = False
    try:
        while time.monotonic() < deadline and process.poll() is None:
            for key, _ in selector.select(timeout=0.5):
                data = os.read(key.fd, 8192)
                if not data:
                    selector.unregister(key.fileobj)
                    break
                sys.stdout.buffer.write(data)
                sys.stdout.buffer.flush()
                log = (log + data)[-1024 * 1024:]
                if b'STRAZH_QEMU_FAIL' in log or b'Kernel panic' in log:
                    sys.exit('VM boot smoke test failed')
                if b'STRAZH_QEMU_PASS' in log and b'shutdown complete; PID 1 remains' in log:
                    passed = True
                    return
        sys.exit('VM did not complete smoke test within 120 seconds')
    finally:
        selector.close()
        if process.poll() is None:
            process.terminate()
            try:
                process.wait(timeout=5)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait(timeout=5)
        process.stdout.close()
        if passed:
            print('\nPASS: disposable VM boot, mounts, service commands, bounded shutdown')


if __name__ == '__main__':
    main()
