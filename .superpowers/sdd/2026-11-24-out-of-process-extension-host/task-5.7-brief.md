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


---

## Plan decisions (verbatim)

- D106. On Windows the **parent** confines the helper at launch, and the helper verifies.
  - Launch: an AppContainer profile `Contour.ExtensionHost` with no capabilities, plus `CHILD_PROCESS_POLICY` restricted, on top of the existing job.
  - Ensured once per supervisor, on its thread, through an injectable API table: the profile, and an idempotent `ALL APPLICATION PACKAGES` RX ACE on the helper's directory.
  - Helper: a token that is not an AppContainer token → `Unsandboxed`. Otherwise it applies the mitigation rows and checks the job. A failure → `Failed`; all pass → `LockedDown`.
  - Dynamic-code policy stays off.
  - Why: an AppContainer can only be entered at creation, and keeping "not contained" apart from "failed" keeps wine and test runs meaningful.
- D107. Under wine (`ntdll!wine_get_version`), test compositions use `ProcessIsolation::None` and `AcceptUnsandboxed`, and AppContainer cases SKIP with a reason. Why: wine stubs AppContainer, so a run where everything fails would hide every other Windows regression.

## Item 0: carried over from the macOS sandbox review (cheap, do first, folded into the commits they belong to)

1. `build.yml` ~1176: `! grep … | grep -qv 'in file "<nil>"'` under `pipefail` can turn a SIGPIPE into a pass. Use a single `grep -v` on a saved file instead.
2. `build.yml` ~1170-1175: `log show` runs right after the helper dies. Retry it a few times with a short sleep before failing, because logd may lag.
3. `Sandbox_test.cpp` profile parser: add a table of allowed top-level form heads (`version`, `deny`, `allow`) so that `( allow …)` with a space, `(import "…")`, and a `;` inside a string literal cannot get past it.

## Controller notes (binding)

- **Windows CI runs on pull requests** (the `Windows` job on draft PR #2431, built with real MSVC). The AppContainer behaviour CAN be proven before merge. I push after your report; expect a CI iteration. Make the Windows CI step's failure output diagnosable: print the Hello line and the probe's per-op results.
- **Do NOT append `Windows` to `_CONTOUR_SANDBOXED_PLATFORMS` in this task (D110).** The gate decides once the Windows CI leg is green with the AppContainer. Land the code so CI runs the contained helper suite, while `contour` itself stays in-process on Windows for now.
- **ACL grant on the install directory.** A per-machine install under `Program Files` cannot be modified by a normal user. `Program Files` normally already grants `ALL APPLICATION PACKAGES` read/execute through inheritance. Design:
  - `ensurePackagesCanExecute` first checks whether such an ACE (inherited or explicit) already grants RX, and does nothing if so.
  - If not, it tries to add the ACE.
  - If that fails with access denied, the error is `LaunchError::Unsupported`, with a line naming the directory and the fix: the installer must grant it.
  - The installer change itself (WiX/NSIS, if the tree has one) goes in this task only if it is small and its file is in the tree. Otherwise list it in the report as a follow-up.
- **MSVC traps.** The wine tree uses clang-cl, which does NOT catch MSVC-only gaps: `__builtin_*`, `[[nodiscard]]` on certain declarations, C4xxx warnings at `/W4 /WX`, designated-initializer order, `std::expected` quirks. Read your Windows code with real MSVC in mind. Also read `task-ci1-report.md` for the traps CI already found.
- **Before you report, run locally:**
  - `ctest -R 'vtextension|wasm_runtime_containment'`;
  - `ctest -L lint`, which includes `check_workflows`;
  - CI's tidy-diff;
  - clang-format, typos and editorconfig on the changed files;
  - the wine runs the brief names, using the clang-cl cross tree described in memory (`/opt/msvc`, `out/clangcl-debug`, or the preset the brief names).
- **Commits:**
  - Item 0 first, folded into the commits it belongs to; then the two commits the brief names.
  - Each commit must build on its own.
  - `git commit -s`, with no trailers.
  - Messages never mention the plan, task numbers or "phase".
  - User-facing text never says wasm, WAMR or Wasmtime.
