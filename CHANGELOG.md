# Changelog

## [0.1.0] - 2026-11-15

First official experimental release of Strazh, a minimalist Linux PID 1 and
foreground service supervisor in C++17. This is an MVP for controlled testing;
production readiness and compatibility with other init systems are not claimed.

### Supported functionality

- Four separate executables: `myinit`, `stage1`, `supervisor`, and `svctl`.
- PID 1 signal handling and child reaping, checked early-init status, boot timeout,
  bounded shutdown, and fail-closed handling of unexpected supervisor exit.
- Early procfs, sysfs, devtmpfs and `/run` tmpfs setup with existing-mount checks.
- Executable directory services with deterministic dependency ordering. Cycles,
  self references and missing dependencies fail before services start.
- Foreground process supervision, process-group cleanup, exponential restart
  backoff, idempotent start/stop, and restart after old-instance cleanup.
- Private mode-0600 Unix control sockets, ownership/instance locks, stale-path
  protection, bounded nonblocking I/O and strict command validation.
- CLI status/start/stop/restart, with meaningful response and usage exit codes.
- No additional third-party runtime libraries beyond libc/libstdc++.

### Reliability and validation

- Children restore default signal dispositions and an empty mask before exec;
  asynchronous error pipes distinguish failed setup/exec from successful exec.
- Shutdown deadlines are checked under control traffic, TERM is delivered once,
  and KILL escalation avoids blocking waits.
- Tests cover discovery/dependencies, process churn and reaping, duplicate
  prevention, failed exec/resource exhaustion, restart timing, manual commands,
  socket/path safety, malformed/partial traffic, timeouts, and supervisor failure.
- Release gates require GCC Debug/Release, Clang ASan/UBSan with LeakSanitizer
  enabled, all 27 integration scenarios, five connection/readiness regressions,
  source formatting/whitespace checks, and a static Release QEMU boot smoke.
- The diskless, networkless VM checks real PID 1 boot, mounts, service commands
  and bounded shutdown. Kernel, BusyBox and initramfs hashes are recorded in CI.

### Known limitations

- Linux only; stage1 requires Linux 5.8+ mount-ID support and matching headers.
- Services and configuration are trusted administrator code and must remain in
  the foreground. Process groups are lifecycle management, not a security sandbox.
- Dependencies gate on exec success, not application readiness. There is no
  cascading dependency stop, oneshot service type, reload or finish hook.
- Supervisor death causes controlled shutdown rather than automatic recovery.
- Shutdown leaves real PID 1 alive in reap-only mode. Physical poweroff/reboot,
  filesystem teardown, device/session/network managers and PTY setup are absent.
- Console/log I/O is inherited and unmanaged. Kernel stalls and uninterruptible
  tasks can exceed userspace shutdown-policy deadlines.
- Validation is limited to the documented Linux toolchains and VM smoke path;
  broad hardware and VM fault-injection coverage remain future work.

### Distribution

Source-only release. No prebuilt binaries or VM images are attached. Build and
stage into a disposable rootfs using the README; never replace the host's PID 1
for testing. The annotated `v0.1.0` tag identifies the verified merge commit.

See `docs/RELEASE-AUDIT.md`, `docs/VALIDATION.md`, and the repository Actions logs
for the engineering review, exact validation evidence and VM input hashes.
