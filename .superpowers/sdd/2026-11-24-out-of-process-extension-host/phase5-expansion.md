## Phase 5 — Sandboxes, and WAMR in the helper

Paths are relative to `src/vtextension/wasm/` unless they start with `src/`, `cmake/`, `scripts/`, `support/` or
`.github/`. Builds: `flock -o /tmp/contour-extension-host.build.lock nice -n 19 ionice -c 3 cmake --build --preset
clang-asan -j 4 --target <t>`. Test shells start with `unset $(env | grep -o '^CONTOUR_[A-Z_]*')`. Binaries are
under `out/clang-asan/src/vtextension/` (`vtextension_test`), `.../wasm/remote/` (process, remote and helper
tests) and `.../wasm/wasmtime/`. Before each commit: `ctest --preset=clang-asan -R vtextension`, clang-format
(`/usr/bin/clang-format`), editorconfig-checker. After each task: `/simplify`. Release, TSan and coverage trees
are used only where a step names them, one at a time.

**Where each platform is proven.** Linux x86_64: here (kernel 7.2, Landlock ABI with `RESTRICT_SELF_TSYNC`),
in all four trees. Linux arm64 (`membarrier`, the aarch64 syscall table): CI only. Windows: clang-cl
cross-build with `-Werror`, plus wine 11.20, which shows the plumbing but proves no containment (Task 5.7
says which). macOS and FreeBSD: reading only here; CI decides, and only if the owner allows a push.

### Task 5.1: The sandbox seam and the lockdown order (no OS mechanism yet)
**Files:**
- Create: `host/Sandbox{.hpp,.cpp,_none.cpp}` (`_none.cpp` is compiled where no platform file exists yet), `host/Sandbox_test.cpp` (remote_test).
- Modify:
  - `remote/proto/{Pdu.hpp,PduTable.hpp,Codec.cpp,Codec_test.cpp}`.
  - `host/{HostBootstrap.hpp,HostBootstrap_posix.cpp,HostBootstrap_win32.cpp,HostProcess.hpp,HostProcess.cpp,HostProcess_test.cpp,HostServer.hpp,HostServer.cpp,HostServer_test.cpp,IHostedEngine.hpp,CMakeLists.txt}`.
  - `wasmtime/{WasmtimeEngine.hpp,WasmtimeEngine.cpp,HostedEngine.cpp,WasmtimeEngine_test.cpp}`, `testing/HostedEngine.hpp`.
  - `remote/{HelperIncarnation.cpp,HostRefusals.hpp,HostRefusals.cpp,HostRefusals_test.cpp}`, `process/{ProcessHostConnector.hpp,.cpp}`, `process/testing/FakeProcessLauncher`, the supervisor/connector tests.
  - `src/contour-extension-host/testing/HostileHost.cpp`.

**Produces:**
- Proto:
  - `ProtocolVersion = 4`.
  - `LockdownStep : uint8_t { None, CoreDumps, Dumpable, Tracing, FaultReports, NoNewPrivileges, FileSystem, SystemCalls, ThreadSync, ResourceCaps, Capabilities, DescriptorRights, Profile, Container, Mitigations }`. The hardening steps fold in here (D87).
  - `Hello` gains `LockdownStep failedStep` and `uint32_t failureCode` (errno or HRESULT bit pattern).
- `lockdownSteps()` rows `{step, text}` in `remote_wire`; both sides phrase a step from it.
- `ConfinementFailure{LockdownStep step; int code}` replaces `HardeningFailure`. `hardenProcess()` returns `expected<void, ConfinementFailure>`.
- `host::ISandbox`:
  - `restrictBeforeThreads() -> expected<void, ConfinementFailure>` (what threads inherit);
  - `lockDown() -> expected<void, ConfinementFailure>` (after every thread started and was prepared).
- `SandboxDescriptors{NativeHandle control}`.
- `makeSystemSandbox(SandboxDescriptors) -> unique_ptr<ISandbox>`. `NullSandbox` reports `Unsandboxed` and is what `_none.cpp` returns.
- `IHostedEngine::prepareThread() -> expected<void, ExtensionError>`.
- `HostConfinement` gains `sandbox` (`function<unique_ptr<ISandbox>(SandboxDescriptors)>`). `confine` becomes caps then `lockDown`.
- `ProcessHostConnector` remembers a `LockdownFailed` refusal (D90).

- [ ] **Step 1: Write the order tests** (remote_test, scripted parent, recording fakes).
  - `"HostProcess.locksDownInTheSandboxOrder"`. The recorded sequence is exactly: bind, harden, adopt, engine, restrict, listener started, 4 executors started, 5 × prepareThread (4 lanes + control), caps, lockDown, Hello sent. `restrict` saw 0 threads started.
  - `"HostProcess.everyConfinementStepRunsPastAFailure"`: restrict fails `FileSystem` and lockDown fails `SystemCalls`. Both run. Hello is `{Failed, FileSystem, code}`, and one `sandbox failed: <text>: <system text>` line is told.
  - `"HostProcess.hardeningFailureIsStatedAsItsStep"`: a fake hardening row fails `Dumpable` → Hello `{Failed, Dumpable, code}`.
  - `"HostProcess.nullSandboxSaysUnsandboxed"`: Hello `{Unsandboxed, None, 0}`, which is today's behaviour.
- [ ] **Step 2: Write the server and engine tests.**
  - `"HostServer.everyThreadIsPreparedBeforeConfinement"`: a counting engine. At `confine`, prepareThread calls == `MaxLaneChannels + 1`, each on a distinct thread id, and the control thread is among them.
  - `"HostServer.unpreparedThreadEndsTheHost"`: lane 2's prepareThread fails → `ServerExit::ExecutorUnavailable`, confine never runs, nothing is sent.
  - `"WasmtimeEngine.prepareThreadRunsTheGuestPath"`: Software and Signals (counts as one of the two Signals cases, through `EngineSignalHandlers`). Returns ok, is idempotent per thread, and loads its built-in module once per engine.
- [ ] **Step 3: Write the wire, refusal and connector tests.**
  - `"PduCodec.protocolVersionIsFour"`. The Hello round-trip samples gain every `LockdownStep`. Over-cap and mutation cases cover the row.
  - `"LockdownSteps.oneRowPerStep"`: the texts name no engine.
  - `"HostRefusals.lockdownLineNamesTheStep"`: `refusalLine(LockdownFailed, step)` contains the step's text.
  - `"ProcessHostConnector.lockdownFailureIsNotRelaunched"` (FakeProcessLauncher): the first helper says `Failed` → `HostUnavailable`, and the line is logged once. Three later connects → `HostUnavailable` with `launches == 1`.
- [ ] **Step 4: Build** `vtextension_remote_test vtextension_process_test vtextension_wasmtime_test`; expect failure.
- [ ] **Step 5: Implement the wire row and the step table** (D89). Rename `HardeningStep` → `LockdownStep` throughout, and `HardeningFailure` → `ConfinementFailure`.
- [ ] **Step 6: Implement the seam and the order** (D86, D87).
  - `runExtensionHost`: bind → harden → adopt → engine → `sandbox(descriptors)->restrictBeforeThreads()` → `HostProcess` (listener, then executors) → prepare → caps → `lockDown()` → Hello.
  - Every step runs, and the first failure is the one stated.
  - The hostile host passes a `NullSandbox` factory (D97).
- [ ] **Step 7: Implement priming** (D88).
  - Executors call `prepareThread()` first and count down a latch. `HostServer::run()` prepares its own (control) thread, then waits on the latch, then runs `confine`.
  - Wasmtime: compile a built-in `(module (func (export "p")))` once per engine, then instantiate and call it per thread.
  - The copying engines and the test doubles: no-op.
- [ ] **Step 8: Implement the sticky refusal** (D90) and the refusal line.
- [ ] **Step 9: Run** `remote_test "HostProcess.*" "HostServer.*" "PduCodec.*" "LockdownSteps.*" "HostRefusals.*" "Sandbox.*"`, `process_test "ProcessHostConnector.*" "Supervisor.*"` and `wasmtime_test "WasmtimeEngine.*"` → PASS. Then `helper_test "~[.measure]"` whole → PASS (Unsandboxed; hostility unchanged); ctest → PASS.
- [ ] **Step 10: Commit** (three):
  - `refactor(vtextension): one table names every step of the extension host's confinement`
  - `feat(vtextension): the extension host locks down in a fixed order, its threads prepared first`
  - `feat(vtextension): say which confinement step of an extension host failed, and start it only once`

### Task 5.2: The Linux sandbox — seccomp, no_new_privs and Landlock
**Files:**
- Create:
  - `host/Sandbox_linux.cpp`.
  - `host/linux/{SeccompFilter.hpp,SeccompFilter.cpp,SystemCallRows.hpp,SystemCallRows.cpp,Landlock.hpp,Landlock.cpp}`.
  - `host/linux/testing/BpfEvaluator.hpp` (a test-only classic-BPF interpreter for `seccomp_data`).
  - `host/linux/{SeccompFilter_test,Landlock_test}.cpp` (process_test, Linux).
  - `process/SandboxProbe_test.cpp` (process_test, Linux): real lockdown in a child.
- Modify:
  - `process/testing/LaunchProbe.cpp`: gains `--sandbox-op=<op>`, `--outer-filter=<kind>` and `--sandboxed` (with `--receive-grant`).
  - `transport/Channel_posix.cpp`: `refuseDescriptors` gets the errno table of D95, with a test in `Channel_test.cpp`.
  - `remote/testing/ProcessDefaultBackend.cpp` and `process/testing/HelperComposition.cpp`: `RequireLockedDown` where `ThisPlatformSandbox != None`.
  - `host/CMakeLists.txt` and `remote/CMakeLists.txt`: the probe links `vtextension_host`.

**Produces:**
- `SystemCallAction { Kill, Refuse, Allow }`. `Refuse` carries an errno.
- `ArgumentCheck{uint8_t index; ArgumentTest test; uint64_t mask; uint64_t value}`, where `ArgumentTest { Equals, AllClear, AllSet, EqualsControl, EqualsOwnProcess }`.
- `SystemCallRow{long number; std::span<ArgumentCheck const> checks; SystemCallAction action; int error; std::string_view reason}`.
- `releaseRows()`, `sanitizerRows()`, `coverageRows()`, and `buildRows()` (release + the set `ThisBuildInstrumentation` names).
- `assembleFilter(span<SystemCallRow const> rows, SandboxDescriptors, int ownProcess, AuditArch) -> expected<vector<sock_filter>, ConfinementFailure>`.
- `installFilter(span<sock_filter const>) -> expected<void, ConfinementFailure>`: `SECCOMP_SET_MODE_FILTER` with `SECCOMP_FILTER_FLAG_TSYNC`. A positive return (a thread id) → `ThreadSync`.
- `LandlockAbiRow{int abi; uint64_t fsRights; uint64_t netRights; uint64_t scopes; uint32_t restrictFlags}` and `landlockAbis()`. The constants are ours, because CI headers are older.
- `LandlockOutcome { Unavailable, Restricted }`.
- `restrictFileSystem(span<PathBeneathRule const> allowed, LandlockSystem const&) -> expected<LandlockOutcome, ConfinementFailure>`. `LandlockSystem` is three injectable function pointers (DI for the syscalls).
- `ThisPlatformSandbox : enum class PlatformSandbox : uint8_t { None, Linux, FreeBSD, MacOS, Windows }`.

Probe ops table (`--sandbox-op`). The op runs on an `ExecutorThread` started before lockdown, after a go signal:

| Op | Expect |
|---|---|
| `open-file` (`/etc/hostname`) | refused `EACCES` |
| `open-directory` (`/`) | refused `EACCES` |
| `socket-inet`, `socket-unix` | refused `EPERM` |
| `send-descriptor` (sendmsg + `SCM_RIGHTS` on control) | killed `SIGSYS` |
| `fork`, `spawn-thread`, `execve /bin/true`, `ptrace-traceme` | killed `SIGSYS` |
| `signal-parent` (`kill(getppid(), 0)`) | killed `SIGSYS` |
| `map-writable-executable` | killed `SIGSYS` |
| `map-shared-unfixed` | killed `SIGSYS` |
| `protect-writable-executable` | killed `SIGSYS` |
| `permitted` | `ok`. Runs channel send/recv, `recvmsg` of a grant, `mmap(MAP_SHARED\|MAP_FIXED)` over a reservation, `mprotect` RW, `munmap`, `close`, `getrandom`, `clock_gettime`, a condvar wait (futex), `sigaltstack`, and `membarrier` on aarch64 |

- [ ] **Step 1: Derive the allow-list by measurement** (D93).
  - Run `strace -f -qq -o <scratchpad>/<tree>.%p` over the unsandboxed helper while `vtextension_helper_test "~[.measure]"` runs in `out/clang-asan` and `out/clang-release`. Then repeat for `out/clang-tsan` and `out/clang-coverage` (one tree at a time).
  - Keep only the calls after each helper's `prctl(PR_SET_NO_NEW_PRIVS)` (Task 5.1 placed that marker).
  - Tabulate syscall × tree. The scripts stay in the scratchpad and are not committed.
  - Every syscall becomes a row with a reason, or is named in the report as refused-and-handled.
- [ ] **Step 2: Write the filter tests** (`BpfEvaluator`, no installation).
  - `"SeccompFilter.decidesEachRowAsItsTableSays"`, a table of `(arch, nr, args) → action`:
    - sendto/recvfrom/futex → Allow;
    - recvmsg on control → Allow, recvmsg on a lane → Kill;
    - sendmsg, sendmmsg → Kill (R38);
    - socket(AF_INET), socket(AF_UNIX) → Refuse EPERM;
    - openat → Refuse EACCES (release) and Allow (coverage set);
    - mmap `PROT_WRITE|PROT_EXEC` → Kill; mmap `MAP_SHARED` without `MAP_FIXED` → Kill; mmap `MAP_SHARED|MAP_FIXED` RW → Allow;
    - mprotect W+X → Kill;
    - clone, clone3, fork, vfork, execve, execveat, ptrace → Kill;
    - tgkill(own) → Allow, tgkill(other) → Kill;
    - a foreign arch → Kill; `__X32_SYSCALL_BIT` → Kill; nr 4095 → Kill.
  - `"SeccompFilter.noRowSetAllowsTheForbidden"`: no row in any of the three sets allows execve*, ptrace, process_vm_*, socket, connect, bind, sendmsg, sendmmsg, clone*, fork, vfork, unshare, setns, mount, bpf or perf_event_open.
  - `"SeccompFilter.everyRowSaysWhy"`: the reason is non-empty, with no duplicate number per set.
  - `"SeccompFilter.assemblesForBothArchitectures"`: x86_64 and aarch64 row numbers (aarch64 has no `open`, `fork` or `poll`). The program stays ≤ `BPF_MAXINSNS`.
- [ ] **Step 3: Write the Landlock tests** (fake `LandlockSystem`).
  - `"Landlock.abiTableIsMonotonic"`.
  - `"Landlock.usesTheBestAbiTheKernelHas"`: ABI 3 → no net/scopes, no TSYNC; ABI ≥ the TSYNC row → `RESTRICT_SELF_TSYNC`.
  - `"Landlock.unavailableIsNotAFailure"`: `ENOSYS`, `EOPNOTSUPP` → `Unavailable`.
  - `"Landlock.otherErrorsAreFailures"`: `EPERM` from restrict_self (no NNP) → `FileSystem`.
- [ ] **Step 4: Write the real lockdown tests** (process_test, Linux; the probe in a child).
  - `"Sandbox.forbiddenOperationsFailFromAnExecutor"`: over the ops table, each row's expectation (refused errno reported on control, or `ExitStatus{Signaled, SIGSYS}`).
  - `"Sandbox.permittedOperationsWorkFromAnExecutor"`.
  - `"Sandbox.landlockCoversEveryThread"`: the probe reads `/proc/self/status` `Seccomp:\t2` and `NoNewPrivs:\t1` for each `/proc/self/task/*` (opened before lockdown). `open-file` is refused from a thread started before restrict too, when the ABI has TSYNC.
  - `"Sandbox.locksDownWithoutLandlock"` (`--outer-filter=no-landlock`: an outer ALLOW filter returning `ENOSYS` for `landlock_*`) → `LockedDown`, and `open-file` is still refused (the seccomp row).
  - `"Sandbox.locksDownUnderAFlatpakLikeFilter"` (`--outer-filter=flatpak`: ALLOW default; `clone3` → `ENOSYS`; `unshare`, `setns`, `mount`, `personality`, `ptrace` → `EPERM`) → `LockedDown`, and the permitted op is `ok`.
  - `"Sandbox.threadSyncFailureFailsLockdown"`: a thread installs its own filter first → `ThreadSync`.
  - `"Channel.refusingDescriptorsMayBeUnavailable"` (outer filter: setsockopt `SO_PASSRIGHTS` → `EPERM`): `makeChannelPair()` succeeds; `upward == Discarded`.
- [ ] **Step 5: Write the helper case.** `"Helper.locksDown"` (helper_test, Linux): Hello `{LockedDown, None, 0}`. The binary's composition now requires `LockedDown`, so the whole helper_test runs sandboxed, including `everyAllowedWasiFunctionWorks` and `[bundled]`.
- [ ] **Step 6: Build** `vtextension_process_test extension_host_probe vtextension_remote_test vtextension_helper_test contour-extension-host`; expect failure.
- [ ] **Step 7: Implement the filter** (D91, D92).
  - Hand-assembled cBPF: arch check, the x32 bit, then the rows in frequency order, default `SECCOMP_RET_KILL_PROCESS`.
  - Argument checks use 32-bit halves (`seccomp_data.args` low/high words).
- [ ] **Step 8: Implement Landlock and the sandbox** (D94, D96).
  - `restrictBeforeThreads`: `PR_SET_NO_NEW_PRIVS`, then Landlock. Every handled FS right of the ABI, with no rules except the instrumented builds' (coverage object root write, sanitizer `/proc` read). Also net TCP bind/connect and the abstract-unix + signal scopes where the ABI has them.
  - `lockDown`: `buildRows()` → assemble → install with TSYNC. Unavailable Landlock tells once: `the file system restriction is unavailable on this system; system calls are restricted`.
- [ ] **Step 9: Implement the instrumented rows** (D96).
  - Sanitizer helper environment: `detect_leaks=0` appended to `ASAN_OPTIONS`/`LSAN_OPTIONS` (a row in the launcher's sanitizer environment table).
  - Coverage: the helper opens `CONTOUR_COVERAGE_OBJECT_ROOT` (a compile definition, the build tree) before Landlock and adds a `PATH_BENEATH` write rule.
  - `refuseDescriptors`: `EPERM`/`EACCES` → `Discarded` (D95).
- [ ] **Step 10: Run.**
  - `process_test "SeccompFilter.*" "Landlock.*" "Sandbox.*" "Channel.*"` → PASS.
  - `helper_test "~[.measure]"` whole → PASS, with `uncleanExits == 0` (a `SIGSYS` kill would show there).
  - Then the release tree: `cmake --build --preset clang-release -j 4 --target vtextension_helper_test contour-extension-host extension_host_probe vtextension_process_test`, and the same two runs. This is Signals mode, which is the product's row set.
  - ctest → PASS.
- [ ] **Step 11: Commit** (three):
  - `feat(vtextension): the extension host restricts its system calls on Linux`
  - `feat(vtextension): the extension host gives up file system access on Linux before it starts a thread`
  - `fix(vtextension): an outer sandbox refusing descriptor control no longer stops every extension host`

### Task 5.3: `WamrEngine : IHostedEngine`, and a helper that runs it
Order change: this comes before the default switch, because a Linux build without Wasmtime (the Fedora RPM) must still have a helper when the default flips (D85).

**Files:**
- Create:
  - `wamr/{WamrEngine.hpp,WamrEngine.cpp,HelperEngine.cpp,WamrEngine_test.cpp}`. The test goes in `vtextension_test`, which already links WAMR.
  - `remote/testing/WamrHelperDefaultBackend.cpp` and the target `vtextension_wamr_helper_test` (D99).
- Modify:
  - `wamr/{WamrBackend.hpp,WamrBackend.cpp,WamrRuntime.hpp,CMakeLists.txt}`: the null device is injected.
  - `src/contour-extension-host/CMakeLists.txt`: the engine follows `CONTOUR_WASM_RUNTIME`; test-only `contour-extension-host-wamr`.
  - `remote/CMakeLists.txt`.
  - `cmake/ContourThirdParties.cmake`: only if the spike adopts a hook.

**Produces:**
- `wamr::NullDevice` (an owned descriptor, or `OpenPerRuntime`).
- `wamr::WamrConfig{NullDevice nullDevice}` and `makeWamrBackend(WamrConfig = {})`. In-process behaviour is unchanged.
- `wamr::makeWamrEngine() -> unique_ptr<host::IHostedEngine>`: `memoryMode() == Copy` (or `Shared` per the spike), `memoryGuardBytes() == 0`, and `prepareThread()` = WAMR's thread environment.
- The target `vtextension_wamr_helper_engine` defines `makeHelperEngine()` and `makeCopyingHelperEngine()` for WAMR.

- [ ] **Step 1: Spike** (D98): can WAMR 2.4.5 place a linear memory at a base we give and grow it in place?
  - Read `core/iwasm/common/wasm_memory.c` and the runtime's init options in the CPM checkout (`out/clang-asan/_deps/wamr-src`).
  - Rule: Shared only if such a hook exists **and** the configure probe can detect it in a system WAMR. Otherwise Copy.
  - Record the finding as a Phase-5 fact. Expected: Copy.
- [ ] **Step 2: Write the engine tests.**
  - `"WamrEngine.copiesMemory"`: `memoryMode() == Copy`, `memoryGuardBytes() == 0`.
  - `"WamrEngine.runtimesShareOneNullDevice"`: three runtimes and the engine's open-descriptor count stay unchanged. No `open()` happens after construction (counted through an injected opener).
  - `"WamrEngine.prepareThreadIsIdempotent"`.
  - The contract runs over a `RemoteBackend` in-process `HostServer` with `makeWamrEngine()` (a `[wasm-contract]` composition in remote_test, beside the Wasmtime one).
- [ ] **Step 3: Write the WAMR helper suite.** `vtextension_wamr_helper_test` is the contract, `[bundled]` and the Review Focus process cases against `contour-extension-host-wamr`, with `RequireLockedDown` on Linux. `"Helper.wamrHelperSaysCopy"`: Hello `{Copy, LockedDown}`.
- [ ] **Step 4: Build** `vtextension_test vtextension_remote_test contour-extension-host-wamr vtextension_wamr_helper_test`; expect failure.
- [ ] **Step 5: Implement** `NullDevice` injection, `WamrEngine` and `HelperEngine.cpp`. The engine opens the null device at construction, which is before lockdown (D98).
- [ ] **Step 6: Wire the helper's engine** (D99): `contour-extension-host` links `vtextension_${CONTOUR_WASM_RUNTIME}_helper_engine`. With Wasmtime present and the runtime `wasmtime`, `contour-extension-host-wamr` is also built (never installed). The status line `contour-extension-host is not built: …` goes.
- [ ] **Step 7: Run** `vtextension_test "WamrEngine.*" "[wasm-contract]"`, `remote_test "[wasm-contract]"` (both engines) and `vtextension_wamr_helper_test` whole → PASS; then the WAMR helper suite in the release tree; ctest → PASS.
- [ ] **Step 8: Commit** (two):
  - `feat(vtextension): the extension host can run its modules interpreted`
  - `build: build the extension host with whichever engine the build selected`

### Task 5.4: The default composition becomes out-of-process on Linux
**Files:**
- Create: `remote/process/{HostComposition.hpp,HostComposition.cpp,HostComposition_test.cpp}` (process_test), `remote/DefaultBackend.cpp` (target `vtextension_out_of_process`).
- Modify:
  - `CMakeLists.txt` (top level): `CONTOUR_EXTENSION_HOST`, `_CONTOUR_SANDBOXED_PLATFORMS`, `CONTOUR_WASM_RUNTIME` defaulting, summary lines.
  - `src/vtextension/CMakeLists.txt`: `vtextension_runtime`; `vtextension_test` links `vtextension_wamr` by name.
  - `src/contour-extension-host/CMakeLists.txt`: `contour_place_extension_host_beside()`.
  - `src/contour/CMakeLists.txt`, `src/vtbackend/CMakeLists.txt` (bench-headless).
  - `process/ProcessLauncher.hpp` and `Process_*.cpp`: the sanitizer environment row from 5.2 applies to production compositions too.
  - `scripts/check-wasm-runtime-containment.py`: only if the new target trips a rule; the new rules are Task 6.2's.

**Produces:**
- `process::HostComposition` (supervisor over `makeSystemProcessLauncher()`, kind, `ProcessHostConnector`).
- `process::sharedHostComposition(std::filesystem::path helper) -> std::shared_ptr<HostComposition>` (weak-cached, D101).
- `makeDefaultBackend(BackendConfig)` for out-of-process. With a null connector it locates the helper with `locateExtensionHost(vtpty::currentExecutablePath())`. If not found → every load `HostUnavailable`, with the `HelperNotFound` line logged once.
- CMake `contour_place_extension_host_beside(<target>)`.

- [ ] **Step 1: Write the tests.**
  - `"HostComposition.backendsShareOneSupervisor"`: two backends, one supervisor. Dropping both stops it, and a third backend gets a new one.
  - `"HostComposition.missingHelperRefusesEveryLoad"`: `HostUnavailable`, no launch, one log line.
  - `"HostComposition.teardownGraceFollowsTheBuild"`.
- [ ] **Step 2: Write the configure checks** (a script in the scratchpad, recorded in the report). Fresh configures under `<scratchpad>/cfg-*`:
  - `-DCONTOUR_EXTENSION_HOST=in-process -DCONTOUR_WASM_RUNTIME=wasmtime` → FATAL_ERROR naming both.
  - The default on Linux → `out-of-process` with `wasmtime` (CPM).
  - `-DCONTOUR_USE_CPM=OFF` with no Wasmtime → `out-of-process`, `wamr`.
  - An unknown value → FATAL_ERROR.
- [ ] **Step 3: Implement CMake** (D100).
- [ ] **Step 4: Implement the composition and the default backend** (D101, D102).
  - `ContourGuiApp`, `ContourApp` and `bench-headless` stay unchanged, as they call `makeDefaultBackend(BackendConfig{})`.
  - The build tree gets the helper copied beside `contour` and `bench-headless`. On macOS, the bundle's `Helpers/` copy stays as it is.
- [ ] **Step 5: Prove the GUI and daemon link no engine.**
  - `nm -C out/clang-asan/src/contour/contour | grep -cE 'wasm_runtime_|wasmtime_'` → 0.
  - The same for `bench-headless`.
  - `contour-extension-host` has them.
- [ ] **Step 6: Run the suites on the new default.**
  - ctest `-R 'vtextension|wasm_runtime_containment'` → PASS.
  - `bench-headless` extension rows run through a helper: the supervisor launch count > 0 in its output, or `strace -f -e trace=execve` shows the helper.
  - The `e2e`-labelled app runs pass (`ctest --preset=clang-asan -L e2e`).
- [ ] **Step 7: Run a manual check from the build tree** (offscreen, PID-recorded, killed by PID): `QT_QPA_PLATFORM=offscreen` contour with the bundled status-line extension renders. `pgrep -P <contour pid>` shows `contour-extension-host`, whose `/proc/<pid>/status` says `Seccomp: 2`, `NoNewPrivs: 1`.
- [ ] **Step 8: Commit** (two):
  - `feat(vtextension): run extensions in a sandboxed extension host process by default on Linux`
  - `build: select where extensions run and which engine the extension host uses`

### Task 5.5: FreeBSD sandbox
**Files:**
- Create: `host/Sandbox_freebsd.cpp`.
- Modify: `host/HostBootstrap_posix.cpp` (FreeBSD caps gain `RLIMIT_AS`; D104), `process/SandboxProbe_test.cpp` (a FreeBSD expectation column), `CMakeLists.txt` (`FreeBSD` appended to `_CONTOUR_SANDBOXED_PLATFORMS`; D110).

**Produces:**
- `DescriptorRole { Control, Lane, Page, Wake, StandardError, EngineDevice }` and its rights table (`cap_rights_t` sets per role, ioctls none, fcntls `F_GETFL`).
- `lockDown()` limits every descriptor in `SandboxDescriptors` (which gains roles) and then calls `cap_enter()`.
- `restrictBeforeThreads()`: `procctl(PROC_NO_NEW_PRIVS_CTL)` where defined, otherwise nothing.

- [ ] **Step 1: Write the tests** (they run on FreeBSD CI only). The probe ops table gains FreeBSD expectations:
  - open → `ECAPMODE`;
  - socket → `ECAPMODE`;
  - `pdfork` → `EAGAIN` (`RLIMIT_NPROC`);
  - `send-descriptor` → `ENOTCAPABLE` or success. Capsicum cannot deny `SCM_RIGHTS` on a right-holding socket; the result is recorded, and D70/R38 cover the parent;
  - an `ioctl` on control → `ENOTCAPABLE`.
  - `"Sandbox.rightsTableCoversEveryRole"` (compiles everywhere, a pure table test).
- [ ] **Step 2: Implement.** Rights per role:
  - control: `READ|WRITE|RECV|SEND|EVENT|SHUTDOWN|FSTAT`;
  - lanes: the same minus `FSTAT`;
  - page: `MMAP_R|FSTAT`;
  - wake: `READ|EVENT`;
  - stderr and the WAMR null device: `WRITE|FSTAT`.
- [ ] **Step 3: Size `RLIMIT_AS`** = `dataBytes` (WAMR Copy reserves nothing beyond it); CI's `procstat -v` measurement goes in the report.
- [ ] **Step 4: Verify by reading** against a checklist:
  - `cap_rights_init` variadic use;
  - `<sys/capsicum.h>` and `libcasper` not needed;
  - `cap_enter` after every thread;
  - WAMR's interpreter needs no new descriptor (the null device was opened before);
  - `kern.arandom` and `getrandom` work in capability mode;
  - `clock_gettime` is allowed;
  - every `#ifdef __FreeBSD__` path compiles in the mind's eye against FreeBSD 14 headers.
- [ ] **Step 5: CI** only if the owner allows a push: the FreeBSD leg runs `vtextension_process_test` and `vtextension_helper_test` (WAMR). Otherwise mark it outstanding.
- [ ] **Step 6: Commit** `feat(vtextension): the extension host enters capability mode on FreeBSD`.

### Task 5.6: macOS sandbox and the helper's entitlement
**Files:**
- Create: `host/Sandbox_macos.cpp`, `host/macos/SandboxProfile.hpp`, `support/macOS/contour-extension-host.entitlements`.
- Modify: `scripts/macos-bundle.py` (per-helper entitlements), `.github/workflows/build.yml` (the macOS ad-hoc hardened-sign step and the helper suite run), `CMakeLists.txt` (`Darwin` appended; D110).

**Produces:**
- `SandboxProfile` (a constexpr profile text) and its parameter rows `{name, value}`. `SHM_PREFIX = "/contour-eh-"` comes from `SharedObjectName.hpp`.
- Hand-declared `extern "C" int sandbox_init_with_parameters(const char*, uint64_t, const char* const[], char**)` and `void sandbox_free_error(char*)`.
- `lockDown()` applies the profile. An error → `Profile`, with `errorbuf` told.

Profile: `(version 1) (deny default)`, plus:
- `(allow ipc-posix-shm-read-data ipc-posix-shm-write-data (ipc-posix-name-prefix (param "SHM_PREFIX")))`;
- `(allow signal (target self))`;
- `(allow sysctl-read (sysctl-name …))`, the names taken from CI's first `sandbox` deny log;
- no `network*`, no `mach-lookup`, no `file-read*`, no `process-fork` or `process-exec`.

- [ ] **Step 1: Write the tests** (macOS CI only).
  - The probe ops table gains macOS expectations (open → `EPERM`, socket → `EPERM`, fork → `EPERM`, `mach-lookup` of `com.apple.system.logger` → denied).
  - `"Sandbox.profileHasNoNetworkOrMachLookup"` (compiles everywhere; a text table test on the profile string).
- [ ] **Step 2: Implement the profile and the declaration.** Wasmtime's mach ports are already off (`wasmtime_config_macos_use_mach_ports_set(false)`); the fact is restated in the file comment.
- [ ] **Step 3: Implement the entitlement.**
  - `contour-extension-host.entitlements` holds `com.apple.security.cs.allow-unsigned-executable-memory` only.
  - `macos-bundle.py` signs `Contents/Helpers/contour-extension-host` with `--options=runtime --entitlements <helper file>` before the outer bundle. The outer bundle's entitlements are unchanged.
  - A unit test in the script's own test file asserts the helper path gets the helper file and nothing else does (or an equivalent check run under `python3 -I`).
- [ ] **Step 4: Add the CI case.**
  - The macOS leg runs `codesign --force --sign - --options=runtime --entitlements support/macOS/contour-extension-host.entitlements <helper>`, then `vtextension_helper_test "~[.measure]"` with `RequireLockedDown`.
  - A second run signs without the entitlement and expects the loads to fail, which proves the entitlement is load-bearing.
- [ ] **Step 5: Verify by reading** against a checklist:
  - the declaration matches Chromium's;
  - `-Werror` sees no deprecated symbol;
  - shm grants are read/write-data only, never create (R38);
  - `MAP_SHARED|MAP_FIXED` of a received shm descriptor needs no `file-map` rule;
  - Phase 4's open macOS items: `CMSG_SPACE` as an array bound, BSD descriptor attribution.
- [ ] **Step 6: CI** only if the owner allows a push; otherwise mark it outstanding.
- [ ] **Step 7: Commit** (two):
  - `feat(vtextension): the extension host runs in a deny-by-default sandbox on macOS`
  - `build: sign the macOS extension host with the one entitlement its compiler needs`

### Task 5.7: Windows sandbox — AppContainer, child-process policy, mitigations
**Files:**
- Create: `process/AppContainer_win32{.hpp,.cpp}`, `host/Sandbox_win32.cpp`, `process/AppContainer_test.cpp` (process_test).
- Modify: `process/{ProcessLauncher.hpp,Process_win32.cpp,ProcessHostConnector.cpp}`, `process/testing/HelperComposition.cpp`, `remote/testing/ProcessDefaultBackend.cpp` (wine detection, D107), `CMakeLists.txt` (`Windows` appended; D110).

**Produces:**
- `ProcessIsolation : uint8_t { None, AppContainer }` and `LaunchSpec::isolation`. Production: `AppContainer`.
- `AppContainerApi`: a table of the userenv and advapi functions, for DI.
- `ensureAppContainer(AppContainerApi const&) -> expected<OwnedSid, LaunchError>`, run once per supervisor on its thread. It uses profile `Contour.ExtensionHost` with no capabilities; `ALREADY_EXISTS` → derive.
- `ensurePackagesCanExecute(directory, AppContainerApi const&) -> expected<void, LaunchError>`: adds `ALL APPLICATION PACKAGES` (S-1-15-2-1) RX only if no such ACE exists.
- Launch attributes: `SECURITY_CAPABILITIES` and `CHILD_PROCESS_POLICY = RESTRICTED`, beside the handle and job lists.
- Helper `lockDown()`:
  - `TokenIsAppContainer` is false → `Unsandboxed`;
  - otherwise it applies `SetProcessMitigationPolicy` rows (`ExtensionPointDisable`, `ImageLoad` no-remote/no-low-label, `StrictHandleCheck`) and checks job membership (`IsProcessInJob`);
  - any failure → `Mitigations`/`Container`;
  - all pass → `LockedDown`.
  - Dynamic-code policy is deliberately absent.

- [ ] **Step 1: Write the tests.**
  - `"AppContainer.profileIsEnsuredOnce"` (fake API): created once; `ALREADY_EXISTS` → derived; other error → `LaunchError::Unsupported`.
  - `"AppContainer.packageGrantIsIdempotent"` (real ACL on a temp directory): two calls give one ACE.
  - `"AppContainer.launchCarriesTheIsolation"` (RecordingLauncher).
  - `"Sandbox.windowsHelperRunsContained"`: real Windows only. In the AppContainer the probe cannot open a user-profile file, `socket` + `connect` to 127.0.0.1 fails, and `CreateProcessW` fails. Under wine it SKIPs with the reason.
  - `"Helper.windowsHelperIsLockedDown"`: real Windows; under wine Hello is `Unsandboxed` and recorded.
- [ ] **Step 2: Cross-build** with `linux-to-windows-clangcl-debug` (Embedded debug format, extensions on, bundled and GUI off) — `vtextension_process_test extension_host_probe contour-extension-host vtextension_helper_test`; expect failure.
- [ ] **Step 3: Implement the API table and the ensure functions** (D106).
- [ ] **Step 4: Implement the launch attributes and the helper's verification** (D106).
- [ ] **Step 5: Implement wine detection** in the test compositions (D107). `wine_get_version` in ntdll → `ProcessIsolation::None` + `AcceptUnsandboxed`.
- [ ] **Step 6: Run the wine runs** (five of process_test, three of helper_test `~[.measure]`).
  - **Wine can show:** clean `-Werror` compiles; the attribute list builds (whether `CreateProcessW` accepts `SECURITY_CAPABILITIES` under wine is recorded either way); the ACL add and the idempotence check; `TokenIsAppContainer` false → `Unsandboxed` → `AcceptUnsandboxed` path end to end; the fake-API profile logic.
  - **Wine cannot prove:** AppContainer isolation of files, network or registry; mitigation policies taking effect; the child-process policy; that the ACL grant is enough for a real AppContainer to load the helper and its CRT.
- [ ] **Step 7: CI** (Windows leg) only if the owner allows a push; otherwise mark it outstanding.
- [ ] **Step 8: Commit** (two):
  - `feat(vtextension): the extension host runs in an AppContainer on Windows`
  - `feat(vtextension): grant app containers read access to the extension host once`

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
