# v0.1.0 stabilization report

## Release assessment

Strazh v0.1.0 is an experimental MVP; production readiness is not claimed.
The historical runs below establish integration, leak-detection and VM evidence.
The [final engineering audit](RELEASE-AUDIT.md) defines the current release gates:
each final candidate and the resulting main merge must pass the complete workflow
before an annotated tag or GitHub Release may be published.

Initial stabilization used a source snapshot from
`a6fd544a4f91dff757618cebe6c118ee4b2d4dd6` because shell DNS prevented cloning.
The current `release/v0.1.0` branch follows the actual upstream Git history and
updates [PR #1](https://github.com/kuroki727/strazh/pull/1). Follow-up commits were
published through the connected GitHub API with expected-head checks because
shell Git networking remained unavailable.

## CI and VM follow-up — 2026-11-15

The [PR run](https://github.com/kuroki727/strazh/actions/runs/43745110988) and
[branch run](https://github.com/kuroki727/strazh/actions/runs/43745105859) both
passed on implementation commit `843873071e77fc32a3942c57bb05123089555dc4`:

- GCC Debug and Release: all four CTest groups passed.
- Clang ASan/UBSan: all groups passed with `detect_leaks=1:abort_on_error=1`
  and `UBSAN_OPTIONS=halt_on_error=1`.
- All 27 integration scenarios plus five connection/readiness regressions passed;
  there were no skipped tests. The host-safety group also ran its three original
  cases and the five regressions.
- QEMU boot smoke passed after the test matrix. Guest procfs, sysfs, devtmpfs,
  `/run` tmpfs, service stop/start/restart, and successful bounded shutdown were
  checked. The log contains `STRAZH_QEMU_PASS` and PID 1's successful shutdown
  marker. The VM had no disks, network or host directory shares.

The permission failure was a test readiness race: an existing stale socket with
mode 0755 was inspected before the supervisor replaced it. Readiness now requires
an actual supervisor status reply. Socket creation additionally sets mode 0600
on the Linux socket descriptor before bind and verifies the filesystem entry
before listen; the permission assertion and ownership/stale/replacement tests
remain intact. The client-burst test retries only EAGAIN, uses fresh sockets and
a two-second total connect deadline, and requires a real status reply afterwards.
Other connect failures and invalid readiness responses are not hidden.

VM inputs recorded in the job log: Ubuntu kernel `6.8.0-149-generic`, QEMU
`1:8.2.2+ds-0ubuntu1.18`, BusyBox `1:1.36.1-6ubuntu3.1`. SHA-256 values:

```text
kernel deb  b8757670b7ec8aead358d4fba420064b9f31d344c3393548105aca2b3956d2ab
kernel      9e9bb539fbf5e25c3fa737180e3586dacd0922257cdffe319daf8e5ceecaeaf0
BusyBox     dbac288c29ba568459550a2da9e7ae0ded6b1fc728ee9fad3044c44e62d6ac14
initramfs   87ad085473214a2751a81162a98ba57d09546b6e878b52aff56a68a76d6aae76
```

The local sandbox limitations below remain true for local execution; they no
longer block the CI/VM evidence above. VM fault injection and broader hardware
coverage remain future validation, as does any production-readiness assessment.

## Critical defects and implemented changes

- Children inherited blocked TERM/INT/CHLD and possibly ignored dispositions.
  Shared child setup now resets dispositions/mask before exec, establishes a
  separate session and sets a parent-death signal with a parent-race check.
- Failed stage1 was treated as successful boot; PID 1 could exit or wait forever.
  Boot now checks the exact child status, has a 30-second deadline, and fails
  closed. TERM/INT initiate bounded cleanup. Real PID 1 remains alive in a
  reap-only state after normal shutdown or failure.
- Unexpected supervisor death could restart over surviving services. PID 1 now
  shuts down adopted children and reports failure, with no automatic supervisor
  restart. This is an intentional MVP policy, not transparent recovery.
- A service's descendants could outlive stop/restart or leader exit. Supervision
  now combines sessions/process groups with subreaping, retains mappings through
  waitpid, and retains group slots until old-instance cleanup finishes.
- Manual stop/restart could hang; fork errors could spin; crash delays reset too
  quickly. Each stop has a fixed TERM deadline and KILL escalation. Automatic
  delays grow from 500 ms to 30 s, including fork/pipe failures. Only a 30-second
  stable run resets the next automatic delay, and start does not bypass backoff.
- Cycles and missing dependencies were ignored, and failed exec looked ready.
  Invalid graphs now reject startup before spawning; an asynchronous CLOEXEC
  error pipe gates dependents on actual exec success. Dependency semantics and
  application-readiness limitations are documented.
- A client could block reaping/shutdown or kill the supervisor with SIGPIPE.
  Nonblocking per-client state supports partial transfers, limits requests,
  caps concurrency, sets total lifetimes, and suppresses SIGPIPE on sends.
  Deadlines run regardless of event traffic; accept resource errors pause
  listening briefly instead of spinning.
- Socket creation could unlink arbitrary files or another live supervisor's
  endpoint. Socket and service-directory locks, owner/type/path checks, private
  permissions, stale probing and inode-checked cleanup protect ownership.
- CLI could truncate paths, silently lose partial writes, hang, or succeed on
  empty/truncated replies. It now validates syntax, bounds socket I/O and response
  memory, verifies framing, and returns consistent 0/1/2 codes.
- Stage1 could hide existing mounts and device nodes and change host hostname.
  It now validates real directories, uses mount IDs to detect existing/bind
  mounts, preserves matching filesystem types, uses devtmpfs and `/run` tmpfs
  (mode 0755), and leaves hostname unchanged. Host use is validation-only;
  real execution requires explicit boot mode and a PID 1 parent.

The four-executable architecture and directory service format are retained.
Small RAII helpers own descriptors and provide bounded exception cleanup;
service lifecycle transitions and stream framing are shared directly with tests.
There is no added third-party runtime dependency.

## Initial local validation (historical)

Environment: Arch Linux, kernel `7.2.15-arch1-1`, GCC `16.2.1`, Clang `23.1.4`,
CMake `4.4.5`, Python `3.14.10`. The upstream baseline already compiled here;
its README incorrectly claimed broken source paths.

| Configuration | Build | CTest result |
| --- | --- | --- |
| GCC Debug, strict warnings as errors | all four executables and fixtures pass | core, control, host-safety pass; integration skipped |
| GCC Release, strict warnings as errors | all four executables and fixtures pass | core, control, host-safety pass; integration skipped |
| Clang Debug, ASan + UBSan, warnings as errors | all four executables and fixtures pass | core, control, host-safety pass; integration skipped |
| GCC static Release, warnings as errors, tests off | all four executables pass | build only; VM not run |

The sanitizer pass used `ASAN_OPTIONS=detect_leaks=0:abort_on_error=1` and
`UBSAN_OPTIONS=halt_on_error=1`. No ASan or UBSan diagnostics occurred. An initial
run with leak detection enabled aborted because LeakSanitizer cannot perform its
ptrace inspection in this sandbox. This is **not** a successful leak check.
No host kernel security settings were modified.

The core test exercises actual fork/exec/kill/waitpid behavior for discovery,
ordering, cycles/missing dependencies, restored signals, duplicate prevention,
manual commands, blocked dependents, failed exec, descriptor exhaustion,
exponential timing/capping, stubborn-service timeout, single-TERM graceful shutdown, descendant cleanup,
orphan churn and reaping. TERM and INT delivery through signalfd is checked.

The control test uses Unix socket pairs with the production stream functions:
fragmented/incomplete/oversized requests, disconnected peers, forced partial
writes, fragmented CLI responses, invalid/empty/oversized replies and a total
socket deadline. The host-safety group runs three Python cases covering stage1
and init guards, unchanged host mounts/hostname, failed stage1/exec paths,
unexpected fake-supervisor exit, adopted-child cleanup and the eight-second
shutdown timeout. It never starts the real early-init mount path.

The separate Python integration suite contains 27 scenarios. This sandbox denies
Unix socket bind with EPERM, so the suite cannot run here. CTest explicitly
reports it skipped. `STRAZH_REQUIRE_INTEGRATION=1` turns this restriction into
failure rather than allowing a release gate to skip. The subsequent unrestricted CI results are recorded above.

The QEMU archive writer's newc records were inspected with `cpio -itv`, including
character-device metadata without extracting nodes. Dynamic ELF rejection and
Python syntax were checked. Actual static Strazh binaries were built; full
rootfs assembly and VM execution need a supplied static BusyBox and QEMU, neither
available in this environment. Staged installation and static ELF inspection
are also recorded in the task's command results.

Final review covered lifecycle state transitions, map/group lifetime, fatal
setup cleanup, socket ownership, deadline checks under traffic and documentation
consistency. `git diff --check` passes. No test helper processes remained after
local runs. This is code-review and component-test evidence, not proof that no
critical defect remains in untested boot/integration paths.

## Remaining limits and next steps

1. Keep the required unrestricted integration and leak-detection gates green.
   Review the passing CI results and the change set before considering a tag.
2. Extend [the QEMU fixture](QEMU.md) with fault injection: failing/hung stage1,
   supervisor death, unexpected preexisting mounts and shutdown under churn.
   Basic real PID 1 boot and mount policy passed; these additional VM fault paths
   have not yet been validated.
3. Review the documented policy choices: foreground services, exec-based startup
   gates, no cascading dependency stops and fail-closed supervisor death.
4. Physical poweroff/reboot and filesystem teardown, emergency recovery,
   readiness/oneshot services, logging, reload and cgroup containment remain
   future work. Inherited console/log I/O is not managed; kernel stalls and
   uninterruptible tasks can exceed userspace shutdown policy deadlines.
5. Preserve existing release tags and publish only verified source. Passing
   these release gates does not establish production readiness.

## Files changed

Existing: `main.cpp`, `stage1.cpp`, `supervisor.cpp`, `svctl.cpp`,
`CMakeLists.txt`, `README.md`. `LICENSE` is unchanged.

Added: `linux_support.h`, `control_socket.h`, `.gitignore`,
`tests/core.cpp`, `tests/control.cpp`, `tests/test_service.cpp`,
`tests/integration.py`, `docs/AUDIT.md`, `docs/VALIDATION.md`, `docs/QEMU.md`,
`tools/make-qemu-rootfs.py`, `tools/qemu-smoke.py`, `.github/workflows/ci.yml`.
