# Phase-5 review fix wave

The base is HEAD `c130dab7c`, pushed as draft PR #2431. The full review is in the ledger (`progress.md`, the "Task 5.8 phase review" line); its text is reproduced below. Fold each fix into the commit that introduced the defect (fixup + `GIT_SEQUENCE_EDITOR=: git rebase -i --autosquash origin/master` after `git fetch`). Use a new commit only where nothing owns the fix: a pre-existing bug, a new CI gate, or new tests of a new seam. Do NOT push.

## 1. Linux aarch64: the switch, the guard and CI
- `src/vtextension/CMakeLists.txt:206-209`: key `VTEXTENSION_LINUX_SANDBOX` on Linux with a processor in {x86_64, aarch64}, independently of `_CONTOUR_SANDBOXED_PLATFORMS` and `CONTOUR_SANDBOXED_PLATFORM`. FreeBSD and macOS already work this way. Removing a row from the defaults table must never compile a sandbox out.
- Add a configure-time FATAL_ERROR when `CONTOUR_EXTENSION_HOST=out-of-process` and the selected sandbox source is `Sandbox_none.cpp` (for example Linux riscv64 or ppc64le), with a clear reason. Windows/macOS/FreeBSD each have a sandbox source, so they are unaffected.
- **CI**: the two `ubuntu-24.04-arm` legs (`build.yml` ~1994) run on master and merge_group only. Give them the same label gate as the FreeBSD job: they run on a pull request carrying the label `ci-arm`. Copy the `ci-freebsd` pattern from commit `71cbaa680`, including the comment, and stay YAML-safe: no colon in the label. This is a separate `ci:` commit. Keep aarch64 in `_CONTOUR_SANDBOXED_PLATFORMS`. The controller removes it if the arm leg fails.

## 2. (Deferred: per-commit build check, done once before merge.)

## 3. Pre-existing bug: WAMR's global native-symbol list is raced
- WAMR's `wasm_native.c` prepends (`:280-281`), unlinks (`:314`) and looks up (`:215`) with no lock.
- In our code, `HostRegistrations` (`wamr/WamrBackend.cpp:164,178`) registers and unregisters without a lock, `registerWasiOverrides` (`wamr/WasiOverrides.cpp:114`) uses a private mutex, and `ExtensionRegistry.cpp:145` loads different extensions concurrently.
- Result: a use-after-free on the in-process path, which is the default on macOS, Windows and FreeBSD.
- Fix: one process-wide `std::shared_mutex`, owned in one place in `wamr/`. Take it exclusively around every register and unregister, and shared around `wasm_runtime_load`, `wasm_runtime_instantiate` and any lookup WAMR does through the native list.
- Test: a `[threading]` test that loads and unloads distinct modules with distinct registrations concurrently. It must fail under TSan without the lock; verify that in `out/clang-tsan`, and say if it doesn't.
- This goes in its own `fix(vtextension):` commit, because the bug predates this branch.

## 4. W^X on the platforms that are not flipped yet
- macOS: sign the nested helper with the `allow-unsigned-executable-memory` entitlement ONLY when its engine compiles to native code (wasmtime). An interpreting (WAMR) helper gets no entitlement.
  - Pass the engine into `macos-bundle.py`, or detect it from the build, in a data-driven way.
  - Update its self-test, the `build.yml` macOS step (the negative-entitlement case applies only to a Wasmtime helper), and `docs/internals/macos-code-signing.md:221`, which wrongly says the helper always compiles.
- FreeBSD: in `restrictBeforeThreads()`, call `procctl(P_PID, 0, PROC_WXMAP_CTL, &PROC_WX_MAPPINGS_DISALLOW_EXEC)` where `PROC_WXMAP_CTL` is defined. Add a `LockdownStep` row if that is needed to report a failure. Syntax-check it against FreeBSD 14.4 and 15.1 headers, as before.

## 5. Make the promised diagnostics observable in release builds
- The Hello's `failureCode` is never read by the parent (`HelperIncarnation.cpp:122` passes only the step). Add the code's text to the refusal line, for example "…: <step text> (<code text>)". Phrase POSIX errnos and Windows HRESULT/Win32 codes on the parent side with the existing helpers. Test it.
- A `prepareThread()` failure ends the helper before Hello. Give priming a Hello: add a `LockdownStep::Priming` row (enumerator, codec, table), and make the server send a `Failed` Hello naming `Priming` and its code instead of exiting silently. That refusal is lasting (sticky): put it in the `HostRefusal`/lasting table.
- The "Landlock unavailable" notice goes to stderr, which is the null device in release builds. Carry it to the parent: a Hello field or state (for example `LockdownState::LockedDownPartially`, or a notice bit) that the parent logs once per process. Pick the cleanest representation that keeps `ProtocolVersion` consistent. Bump it to 5 if the wire changes, and say so.

## 6. Bring the Windows sandbox onto the shared seam (`Sandbox_win32.cpp:119-130`)
- Inject the token, mitigation and job queries through an API table, as `AppContainerApi` does.
- Run every step, and state the first failure (D86).
- Report "not in a job" as its own `LockdownStep::Job` row, not `Container`.
- Add fake-API tests for the three-way decision (Unsandboxed / Failed(step) / LockedDown). They run under wine and Linux too, wherever the code compiles.

## 7. Log `HelperNotGranted` once (`AppContainer_win32.cpp:219-229`)
Remember the failed directory and its error inside `AppContainerProvision`, and log once. A test must show that a second connect writes no second line.

## 8. The probe re-implements the host's order (`SandboxProbe.cpp:768-843`)
- Build the probe's held list with `sandboxDescriptorsOf()`'s logic, or share a function.
- Fold the hardening result into the reported lockdown, as the host does.
- Keep the deliberate early thread for the TSYNC test.

## 9. Comments that contradict the code
- `SanitizerOptions.cpp:3` says "whatever environment", but lines 6-7 say the environment overrides.
- `LaunchedHelper.cpp:42` says "whatever they say".
- `host/CMakeLists.txt:19-39` hard-codes the `PlatformSandbox` order 0..4. Derive it from a list, or state why not.

## 10. Wasmtime helper idle memory (17.2 MiB, was 10.6)
- Release the engine's priming engine (`_priming`, `WasmtimeEngine.cpp:509-515`) once every lane is prepared. Add an `IHostedEngine` hook, for example `primingDone()` (WAMR's is empty), called after `lanesPrepared.wait()` (`HostServer.cpp:380`) and before confinement. Handlers and sigaltstacks don't depend on that engine.
- Measure idle VmRSS before and after in the release tree, the same way the gate did, and report both.
- Make sure no `munmap` or `madvise` the release needs happens after lockdown. It runs before confinement, so it should be fine; check.

## Rules and checks
- `run-rules.md` applies: gentle limits, one tree at a time, `unset $(env | grep -o '^CONTOUR_[A-Z_]*')`, `git commit -s`, no trailers, no plan/task/phase wording, no wasm/WAMR/Wasmtime in user-facing text.
- Before reporting, run:
  - ASan ctest `-R 'vtextension|wasm_runtime_containment'`;
  - `-L lint`;
  - tidy-diff;
  - clang-format, typos and editorconfig;
  - the TSan run of item 3's test and the remote suites;
  - a release build for item 10;
  - wine runs for items 6 and 7 (process_test and remote_wamr_test).
- Write `task-5.8-fixwave-report.md` with, for each item: the fix, where it was folded, the test, and the evidence. Include the old head → new head.
