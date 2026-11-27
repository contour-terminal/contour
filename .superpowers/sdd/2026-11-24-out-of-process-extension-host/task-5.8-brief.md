### Task 5.8: Gate
- [ ] **Step 1: ASan/LSan.** Build every vtextension test binary, the three executables and `contour-extension-host-wamr`; `ctest --preset=clang-asan -R 'vtextension|wasm_runtime_containment'` → all pass. Record:
  - contract cases per composition (now including the WAMR in-process host and the WAMR helper);
  - `uncleanExits == 0`;
  - every helper Hello `LockedDown`.
- [ ] **Step 2: TSan** (`out/clang-tsan`, the same targets) → zero reports, no suppressions. The TSan helper locks down (its background thread is covered by Landlock TSYNC and seccomp TSYNC).
- [ ] **Step 3: Release** (`out/clang-release`): the helper suites whole, Signals mode, `RequireLockedDown`, then the syscall audit.
  - Re-run Task 5.2 Step 1's strace over the sandboxed helper.
  - No syscall is refused or killed outside the expected rows.
  - Kernel audit (`journalctl -k --since` for `type=1326`) shows no `SECCOMP` kill.
- [ ] **Step 4: Coverage** (`out/clang-coverage`).
  - `.gcda` files from the sandboxed helper appear, which proves the coverage rows.
  - Lines of `Sandbox*.cpp`, `SeccompFilter.cpp`, `SystemCallRows.cpp`, `Landlock.cpp`, `HostComposition.cpp`, `WamrEngine.cpp`, `AppContainer_win32.cpp` (wine has no gcov: excluded), `HostProcess.cpp` and `HostServer.cpp`.
  - Name the kinds of uncovered lines.
- [ ] **Step 5: Measurement** (release, three runs, medians).
  - Plasma steady/uncached through the sandboxed Wasmtime helper (against phase 4's 8.4 µs).
  - The WAMR helper against in-process WAMR.
  - Round trip; spawn to `RuntimeCreated` (including priming and lockdown).
  - Idle helper VmRSS.
  - Thresholds per D108; record everything in "Phase-5 facts".
- [ ] **Step 6: Windows cross-build + wine**, as Task 5.7 Step 6, on the gate head.
- [ ] **Step 7: The CI checklist** (only with the owner's push), listed as outstanding otherwise:
  - Linux arm64: the `membarrier` row and the aarch64 table, with the contract sandboxed;
  - FreeBSD: Capsicum and the WAMR helper; phase 4's descriptor attribution;
  - macOS: the profile, the signed helper with and without the entitlement; phase 4's `CMSG_SPACE`;
  - Windows: AppContainer, placeholders, cross-view commits.
- [ ] **Step 8: Phase review** (`/code-review` of `009b13c5e..HEAD`); address every finding.
- [ ] **Step 9: Phase summary.**
  - Performance (Step 5).
  - Risk (D90, D92, D95, D98, D109, D110).
  - Coverage.
  - What wine, CI and reading did and did not prove, per platform.

### Phase-5 facts
*(Filled in by 5.2 Step 1, 5.3 Step 1 and 5.8.)*

From 5.1:
- **Wire:** `ProtocolVersion = 4`. `proto::LockdownStep : uint8_t` (Pdu.hpp) `{None, CoreDumps, Dumpable, Tracing,
  FaultReports, NoNewPrivileges, FileSystem, SystemCalls, ThreadSync, ResourceCaps, Capabilities, DescriptorRights,
  Profile, Container, Mitigations}`; `Hello{version, memoryMode, lockdown, failedStep, failureCode}` (`failureCode`
  = errno or the HRESULT's bit pattern, `static_cast<uint32_t>`). Hello's row cap stays 16 (largest body 13). A new
  step is an enumerator, a case in `Codec.cpp`'s `isEnumerator(LockdownStep)` switch and a row in
  `proto/LockdownSteps.cpp` (at its index; `lockdownSteps()`/`lockdownStepText()` in `vtextension_remote_wire`).
  `LockdownSteps.oneRowPerStep` ties table and codec (one past the last row is BadEnum).
- **Seam:** `host/Sandbox.hpp`: `ConfinementFailure{LockdownStep step; int code}` (`==` defaulted),
  `confinementFailureText()` = `"<step text>: <systemErrorText(code)>"`, `SandboxDescriptors{NativeHandle control}`
  (the adopted control end's value), `ISandbox{restrictBeforeThreads(), lockDown(), lockdownState()}` -- the
  third says what Hello states when both succeeded (`NullSandbox`: `Unsandboxed`; a real one: `LockedDown`).
  `makeSystemSandbox()` is defined per system; `host/CMakeLists.txt`'s `_sandbox_source` is `Sandbox_none.cpp` for
  every system today -- a platform task sets it to its own file.
- **Bootstrap:** `HardeningRow{LockdownStep step; apply}` (no text: the table phrases it); `hardenProcess()` and
  `capResources()` return `expected<void, ConfinementFailure>` (caps: `ResourceCaps` + errno, every cap applied
  past a refusal; `Skip` is success).
- **Order** (`runExtensionHost`): bind → `beforeThreads` (harden) → adopt → engine → `confinement.sandbox(
  {control})` → `restrictBeforeThreads()` → `HostProcess::run`: listener thread, then the server: 4 executors,
  each `prepareThread()` then latch count-down; control `prepareThread()`; latch wait → `confine` = caps then
  `lockDown()` → Hello. First failure of hardened/restricted/capped/lockedDown is stated. Tell lines: `hardening
  failed: …`, `sandbox failed: …` (only the sandbox's first failure), `resource caps failed: …`.
- `HostConfinement` is now `{bindToParent, beforeThreads, sandbox, startThread, confine, tell}`; `startThread`
  (an `ExecutorStarter`, now declared in `ExecutorThread.hpp`) starts the listener (`InterruptListenerStackBytes`)
  and every executor (`ExecutorStackBytes`): tests tell them apart by stack size. `InterruptListener::start` takes
  the starter. `HostServerConfig::confine` returns `expected<LockdownState, ConfinementFailure>`.
- **Priming:** `IHostedEngine::prepareThread()`. A lane whose prepare fails returns without serving; the server
  ends `ExecutorUnavailable` (exit 75), confines nothing, sends nothing. Wasmtime: each `WasmtimeEngine` facade
  compiles `(module (func (export "p")))` once (its own `wasm_engine_t`: the facade's trap mode, `Private`
  memory, 64 KiB stack, epoch deadline 1), then per thread a store + `wasmtime_instance_new` + call; a
  `thread_local` holds the serial of the last facade that primed the thread (idempotent per thread and engine).
  `wasmtime::testing::primingCompilations()` counts compiles process-wide. The copying engine primes too (same
  class). `testing/PreparationHookEngine.hpp` wraps an engine with a hook run before each `prepareThread()`.
- The ASan tree skips the Signals section of `WasmtimeEngine.prepareThreadRunsTheGuestPath` (it asserts the
  thread gains a `sigaltstack` it lacked): run it in a release tree once, as 5.2 measures there anyway.
- **Refusal:** `IHostConnector::refused(HostRefusal)` (pure virtual; `HelperIncarnation::connect` calls it beside
  `HostRefusalLog::refused(refusal, failedStep)`). `ProcessHostConnector` keeps `LockdownHistory::Failed` on a
  `LockdownFailed` refusal; later `connect()` → `HostUnavailable`, no launch, no line (the backend's log said it).
  `refusalLine(LockdownFailed, step)` = "The extension host could not confine itself, and was refused: <step
  text>. Extensions do not run." The parent does not phrase `failureCode` (helper-side `systemErrorText`).
- `FakeProcessLauncher(LaunchHook)`: the hook runs at launch with the spec, whose inherited handles are open
  until launch returns -- a test plays the host by duplicating `spec.inherited[0]` and sending on it.
  `ProcessHostConnector.lockdownFailureIsNotRelaunched` lives in `vtextension_remote_test`
  (`process/LockdownRefusal_test.cpp`, added only where `_process_source`), since `vtextension_process_test` does
  not link `HelperIncarnation`. `PduCodec.*`/`LockdownSteps.*` live in `vtextension_test`.
- The hostile host passes `systemConfinement()` with `sandbox` = a `NullSandbox` factory.

From 5.2:
- **Item 0.1 (release tree):** `WasmtimeEngine.prepareThreadRunsTheGuestPath` passes with its Signals section
  (18 assertions: a fresh thread lacks a `sigaltstack` before priming and has one after). The strace of the
  unsandboxed release helper agrees: `rt_sigaction` appears only before lockdown (3 per helper: SEGV/ILL/FPE at
  engine priming), never after. No `rt_sigaction` row; `sigaltstack` after lockdown is thread exit only.
- **Measured allow-list (D93):** strace `-ff` of `vtextension_helper_test "~[.measure]"` in release, ASan, TSan
  and coverage (182/181/181/182 helpers), calls after a temporary `PR_GET_NO_NEW_PRIVS` marker at `lockDown()`.
  Release: shutdown, sendto, recvfrom, munmap, close, brk, recvmsg (fd = control only), madvise, sigaltstack,
  rt_sigprocmask, exit, mmap (never PROT_EXEC; shared only with MAP_FIXED), mprotect (RX/RW/NONE, never W+X),
  fstat, ioctl (`TCGETS2` on fd 2, every CreateRuntime: Wasmtime's isatty -> refused ENOTTY), write,
  exit_group, futex, mremap, rt_sigreturn (Signals traps, SIGILL), poll, restart_syscall, and 2x
  openat+read of `/proc/sys/vm/overcommit_memory` (glibc shrinking a thread arena; refused EACCES, handled).
  ASan adds getpid, gettid, sched_yield (+ LSan's clone/wait4/ptrace/prctl at exit, gone with
  `detect_leaks=0`). TSan adds clock_gettime (12855), gettimeofday, nanosleep. Coverage adds openat/read/lseek/
  fcntl(F_SETLKW)/getpid and `mmap(MAP_SHARED)` unfixed of counter files (2.9 M writes). ftruncate added to the
  coverage rows from GCDAProfiling's source (a counter file new to the tree), not seen.
- **Filter:** `host/linux/{SystemCallRows,SeccompFilter,Landlock}`; numbers per arch are our table
  (`systemCallNumbers()`, one row per `SystemCall`). One row per number per set; a row whose number matches
  decides (checks pass -> action, else its local `ret KILL`), so jumps stay < 256. `ArgumentTest` gained
  `NotEquals` (needed for "shared => fixed" and "not W+X" without two rows per number). mmap forbids
  `PROT_EXEC` outright (stricter than D92: nothing maps executable after lockdown). Int arguments compare the low
  half only. `rowsFor(BuildInstrumentation, arch)` merges; `buildRows(arch)` = `rowsFor(ThisBuildInstrumentation)`.
  aarch64 rows include `membarrier` (only there). `seccompReturnOf()` maps an action to its return value.
- **Landlock:** kernel here reports ABI 10; TSYNC is ABI 8 (kernel docs: "Thread synchronization (ABI < 8)").
  Our table covers 1..10 (RESOLVE_UNIX at 9, UDP at 10 handled too). `LandlockSystem` has four pointers (close
  added, so a fake never closes a real descriptor). Return convention: result or `-errno`.
- **Instrumented builds:** sanitizer rows allow openat (Landlock grants `/proc` read only); coverage rows allow
  openat/fcntl/lseek/ftruncate and shared unfixed mmap (Landlock grants the build tree, `CONTOUR_COVERAGE_OBJECT_ROOT`
  = `CMAKE_BINARY_DIR`, defined only when `CONTOUR_COVERAGE`). So **without Landlock, an instrumented helper can open
  files**; release cannot. Helper environment (`LaunchedHelper.hpp` `sanitizerEnvironment()`): `detect_leaks=0`
  appended to ASAN/LSAN_OPTIONS in sanitizer builds, whether set or not.
- `ThisPlatformSandbox` (Sandbox.hpp, `CONTOUR_PLATFORM_SANDBOX` public on vtextension_host); CMake switch
  `VTEXTENSION_LINUX_SANDBOX` (src/vtextension/CMakeLists.txt: Linux on x86_64/aarch64). Compositions:
  `HelperLockdown` (HelperComposition.hpp) = RequireLockedDown where a sandbox exists.
- **Probe:** `--receive-grant [--sandboxed] [--outer-filter=no-landlock|flatpak|passrights|thread]
  [--sandbox-op=…]` (`process/testing/SandboxProbe.cpp`); lockdown report on lane 0, op report on lane 1
  (one lane raced: `readReport` swallowed the second report in the coverage tree). The parent reads `/proc/<pid>/task/*/status`
  (Landlock forbids the probe opening it after restrict). A killed op: the second report never comes, exit
  `Signalled SIGSYS`.
- `Sandbox.systemWithoutOneRunsUnsandboxed` (remote_test) SKIPs where a sandbox exists: it locked down the
  test binary itself (SIGSYS later). Never call `makeSystemSandbox()`'s steps in a test process.
- **Coverage tree:** its `.gcda` files were corrupt (helpers killed mid-write); the profile runtime then
  segfaults at exit in every process. `find out/clang-coverage -name '*.gcda' -delete` before a coverage run.

## Review Focus addendum (phase 5)
11. **A syscall taken only on a rare path kills a helper in the field.** Examples: the first trap on a thread, growth, a large compile, an aarch64 icache flush, a sanitizer report. Pinned by:
    - the measured allow-list (5.2 Step 1, re-audited at the gate);
    - the release-tree helper suites under `RequireLockedDown`;
    - `"HostServer.everyThreadIsPreparedBeforeConfinement"`;
    - `uncleanExits == 0`, which counts a `SIGSYS`.

    Review rule: every row says why, and no row allows a call from `"SeccompFilter.noRowSetAllowsTheForbidden"`.
12. **A thread outside the sandbox.** A thread started before no_new_privs/Landlock, or a TSYNC that did not reach one. Pinned by `"HostProcess.locksDownInTheSandboxOrder"`, `"Sandbox.landlockCoversEveryThread"` and `"Sandbox.threadSyncFailureFailsLockdown"`.
13. **Systems without Landlock, or inside an outer filter (Flatpak, old kernels).** The helper must lock down by seccomp alone or say `Failed` with its step, never run silently unsandboxed. The parent must still launch. Pinned by `"Sandbox.locksDownWithoutLandlock"`, `"Sandbox.locksDownUnderAFlatpakLikeFilter"` and `"Channel.refusingDescriptorsMayBeUnavailable"`.
14. **A lockdown failure loop.** One failing system must not spawn a helper per load. Pinned by `"ProcessHostConnector.lockdownFailureIsNotRelaunched"`.
15. **The GUI growing an engine back.** Pinned by the `nm` check (5.4 Step 5) until Task 6.2's lint rule replaces it.
16. **Platforms flipped on evidence we do not have.** macOS, FreeBSD and Windows sandboxes are proven by CI only (D110).

## Task order
5.1 → 5.2 → 5.3 → 5.4 → 5.5, 5.6, 5.7 → 5.8.
- 5.3 depends only on 5.1 and may run beside 5.2 (disjoint directories; both touch `remote/CMakeLists.txt`, so they merge in order).
- 5.5, 5.6 and 5.7 are independent of each other. 5.7 goes first of the three because wine gives it local feedback.
- 5.4 needs 5.2 (a sandbox to require) and 5.3 (a helper without Wasmtime).

## Phase-5 decisions

Proposed for a controller ruling. Each names the choice and, after the dash, why.

- D85. Eight tasks: 5.1 seam and order → 5.2 Linux → 5.3 WAMR helper → 5.4 Linux default → 5.5 FreeBSD, 5.6 macOS, 5.7 Windows → 5.8 gate. The outline's 5.1 splits into the seam (5.1) and Linux (5.2), and WAMR moves before the default switch. — The order, priming and refusal path are platform-free and testable with fakes. A Linux build without Wasmtime must have a helper the moment the default flips.
- D86. `host::ISandbox` has two steps: `restrictBeforeThreads()` (what threads inherit: Linux no_new_privs + Landlock; FreeBSD `PROC_NO_NEW_PRIVS`) and `lockDown()` (after every thread is prepared: seccomp TSYNC, `cap_enter`, Seatbelt, the Windows verification).
  - Made by `makeSystemSandbox(SandboxDescriptors)`.
  - `NullSandbox` → `Unsandboxed`.
  - Every step runs, and the first failure is stated, as D66 does for hardening.

  — Spec §5.6's order has two points, and only one of them is per thread.
- D87. One step vocabulary: `HardeningStep` folds into `proto::LockdownStep`, and `HardeningFailure` becomes `ConfinementFailure`. `runExtensionHost` order: bind → harden → adopt → engine → restrict → listener + executors → prepare → caps → lockDown → Hello. Caps run before seccomp. — One table phrases every confinement failure. Caps first means `setrlimit` needs no row.
- D88. Priming is `IHostedEngine::prepareThread()`, on every executor and on the control thread (which runs start functions, R29), behind a latch before `confine`. A failure → `ExecutorUnavailable`.
  - Wasmtime instantiates and calls a built-in one-function module. That installs its lazily-installed handlers and the thread's sigaltstack, so `rt_sigaction` needs no row.
  - WAMR initializes its thread environment.
  - The listener thread runs no guest, so it is not primed.

  — Phase 4 measured lazy handler installation; priming makes the post-lockdown syscall set deterministic.
- D89. Hello states the failed step and code (`failedStep`, `failureCode`): `ProtocolVersion = 4`. The parent's `LockdownFailed` line names the step from the shared table. — Users on unusual kernels and in Flatpak get a reason. Release helpers' stderr reaches nobody.
- D90. **Deviation, needs a ruling.** A `LockdownFailed` refusal is sticky in its `ProcessHostConnector`: later connects fail `HostUnavailable` without launching, and the breaker counts faults as usual. It is not "open the breaker at once" (spec §5.3; D52 assigned it here). — A lockdown failure is a property of the system and build, not of an extension. This needs no new error across the seam. Cost: it stays refused until the backend is recreated (restart), even if the cause was transient (ENOMEM).
- D91. Seccomp is hand-assembled classic BPF, with no libseccomp.
  - Arch check (x86_64, aarch64; anything else and the x32 bit → kill).
  - Rows from three tables (release, sanitizer, coverage) plus `SandboxDescriptors`, in frequency order.
  - Default `SECCOMP_RET_KILL_PROCESS`.
  - Tested through a test-only BPF evaluator.

  — No new dependency, and argument checks become unit-testable without killing a process.
- D92. Argument policy:
  - recvmsg only on the control descriptor;
  - sendmsg/sendmmsg denied entirely (the helper sends with `send`; R38);
  - socket/socketpair → `EPERM`; open/openat/openat2 → `EACCES` (soft refusal for library probes; Landlock is the second layer);
  - connect/bind/accept/listen killed;
  - mmap never `W|X`; `MAP_SHARED` only with `MAP_FIXED` and without `PROT_EXEC`; mprotect never `W|X` (W^X enforced);
  - clone*/fork/vfork/exec*/ptrace/process_vm_* killed;
  - kill/tgkill only on the own tgid;
  - read/write/poll are **not** descriptor-filtered.

  — After lockdown the helper can acquire no descriptor except those the parent grants, so a descriptor filter adds rows without adding a boundary.
- D93. The allow-list is measured, not guessed: strace of the unsandboxed helper across the helper suites in all four trees, from the no_new_privs marker on. It is re-audited at the gate against kernel seccomp audit records. — The engine's rare paths (traps, growth, compile) are what a guessed list misses.
- D94. Landlock:
  - uses the best ABI from our own constant table;
  - handles every FS right of that ABI with no rules (deny all);
  - denies TCP bind/connect (ABI ≥ 4) and scopes abstract-unix and signals (ABI ≥ 6);
  - uses `LANDLOCK_RESTRICT_SELF_TSYNC` where the ABI has it.
  - Unavailable (`ENOSYS`/`EOPNOTSUPP`) is **not** a failure: seccomp alone confines, and it is told once. Other errors → `FileSystem`.

  — Fedora, Ubuntu and Debian kernels differ, and Flatpak or an LSM list may lack it. Failing every extension there would push users to in-process builds.
- D95. Flatpak:
  - nested seccomp under bwrap's filter is fine (no_new_privs is already set);
  - Landlock follows D94;
  - the parent's `SO_PASSRIGHTS` treats `EPERM`/`EACCES` like `ENOPROTOOPT`/`EINVAL` (Discarded, not `Io`), because the helper's own filter now denies upward descriptors;
  - an outer filter is emulated by the probe.
  - A real in-Flatpak run is outstanding for Task 6.3: a host-built test binary does not run in the Platform runtime, and only Platform (no Sdk) is installed here.

  — This closes the 4.7 phase-input minor. The emulation covers the two mechanisms that differ.
- D96. Instrumented builds:
  - sanitizer helpers get `detect_leaks=0` (LSan's stop-the-world needs ptrace) and a Landlock read rule on `/proc`, so reports are unsymbolized (module+offset);
  - coverage helpers open the build tree's object root before Landlock and get a write rule plus the coverage seccomp rows;
  - D57's leak half moves to the in-process `HostServer` tests, as spec §5.6 says.

  — Every widening is compile-time and lives in its own row set.
- D97. The hostile host stays unsandboxed (`NullSandbox`, `AcceptUnsandboxed`). — Its relay forwards descriptors with sendmsg, and its rows test Contour's defences, not the sandbox's.
- D98. **Deviation, flag.** WAMR in the helper keeps WAMR's own WASI (`LIBC_WASI`), not our shim. One null device is opened when the engine is made (before lockdown) and shared by every runtime. The memory mode is Copy unless the 5.3 spike finds an in-place linear-memory hook that a system WAMR also exposes. — WAMR creates its WASI context whenever a module imports WASI, whatever resolves the imports, so a descriptor is needed either way. Contract equality across both engines is the proof that behaviour matches.
- D99. The helper links the engine `CONTOUR_WASM_RUNTIME` names. Where Wasmtime is also present, a test-only `contour-extension-host-wamr` and `vtextension_wamr_helper_test` are built. — The FreeBSD/RPM helper is then proven sandboxed on this Linux box, not only in CI.
- D100. CMake:
  - `CONTOUR_EXTENSION_HOST` takes `out-of-process` or `in-process`. The default is `out-of-process` iff `CMAKE_SYSTEM_NAME` is in `_CONTOUR_SANDBOXED_PLATFORMS`; Linux is added in 5.4, and the others by their tasks.
  - `CONTOUR_WASM_RUNTIME` now names the engine: default `wasmtime` if one resolved and out-of-process, else `wamr`. `in-process` + `wasmtime` → FATAL_ERROR.
  - Only programs link `vtextension_runtime`; test binaries name their composition (`vtextension_test` links `vtextension_wamr`).

  — One table row per platform flip, and tests do not silently start spawning helpers.
- D101. `process::HostComposition` (system launcher, supervisor, kind, connector) is shared by every default backend in a process through a weak-cached `sharedHostComposition()`. The supervisor ends with the last backend, not at static destruction. Teardown grace follows `ThisBuildInstrumentation`. — One supervisor per hosting process (spec §5.3), with no `jthread` joined during exit.
- D102. In the build tree, a POST_BUILD copy places the helper beside `contour` and `bench-headless`, and `locateExtensionHost`'s "beside" candidate finds it. No build-tree path is compiled into a binary. — Developer runs work, and installed binaries never consult a path outside their install.
- D103. R37 stands: the lease is the object size. — Seccomp cannot tell regions apart, so a compromised helper may `mprotect` its own `PROT_NONE` tail; leasing the initial size would undercount what it can dirty.
- D104. FreeBSD:
  - Capsicum rights per descriptor role (a table), no ioctls, fcntl `GETFL` only, `cap_enter` in `lockDown`;
  - `RLIMIT_AS = dataBytes` (closes D38's open FreeBSD cap; WAMR Copy reserves nothing extra);
  - `PROC_NO_NEW_PRIVS_CTL` where defined.
  - Proven by CI only. Capsicum cannot refuse `SCM_RIGHTS` on a socket with `SEND`; D70/R38 cover the parent.
- D105. macOS:
  - a constexpr deny-default profile with parameters, shm read/write-data by prefix only (R38), no network, mach-lookup, file reads, fork or exec;
  - hand-declared `sandbox_init_with_parameters`;
  - `allow-unsigned-executable-memory` on the helper only, through `support/macOS/contour-extension-host.entitlements`, with the helper signed before the bundle;
  - a CI case with and without the entitlement.
  - No parent-death binding remains (documented, spec §5.3).
- D106. Windows: the **parent** confines at launch, and the helper verifies.
  - Launch: an AppContainer profile `Contour.ExtensionHost` with no capabilities, plus `CHILD_PROCESS_POLICY` restricted, on top of the existing job.
  - The profile and an idempotent `ALL APPLICATION PACKAGES` RX ACE on the helper's directory are ensured once per supervisor, on its thread, through an injectable API table.
  - Helper: not an AppContainer token → `Unsandboxed`. Otherwise it applies the mitigation rows and checks the job; a failure → `Failed`; all pass → `LockedDown`.
  - Dynamic-code policy stays off.

  — An AppContainer can only be entered at creation. Distinguishing "not contained" from "failed" keeps wine and test runs meaningful.
- D107. Under wine (`ntdll!wine_get_version`), test compositions use `ProcessIsolation::None` and `AcceptUnsandboxed`, and AppContainer cases SKIP with a reason. — Wine stubs AppContainer, so a Failed-everywhere run would hide every other Windows regression.
- D108. Gate thresholds:
  - steady render through the sandboxed helper: stop and report if the median is above phase 4's range (8.3–9.2 µs) beyond noise (> 9.5 µs) — filter order is the lever;
  - spawn to `RuntimeCreated`: stop above 2× phase 4 (2.6 ms) — priming is the lever;
  - the WAMR helper: reported against in-process WAMR, never gated (spec §10.2: "as isolated, but not faster").
- D109. **Residual risks, for the owner.**
  - (a) `newfstatat`/`statx` with `AT_EMPTY_PATH` must be allowed for `fstat`, and a path given with it still resolves: file metadata (not contents) stays observable. Landlock does not cover stat.
  - (b) Where Landlock is absent, files are guarded by seccomp alone.
  - (c) macOS has no parent-death binding.
  - (d) FreeBSD threads are not bounded by `RLIMIT_NPROC` (it counts processes).

  Proposed: accept and document in Task 6.4.
- D110. **Needs the owner.** macOS, FreeBSD and Windows join `_CONTOUR_SANDBOXED_PLATFORMS` in their tasks, but the gate passes only if their CI legs ran green, which needs a push. Without it, their rows are left out at the gate (those platforms stay in-process), and the code lands dormant. — Flipping a platform's default on reading alone risks every extension failing there.

Rulings on D85–D110 (R39–R43), applied by the controller:
- R39. D85–D110 are accepted, except as changed below.
- R40 (D90). Accepted: a `LockdownFailed` refusal is sticky per `ProcessHostConnector` and clears only when the backend is recreated. Why: a lockdown failure is a property of the system and build; relaunching per extension would spawn a doomed helper per pane. Cost if wrong: after a transient failure (ENOMEM) extensions stay off until restart; the Hello's named step (D89) makes that diagnosable.
- R41 (D98). Accepted, with the 5.3 spike time-boxed to one hour of investigation; if no in-place hook that a system WAMR also exposes is found, Copy is final for the WAMR helper in this branch. Cost: the WAMR helper pays the copy path (phase 4 measured 18.7 µs vs 8.4 µs steady), which spec §10.2 already allows ("as isolated, but not faster").
- R42 (D109). Residual risks (a)–(d) are accepted provisionally and documented in 6.4; they are surfaced to the owner, who may overrule.
- R43 (D110). Accepted. Without the owner's permission to push, macOS, FreeBSD and Windows stay in-process at the gate and their sandbox code lands dormant (compiled where CI would compile it, tested here as far as wine/fakes allow). Only Linux flips by default.

---

## Phase 6 — Cache, packaging, documentation, gates


---

# Controller notes for the gate (binding; they override the text above where they differ)

## Scope of this dispatch
Do Steps 1–6 and the reporting half of Step 7. **Step 8 (the phase review) and Step 9 (the summary) are the controller's.** Your report feeds them. The decision on which platforms join `_CONTOUR_SANDBOXED_PLATFORMS` is also the controller's. Do not append any platform.

## Item 0: carried over from the review of the last CI fix (do it first; fold each change into the commit it belongs to, or make a separate commit where it has no natural owner)
1. `HelperComposition.cpp:129` calls `errorLog.setSink()` from every constructor, which races with a running supervisor that reads the sink. TSan in Step 2 would report it. Install the sink once.
2. `scripts/check-wasm-runtime-containment.py:133-138` misses two cases. Make the lint see both, and extend its self-test:
   - targets made in a `foreach` (`${target}`, for example `contour_gui_test_heavy`);
   - a variable-named runtime link (`vtextension_${…}`) combined with a named engine.
3. Move the host unit tests that need no engine (`ExecutorThread_test`, `Sandbox_test`) into a binary built wherever `vtextension_host` is, for example `vtextension_remote_wamr_test`, so they also run on FreeBSD and in builds without Wasmtime.

## Resource limits
Another Claude session is still busy on this machine.
- **One tree at a time**, in the order ASan → TSan → release → coverage → the wine cross tree, never two concurrently. Use `flock -o … nice -n 19 ionice -c 3 … -j 4`.
- Configure missing trees with `PACE_SCHEDULER=127.0.0.1:6674`.
- Before the coverage run, delete stale counters: `find out/clang-coverage -name '*.gcda' -delete`.
- Measurements (Step 5): run them while no build of yours is running, and note in the report whether the other session was busy, since that adds noise. Three runs, medians.

## Step 7: real CI instead of a checklist
The owner allowed the push, and draft PR #2431 runs CI on every push, including FreeBSD through the `ci-freebsd` label.
- Run `gh pr checks 2431` and record per leg: Linux x86_64 (several), macOSArm, macOS (no GUI frontend), FreeBSD, Windows, and the lints.
- For each leg that failed, read its log (`gh api --allow-escape-sequences repos/contour-terminal/contour/actions/jobs/<id>/logs`) and fix the cause the same way as before: fold the fix in, rebase, do NOT push.
- What CI has shown so far:
  - FreeBSD passed with the Capsicum sandbox locking down for real, at `9fb129082`.
  - The macOS sandbox has not completed a run yet.
  - Windows failed to link at `9fb129082` (fixed since).
  - The current pushed head is `223c1aa25`.
- **Linux arm64:** there is no CI leg for it here. Say so; that stays outstanding.

## Before reporting
- Run lint (`-L lint`), tidy-diff, clang-format, typos and editorconfig.
- Each commit must build on its own.
- `git commit -s`, no trailers. Never mention the plan, task numbers or "phase".

## Report
Write `task-5.8-report.md`. Include:
- Steps 1–6 with numbers, including Step 5's table against D108's thresholds, PASS or STOP;
- Step 7 per CI leg;
- the "Phase-5 facts" to record;
- deviations and concerns.
