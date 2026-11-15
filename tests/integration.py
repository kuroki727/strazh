#!/usr/bin/env python3
"""Isolated integration tests. Never mount, reboot, or run init as host PID 1."""
import concurrent.futures
import os
from pathlib import Path
import signal
import socket
import subprocess
import sys
import tempfile
import threading
import time
import unittest

BIN = Path(sys.argv.pop(1)).resolve()


def eventually(predicate, timeout=4):
    end = time.monotonic() + timeout
    while time.monotonic() < end:
        value = predicate()
        if value:
            return value
        time.sleep(0.025)
    raise AssertionError("condition did not become true within %ss" % timeout)


def alive(pid):
    try:
        return Path('/proc/%d/stat' % pid).read_text().split()[2] != 'Z'
    except FileNotFoundError:
        return False


class Harness:
    def __init__(self):
        self.temp = tempfile.TemporaryDirectory(prefix='strazh-test-')
        self.root = Path(self.temp.name)
        self.services = self.root / 'sv'
        self.services.mkdir()
        self.sock = self.root / 'control.sock'
        self.env = dict(os.environ, STRAZH_SOCKET=str(self.sock))
        self.log = (self.root / 'supervisor.log').open('w+')
        self.process = None
        self.extra = []

    def service(self, name, mode='hold', after=None):
        path = self.services / name
        path.mkdir()
        (path / 'run').symlink_to(BIN / 'test_service')
        (path / 'mode').write_text(mode)
        if after is not None:
            (path / 'after').write_text(after)
        return path

    def events(self, name):
        path = self.services / name / 'events'
        return [list(map(int, row.split())) for row in path.read_text().splitlines()] if path.exists() else []

    def start(self, args=None, preexec=None):
        self.process = subprocess.Popen(args or [str(BIN / 'supervisor'), str(self.services), str(self.sock)],
                                        env=self.env, stdout=self.log, stderr=self.log,
                                        preexec_fn=preexec, start_new_session=True)
        return self.process

    def wait_socket(self):
        eventually(lambda: self.sock.exists() or self.process.poll() is not None)
        if self.process.poll() is not None:
            raise AssertionError('startup failed: ' + (self.root / 'supervisor.log').read_text())

    def cli(self, *args, expected=0):
        result = subprocess.run([str(BIN / 'svctl'), *args], env=self.env,
                                capture_output=True, text=True, timeout=5)
        if result.returncode != expected:
            raise AssertionError('CLI %r: rc=%s out=%s err=%s' % (args, result.returncode, result.stdout, result.stderr))
        return result.stdout

    def raw(self, data):
        with socket.socket(socket.AF_UNIX) as client:
            client.settimeout(4)
            client.connect(str(self.sock))
            client.sendall(data)
            client.shutdown(socket.SHUT_WR)
            result = b''
            while True:
                part = client.recv(4096)
                if not part:
                    return result
                result += part

    def stop(self, sig=signal.SIGTERM, timeout=8):
        self.process.send_signal(sig)
        rc = self.process.wait(timeout=timeout)
        if rc != 0:
            raise AssertionError('shutdown failed: ' + (self.root / 'supervisor.log').read_text())
        return rc

    def close(self):
        for client in self.extra:
            client.close()
        if self.process is not None and self.process.poll() is None:
            self.process.terminate()
            try:
                self.process.wait(timeout=11)
            except subprocess.TimeoutExpired:
                self.process.kill()
                self.process.wait(timeout=2)
        self.log.close()
        self.temp.cleanup()


class Integration(unittest.TestCase):
    def setUp(self):
        self.h = Harness()
        self.addCleanup(self.h.close)

    def running(self, *names):
        self.h.start()
        self.h.wait_socket()
        for name in names:
            eventually(lambda: self.h.events(name))
        return self.h

    def test_discovery_empty_and_executable_filter(self):
        h = self.h
        (h.services / 'ignored').mkdir()
        (h.services / 'ignored' / 'run').write_text('no executable bit')
        (h.services / 'random-file').write_text('x')
        h.service('.hidden')
        self.running()
        self.assertEqual(h.cli('status'), 'OK\n(no services)\n')
        h.stop()
        self.assertFalse(h.sock.exists())

    def test_dependency_order_and_signal_restoration(self):
        h = self.h
        h.service('a', after='z\nz\n# deduplicated\n')
        h.service('z')
        def inherited_signals():
            signal.pthread_sigmask(signal.SIG_BLOCK, {signal.SIGTERM, signal.SIGINT, signal.SIGCHLD})
            for sig in (signal.SIGTERM, signal.SIGINT, signal.SIGCHLD, signal.SIGPIPE):
                signal.signal(sig, signal.SIG_IGN)
        h.start(preexec=inherited_signals)
        h.wait_socket()
        eventually(lambda: h.events('a'))
        a, z = h.events('a')[0], h.events('z')[0]
        # Exec ordering does not imply application-level readiness or that the
        # prerequisite writes its event before a newly scheduled dependent.
        log = (h.root / 'supervisor.log').read_text()
        self.assertLess(log.index('started z pid='), log.index('started a pid='))
        self.assertEqual(a[4:], [0, 0, 1])
        self.assertEqual(a[0], a[2])
        h.stop(signal.SIGINT)
        self.assertFalse(alive(a[0]))
        self.assertFalse(alive(z[0]))

    def test_invalid_dependency_graphs_fail_before_start(self):
        h = self.h
        a = h.service('a', after='b\n')
        b = h.service('b', after='a\n')
        for after in ('b\n', 'a\n', 'missing\n', '../escape\n'):
            (a / 'after').write_text(after)
            p = h.start()
            self.assertEqual(p.wait(timeout=3), 1)
            self.assertFalse(h.events('a'))
            self.assertFalse(h.events('b'))
            self.assertFalse(h.sock.exists())
        self.assertTrue(b.exists())

    def test_missing_service_root_is_error(self):
        h = self.h
        p = h.start([str(BIN / 'supervisor'), str(h.root / 'missing'), str(h.sock)])
        self.assertEqual(p.wait(timeout=3), 1)

    def test_manual_start_stop_restart_no_duplicates(self):
        h = self.h
        h.service('worker')
        self.running('worker')
        first = h.events('worker')[0][0]
        for _ in range(8):
            h.cli('start', 'worker')
        self.assertEqual(len(h.events('worker')), 1)
        h.cli('restart', 'worker')
        eventually(lambda: len(h.events('worker')) == 2)
        self.assertFalse(alive(first))
        second = h.events('worker')[-1][0]
        h.cli('stop', 'worker')
        eventually(lambda: 'worker: disabled' in h.cli('status'))
        time.sleep(0.6)
        self.assertEqual(len(h.events('worker')), 2)
        self.assertFalse(alive(second))
        h.cli('start', 'worker')
        eventually(lambda: len(h.events('worker')) == 3)
        h.stop()

    def test_rapid_conflicting_commands(self):
        h = self.h
        h.service('worker')
        self.running('worker')
        def command(i):
            return h.cli(('stop', 'start', 'restart')[i % 3], 'worker')
        with concurrent.futures.ThreadPoolExecutor(max_workers=6) as pool:
            list(pool.map(command, range(30)))
        h.cli('start', 'worker')
        eventually(lambda: 'worker: running' in h.cli('status'))
        time.sleep(0.2)
        self.assertEqual(sum(alive(row[0]) for row in h.events('worker')), 1)
        h.stop()

    def test_crash_restart_exponential_backoff(self):
        h = self.h
        h.service('crash', 'exit')
        self.running('crash')
        eventually(lambda: len(h.events('crash')) >= 4, timeout=6)
        times = [row[3] for row in h.events('crash')]
        gaps = [b - a for a, b in zip(times, times[1:])]
        for actual, minimum in zip(gaps, [500, 1000, 2000]):
            self.assertGreaterEqual(actual, minimum)
            self.assertLess(actual, minimum + 1000)
        self.assertIn('backoff_ms=4000', h.cli('status'))
        h.cli('start', 'crash')
        self.assertIn('backoff_ms=4000', h.cli('status'))
        h.stop()

    def test_failed_exec_blocks_dependents(self):
        h = self.h
        bad = h.service('bad')
        (bad / 'run').unlink()
        (bad / 'run').write_text('#!/does/not/exist\n')
        (bad / 'run').chmod(0o700)
        h.service('dependent', after='bad\n')
        self.running()
        eventually(lambda: 'backoff_ms=1000' in h.cli('status'))
        self.assertIn('dependent: blocked', h.cli('status'))
        self.assertFalse(h.events('dependent'))
        h.stop()

    def test_stopped_dependency_blocks_new_start(self):
        h = self.h
        h.service('a')
        h.service('b', after='a')
        self.running('a', 'b')
        h.cli('stop', 'b')
        eventually(lambda: 'b: disabled' in h.cli('status'))
        h.cli('stop', 'a')
        eventually(lambda: 'a: disabled' in h.cli('status'))
        h.cli('start', 'b')
        self.assertIn('b: blocked', h.cli('status'))
        h.cli('start', 'a')
        eventually(lambda: len(h.events('b')) == 2)
        h.stop()

    def test_group_cleanup_after_leader_exit(self):
        h = self.h
        svc = h.service('orphan', 'orphan')
        self.running('orphan')
        eventually(lambda: len(h.events('orphan')) >= 2)
        children = [int(x) for x in (svc / 'descendants').read_text().split()]
        eventually(lambda: all(not alive(pid) for pid in children))
        h.stop()

    def test_process_tree_cleanup_on_stop(self):
        h = self.h
        svc = h.service('tree', 'tree')
        self.running('tree')
        eventually(lambda: (svc / 'descendants').exists())
        child = int((svc / 'descendants').read_text())
        h.cli('stop', 'tree')
        eventually(lambda: 'tree: disabled' in h.cli('status'))
        self.assertFalse(alive(child))
        h.stop()

    def test_manual_restart_kills_term_ignoring_service(self):
        h = self.h
        h.service('stubborn', 'ignore')
        self.running('stubborn')
        time.sleep(0.1)
        pid = h.events('stubborn')[0][0]
        start = time.monotonic()
        h.cli('restart', 'stubborn')
        h.cli('start', 'stubborn')
        eventually(lambda: len(h.events('stubborn')) == 2, timeout=7)
        self.assertGreaterEqual(time.monotonic() - start, 4.8)
        self.assertFalse(alive(pid))
        # Make the new instance responsive to avoid another five-second grace.
        os.kill(h.events('stubborn')[-1][0], signal.SIGKILL)
        h.cli('stop', 'stubborn')
        h.stop()

    def test_heavy_child_churn_is_reaped(self):
        h = self.h
        svc = h.service('churn', 'churn')
        self.running('churn')
        eventually(lambda: (svc / 'descendants').exists() and len((svc / 'descendants').read_text().split()) == 48)
        h.cli('restart', 'churn')
        eventually(lambda: len(h.events('churn')) == 2)
        old = [int(x) for x in (svc / 'descendants').read_text().split()[:48]]
        eventually(lambda: all(not Path('/proc/%d' % pid).exists() for pid in old))
        h.stop()

    def test_partial_and_malformed_control_requests(self):
        h = self.running()
        for data in [b'', b'garbage\n', b'status extra\n', b'start\n', b'stop x extra\n',
                     b'status\0\n', b'x' * 513, b'status\nstatus\n', b'status']:
            self.assertTrue(h.raw(data).startswith(b'ERR '), repr(data))
        with socket.socket(socket.AF_UNIX) as c:
            c.settimeout(3)
            c.connect(str(h.sock))
            for part in [b'st', b'at', b'us', b'\n']:
                c.sendall(part)
                time.sleep(0.03)
            self.assertTrue(c.recv(1024).startswith(b'OK\n'))
        h.cli('stop', 'missing', expected=1)
        h.cli('start', expected=2)
        h.cli('status', 'extra', expected=2)
        h.cli('unknown', expected=2)
        h.cli('stop', '../escape', expected=2)
        h.stop()

    def test_idle_disconnected_and_excess_clients(self):
        h = self.running()
        idle = socket.socket(socket.AF_UNIX)
        idle.settimeout(3)
        idle.connect(str(h.sock))
        h.extra.append(idle)
        for _ in range(70):
            client = socket.socket(socket.AF_UNIX)
            client.settimeout(2)
            try:
                client.connect(str(h.sock))
                client.sendall(b'status\n')
            finally:
                client.close()
        self.assertIn('OK\n', h.cli('status'))
        eventually(lambda: idle.recv(1024) == b'', timeout=4)
        h.stop()

    def test_graceful_shutdown_sends_single_term(self):
        h = self.h
        path = h.service('grace', 'grace')
        self.running('grace')
        time.sleep(0.1)
        h.stop()
        self.assertEqual((path / 'terms').read_text(), '1')

    def test_shutdown_deadline_with_continuous_control_traffic(self):
        h = self.h
        h.service('stubborn', 'ignore')
        self.running('stubborn')
        time.sleep(0.1)
        done = threading.Event()
        def flood():
            while not done.is_set():
                try:
                    h.raw(b'status\n')
                except OSError:
                    pass
        thread = threading.Thread(target=flood, daemon=True)
        thread.start()
        start = time.monotonic()
        try:
            h.stop(timeout=8)
        finally:
            done.set()
            thread.join(timeout=2)
        duration = time.monotonic() - start
        self.assertGreaterEqual(duration, 4.8)
        self.assertLess(duration, 7)
        self.assertFalse(alive(h.events('stubborn')[0][0]))

    def test_socket_permissions_locks_and_stale_paths(self):
        h = self.h
        stale = socket.socket(socket.AF_UNIX)
        stale.bind(str(h.sock))
        stale.close()
        self.running()
        self.assertEqual(h.sock.stat().st_mode & 0o777, 0o600)
        for path in (h.sock, h.root / 'other.sock'):
            p = subprocess.run([str(BIN / 'supervisor'), str(h.services), str(path)],
                               capture_output=True, timeout=3)
            self.assertEqual(p.returncode, 1)
            self.assertTrue(h.sock.exists())
            h.cli('status')
        h.stop()
        self.assertTrue(Path(str(h.sock) + '.lock').exists())
        h.sock.write_text('keep me')
        p = h.start()
        self.assertEqual(p.wait(timeout=3), 1)
        self.assertEqual(h.sock.read_text(), 'keep me')
        h.sock.unlink()
        h.sock.symlink_to(h.root / 'target')
        p = h.start()
        self.assertEqual(p.wait(timeout=3), 1)
        self.assertTrue(h.sock.is_symlink())

    def test_socket_parent_and_long_path_rejected(self):
        h = self.h
        h.root.chmod(0o777)
        self.assertEqual(h.start().wait(timeout=3), 1)
        h.root.chmod(0o700)
        self.assertEqual(h.start([str(BIN / 'supervisor'), str(h.services), str(h.root / ('x' * 110))]).wait(timeout=3), 1)
        env = dict(h.env, STRAZH_SOCKET='x' * 110)
        self.assertEqual(subprocess.run([str(BIN / 'svctl'), 'status'], env=env, capture_output=True).returncode, 1)

    def test_socket_cleanup_preserves_replacement_file(self):
        h = self.running()
        h.sock.unlink()
        h.sock.write_text('replacement')
        h.stop()
        self.assertEqual(h.sock.read_text(), 'replacement')

    def test_live_socket_is_never_unlinked(self):
        h = self.h
        with socket.socket(socket.AF_UNIX) as server:
            server.bind(str(h.sock))
            server.listen(1)
            inode = h.sock.stat().st_ino
            self.assertEqual(h.start().wait(timeout=3), 1)
            self.assertEqual(h.sock.stat().st_ino, inode)

    def test_cli_rejects_empty_partial_and_slow_responses(self):
        h = self.h
        for reply in (b'', b'OK', b'nonsense\n', b'OK\n\0\n', b'X' * (1024 * 1024 + 1), None):
            with socket.socket(socket.AF_UNIX) as server:
                server.bind(str(h.sock))
                server.listen(1)
                def respond():
                    conn, _ = server.accept()
                    with conn:
                        conn.recv(512)
                        if reply is None:
                            time.sleep(3.2)
                        else:
                            try:
                                for pos in range(0, len(reply), 1024):
                                    conn.sendall(reply[pos:pos + 1024])
                            except (BrokenPipeError, ConnectionResetError):
                                pass
                thread = threading.Thread(target=respond)
                thread.start()
                h.cli('status', expected=1)
                thread.join(timeout=4)
            h.sock.unlink()
        # Successful fragmented response exercises the CLI receive loop.
        with socket.socket(socket.AF_UNIX) as server:
            server.bind(str(h.sock))
            server.listen(1)
            def fragmented():
                conn, _ = server.accept()
                with conn:
                    conn.recv(512)
                    for part in (b'O', b'K\n', b'service: running\n'):
                        conn.sendall(part)
                        time.sleep(0.03)
            thread = threading.Thread(target=fragmented)
            thread.start()
            self.assertIn('service: running', h.cli('status'))
            thread.join(timeout=3)

    def test_init_stage1_failure_and_failed_supervisor_exec(self):
        h = self.h
        for early, manager in [('/bin/false', str(BIN / 'supervisor')),
                               (str(h.root / 'missing'), str(BIN / 'supervisor')),
                               ('/bin/true', str(h.root / 'missing')),
                               ('/bin/true', '/bin/true')]:
            p = h.start([str(BIN / 'myinit'), '--test', early, manager, str(h.services)])
            self.assertEqual(p.wait(timeout=4), 1)
            self.assertFalse(h.sock.exists())

    def test_init_supervisor_crash_cleans_adopted_tree_without_restart(self):
        h = self.h
        svc = h.service('tree', 'tree')
        p = h.start([str(BIN / 'myinit'), '--test', '/bin/true', str(BIN / 'supervisor'), str(h.services)])
        h.wait_socket()
        eventually(lambda: h.events('tree') and (svc / 'descendants').exists())
        leader = h.events('tree')[0]
        child = int((svc / 'descendants').read_text())
        os.kill(leader[1], signal.SIGKILL)
        self.assertEqual(p.wait(timeout=11), 1)
        self.assertFalse(alive(leader[0]))
        self.assertFalse(alive(child))
        self.assertEqual(len(h.events('tree')), 1)

    def test_init_normal_shutdown_and_early_shutdown(self):
        h = self.h
        h.service('worker')
        h.start([str(BIN / 'myinit'), '--test', '/bin/true', str(BIN / 'supervisor'), str(h.services)])
        h.wait_socket()
        eventually(lambda: h.events('worker'))
        pid = h.events('worker')[0][0]
        h.stop(signal.SIGINT)
        self.assertFalse(alive(pid))
        script = h.root / 'slow-stage1'
        script.write_text('#!/bin/sh\nexec /bin/sleep 60\n')
        script.chmod(0o700)
        h.start([str(BIN / 'myinit'), '--test', str(script), str(BIN / 'supervisor'), str(h.services)])
        time.sleep(0.15)
        h.stop()
        self.assertFalse(h.sock.exists())

    def test_init_fake_supervisor_crash_and_adopted_child_timeout(self):
        h = self.h
        path = h.service('fake-manager', 'tree')
        script = h.root / 'fake-manager'
        # Paths come from mkdtemp; use shell quoting for the generated fixture.
        import shlex
        script.write_text('#!/bin/sh\ncd ' + shlex.quote(str(path)) +
                          '\nexec ' + shlex.quote(str(BIN / 'test_service')) + '\n')
        script.chmod(0o700)
        p = h.start([str(BIN / 'myinit'), '--test', '/bin/true', str(script), str(h.services)])
        eventually(lambda: h.events('fake-manager') and (path / 'descendants').exists())
        time.sleep(0.1)
        manager = h.events('fake-manager')[0][0]
        child = int((path / 'descendants').read_text())
        start = time.monotonic()
        os.kill(manager, signal.SIGKILL)
        self.assertEqual(p.wait(timeout=11), 1)
        duration = time.monotonic() - start
        self.assertGreaterEqual(duration, 7.8)
        self.assertLess(duration, 10.5)
        self.assertFalse(alive(child))
        self.assertEqual(len(h.events('fake-manager')), 1)

    def test_host_guards_and_stage1_dry_run(self):
        h = self.h
        self.assertEqual(subprocess.run([str(BIN / 'myinit')], capture_output=True).returncode, 2)
        for args in ([], ['--boot'], [str(h.root)]):
            self.assertEqual(subprocess.run([str(BIN / 'stage1'), *args], capture_output=True).returncode, 2)
        hostname = Path('/proc/sys/kernel/hostname').read_text()
        mounts = Path('/proc/self/mountinfo').read_text()
        for name in ('proc', 'sys', 'dev', 'run'):
            (h.root / name).mkdir()
        result = subprocess.run([str(BIN / 'stage1'), '--dry-run', str(h.root)], capture_output=True, text=True)
        self.assertEqual(result.returncode, 0)
        self.assertIn('devtmpfs', result.stdout)
        (h.root / 'dev').rmdir()
        (h.root / 'dev').symlink_to('/dev')
        self.assertEqual(subprocess.run([str(BIN / 'stage1'), '--dry-run', str(h.root)], capture_output=True).returncode, 1)
        self.assertEqual(hostname, Path('/proc/sys/kernel/hostname').read_text())
        self.assertEqual(mounts, Path('/proc/self/mountinfo').read_text())


if __name__ == '__main__':
    if '--host-only' in sys.argv:
        names = ['test_init_stage1_failure_and_failed_supervisor_exec',
                 'test_host_guards_and_stage1_dry_run',
                 'test_init_fake_supervisor_crash_and_adopted_child_timeout']
        suite = unittest.TestSuite(Integration(name) for name in names)
        sys.exit(0 if unittest.TextTestRunner(verbosity=2).run(suite).wasSuccessful() else 1)
    # Report restricted environments explicitly; do not convert failures in the
    # actual suite into skips. Run this suite on a normal host before release.
    with tempfile.TemporaryDirectory(prefix='strazh-bind-probe-') as path:
        try:
            with socket.socket(socket.AF_UNIX) as probe:
                probe.bind(path + '/probe.sock')
        except PermissionError as exc:
            if os.environ.get('STRAZH_REQUIRE_INTEGRATION') == '1':
                raise
            print('SKIP: sandbox denies Unix socket bind: %s' % exc)
            sys.exit(77)
    unittest.main(verbosity=2)
