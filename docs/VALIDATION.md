# v0.1.0 stabilization report

## Release assessment

The implementation is a testable MVP candidate. **A stable release is not yet
verified:** named-socket integration, LeakSanitizer and actual VM boot remain
release gates. No tag, push, merge, release or host init installation was made.

Work is on local branch `stabilize/v0.1.0`. Shell DNS prevented a Git clone;
the seven upstream files were imported through the connected GitHub reader from
`a6fd544a4f91dff757618cebe6c118ee4b2d4dd6`. Their original blob contents were
preserved as a local baseline. The local history is a snapshot import, not the
upstream Git history. Apply the resulting commits/patch to a full upstream clone
before preparing a PR; do not push the import as replacement upstream history.

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

## Verified locally

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
failure rather than allowing a release gate to skip. A checked-in CI matrix
requires integration, but has not been pushed or run remotely.

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

1. Apply the patch to the real upstream history and run the full suite on an
   unrestricted Linux machine with `STRAZH_REQUIRE_INTEGRATION=1`, including leak
   detection. Review the CI results before considering a tag.
2. Supply pinned kernel/BusyBox inputs and run [the QEMU smoke fixture](QEMU.md).
   Test failing/hung stage1, supervisor death, unexpected preexisting mounts and
   shutdown under churn in the VM. Real PID 1 and mount policy are unverified here.
3. Review the documented policy choices: foreground services, exec-based startup
   gates, no cascading dependency stops and fail-closed supervisor death.
4. Physical poweroff/reboot and filesystem teardown, emergency recovery,
   readiness/oneshot services, logging, reload and cgroup containment remain
   future work. Inherited console/log I/O is not managed; kernel stalls and
   uninterruptible tasks can exceed userspace shutdown policy deadlines.
5. Create a release tag only after explicit review and approval. No production
   readiness claim is justified by the current evidence.

## Files changed

Existing: `main.cpp`, `stage1.cpp`, `supervisor.cpp`, `svctl.cpp`,
`CMakeLists.txt`, `README.md`. `LICENSE` is unchanged.

Added: `linux_support.h`, `control_socket.h`, `.gitignore`,
`tests/core.cpp`, `tests/control.cpp`, `tests/test_service.cpp`,
`tests/integration.py`, `docs/AUDIT.md`, `docs/VALIDATION.md`, `docs/QEMU.md`,
`tools/make-qemu-rootfs.py`, `tools/qemu-smoke.py`, `.github/workflows/ci.yml`.
