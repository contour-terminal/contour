# Task 3.2 report: the `contour-extension-host` executable

Status: DONE_WITH_CONCERNS. The concerns are about what only CI can verify (macOS and FreeBSD) and about the in-process HostProcess tests on Windows.

## Commits (on 1551a7793)
- abe316439 `fix(vtextension): state what a helper can do with its page, and how a wait fails` (item 0)
  - a. Softened the Windows control-page doc in `ControlPage_win32.cpp` (file head) and in `ControlPage.hpp` (`nativeHandle()`). It now says:
    - A well-behaved helper cannot map the page writable.
    - A hostile helper can widen its own handle with `DuplicateHandle`, because an unnamed section's access is not checked.
    - That grants nothing, because Contour never reads the page back.
    - An AppContainer around the helper narrows this further. The source does not name the phase.
  - b. The `IExitWaitSet::wait()` doc now says "An error (any LaunchError)", with the closed handle given as one instance.
  - c. New test `ExitWaitSet.closedWatchedHandleIsAnError` (Linux/FreeBSD):
    - It saves a copy of the pidfd, closes the original under the set, and expects `wait()` to return an error.
    - It then restores the original number with `dup3`.
    - Mutation check: with the POLLNVAL return removed, the test fails.
- 952e7ba41 `feat(vtextension): the contour-extension-host executable`
- 588d234cc `build: install contour-extension-host beside contour`

## Files
- New:
  - `src/contour-extension-host/{CMakeLists.txt,main.cpp}`
  - `wasm/host/{HelperEngine.hpp,HostBootstrap.hpp,HostBootstrap_posix.cpp,HostBootstrap_win32.cpp,HostProcess.hpp,HostProcess.cpp,HostProcess_test.cpp}`
  - `wasm/wasmtime/HelperEngine.cpp`
  - `wasm/remote/process/{HelperLocation.hpp,HelperLocation.cpp,HelperLocation_test.cpp}`
  - `wasm/remote/Helper_test.cpp`
  - `wasm/remote/process/testing/LaunchedHelper.{hpp,cpp}` and `wasm/remote/transport/testing/HandleCopies.hpp` (dedup, see deviations)
- Modified:
  - `HostServer.{hpp,cpp}`, `HostServer_test.cpp`
  - `IHostedEngine.hpp` (`EngineFactory` moved here) and `InProcessHost.hpp`
  - `host/CMakeLists.txt`, `remote/CMakeLists.txt`, `wasmtime/CMakeLists.txt`, `src/CMakeLists.txt`, `src/contour/CMakeLists.txt`
  - `LaunchProbe.cpp`, `ProcessLauncher_test.cpp`, `Channel_test.cpp`, `ControlPage_test.cpp`

## What was built
- **`HostServer::run()` order**: start the executors, then `confine()` if one is set, then send Hello with the result, then serve.
  - If an executor fails, the server stops with `ExecutorUnavailable`: nothing is confined and nothing is sent.
  - The existing test `executorThatCannotStartEndsTheServer` no longer expects a Hello first.
- **`HostProcess`**:
  - The exit table: Served 0, then 70–75, each row with user-readable text that names no engine. A `static_assert` checks that every enumerator sits at its own index.
  - `exitCodeOf` and `hostProcessExitOf`.
  - `adoptEndpoints`: any adopt failure gives `HandlesUnusable`.
  - The `HostProcess` class: it owns the page and the wake receiver beside the `HostServer`.
  - `runExtensionHost` runs: parse → `bindToParent` (first) → adopt → engine → confine via the config hook → run.
    - Every non-`Served` exit writes one `contour-extension-host: <text>` line to stderr.
    - The engine is declared before the host, so engine objects are released before the engine.
- **`HostBootstrap`**:
  - POSIX `bindToParent` is `prctl(PR_SET_PDEATHSIG)` on Linux or `procctl(PROC_PDEATHSIG_CTL)` on FreeBSD, then a `getppid()` check. It is chained with `and_then`. macOS only checks.
  - `capResources` works from a per-OS table:
    - Linux: NPROC 0 and DATA. FreeBSD: NPROC 0. Others: none.
    - Soft and hard limits are both set.
    - `Skip` applies nothing.
  - On Windows both functions are no-ops.
  - `BuildCapPolicy` follows `CONTOUR_SANITIZED_BUILD`. That is a PUBLIC compile definition on `vtextension_host`, set when `CONTOUR_SANITIZE` is set, so every user of the header sees the same value.
- **`HelperEngine.cpp`**: one process-wide kind state. It uses `TrapMode::Signals`, or `Software` under `CONTOUR_SANITIZE`/`CONTOUR_COVERAGE` (D33).
- **The executable**:
  - Built `if(TARGET vtextension_wasmtime)`, otherwise the prescribed status line.
  - `WIN32_EXECUTABLE OFF`.
  - `main` is exactly `runExtensionHost(args, &makeHelperEngine)`.
- **`locateExtensionHost`** tries candidates per platform, in order:
  - Unix: `<rel>`, then `.`.
  - macOS: `../Helpers`, then `<rel>`, then `.`.
  - Windows: `.`.
  - Each result is `lexically_normal`.
- **Install**: Unix to `${CMAKE_INSTALL_LIBEXECDIR}/contour`, Windows to `bin`, macOS bundle by a POST_BUILD copy into `Contents/Helpers/`.
  - Verified: `DESTDIR=<scratchpad>/install cmake --install out/clang-asan` installs `<prefix>/libexec/contour/contour-extension-host`.
  - The clang-cl tree's `cmake_install.cmake` installs `bin/contour-extension-host.exe`.

## TDD evidence
- **RED**: tests, headers and CMake were written first, with empty `.cpp` stubs. Linking failed with undefined `bindToParent`, `capResources`, `exitCodeOf`, and so on.
- **GREEN**: everything passed after the implementation.
- **Mutations**:
  - The helper leaking `new int[64]` makes `Helper.saysHelloOverInheritedChannels` fail: LSan exits 23, not 0. This is how D57's helper-side sanitizer reports fail a test for now.
  - The item-0 wait test bites, as noted above.

## Test results (clang-asan, real counts)
- `vtextension_remote_test "HostServer.*,HostProcess.*,HostBootstrap.*"`: 35 cases / 1125 assertions. Full binary: 197 / 4343.
- `vtextension_process_test`: 31 cases / 248 assertions, 1 skipped. The skip is `HostBootstrap.capsHold`: the probe reports `caps=skip` in the ASan build, as the brief prescribes.
- `vtextension_helper_test`: 3 cases / 29 assertions, run against the real executable.
- `vtextension_test "Channel.*"`: 8 / 36, after the shared `HandleCopies` change.
- `ctest --preset=clang-asan -R "vtextension|check_wasm_runtime_containment"`: 7/7 pass.
- No leftover `contour-extension-host` or `extension_host_probe` processes afterwards (`ps` count 0). Every real-process case reaps through `awaitReap`, and `LaunchedHelper`'s destructor kills and reaps on a failure path.
- **Windows** (clang-cl cross, `out/clangcl-debug`, `-Werror`):
  - `contour-extension-host.exe`, `vtextension_helper_test.exe`, `vtextension_process_test.exe`, `extension_host_probe.exe` and `vtextension_remote_test.exe` all build clean.
  - Under wine with my own prefix (in the scratchpad), `vtextension_process_test` passed 24 cases / 203 assertions.
  - `vtextension_helper_test` passed 2 / 20: the real Wasmtime helper says Hello over inherited handles and exits 0, and exits 70 on bad arguments.
  - `vtextension_remote_test.exe` does not start under wine: `zlib1.dll` is missing, because libpng is linked dynamically and `out/windows-cross-deps` has no DLLs. So the in-process HostProcess cases were compiled for Windows but not run there.
  - `wineserver -k` was run on my own prefix only.
- **clang-tidy**: `clang-tidy -p out/clang-asan --quiet <the 15 changed portable/POSIX .cpp files>` (HostProcess, HostBootstrap_posix, HostServer, both host tests, Helper_test, HelperLocation(+test), LaunchedHelper, LaunchProbe, ProcessLauncher_test, Channel_test, ControlPage_test, HelperEngine, main). 0 findings after the fixes: concise `#ifdef`/`#elifdef`, unused usings, by-value params. `HostBootstrap_win32.cpp` is covered only by the clang-cl `-Werror` build.
- `/usr/bin/clang-format` and `editorconfig-checker` are clean on every changed file. The containment lint passes.

## Deviations, and why
1. **`runExtensionHost` has a second overload that takes a `HostConfinement`** (`bindToParent` and `confine` functions). `systemConfinement()` supplies the real ones, and the brief's 2-argument form forwards to it, so `main` is unchanged.
   - Why: in-process, the real `confine` sets `RLIMIT_NPROC 0` on the test process in any non-sanitizer build, after which no further thread can start. The real `bindToParent` sets PDEATHSIG on the test runner and fails `getppid()` for "own pid".
   - So the in-process `HostProcess.*` tests use a double: it binds only to a fixed fake pid, and caps through `capResources(…, Skip)` while recording the data bytes.
   - The real binding and caps are tested out of process instead: the probe and the real helper exit 72 with `--parent=1`; the probe reports its rlimits; and the new `HostBootstrap.capResourcesApplies` runs `Apply` in a forked child (Linux/FreeBSD), so the Apply path is tested in the ASan build too.
2. **`HostProcess.wrongParentExits` uses the own pid**, which is never the test's parent, instead of own pid + 1. With the double it checks the ordering: 72 before any engine is made and before any confine.
3. **`vtextension_helper_test` is gated on `if(TARGET vtextension_wasmtime AND TARGET vtextension_remote_process)`**, not `TARGET contour-extension-host`. `src/contour-extension-host` is added after `vtextension`, so at that point `if(TARGET contour-extension-host)` is always false. The condition used is the one the executable is built under.
4. **`CONTOUR_EXTENSION_HOST_RELATIVE_DIR` and `CONTOUR_EXTENSION_HOST_INSTALL_DIR` are computed once in `src/CMakeLists.txt`** (GNUInstallDirs + `file(RELATIVE_PATH)`), in the first commit. They are not in `src/contour-extension-host/CMakeLists.txt`.
   - `HelperLocation.cpp`, which is always built, and its test need the define even where the executable is not built.
   - The install rule (second commit) uses the same variable, so the two cannot disagree.
   - The define is PUBLIC on `vtextension_remote_process`.
5. **`CONTOUR_MACOS_APP_BUNDLE` was hoisted from `src/contour/CMakeLists.txt` to `src/CMakeLists.txt`**, with an unchanged condition. `contour-extension-host` is configured before `contour` and needs the flag.
6. **macOS candidates include the Unix libexec directory** between `../Helpers` and `.`. A GUI-less macOS build installs like Unix, into libexec, which D54's two-entry macOS list would miss.
7. **Probe**:
   - It now does `bindToParent` (before adopting) and `capResources(--data-bytes, BuildCapPolicy)`. It exits `exitCodeOf(ParentGone)` (72) when the binding fails, so it links `vtextension_host`.
   - It reports `caps=apply|skip`, plus `nproc=` and `data=` on POSIX.
   - `capsHold` SKIPs on `caps=skip`, as reported by the probe. This way `vtextension_process_test` needs no header or link edge to `vtextension_host` (D55).
   - `HostBootstrap.wrongParentExits` asserts the literal 72, which is the contract.
8. **Dedup**:
   - The launcher tests' child handling (`Probe`, `launchProbe`'s channel/page/wake setup, `awaitReap`, `endAndReap`, `ownProcessId`, `ChildPatience`) moved to `process/testing/LaunchedHelper.{hpp,cpp}`. The launcher tests and the helper tests share it, and `Probe` is now an alias of it.
   - `duplicate`/`closeHandle`, which were copied in `Channel_test` and `ControlPage_test` and are needed by `HostProcess_test`, moved to `transport/testing/HandleCopies.hpp`.
9. **Extra tests beyond the named list**:
   - `HostServer.executorFailureSkipsConfine`
   - `HostProcess.noEngineExits` (73)
   - `HostProcess.serverExitsMapToProcessExits`
   - `HostBootstrap.capResourcesApplies` and `HostBootstrap.skipCapsNothing`
   - `Helper.exitsWhenItsParentIsAnother` (72, POSIX)
   - `HelperLocation.libexecIsNotBesideTheBinaries`
10. **`Helper_test` passes the test process's sanitizer variables to the helper.** These are `ASAN/LSAN/UBSAN/TSAN_OPTIONS`, from a 4-row table, read through `core::LiveEnvironment`. Task 3.3's composition should own this table (D39).
11. **`InProcessHost` is not rebuilt on `HostProcess`** (D34). That needs a page and a wake pair per in-process host and only pays off with the interrupt listener. It is left for the task that adds the listener. `InProcessHost.hpp` only lost `EngineFactory`.

## Not verified here
- macOS and FreeBSD were not compiled:
  - `procctl(PROC_PDEATHSIG_CTL)`
  - `#elifdef __FreeBSD__` in `HostBootstrap_posix.cpp`
  - the macOS candidate table
  - the bundle POST_BUILD copy (`$<TARGET_BUNDLE_CONTENT_DIR:contour>` refers to a target defined later, which is legal for generator expressions)
- The macOS bundle:
  - The `contour` target does not depend on `contour-extension-host`. Building only `contour` leaves `Helpers/` empty or stale.
  - macdeployqt/signing of the helper inside `Helpers/` is not handled. Both belong to packaging (6.3).
- Caps under `Apply` in a real plain build were not run end to end (the probe's `capsHold` skips under ASan). The forked-child test covers `capResources(Apply)` itself.
- Wasmtime under `RLIMIT_NPROC 0` after Hello: parallel compilation is off (`WasmtimeEngine.cpp:168`), so no thread should be spawned. This was not measured in a plain build. Task 3.7's `VmData` measurement is the place.

## Self-review
- **Risk**:
  - The `HostServer` reorder changes when Hello is sent: only after all executors started. `InProcessHost` and `RemoteBackend` are unaffected; the full `vtextension_remote_test` passes.
  - The helper's stderr is inherited, so its exit lines reach Contour's stderr. That is intended until Task 5.2.
- **Performance**: none on any hot path.
- **Coverage**: not measured. Uncovered: `BootstrapError::Refused` (prctl failing), the `Failed` lockdown path, and the macOS/FreeBSD branches.

## Concerns
1. macOS/FreeBSD sources and the bundle copy are compiled only by CI.
2. The in-process `HostProcess.*` cases did not run under wine (`zlib1.dll` missing for `vtextension_remote_test.exe`). The real helper did run under wine.
3. The `HostConfinement` overload is an addition to the brief's interface. The controller should confirm.

## Fix round 1

1. **The real helper's caps are asserted.**
   - New test `Helper.capsItselfBeforeHello` (Linux) in `vtextension_helper_test`. It launches the helper with `--data-bytes` 768 MiB, waits for Hello, then reads `/proc/<pid>/limits`.
   - It expects `Max processes` soft/hard `0`/`0` and `Max data size` soft/hard 805306368/805306368.
   - It SKIPs under `BuildCapPolicy == Skip`, as `capsHold` does. To read that policy, `vtextension_helper_test` now links `vtextension_host`, which carries the PUBLIC `CONTOUR_SANITIZED_BUILD` define.
   - Hello is now read by a shared `receiveHello()`.
   - **Mutation evidence.** The ASan tree skips this test, so it was shown in `out/clang-release`, an Apply build with Signals traps. Only `contour-extension-host` and `vtextension_helper_test` were built there. This departs from the run rules' one-tree rule, because the mutations cannot be shown in a sanitizer build.
     - Unmutated: 4 cases / 42 assertions pass.
     - Each of the three mutations fails `Helper.capsItselfBeforeHello` (1 of 1 failed):
       - `systemConfinement().confine` calling `capResources(d, CapPolicy::Skip)`
       - `confine(0)` instead of `confine(dataBytes)` in `runExtensionHost`
       - the `config.confine = …` assignment removed
     - After restoring the file (checked with `cmp`), 4 / 42 pass again.
2. **`endWith` and the Skip notice can no longer throw.**
   - They go through a `noexcept` `tell()` that writes with `std::fwrite`/`std::fputc`. Those cannot throw, and a failed write is ignored.
   - `endWith` is `noexcept`, and `<print>` is gone.
   - I chose this over try/catch(...): an empty catch would be a clang-tidy `bugprone-empty-catch` finding.
3. **The bundle copy is on contour's side.**
   - `src/contour/CMakeLists.txt`, bundle branch: `if(TARGET contour-extension-host)` → `add_dependencies(contour contour-extension-host)` plus a POST_BUILD on `contour` that copies `$<TARGET_FILE:contour-extension-host>` into `$<TARGET_BUNDLE_CONTENT_DIR:contour>/Helpers/`.
   - `contour-extension-host/CMakeLists.txt` no longer refers to `contour`. It installs to `bin` on Windows, and to `CONTOUR_EXTENSION_HOST_INSTALL_DIR` unless it is a macOS bundle build.
   - The target check works because the helper's directory is added before `contour/`.
   - Not compiled on macOS here.
4. **One source for the binaries directory.** contour's Unix install is now `DESTINATION "${CMAKE_INSTALL_BINDIR}"`, matching the `CMAKE_INSTALL_FULL_BINDIR` that `CONTOUR_EXTENSION_HOST_RELATIVE_DIR` is computed from. Comments on both sides point at each other.
   - The Windows `install(TARGETS contour DESTINATION bin)` stays literal, because the helper's Windows install is the same literal `bin`.
   - Verified again: `DESTDIR` install gives `<prefix>/bin/contour` and `<prefix>/libexec/contour/contour-extension-host`.
5. **`ExitWaitSet.closedWatchedHandleIsAnError` is race-free.** The set now watches a test-local `CopiedWaitable`, an `IProcessHandle` that forwards to the probe's handle but whose `waitable()` is the test's own `F_DUPFD_CLOEXEC` copy.
   - The test closes only that copy, which nothing ever closes again: no `dup3` and no restoring by number. The probe's own pidfd stays whole.
   - The wait set records the descriptor at `watch()` (`PosixLaunch.cpp:149`), so this works.
   - Mutation check: with the POLLNVAL return removed, the test fails. Restored afterwards.
6. **The `HandleCopies.hpp` doc comment** is rejoined.

### Results
- ASan:
  - `vtextension_helper_test`: 4 cases, 3 passed, 1 skipped (caps, under Skip).
  - `vtextension_process_test`: 31 cases, 30 passed, 1 skipped.
  - `vtextension_remote_test "HostServer.*,HostProcess.*,HostBootstrap.*"`: 35 / 1125.
  - `ctest --preset=clang-asan -R "vtextension|check_wasm_runtime_containment"`: 7/7.
  - 0 leftover helper or probe processes.
- Release (`out/clang-release`): `vtextension_helper_test` 4 / 42, the caps case included.
- Windows: the clang-cl cross-build is clean, after guarding the Linux-only `limitOf` against `-Wunused-function`. Under wine, `vtextension_helper_test` passes 2 / 20 and `vtextension_process_test` passes 24 / 203. `wineserver -k` was run on my own prefix only.
- `clang-tidy -p out/clang-asan --quiet` on `HostProcess.cpp`, `Helper_test.cpp` and `ProcessLauncher_test.cpp`: 0 findings, after moving `CopiedWaitable` into an anonymous namespace.
- clang-format and editorconfig-checker: clean.
