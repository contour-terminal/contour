# Task 5.8 fix-wave report

Branch `feature/out-of-process-extension-host`. **Old head `c130dab7c` → new head `75eb4d4a7`** (local, not
pushed; base `origin/master` = `8c3afaa0c`, unchanged). 163 → 168 commits: 4 new commits, plus fixups folded
into their owners with `GIT_SEQUENCE_EDITOR=: git rebase -i --autosquash origin/master` (every autosquash ran
clean, no conflict). `git diff c130dab7c HEAD`: 58 files, +1248/−281. A local tag `fw-pre` marks the old head
(for `tidy-diff --base fw-pre`); delete it at will.

New commits (end of branch):

| Commit | Item |
|---|---|
| `adff97693` ci: run the arm legs on pull requests labelled for them | 1 (CI) |
| `77b44bea8` fix(vtextension): lock WAMR's process-wide state around every call that changes it | 3 |
| `78eb966ba` feat(vtextension): Contour says what an extension host's confinement failed with, and went without | 5 (+ part of 8) |
| `a85ae3224` feat(vtextension): the extension host on FreeBSD maps no memory both writable and executable | 4 (FreeBSD) |
| `75eb4d4a7` perf(vtextension): the extension host drops what priming its threads took before it locks down | 10 |

## Per item

### 1. Linux aarch64: switch, guard, CI — done
- `src/vtextension/CMakeLists.txt`: `VTEXTENSION_LINUX_SANDBOX` keyed on `CMAKE_SYSTEM_NAME STREQUAL Linux` and
  `CMAKE_SYSTEM_PROCESSOR MATCHES ^(x86_64|AMD64|aarch64|arm64)$`, no longer on `CONTOUR_SANDBOXED_PLATFORM`.
  Folded into "run extensions in a sandboxed extension host process by default on Linux".
- `src/vtextension/wasm/host/CMakeLists.txt`: `FATAL_ERROR` when `CONTOUR_EXTENSION_HOST=out-of-process` and the
  sandbox is `None`. Folded into "the extension host runs in an AppContainer on Windows" (the last commit that
  shaped that block; it now uses item 9's `_platform_sandbox_name`).
  Evidence: a scratch riscv64 cross configure (toolchain file, GUI/tests off) stops with "CONTOUR_EXTENSION_HOST
  is out-of-process, but the extension host has no sandbox on Linux (riscv64)…"; `in-process` configures (exit 0).
  The ASan tree reconfigures with `CONTOUR_PLATFORM_SANDBOX=1`.
- CI (`adff97693`, its own `ci:` commit): the PR matrix becomes `labels contain 'ci-arm' ? [x86 Clang, arm GCC,
  arm Clang] : [x86 Clang]`, with the `ci-freebsd`-style comment; the concurrency comment says so too. No colon
  in the label. `check-workflows.py`/actionlint: OK. aarch64 stays in `_CONTOUR_SANDBOXED_PLATFORMS`.

### 2. Deferred (per brief).

### 3. WAMR's native-symbol list race — done (`77b44bea8`, own `fix:` commit)
- New `wamr/WamrGlobals.{hpp,cpp}`: one process-wide `std::shared_mutex`. Exclusive: `registerNativesRaw()`,
  `unregisterNatives()` (used by `HostRegistrations` and `registerWasiOverrides`), `wasm_runtime_init()`,
  `wasm_runtime_destroy()` and **`wasm_runtime_load()`**. Shared: `wasm_runtime_instantiate_ex2()`.
- **Deviation: load is exclusive, not shared.** With load shared, TSan still reported two more WAMR globals
  written unlocked: the loader writes the fast interpreter's global handle table on every load
  (`wasm_loader.c:6494`, `wasm_interp_fast.c:1592`), and every `acquireRuntime()` wrote the log level
  (`bh_log.c:17/30`). Load is therefore exclusive (loads serialize process-wide; a load is short), and the log
  level is now set once per bring-up under the ownership mutex every later holder takes.
- Test `Wamr.registrationsRaceLoads` `[threading]` (vtextension_test): 4 threads × 50 rounds, each creates a
  runtime registering its own import module (`arit<n>`), loads a module importing it, checks the import is
  `Satisfied`, instantiates, drops all.
  - TSan **without the lock** (`std::defer_lock` swapped in): data races in `register_natives`,
    `wasm_native_unregister_natives`, `wasm_native_resolve_symbol`, **heap-use-after-free in
    `wasm_native_resolve_symbol`**, and the run hangs (timeout 124: a corrupted list). Reproduced twice (first and
    final form of the test).
  - TSan with it: 8 + 5 runs, 0 reports.
- **Found on the way, not fixed (concern 1):** with a *call* in the loop, TSan (1 of ~5 ctest runs) reported a
  further WAMR race: `wasm_runtime_clear_exception`/`wasm_set_exception` walks the global thread-cluster list
  (`wasm_clusters_search_exec_env`) while `wasm_runtime_destroy_exec_env` on another thread unlinks and frees an
  exec env without the cluster lock (`thread_manager.c:427`, `wasm_exec_env.c:215`) — a possible UAF in process
  whenever one instance is destroyed while another clears or sets its exception (every call, every trap). I tried
  holding the lock shared for each outermost call and exclusive for exec-env destruction: TSan then reported
  lock-order inversions (a call's host functions take other mutexes while other threads hold them and wait for
  the exclusive lock) — a real deadlock risk, so I reverted it. The test leaves calls out and its comment says
  why. Needs its own fix (e.g. destroy exec envs only on a dedicated thread, or patch WAMR to lock
  `cluster->lock` in `wasm_exec_env_destroy`).

### 4. W^X — done
- macOS (fixup of "build: sign the macOS extension host with the one entitlement its compiler needs"):
  `NESTED_ENTITLEMENTS` maps the helper's path to `{ 'wasmtime': <entitlements file>, 'wamr': None }`;
  `sign --helper-engine {wamr,wasmtime}`; a bundle holding the helper without `--helper-engine` is refused; a
  moved helper is refused only where its engine needs an entitlement. `src/contour/CMakeLists.txt` passes
  `--helper-engine ${CONTOUR_WASM_RUNTIME}` when the helper target exists. Self-test: 8 → **12 cases OK**
  (interpreting helper gets no entitlement but keeps the hardened runtime; unnamed engine refused).
  `build.yml` macOS step: also builds `vtextension_wamr_helper_test`, signs `contour-extension-host-wamr` with
  the hardened runtime and **no** entitlement and runs that suite; the comment says the negative-entitlement case
  applies to the compiling helper only. `docs/internals/macos-code-signing.md` and the lint-gate comment in
  `CMakeLists.txt` corrected. (Not run on macOS here.)
- FreeBSD (`a85ae3224`). **Deviation:** `PROC_WX_MAPPINGS_DISALLOW_EXEC` only takes effect at the next `execve`
  (FreeBSD 14.3 `procctl(2)`: "In a new address space created by a future call to execve…"; `kern_procctl.c`
  `wxmap_ctl` only sets `P2_WXORX_ENABLE_EXEC`). Called in `restrictBeforeThreads()` it would do nothing. So the
  launcher (`Process_freebsd.cpp`, pdfork child, before `execve`) asks for it, and the host's
  `restrictBeforeThreads()` verifies `PROC_WXMAP_STATUS & PROC_WXORX_ENFORCE`, failing a new
  `LockdownStep::WriteXorExecute` (errno, or 0 if not enforced). Both steps run, first failure stated. The probe's
  FreeBSD expectations add `map-writable-executable` / `protect-writable-executable` → `EACCES` (`vm_map.c`
  returns `KERN_PROTECTION_FAILURE` → `EACCES`). Syntax check against 14.4 and 15.1 amd64 headers:
  Sandbox_freebsd, Process_freebsd, SandboxProbe(+_test), HostProcess(+_test), HostBootstrap_posix, Sandbox(+_test),
  ContainerSandbox, LockdownSteps, HostServer, HostRefusals — **0 diagnostics** on both. Never run on FreeBSD.
  A new commit rather than a fold: the step row must follow item 5's `Priming`.

### 5. Diagnostics observable in release builds — done (`78eb966ba`, new commit — deviation)
- `proto::systemErrorText()` moved from the host to `LockdownSteps.cpp` (both ends; Windows formats the HRESULT);
  `proto::LockdownFailure {step, code}`, `failureCodeText()`, `lockdownFailureText()` → "`<step> (<code text>)`",
  step alone when the code says nothing. `LockdownStepRow` gains `FailureCodeKind {System, Extension}`.
  The host's `confinementFailureText()` uses the same function. Parent: `refusalLine(refusal, LockdownFailure)`,
  `HostRefusalLog::refused(refusal, LockdownFailure)`; `HelperIncarnation` passes `failedStep` and `failureCode`.
- `LockdownStep::Priming` (enumerator, codec, table, `FailureCodeKind::Extension`, phrased by its own small table
  naming no engine: "the extension runtime is unavailable"). `HostServer` collects the first `prepareThread()`
  error (`Priming` class) and sends `Hello{Failed, Priming, <ExtensionError>}` before ending
  `ExecutorUnavailable`. It maps to `HostRefusal::LockdownFailed`, which is already `Lasting` in
  `RefusalPersistenceRows` — no new refusal needed.
- Landlock notice: `Hello.unavailableStep` (new field, `LockdownStep`), from `ISandbox::unavailableStep()`
  (default `None`; Linux returns `FileSystem` when Landlock is unavailable), through
  `HostServerConfig::confine` now returning `Confinement {state, unavailableStep}`. The parent logs
  `unavailableStepLine()` once per `HostRefusalLog` (= per backend), not via a process-global: DI over hidden
  state. **`ProtocolVersion` 4 → 5.**
- Tests: Codec (v5, new field round-trip, both step positions BadEnum past the table), HostRefusals (code text in
  the line, unavailable logged once, no engine words), RemoteBackend (unprimed Hello refused LockdownFailed and
  logged with its code; `unavailableStepIsLoggedOnceAndServed`), LockdownRefusal (line with code), HostServer
  (`unpreparedThreadEndsTheHost` now receives the Priming Hello; `helloSaysWhatTheSystemLacks`), HostProcess
  (Hello carries the fake sandbox's unavailable step), Sandbox (`failureCodeIsReadAsItsStepSays`), and the sandbox
  probe reports `unavailable=` — `Sandbox.locksDownWithoutLandlock` checks it is `FileSystem` end to end under
  the no-Landlock outer filter.
- **Why a new commit:** folding into "say which confinement step … failed" conflicted in 12 files (every later
  commit adds steps, rows and codec cases); I aborted that rebase.

### 6. Windows sandbox on the shared seam — done (fixup of "the extension host runs in an AppContainer on Windows")
- New Windows-free `host/ContainerSandbox.{hpp,cpp}`: `ContainerApi {tokenContainer, mitigations (one function
  each), jobMembership}`, `makeContainerSandbox()`. Token error → `Failed(Container)`; no container →
  `Unsandboxed`, nothing else asked; in a container every mitigation and the job run, first failure stated;
  not in a job → **`LockdownStep::Job`** (`NotInJobCode` = `HRESULT_FROM_WIN32(ERROR_PROCESS_NOT_IN_JOB)`,
  static-asserted in `Sandbox_win32.cpp`). `Sandbox_win32.cpp` only builds the system API.
- Test `Sandbox.containerDecidesUnsandboxedFailedOrLockedDown` (6 sections) in `Sandbox_test.cpp`
  (remote_wamr_test): passes on Linux ASan/TSan and under wine.

### 7. `HelperNotGranted` logged once — done (fixup of the AppContainer launch commit)
`AppContainerProvision` keeps `_ungrantableDirectories`; a directory refused `NotGranted` is refused again without
asking the system or logging. `AppContainer.packageGrantIsIdempotent` checks a second `prepare()` →
`NotGranted`, still 1 line, no further DACL write. Under wine: `AppContainer.*` 4 cases / 58 assertions passed.
(Only `NotGranted` is remembered; `Io` may be transient.)

### 8. Probe's own order — done (fixup of "the extension host enters capability mode on FreeBSD")
`sandboxDescriptorsOf(HostEndpoints const&, std::span<NativeHandle const> engineDevices)` (the engine overload
delegates); the probe adopts its endpoints with `host::adoptEndpoints()` and confines exactly that list (all
lanes, page, wake, stop pipe, streams); the hardening result is folded into `lockdown=` as the host folds it. The
early thread for the TSYNC test is unchanged. `Sandbox.heldDescriptorsAreLimitedToTheirRoles` now expects
`MaxLaneChannels + 4 + 3` descriptors.

### 9. Comments — done
SanitizerOptions.cpp ("unless the environment … says otherwise") and LaunchedHelper.cpp ("unless they turn it back
on") folded into "a sanitizer build's extension host no longer dies by its own filter as it exits". The host
CMake derives `CONTOUR_PLATFORM_SANDBOX` with `list(FIND _platform_sandboxes …)` over
`None Linux FreeBSD MacOS Windows`; `Sandbox.hpp`'s enum doc points at that list.

### 10. Wasmtime helper idle memory — done, but it gives back little (`75eb4d4a7`)
`IHostedEngine::primingDone()` (WAMR: nothing; decorators forward), called by `HostServer` after
`lanesPrepared.wait()`, before confinement. Wasmtime drops its priming engine+module; the priming is a
`shared_ptr` so a release by one host does not pull it from a thread another host on the same engine is priming
(TSan found that UAF in `remote_test` with a plain `optional`; fixed in the same commit). Tests:
`HostServer.everyThreadIsPreparedBeforeConfinement` (called once, after all prepared, before confine),
`WasmtimeEngine.prepareThreadRunsTheGuestPath` (guests run after release; a later thread recompiles).
- **Idle VmRSS, release tree, `Process.measureSpawn` "idle", 3 runs each:**
  - before (release call commented out): 17.3 / 17.3 / 17.2 MiB, RssAnon 1.3
  - after: 17.3 / 17.2 / 17.3 MiB (final build 17.4 / 17.3 / 17.2), RssAnon 1.2
  - WAMR helper unchanged: 6.8 / 6.7 / 6.8 MiB.
  The +6.6 MiB since phase 4 is **file-backed**: Cranelift's own code paged in from the executable by the first
  compile (anonymous memory is only 1.2–1.3 MiB). Releasing the engine cannot drop it; clean pages the kernel
  reclaims under pressure. The commit says so.
- munmap after lockdown: none needed — the release runs before `confine`; and the Linux filter allows `munmap`
  anyway (the gate's audit counted 2063 after lockdown).

## Gates (final head unless noted)

| Gate | Result |
|---|---|
| ASan build (all 11 vtextension targets) | exit 0, **0 warnings** |
| `ctest --preset=clang-asan -R 'vtextension\|wasm_runtime_containment'` | **9/9 passed** |
| per suite (ASan, `~[.measure]`) | vtextension_test 485/26714; wasmtime 65 (62 + 3 skip)/1196; process 139 (138 + 1 skip)/5576; remote 301/12372; remote_wamr 55 (54 + 1 skip)/655; helper 161 (160 + 1 skip)/3916; wamr_helper 62/1237 (counts from the head before the last item-3/10 fixups; ctest re-run green after) |
| `ctest -L lint` | **34/34** (spelling/typos, workflows, containment, macos-bundle self-test) |
| `scripts/clang-tidy-diff.sh --base fw-pre` | **66 TUs clean** (2 findings fixed on the way) |
| clang-format --dry-run -Werror on changed C++ / editorconfig-checker | clean / clean |
| TSan build | exit 0 |
| TSan `ctest -R vtextension` | vtextension_test, wasmtime, process, remote: **passed, 0 reports**; remote_wamr, helper, wamr_helper run directly: **all passed, 0 reports** (remote_wamr: see concern 2) |
| TSan item-3 test | with lock 13/13 clean; without: races + heap-use-after-free + hang |
| Release build + helper suite | 161/3929 passed; RSS numbers above |
| Windows cross build (clang-cl, process_test, wamr_helper_test, remote_wamr_test, probe, helper) | exit 0, no warnings from Contour's sources |
| wine | process_test 114 (112 + 2 skip)/991; remote_wamr_test 53/627; wamr_helper_test 49 (48 + 1 skip)/637 |
| FreeBSD 14.4 / 15.1 syntax check | 0 diagnostics (13 files + 3 tests) |
| riscv64 configure guard | FATAL_ERROR out of process, configures in process |

Local workarounds as before (cross tree only, not committed): `/opt/msvc/bin/x64` on PATH, `LIB` → scratch
`Pathcch.lib`, own `WINEPREFIX`.

## Concerns
1. **WAMR thread-cluster race (pre-existing, unfixed)** — see item 3. In process (default on macOS, Windows,
   FreeBSD) destroying one instance while another clears/sets its exception can touch a freed exec env. The
   call-scoped lock is not the fix (deadlock risk). Suggest a ticket.
2. **Flaky pre-existing test** `ExecutorThread.startReturnsOnceTheBodyRuns` (from "locks down in a fixed order"):
   `start()` counts its latch down *before* calling the body, so `CHECK(entered == 1)` races the body's first
   statement. It failed twice under TSan ctest, passed 6/6 alone and in the direct suite run. The assertion is
   wrong, not the code; not fixed here.
3. clang-tidy over the clang-cl database reports two **pre-existing** findings in Windows-only TUs
   (`DescriptorRights.hpp:46` member init, `makeSystemSandbox`'s by-value `SandboxDescriptors` on Windows). CI
   does not lint Windows TUs.
4. Item 3 makes module loads process-wide exclusive (they also wait for running instantiations).
5. Items 4/5/10 landed as new commits instead of folds (conflict cascades); item 1's guard folded into the
   Windows commit, where the block it reads was last shaped. Per-commit builds (item 2) still to be done.
6. macOS signing and the FreeBSD W^X path are untested on their systems; the macOS CI step and FreeBSD job
   (labels `ci-freebsd`, and macOS on PRs) are what proves them.
