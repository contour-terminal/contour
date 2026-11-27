### Task 4.1: Carried minors
**Files:**
- Create: `wasm/BuildInstrumentation.hpp`, `remote/process/Process_linux.hpp`.
- Modify: `process/{Process_linux,ProcessLauncher.hpp,ProcessHostConnector,PosixLaunch,Process_win32,ExtensionHostSupervisor,ProcessLauncher_test}`, `remote/HostRefusals{.hpp,.cpp,_test}`, `host/{HostBootstrap.hpp,_posix,_win32,HostProcess.hpp,.cpp,_test}`, `wasmtime/HelperEngine.cpp`, three CMakeLists.

**Produces:**
- `BuildInstrumentation` and `ThisBuildInstrumentation`.
- `LaunchErrorRow{LaunchError error; string_view text; optional<HostRefusal> refusal}`, `launchErrors()`.
- `refusalLine(HostRefusal) -> std::string`.
- `PidfdWaits`, `probePidfdWaits()`, `makeLinuxProcessLauncher(PidfdWaits)`.
- `HardeningStep`, `HardeningRow{step, text, bool(*apply)() noexcept}`, `hardeningSteps()`.
- `hardenProcess(span<HardeningRow const>) -> expected<void, HardeningStep>`, `hardenProcess()`, `hardeningStepText()`.
- `BuildCapPolicy` derived from `ThisBuildInstrumentation`.

- [ ] **Step 1: Write the tests.**
  - `"ProcessLauncher.unsupportedPidfdWaitsRefuseEveryLaunch"` (Linux): `→ LaunchError::Unsupported`; with the helper ends dropped, the parent control end `receiveSome` → `Closed` (no child).
  - `"ProcessLauncher.thisKernelWaitsOnPidfds"`: `== Supported`.
  - `"LaunchErrors.oneRowPerError"`.
  - `"HostRefusals.launchLinesSayWhatTheLauncherSays"`: `refusalLine(r) == format(prefix, row.text)` and `launchErrorText(e) == row.text`; `Stopped` has no refusal.
  - `"HostRefusals.everyRefusalHasALine"`: none names wasm/WAMR/Wasmtime.
  - `"HostBootstrap.hardeningNamesTheFirstFailedStep"`: fake rows 2 and 3 fail → `error() == rows[1].step`, all three ran.
  - `"HostProcess.failedHardeningSaysWhichStep"`: Hello `Failed`; the step's text is non-empty.
  - `"BuildInstrumentation.drivesCapsAndStderr"`: `BuildCapPolicy` and `defaultHelperStandardError()` match the instrumentation; asan tree → `Sanitizers`.
- [ ] **Step 2: Build** `vtextension_process_test vtextension_remote_test`; expect compile failures.
- [ ] **Step 3: Implement M2** (D64).
- [ ] **Step 4: Implement M3** (D65).
- [ ] **Step 5: Implement hardening rows** (D66).
- [ ] **Step 6: Implement the instrumentation switch** (D63). Add `CONTOUR_BUILD_INSTRUMENTATION=$<IF:$<BOOL:${CONTOUR_SANITIZE}>,2,$<IF:$<BOOL:${CONTOUR_COVERAGE}>,1,0>>` on `vtextension_remote_wire` and delete the old three definitions.
- [ ] **Step 7: Run** `vtextension_process_test "ProcessLauncher.*" "LaunchErrors.*" "HostRefusals.*" "BuildInstrumentation.*"` and `vtextension_remote_test "HostBootstrap.*" "HostProcess.*"` → PASS. Grep for the three old macros → none; ctest → PASS.
- [ ] **Step 8: Commit** (four commits):
  - `refactor(vtextension): inject whether the kernel waits on pidfds into the launcher`
  - `refactor(vtextension): phrase each launch failure once`
  - `feat(vtextension): say which hardening step of the extension host failed`
  - `refactor(vtextension): one switch says how a build is instrumented` (CMake and code are inseparable here).

