# v0.1.0 final engineering audit

Starting candidate: `22db27d84a3d9f6b03d8fe34b0964311da3c747c`.
The final candidate and resulting main merge must pass the checked-in workflow;
prior green runs are historical evidence, not a substitute for these gates.

## Scope and findings

The review covered every tracked source/header, CMake file, test, workflow,
document and QEMU tool. The four executable boundaries and C++17 implementation
are preserved. No new runtime feature, dependency or background process was added.

No unresolved critical or high-severity issue was identified within the stated
trusted-configuration, foreground-service model. This is an engineering review
and automated-test assessment, not a formal proof or production certification.

- PID 1 checks stage1 status, reaps independently of coalesced SIGCHLD, handles
  TERM/INT, has boot/shutdown deadlines and never intentionally exits as PID 1.
  Supervisor loss fails closed. Child exec restores signals; setup descriptors
  use CLOEXEC and RAII. Fatal paths retain bounded cleanup and a reap-only fallback.
- Supervisor PID mappings survive until waitpid, and old groups are retained
  through cleanup. Manual state changes cannot spawn over a stopping instance.
  Automatic restarts and resource failures back off; dependency graph errors
  reject configuration and failed exec does not release dependents.
- Control sockets require a trusted parent, owner/type checks, inode-checked
  cleanup, instance locks, peer credentials and mode 0600 before listen. Client
  traffic is bounded and nonblocking, with no SIGPIPE termination path.
- Stage1 validates mount targets and uses mount IDs to avoid covering existing
  filesystems. Actual mounting is restricted to explicit early boot; host tests
  use dry-run validation and fake stage1. Hostname is left unchanged.
- Medium test-only finding, fixed: the graceful fixture used a check/pause
  sequence that could lose a TERM wakeup. Blocking TERM and using sigsuspend
  makes waiting atomic without changing runtime behavior or weakening assertions.
- Low technical debt retained: fixed 50 ms event-loop polling, duplicate metadata
  checks while reading `after`, synchronous inherited diagnostic I/O, and several
  service state flags. These are future measurement/simplification candidates;
  last-minute abstraction or scheduler changes would add unnecessary risk.

The documented trust boundary is material: service configuration and privileged
service programs can modify the system. Same-UID/root adversaries, daemons that
escape their process groups, cgroup containment and hardware support are outside
this MVP's guarantees. Those limits are stated in the README and changelog.

## Release gates and publication

The workflow requires GCC Debug/Release, Clang ASan/UBSan with leak detection,
all 27 integration scenarios and five regressions, source formatting/whitespace
checks, and a static QEMU boot. Permission preflight verifies the Actions token's
write access without creating anything. Required validation jobs may not skip.

Publication is an explicit source-only Actions step after those gates pass on
main. It accepts only PR #1's merge commit, verifies the PR head and merge trees
match, checks main has not advanced and the worktree is clean, and refuses any
existing `v0.1.0` tag or release. It creates an annotated tag without force and
publishes `CHANGELOG.md` as the `Strazh v0.1.0` release notes. No binaries are
uploaded. Other main pushes cannot publish this release.

The automation is release tooling only; it adds no daemon, parser or runtime
mechanism to Strazh. It is used because local Git networking/CLI authentication
is unavailable and the connected GitHub tools do not expose tag/Release creation.
Normal GitHub merge and tag rules remain enforced; main is never force-pushed.
