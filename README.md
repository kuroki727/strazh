# Strazh v0.1.0

Strazh is an experimental, minimalist Linux PID 1 and service supervisor in C++17. Services are
plain directories with executable `run` files. Runtime dependencies are libc,
libstdc++ and Linux syscalls; no third-party libraries or configuration language
are required. A shell is needed only for services written as scripts.

v0.1.0 is the first experimental MVP release. Production readiness and
compatibility with other init systems are not claimed. See [CHANGELOG.md](CHANGELOG.md),
[the final audit](docs/RELEASE-AUDIT.md), [validation results](docs/VALIDATION.md),
and [VM instructions](docs/QEMU.md).

## Minimalist philosophy

Do one thing well: initialize Linux, supervise foreground services, and provide
explicit control. Keep PID 1 small and predictable, with initialization,
supervision and control in separate executables. Plain service directories and
existing Linux/POSIX interfaces keep configuration understandable and auditable.

Every capability must justify its code complexity, memory use, process cost and
startup latency. Networking, devices, sessions and log processing belong in
separate tools. Strazh does not bundle those managers, D-Bus, plugins, a large
configuration framework or a systemd compatibility layer. Correctness and
measured reductions in overhead take priority over convenience features.

## Architecture

- `main.cpp` builds `myinit`: blocks CHLD/TERM/INT, consumes signals through
  signalfd/epoll, reaps children, runs stage1, and starts the supervisor only
  after stage1 exits successfully. Stage1 has a 30-second boot deadline.
- `stage1.cpp`: validates mount points, ensures procfs, sysfs, devtmpfs and
  `/run` tmpfs, and creates private `/run/strazh`. It preserves suitable existing
  mounts and rejects other mounts. It does not change the hostname.
- `supervisor.cpp`: discovers services once, validates and sorts dependencies,
  starts foreground processes in separate sessions, reaps descendants as a Linux
  subreaper, applies restart backoff, and serves the control socket.
- `svctl.cpp`: validates commands and exchanges one bounded request/response.
- `linux_support.h` and `control_socket.h`: descriptor ownership, child setup,
  signal restoration, process helpers and stream framing shared by components.

All exec children receive default signal dispositions and an empty signal mask.
A CLOEXEC pipe reports setup/exec failures without blocking the supervisor.
Unexpected supervisor exit is fatal: PID 1 stops adopted children and does not
restart over potentially surviving services. This avoids duplicate instances and
supervisor restart loops; automatic supervisor recovery is deferred.

TERM and INT both request shutdown. Services get a five-second TERM grace,
then KILL and a one-second reap deadline. PID 1 allows eight seconds for graceful
shutdown, then one second after KILL. Final exception cleanup can take another
second. No blocking waitpid is used. Unkillable kernel tasks produce an error;
these are deadlines for userspace policy, not real-time kernel guarantees.

After shutdown or boot failure, **real PID 1 remains alive and reaps children**.
Physical poweroff/reboot, filesystem unmounting and an emergency console are not
implemented. In explicit host test mode, `myinit` returns success or failure.

## Building

Use Linux with a C++17 compiler and CMake 3.16 or newer. Stage1's safe mount
inspection needs Linux 5.8 or newer with mount-ID support and matching headers.
Arch Linux's GCC and Clang are supported. No test tools are needed by default.

The release contains source; no prebuilt binaries are provided.

```sh
git clone --branch v0.1.0 --depth 1 https://github.com/kuroki727/strazh.git
cd strazh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j2
```

Outputs: `myinit`, `stage1`, `supervisor`, and `svctl` in `build/`.
Debug, strict warnings, tests and sanitizers:

```sh
cmake -S . -B build-debug -DCMAKE_BUILD_TYPE=Debug \
  -DBUILD_TESTING=ON -DSTRAZH_WERROR=ON
cmake --build build-debug -j2
ctest --test-dir build-debug --output-on-failure

cmake -S . -B build-sanitize -DCMAKE_CXX_COMPILER=clang++ \
  -DCMAKE_BUILD_TYPE=Debug -DBUILD_TESTING=ON -DSTRAZH_WERROR=ON \
  -DSTRAZH_ASAN=ON -DSTRAZH_UBSAN=ON
cmake --build build-sanitize -j2
ASAN_OPTIONS=detect_leaks=1:abort_on_error=1 UBSAN_OPTIONS=halt_on_error=1 \
  ctest --test-dir build-sanitize --output-on-failure
```

ASan and UBSan can be enabled independently. Tests additionally need Python 3.8+
(standard library only) and compiler thread support for a socket-pair fixture.
Warnings include conversion, sign conversion, shadowing, pedantic and format checks.
LeakSanitizer needs permission to inspect processes with ptrace. In restricted
sandboxes, `ASAN_OPTIONS=detect_leaks=0:abort_on_error=1` allows ASan/UBSan checks;
this does not verify leaks. Do not change host kernel settings to run tests.

To stage binaries for a disposable rootfs (no host installation):

```sh
DESTDIR="$PWD/rootfs-stage" cmake --install build --prefix /
```

This places the three system executables under `/etc/myinit` and `svctl` under
`/bin` inside the staging directory. It does not create service definitions.

## Services

Default layout:

```text
/etc/sv/example/
  run       executable regular file, or symlink to one
  after     optional regular text file, at most 64 KiB
```

Names are 1–128 ASCII letters, digits, dots, underscores or hyphens and may not
start with a dot. Entries without an executable regular `run` are ignored.
Discovery errors fail startup; an empty service directory is valid. The maximum
is 4096 services. Directory and run-file symlinks are permitted for trusted
configuration. Services execute with their service directory as working directory.

Example `run`:

```sh
#!/bin/sh
exec /usr/bin/example-daemon --foreground
```

`run` is executed directly, with no implicit shell. Script files require a valid
interpreter line. Keep the leader in the foreground. Double-forking, changing
sessions/process groups, and intentionally escaping supervision are unsupported.
There is no `finish` hook, oneshot service type, user switching or reload command.

`after` contains one service name per line; blank lines and lines starting with
`#` are ignored, surrounding whitespace is trimmed, and duplicates are removed.
Missing dependencies, self references and cycles reject the entire configuration
before any services start. Independent services use deterministic name order.

Dependencies gate each new start until their leaders have successfully exec'd
and are running. Failed, stopped or stopping dependencies keep a service
`blocked`. **Exec success is not application readiness.** A later dependency loss
does not stop already running dependents. Stop/start commands do not recursively
change dependency state; start prerequisites explicitly when needed.

Every unrequested leader exit, including exit code zero, restarts with delays of
500 ms, 1 s, 2 s, 4 s, up to 30 s. A run lasting at least 30 s resets the next
delay to 500 ms. Fork/pipe failures also back off. A process-group slot is retained
until its leader is reaped and the old group disappears. Remaining group members
are killed when the foreground leader exits.

## Control interface

```sh
svctl status
svctl stop example
svctl start example
svctl restart example
STRAZH_SOCKET=/absolute/private/control.sock svctl status
```

`status` reports `starting`, `running`, `stopping`, `disabled`, `blocked`, or
`stopped`, with PID where applicable, backoff and next-start delay.

Commands acknowledge the desired state with `OK`; they do not wait for convergence.
Check status to confirm completion. `start` is idempotent and does not clear an
already enabled service's crash delay. `stop` disables automatic restart.
`restart` explicitly resets the delay and waits for old-process cleanup before
spawning. Repeated commands do not extend the stop deadline. Unknown services,
invalid syntax and mutations during shutdown return `ERR`.

Exit codes: **0** successful response, **1** server/transport/protocol failure,
**2** invalid CLI usage.

The default socket is `/run/strazh/supervisor.sock`. Both programs honor
`STRAZH_SOCKET`; an explicit supervisor socket argument takes precedence:

```text
supervisor [SERVICES_DIR [SOCKET_PATH]]
myinit [STAGE1 [SUPERVISOR [SERVICES_DIR]]]        # PID 1 only
myinit --test FAKE_STAGE1 SUPERVISOR SERVICES_DIR # ordinary process only
```

The socket requires an absolute path with no symlink parents. Its parent must
already exist, be owned by the supervisor user, and have no group/other write
permission. Use a private directory such as one created with `mktemp -d`.
Socket mode is 0600. Persistent 0600 lock files prevent competing socket owners;
an additional directory lock prevents two supervisors for the same service tree.
Do not delete live lock files. Only same-owner, demonstrably stale sockets are
removed; regular files, symlinks, live sockets and foreign sockets are preserved.
Cleanup checks the bound inode before removing it.

Wire protocol: one newline-terminated request per connection, at most 512 bytes
including newline. Response begins `OK\n` or `ERR message\n`; EOF terminates the
response. No pipelining. The server supports at most 64 concurrent clients and a
two-second total client lifetime. CLI socket I/O has a three-second total deadline and a
1 MiB response limit. All socket I/O is nonblocking and supports partial transfers.

## Development and testing

CTest provides core lifecycle tests, socket-pair control tests, host safety tests,
and a separate full integration suite using named Unix sockets. All fixtures use
temporary directories and their own disposable child processes. Tests never
mount, change hostname, broadcast signals, reboot or run init as host PID 1.

Restricted sandboxes may deny Unix socket binding. The named-socket suite then
reports **Skipped**, not validated. Require it for a release gate on an
unrestricted Linux environment:

```sh
STRAZH_REQUIRE_INTEGRATION=1 ctest --test-dir build-debug --output-on-failure
```

The checked-in CI workflow requires named-socket integration, with GCC Debug,
GCC Release, and Clang ASan/UBSan jobs, followed by a diskless QEMU boot smoke
job. Passing unrestricted CI, leak detection and VM results are recorded in
[the validation report](docs/VALIDATION.md).

The full integration suite covers real discovery/startup, command races, crash
backoff, failed exec, dependency blocking, TERM-ignoring services, process churn,
malformed/fragmented control traffic, stale/live/unsafe socket paths, CLI errors,
supervisor failure and shutdown under continuous client traffic.

For mount-point validation without any mount or hostname syscall:

```sh
build/stage1 --dry-run /path/to/disposable/root
```

The root must contain real directories `proc`, `sys`, `dev`, and `run`. Actual
stage1 execution requires `--boot`, root privileges and a PID 1 parent. `myinit`
passes this flag during real boot; its host test mode must use a fake stage1 such
as `/bin/true`. Never install experimental init as the host's PID 1. Use the
[QEMU fixture](docs/QEMU.md) for actual boot and mount validation.

## Security and limitations

Service configuration is executable privileged code. Its directory tree,
symlink targets, binaries and interpreters must be controlled by the administrator.
Do not allow untrusted users to modify them. The control socket checks peer
credentials (same effective UID or root) in addition to filesystem permissions.
Socket directories and their ancestors must be administered safely; same-UID or
root adversaries can modify service configuration and are outside this boundary.

Process groups and subreaping provide lifecycle management, not a security
sandbox. There are no cgroups, privilege isolation, resource limits, readiness
protocol, service-specific environment configuration, managed logging, hot reload,
udev integration or PTY setup. Parent-death signals protect ordinary foreground
leaders, but Linux clears this setting on some credential changes; it is not
complete descendant containment. A standalone supervisor forcibly killed with
SIGKILL cannot run cleanup; PID 1 performs adoption cleanup in the intended
architecture. Uninterruptible kernel tasks may outlive a shutdown deadline.

## Roadmap

Prioritize PID 1 reliability, correct supervision and dependency behavior,
predictable shutdown/restart, clear diagnostics, and measured idle overhead.
Broaden VM fault testing before expanding supported deployment environments.
Future capabilities require a separate justification of complexity and runtime
cost; v0.1.0 deliberately preserves its four-executable architecture.

MIT license. See [LICENSE](LICENSE).
