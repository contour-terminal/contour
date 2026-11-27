# Task 4.1 report: carried minors

Status: DONE_WITH_CONCERNS (minor ones; see Concerns)

BASE `4ec0d13f6`, branch `feature/out-of-process-extension-host`, nothing pushed.

## Commits

| Hash | Subject |
|---|---|
| `2a08435b7` | refactor(vtextension): inject whether the kernel waits on pidfds into the launcher |
| `9fe041e41` | refactor(vtextension): phrase each launch failure once |
| `3193a601d` | feat(vtextension): say which hardening step of the extension host failed |
| `0e71d554d` | refactor(vtextension): one switch says how a build is instrumented |

All are signed off, with no AI trailers. None of them mentions the plan, a task or a phase.

## What changed

- **M2 / D64**: new `src/vtextension/wasm/remote/process/Process_linux.hpp` declares `PidfdWaits`, `probePidfdWaits()` and `makeLinuxProcessLauncher(PidfdWaits)`. `LinuxProcessLauncher` takes the value in its constructor. `makeSystemProcessLauncher()` probes once and injects the result. The header is listed with the Linux sources in `remote/CMakeLists.txt`.
- **M3 / D65**: `ProcessLauncher.hpp` now holds `LaunchErrorRow{error, text, optional<HostRefusal> refusal}` and `launchErrors()`, and `launchErrorText()` reads that table.
  - `HostRefusals.{hpp,cpp}`: `HostRefusalRow` and `hostRefusals()` are gone, replaced by `refusalLine(HostRefusal) -> std::string`.
    - A launch refusal is formatted as `"The extension host cannot be started: {text}. Extensions do not run."` from the launch table.
    - The four handshake refusals keep their own internal table.
  - `ProcessHostConnector.cpp`: the `refusalOf` switch is gone; the connector reads `launchErrors()[e].refusal`.
  - The tests that used `hostRefusals()` (`ProcessHostConnector_test`, `RemoteBackend_test`) now use `refusalLine()`.
- **D66**: `HostBootstrap.hpp` declares the hardening API. The signatures:
  - `HardeningStep{CoreDumps, Dumpable, Tracing, FaultReports}`.
  - `HardeningRow{step, text, std::expected<void,int>(*apply)() noexcept}`.
  - `hardeningSteps()`.
  - `hardenProcess(span) -> expected<void, HardeningStep>` and `hardenProcess()`.
  - `hardeningStepText()`.

  Where the code lives:
  - The per-OS rows are in `HostBootstrap_posix.cpp` and `HostBootstrap_win32.cpp`.
  - The code that runs every row and keeps the first failure is in a new `HostBootstrap.cpp`.
  - `HostConfinement::beforeThreads` now returns `expected<void, HardeningStep>`.
  - On a failed hardening, `runExtensionHost` writes `contour-extension-host: hardening failed: <text>` to stderr once, and Hello still says `Failed`.
- **D63**: new `src/vtextension/wasm/BuildInstrumentation.hpp` declares `BuildInstrumentation{None, Coverage, Sanitizers}` and `ThisBuildInstrumentation`. It has an `#error` if the definition is missing.
  - `CONTOUR_BUILD_INSTRUMENTATION=$<IF:$<BOOL:${CONTOUR_SANITIZE}>,2,$<IF:$<BOOL:${CONTOUR_COVERAGE}>,1,0>>` is a PUBLIC definition on `vtextension_remote_wire`.
  - These now read the value as `constexpr`: `BuildCapPolicy`, `defaultHelperStandardError()` (POSIX and Win32) and `HelperTrapMode`.
  - The three old definitions were deleted from the CMake files for `host/`, `remote/` and `wasmtime/`. A grep over `src`, `cmake`, `CMakeLists.txt` and `docs` (excluding `docs/superpowers`) finds none of them.

## Tests added, and how each was seen to fail first

Each new API was first a compile failure. Each test was also seen to fail against a deliberate mutation of the implementation, which was reverted afterwards (the diffs are clean).

| Test | Suite | How it was seen to fail |
|---|---|---|
| `ProcessLauncher.unsupportedPidfdWaitsRefuseEveryLaunch` (Linux) | process | The constructor ignored `Unsupported`, so the launch went ahead. `CHECK(launch == unexpected(Unsupported))` failed. |
| `ProcessLauncher.thisKernelWaitsOnPidfds` (Linux) | process | Probe mutated (`EINVAL`→`ECHILD`) so that it says `Unsupported`; the test failed. |
| `LaunchErrors.oneRowPerError` | process | Two rows were given the same `error`; the index check failed. |
| `HostRefusals.launchLinesSayWhatTheLauncherSays` | process | The prefix of `LaunchLine` was changed; it failed for 4 rows. With the duplicated row above, the `Stopped`-has-no-refusal check also failed. |
| `HostRefusals.everyRefusalHasALine` (renamed from `everyReasonHasItsLine`) | process | Compile failure only: `refusalLine` did not exist. No behavioural mutation was run. |
| `HostBootstrap.hardeningNamesTheFirstFailedStep` | remote | `hardenProcess(span)` was made to return at the first failure; "all three ran" failed. |
| `HostProcess.failedHardeningSaysWhichStep` | remote | `confine` was made to ignore `hardened`; Hello said `Unsandboxed`, not `Failed`. The stderr line was also checked by hand. It reads `contour-extension-host: hardening failed: core dumps cannot be turned off` on Linux and `... fault reports cannot be turned off` under wine. |
| `HostBootstrap.everyStepSaysWhatFailed` (extra) | remote | Not mutated; it guards each row's text and `hardeningStepText` against each other. |
| `BuildInstrumentation.drivesCapsAndStderr` | process | `BuildCapPolicy` was mutated to key on `Coverage`; the test failed in the ASan tree. |

`HostProcess.hardensBeforeAnyThreadStarts` now covers only the success case. Its body moved into a `checkHardening()` helper, which the new failure test shares.

## Results

- ASan (`out/clang-asan`), focused runs:
  - `vtextension_process_test "ProcessLauncher.*,LaunchErrors.*,HostRefusals.*,BuildInstrumentation.*"`: 18 cases / 182 assertions, all passed.
  - `vtextension_remote_test "HostBootstrap.*,HostProcess.*"`: 16 cases / 248 assertions, all passed.
- ASan, full suites at HEAD:

  | Suite | Cases | Assertions |
  |---|---|---|
  | `vtextension_process_test` | 90: 89 passed, 1 skipped | 632 passed |
  | `vtextension_remote_test` | 221, all passed | 10836 |
  | `vtextension_helper_test` | 153: 152 passed, 1 skipped | 3697 passed |

- `ctest --preset=clang-asan -R vtextension`: 5/5 passed (40 s).
- clang-tidy, by hand on every changed `.cpp` compiled on Linux: clean, after one fix (`#elifdef` in the test). `clang-format --dry-run --Werror` and `editorconfig-checker` on every changed file: clean.
- Windows cross-build (`out/clangcl-debug`, clang-cl, `-Werror`): clean. Targets built: `vtextension_process_test`, `extension_host_probe`, `contour-extension-host`, `contour-extension-host-hostile`, `vtextension_helper_test`, `vtextension_remote_test`. The compile commands carry `CONTOUR_BUILD_INSTRUMENTATION=0`.
- wine 11.20, own prefix `scratchpad/wine41`, mingw `zlib1.dll` on `WINEPATH`:

  | Run | Result |
  |---|---|
  | `vtextension_process_test.exe` | 80 cases / 566 assertions, all passed |
  | `... "LaunchErrors.*,HostRefusals.*,BuildInstrumentation.*"` | 6 cases / 66 assertions, all passed |
  | `vtextension_remote_test.exe "HostBootstrap.*,HostProcess.*"` | 14 cases / 236 assertions, all passed |
  | `vtextension_helper_test.exe "~[.measure]"` | 124 cases: 122 passed, 2 skipped; 2882 assertions |

  `wineserver -k` was run on the own prefix only.
- Not run: TSan, release and coverage trees (outside the brief).
- macOS and FreeBSD: not compiled here; those edits were checked by reading only.
- Performance: no hot path touched. The pidfd probe still runs once per system launcher. The launch table is a function-local static. Hardening runs once per host start.

## Deviations, each with its reason

1. **`apply` returns `std::expected<void, int>`**, not `bool` (the controller's call, left to me). The value is the system's error code: errno on POSIX, the HRESULT on Windows. I chose this shape because a success/failure `bool` is the case `AGENT.md` names as `expected<void, E>`. Nothing reads the code yet.
2. **New `src/vtextension/wasm/host/HostBootstrap.cpp`**, a file not on the brief's list. `hardenProcess(span)`, `hardenProcess()` and `hardeningStepText()` are the same on every OS. Keeping them here avoids a copy in both `_posix` and `_win32`.
3. **`hardeningStepText(step)` looks the step up in this system's rows.** It returns an empty view for a step this system does not take, so each text lives once, in its OS row.
4. **`HostRefusalRow` and `hostRefusals()` are removed** in favour of `refusalLine()`. After D65 they could only have listed the handshake half, which would have been misleading.
5. **`launchErrors()` and `launchErrorText()` are `inline`, not `constexpr`, and the table is a function-local `static const`.** On CI's libstdc++ 14, a `constexpr` table that holds a disengaged `std::optional` does not compile; this is a known trap. So `oneRowPerError` is a runtime test, not a `static_assert`.
6. **The `NotFound` text is unified to the launcher's wording**: "the program is missing, or cannot be executed". The old refusal line said "missing from Contour's installation"; that detail is gone from the user's log line.
7. **One extra test**: `HostBootstrap.everyStepSaysWhatFailed`.
8. **Files on the brief's list that needed no change**: `ExtensionHostSupervisor.cpp` and `HostBootstrap_win32.cpp`'s caps.
   - `ExtensionHostSupervisor.cpp` still calls `launchErrorText()`; only the function's body changed.
   - In `HostBootstrap_win32.cpp` only the hardening changed; the caps were not touched.

## Concerns

- The Unsupported test checks two things. Its second check, that the parent's control end reads `Closed`, did not catch the mutation: a probe started anyway exits on its own soon after, as its arguments are incomplete. The `LaunchError::Unsupported` check is what catches a launcher that starts the child anyway.
- The macOS and FreeBSD hardening rows (FreeBSD `PROC_TRACE_CTL` through `errnoOf`) are not compiled locally. CI will be the first compile of them.
- `HostRefusals.cpp` (in `remote/`) now includes `remote/process/ProcessLauncher.hpp`, as D65 requires. It is header-only use, and the header is std-only plus core, so it builds on systems without a launcher too. But it is an edge from `remote/` into `process/`.
