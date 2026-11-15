# v0.1.0 initial audit

All four source files, CMakeLists.txt and README.md were read before changes.
Baseline: upstream a6fd544a4f91dff757618cebe6c118ee4b2d4dd6.
The baseline builds with GCC on Arch Linux; the README's source-path warning is obsolete.

## P0 — lifecycle and boot safety

- Both fork paths exec with blocked signals; services cannot reliably receive TERM.
  Inherited ignored dispositions also survive exec. Supervisor omits INT handling.
- PID 1 starts supervision after stage1 failure, may exit on shutdown or syscall
  failure, has no boot deadline, and can wait forever for an unresponsive child.
- Supervisor shutdown only checks its deadline when epoll times out. A busy
  socket can postpone it indefinitely; blocking waitpid after KILL can hang.
- Services are tracked only by leader PID: descendants survive stop, restart,
  leader death and supervisor failure. Restarting the supervisor risks duplicates.
- Blocking accept/recv/write lets one client freeze reaping and shutdown.
  Writes can terminate the supervisor with SIGPIPE.
- stage1 mounts over existing filesystems, hides /dev behind empty tmpfs,
  and changes the host hostname even when given a scratch root.

## P1 — correctness and control boundary

- Fork failures can spin immediately; backoff resets after only one second.
  A failed exec is briefly treated as a running dependency.
- Manual stop/restart has no TERM deadline; repeated start clears crash backoff.
- Cycles, self references and missing dependencies are silently discarded.
  Discovery failure is indistinguishable from an empty directory.
- Socket creation unlinks arbitrary paths and live sockets, has no restrictive
  permissions, no instance lock, and can run services with no control endpoint.
- Protocol assumes a request arrives in one recv, silently truncates at 255
  bytes, accepts extra arguments, and has no memory/time limits.
- CLI truncates long socket paths, ignores short writes and recv errors, can
  block forever, and considers empty/truncated replies successful.
- waitpid/read/write paths inadequately handle EINTR; descriptors and socket
  cleanup are duplicated across error paths.

## P2 — maintainability and evidence

- No tests, sanitizers, version metadata, install layout, strict warnings or
  reproducible boot fixture. Headers rely on indirect declarations.
- README describes implemented supervision and CLI as planned, documents a
  nonexistent finish hook, and recommends unsafe host-side stage1 testing.

## Implementation decisions

Preserve PID 1 → stage1 → supervisor and svctl. Use one event loop, explicit
desired/stopping/reaped service state, process groups and a Linux subreaper.
Keep PID mappings until reaping and groups until cleanup. A CLOEXEC error pipe
distinguishes successful exec from failed child setup without blocking the loop.
Dependencies gate startup after exec, not application readiness; reject invalid
graphs. Fail closed on unexpected supervisor death rather than restarting over
unaccounted-for services. Host init testing requires explicit test mode and fake
stage1; real stage1 requires a PID 1 parent. Real PID 1 remains alive after bounded
shutdown. Physical poweroff, filesystem teardown and recovery are future work.

The initial source snapshot was obtained through the connected GitHub reader
because the shell cannot resolve github.com. Local git history starts with an
explicit snapshot import; it does not contain upstream commit history. No remote
branches, commits, tags or releases were created.

## Linux references reviewed

- [signalfd and inherited signal masks](https://man7.org/linux/man-pages/man2/signalfd.2.html)
- [Child subreapers](https://man7.org/linux/man-pages/man2/PR_SET_CHILD_SUBREAPER.2const.html)
- [Parent-death signals and credential-change caveats](https://man7.org/linux/man-pages/man2/PR_SET_PDEATHSIG.2const.html)
