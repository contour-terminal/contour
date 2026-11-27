# Phase 3 fix wave — report

Base 6e99bc621 (branch `feature/out-of-process-extension-host`). Six commits, items 1–9 of
`phase3-fixwave-brief.md`.

## Commits

| SHA | Subject | Items |
|---|---|---|
| 7f5467d0c | fix(vtextension): accept the interrupt page rounded up to the system page | 1 (Critical) |
| be85242a2 | fix(vtextension): keep a stricter inherited limit when the host caps itself | 5 |
| 53e2e07ac | feat(vtextension): harden the extension host before it starts a thread | 2 (+R26) |
| bff81e34d | fix(vtextension): start extension hosts away from Contour's terminal | 3, 6, 7, 9 |
| 85ddbc4c3 | feat(vtextension): log once why no extension host serves | 4 |
| 4ec0d13f6 | refactor(vtextension): drop what the extension host API never reads | 8 |

## Per item

### 1 — Apple Silicon control page (Critical)
- New pure decision `transport::isControlPageSize(objectBytes, pageBytes)` (ControlPage.hpp):
  accepted iff `ControlPageBytes <= size <= roundUp(ControlPageBytes, pageBytes)`. Decision: lower
  bound exact page bytes, upper bound the page-rounded size; only the first 4096 bytes are ever mapped.
- `ControlPage::adopt` (POSIX) uses it with `sysconf(_SC_PAGESIZE)`; comment says guest-sized shared
  memory must not trust fstat() either (no "phase" wording in source). Windows adopt unchanged
  (VirtualQuery region size is exact).
- Tests: `ControlPage.sizeMayBeRoundedUpToThePage` (pure, runs everywhere; 16 KiB case included);
  `ControlPage.adoptRefusesAWrongSize` now uses 4 × page (larger than any rounding; 64 KiB on Apple
  silicon) and, off Apple, half a page; fresh shm object per size on Apple (XNU sizes once).
- Mutation evidence: restoring exact-size semantics in `isControlPageSize` → 1 assertion fails (16384/16384).

### 5 — stricter inherited RLIMIT_DATA
- `capResources` reads the inherited limit and caps to `min(wanted, inherited.rlim_max)`, soft = hard.
- Failing-first: `HostBootstrap.stricterInheritedLimitCountsAsCapped` (fork; child lowers RLIMIT_DATA hard
  to 128 MiB, caps at 256 MiB) failed before the fix (`1 == 0`), passes after.

### 2 — pre-thread hardening step (+ R26)
- `HostConfinement` gains `beforeThreads` (`std::function<LockdownState()>`), injected like `confine`
  (R21). `runExtensionHost`: parse → bindToParent → **beforeThreads** → adoptEndpoints → engine → serve
  (confine after threads). A `Failed` from `beforeThreads` makes Hello state `Failed` regardless of
  `confine`.
- `hardenProcess()` (HostBootstrap): RLIMIT_CORE 0/0 on all POSIX; Linux `PR_SET_DUMPABLE 0`; FreeBSD
  `procctl(PROC_TRACE_CTL, PROC_TRACE_CTL_DISABLE)`; Windows `WerSetFlags(WER_FAULT_REPORTING_NO_UI)`
  (kernel32). Decision: applied in every build (not gated by BuildCapPolicy) — it does not hinder
  sanitizers (LSan's stop-the-world re-enables dumpable itself).
- Docs corrected: plan D37 (`docs/superpowers/plans/…md`) and `phase3-expansion.md` D37 now say phase-5
  no_new_privs/Landlock hook into `beforeThreads`, only seccomp+TSYNC into `confine` (local files, not
  committed). HostBootstrap.hpp file comment updated.
- Tests:
  - `HostProcess.hardensBeforeAnyThreadStarts` (in-process, both Unsandboxed and Failed hardening):
    hardened at step 0, engine at 1, confine at 2; on Linux the thread count grows by ≥ lanes+1
    between hardening and confine; Hello carries the hardening failure. `wrongParentExits` asserts no
    hardening, `noEngineExits` asserts one.
  - `Helper.hardensItselfBeforeHello` (real helper, Linux, every build): `/proc/<pid>/limits` Max core
    file size 0/0, and `/proc/<pid>/status` is no longer owned by our uid (non-dumpable). Note: the
    `/proc/<pid>` *directory* keeps the real owner (kernel `task_dump_owner` exempts it) — the first
    version of the test statted the directory and failed for that reason; it stats `status` now.
    `/proc/<pid>/limits` and `status` stay 0444, so `Helper.capsItselfBeforeHello` still reads them.
- Mutation evidence: `beforeThreads` → no-op in `systemConfinement()` fails both real-helper checks;
  moving the `beforeThreads` call after engine creation fails the order test (hardenedAt 1, engineMadeAt 0).

### 3 / 6 / 9 / 7 — launcher (one commit; see Deviations)
- **6**: `posix::spawn(PreparedLaunch&)` in PosixLaunch.cpp is the one posix_spawn setup (Linux +
  macOS; closefrom_np vs CLOEXEC_DEFAULT the only `#ifdef`); `posix::reapChild(pid)` the one waitpid
  reap loop (macOS + FreeBSD).
- **3**: `POSIX_SPAWN_SETSID` in the shared spawn (glibc ≥ 2.26, macOS), `setsid()` first in FreeBSD's
  `becomeChild`. `LaunchSpec::standardError` nullopt now means **the null device** (was: parent's
  stderr) on all platforms; `PreparedLaunch::childStandardError()`. Production default
  `ProcessHostConfig::standardError = defaultHelperStandardError()`: Contour's own stderr in
  sanitizer/coverage builds (`CONTOUR_INSTRUMENTED_BUILD`, PRIVATE define on
  vtextension_remote_process when CONTOUR_SANITIZE OR CONTOUR_COVERAGE — matches spec §5.6 "sanitizer
  builds give the helper a diagnostics descriptor; release builds have none"), null device otherwise.
  Tests inject theirs (`HelperLaunch::standardError = ownStandardError()`, HelperComposition's original
  stderr copy). Windows: no `GetStdHandle` fallback any more — null device unless named;
  `CREATE_NO_WINDOW` kept as it was (a console of its own that shows no window, not Contour's);
  `DETACHED_PROCESS` not added — it is exclusive with CREATE_NO_WINDOW and buys nothing more here.
  - Probe reports `stderr=` (null/terminal/other), `pid=`, `sid=`.
  - Tests: `ProcessLauncher.childStderrIsTheNullDeviceUnlessGiven` (all platforms, wine incl.),
    `ProcessLauncher.childLeadsASessionOfItsOwn` (POSIX: sid == own pid, != test's sid).
  - Mutation evidence: dropping SETSID fails both session checks; stderr fallback to fd 2 fails the
    stderr test (`{ "other" } == { "null" }`).
- **9**: `addChdir()`: deployment target ≥ macOS 26 → POSIX `posix_spawn_file_actions_addchdir`;
  SDK knows 26 (`__MAC_26_0`) but older target → `__builtin_available(macOS 26.0, *)` picks it at run
  time, else `_np`; older SDK / glibc → `_np`. -Werror-safe by construction: the `_np` call is only
  compiled where the deployment target is below its deprecation version.
- **7**: `IProcessHandle::tryReap()` now returns `std::expected<std::optional<ExitStatus>, LaunchError>`.
  Only ECHILD yields `LostExitStatus` (−1); any other waitid/waitpid/WaitForSingleObject failure is
  returned. Supervisor: such a helper is killed, logged (`launchErrorText()`), and forgotten as an
  unclean exit (it would otherwise be looked at on every wait). Linux: `probePidfdWaits()` once in the
  launcher's constructor (`waitid(P_PIDFD)` on its own pidfd → ECHILD supported / EINVAL not; pidfd_open
  ENOSYS → not); `launch()` returns `Unsupported` before anything is spawned. LaunchError doc: kernel 5.4.
  - Test: `Supervisor.unreapableHelperIsStoppedAndForgotten` (FakeProcessLauncher gains `failReap()`).
    Mutation: disabling the error branch → 6 of 8 assertions fail.
  - The P_PIDFD probe itself is not exercised by a test (this kernel has it); by inspection.

### 4 — refusals logged once per reason
- `remote/HostRefusals.{hpp,cpp}`: `enum class HostRefusal` + `RefusalTable` (one row per reason,
  static_assert-ordered), `HostRefusalLog` (injected `Writer`, wait-free `fetch_or` bitmask: once per
  reason per log), `errorLogWriter()` → `errorLog()`. All reasons go to the error log (the only level
  core::log has), lockdown refusals included.
- `ProcessHostConnector` (ctor gains the writer; one log per connector = per backend) maps
  NotFound/Resources/Unsupported/**Io** → reasons (Stopped is not logged).
- `HelperIncarnation::connect(…, HostRefusalLog&)`: `refusalOf(hello, requirement)` replaces
  `isAcceptable` (version → lockdown Failed / Unsandboxed-required → memory mode). `RemoteBackend` owns
  one `shared_ptr<HostRefusalLog>` threaded through RemoteRuntime/HelperSlot.
- Tests: `HostRefusals.everyReasonHasItsLine` (no engine names), `.eachReasonIsLoggedOnce`,
  `.oneLineHoweverManyThreadsRefuse` ([threading]); `ProcessHostConnector.launchFailureIsLoggedOncePerReason`
  (real supervisor + system launcher, missing helper, 3 attempts → 1 line); 
  `RemoteBackend.refusedHelloIsLoggedOncePerReason` (scripted host, 4 reasons × 2 attempts → 1 line each).
- Mutation evidence: disabling the once-bit → 4 failures; skipping the call in connect → 4 failures.

### 8 — unused API surface
- `BackendConfig::helper` removed (the connector's `ProcessHostConfig::helper` is the one source);
  composition roots untouched (they construct `BackendConfig {}`).
- `IHostHandle::interrupt(instanceId, slot)` → `interrupt(slot)` in both implementations and the test
  doubles; the incarnation still arms the lanes by id first.
- `LaneWatch(Set)::interrupt` returns `void`; tests assert through `observe()` (deep-nesting case
  reordered so the unarmable deep frame is checked before the armable one).

## Verification
All runs with `unset $(env | grep -o '^CONTOUR_[A-Z_]*')`, builds `flock -o … nice -n 19 ionice -c 3 … -j 4`.

| Run | Result |
|---|---|
| clang-asan, whole tree (`cmake --build --preset clang-asan`, GUI included) | clean, -Werror |
| `ctest --preset=clang-asan -R vtextension` | 5/5 passed (48 s) |
| ASan `vtextension_process_test` | 85 cases: 84 passed, 1 skipped |
| ASan `vtextension_remote_test` | 218 cases / 10825 assertions, all passed |
| ASan `vtextension_helper_test` | 153 cases: 152 passed, 1 skipped (the R22 caps case) |
| TSan `vtextension_process_test` (all, incl. `[threading]` HostRefusals) | 85 cases: 84 passed, 1 skipped, no TSan report |
| TSan `vtextension_remote_test` (all) | 218 cases / 10825 assertions, no TSan report |
| clang-release `ctest -R vtextension` | 5/5 passed |
| release `vtextension_helper_test "Helper.*"` (capsItselfBeforeHello runs here) | 5 cases / 56 assertions passed |
| release `HostBootstrap.*,HostProcess.hardens*` | 4 cases / 55 assertions passed |
| release `[hostility]` | 7 cases / 1375 assertions passed |
| clang-cl cross (out/clangcl-debug, -Werror): process test, probe, helper, hostile helper, helper test | clean |
| wine 11.20 (own prefix, `zlib1.dll` on WINEPATH) `vtextension_process_test.exe` | 77 cases passed (incl. `childStderrIsTheNullDeviceUnlessGiven`) |
| wine `vtextension_helper_test.exe "~[.measure]"` | 124 cases: 122 passed, 2 skipped (no guests in that config) — so `WerSetFlags` succeeds under wine and Hello is not `Failed` |

- clang-tidy, by hand on every changed .cpp (Linux-compiled ones):
  `nice -n 19 clang-tidy -p out/clang-asan --quiet <files>` — clean after one fix
  (`cppcoreguidelines-pro-type-member-init` on the test's RefusalRow). Not run on `_macos`/`_freebsd`/`_win32`
  sources (not in the Linux compile database).
- `clang-format --dry-run --Werror` and `editorconfig-checker` over every changed file: clean.
- `wineserver -k` run on my own prefix only. No process killed by name or pattern.
- Performance: no hot path touched (launch path gains one setsid flag; the P_PIDFD probe runs once per
  launcher; refusal log is one atomic fetch_or on the failure path only).
- Coverage: not measured in this wave (no coverage-tree run); every new branch has a test or a recorded
  mutation, except the P_PIDFD-unsupported branch and the platform-only code (macOS/FreeBSD/Windows WER
  failure).

## Deviations
- Items 3, 6, 7 and 9 share one commit: they rewrite the same functions (the shared spawn and reap,
  every `tryReap` override) and splitting by hunk without an interactive add was not practical;
  the commit body names each part.
- Item 4 also logs `LaunchError::Io` (as "the system refused"); the brief listed three.
- Item 2: hardening applies in every build, not only under `CapPolicy::Apply`; the real-helper test
  runs in all builds.

## Concerns
- macOS / FreeBSD code is not compiled here (read only): `POSIX_SPAWN_SETSID` (Darwin 0x0400),
  `posix_spawn_file_actions_addchdir` / `__MAC_26_0` / `__builtin_available` tiering, `PROC_TRACE_CTL`,
  `reapChild`. If the macOS 26 SDK does not in fact declare the non-_np addchdir, the `__MAC_26_0`
  branch fails to compile — CI on a 26 SDK will tell.
- `hardenProcess()` failing makes Hello `Failed`: under a Windows where `WerSetFlags` fails, every host
  would be refused (checked under wine: see Verification).
- The hostile host keeps its own `withoutCoreDump()` (it crashes in its proxy, outside the real host's
  hardening path).
