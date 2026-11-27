# Task 5.7: Windows sandbox (AppContainer, child-process policy, mitigations): report

Status: DONE_WITH_CONCERNS. Everything that can run locally is green. The AppContainer behaviour itself can only
be shown by the first Windows CI run. Nothing was pushed.

## Commits (on top of `f421c11ff`)

| Hash | Subject |
|------|---------|
| `1817406a3` | feat(vtextension): the extension host runs in a deny-by-default sandbox on macOS (Item 0.3 folded in) |
| `d547a5534` | build: sign the macOS extension host with the one entitlement its compiler needs (Item 0.1 and 0.2 folded in) |
| `8d508cba6` | feat(vtextension): the extension host runs in an AppContainer on Windows |
| `ab6ac2a78` | feat(vtextension): grant app containers read access to the extension host once |

The old `4a8546187` and `9fb129082` were rewritten by the autosquash of the Item 0 fixups. Every commit is signed
off and carries no trailers.

## Item 0 (folded into the macOS commits)

1. `build.yml`, the macOS "Extension host: compiling…" step. The `! grep | grep -qv` pipe is gone. The
   rejection lines are saved with `grep -F … > rejected.txt || true`. Then `grep -vF 'in file "<nil>"'
   rejected.txt` runs on that file and its exit status is captured. Only status 1 (no line except `<nil>`)
   passes, so neither a SIGPIPE nor a grep error (status 2) can count as a pass.
2. The same step runs `log show` up to 5 times, 2 s apart, until it sees `in file "<nil>"`, then prints the
   result and asserts as before. This allows for logd lagging behind the helper's death.
3. In `Sandbox_test.cpp`, the old `withoutComments` and substring scan are replaced by `topLevelFormsOf()`. It is
   a lexer written as a range-for state machine (`Code`, `String`, `Escaped`, `Comment`) that splits the profile
   into top-level forms `{head, body}`. It strips `;` comments only outside string literals, and returns nothing
   for unbalanced parentheses, an unterminated string, or text outside a form. A table, `AllowedFormHeads =
   {version, deny, allow}`, is checked against every form of the real profile. The parser test now also covers
   `( allow\tfile-read*)`, `(allow a (literal "x;y"))\n(allow b)` (both forms are seen), `(import "system.sb")`
   (its head is not in the table), and three inputs that do not parse. `vtextension_remote_test "Sandbox.*"`
   gives 7 cases: 6 passed, 1 skipped (by design on Linux), 89 assertions.

## What was built

- `process/ProcessLauncher.hpp`: `enum class ProcessIsolation : uint8_t { None, AppContainer }` and
  `LaunchSpec::isolation`, which defaults to None. `ProcessHostConfig::isolation` defaults to `AppContainer`, the
  production value. `HostComposition.cpp` sets it explicitly, and `ProcessHostConnector` passes it through.
  POSIX launchers ignore it.
- `process/AppContainer_win32.{hpp,cpp}`:
  - `AppContainerApi` is a table of `std::function`s: `createProfile` (CreateAppContainerProfile, which takes no
    capabilities by its signature), `deriveSid`, `readDacl` (GetNamedSecurityInfoW) and `writeDacl`
    (SetNamedSecurityInfoW). `systemAppContainerApi()` returns the system's functions.
  - `ensureAppContainer(api) -> expected<OwnedSid, LaunchError>` uses the profile `Contour.ExtensionHost`. On
    `HRESULT_FROM_WIN32(ERROR_ALREADY_EXISTS)` it derives the SID instead. Any other failure is `Unsupported`.
  - `ensurePackagesCanExecute(directory, api) -> expected<void, LaunchError>` first checks whether the DACL
    already covers `S-1-15-2-1` with RX (`FILE_GENERIC_READ | FILE_EXECUTE`, generic rights mapped) for both the
    directory itself (an ACE that is not INHERIT_ONLY) and its files (OBJECT_INHERIT). Inherited and explicit
    ACEs both count. A null DACL counts as covered. Only if the DACL does not cover it is one entry added:
    `SUB_CONTAINERS_AND_OBJECTS_INHERIT`, `GRANT_ACCESS`. `ERROR_ACCESS_DENIED` on the read or the write gives
    `Unsupported`; any other error gives `Io`.
  - `AppContainerProvision` is the per-launcher cache, used only on the supervisor's thread. It ensures the
    profile once and each executable directory once. On `Unsupported` from the grant, it writes one line to
    Contour's error log (`errorLogWriter()`). The line names the directory, says the installer must grant ALL
    APPLICATION PACKAGES (S-1-15-2-1) read and execute access there, and is UTF-8.
- `process/Process_win32.cpp`:
  - The launcher holds an `AppContainerProvision`.
  - For an `AppContainer` spec it adds `PROC_THREAD_ATTRIBUTE_SECURITY_CAPABILITIES` (the SID, no capabilities)
    and `PROC_THREAD_ATTRIBUTE_CHILD_PROCESS_POLICY = PROCESS_CREATION_CHILD_PROCESS_RESTRICTED`. These sit beside
    the handle list and the job list, so the list has 4 attributes instead of 2.
  - `AttributeList::set` is now a template over `std::span<T, Extent>`.
  - A contained child's working directory is its executable's directory, not the volume root (see Deviations).
  - Two clang-tidy findings already in this file were fixed along the way: the parentheses in
    `appendArgument`, and an explicit `void*` cast.
- `host/Sandbox_win32.cpp`, selected on every WIN32 build as `PlatformSandbox::Windows = 4`
  (`host/CMakeLists.txt`). `lockDown()` works as follows:
  - It reads `GetTokenInformation(TokenIsAppContainer)`. If the token is not an AppContainer token, the state is
    `Unsandboxed` and the call returns success.
  - Otherwise it applies the rows of `MitigationRows` through `SetProcessMitigationPolicy`:
    `ExtensionPointDisable`, `ImageLoad` (`NoRemoteImages` and `NoLowMandatoryLabelImages`), and
    `StrictHandleCheck` (raise, permanently).
  - It then checks `IsProcessInJob`. Not being in a job fails `Container` with
    `HRESULT_FROM_WIN32(ERROR_PROCESS_NOT_IN_JOB)`.
  - A failed token query fails `Container`; a failed mitigation fails `Mitigations`. Codes are HRESULTs.
  - Only when everything passes is the state `LockedDown`.
  - There is no dynamic-code policy, and `limitOf` is `Unlimited`.
  - `Sandbox.hpp`: the doc of `lockdownState()` now notes the Windows case.
- Test compositions (D107):
  - `testing/LaunchedHelper.{hpp,cpp}` gain `runsOnWine()`, which looks up `ntdll!wine_get_version`, and
    `helperIsolation()`, which is AppContainer on Windows, None under wine and None on POSIX.
    `HelperLaunch::isolation` defaults to None.
  - In `HelperComposition`, the constexpr `HelperLockdown` became `helperLockdown()`: AcceptUnsandboxed where
    there is no platform sandbox or under wine. The connector there uses `helperIsolation()`.
  - `ProcessDefaultBackend.cpp` and `ReviewFocusProcess_test.cpp` use the function.
  - The constexpr `ThisBuildLockdown` in `Helper_test.cpp` became `thisBuildLockdown()`, and `helperLaunch()`
    carries `helperIsolation()`. `WamrHelper_test`'s expectation likewise. The Linux-only `STATIC_CHECK` became
    `CHECK`.
  - The `--parent` mode of `LaunchProbe` starts its helper with `isolation = None`: it proves the parent end,
    not containment.
- Probe: `extension_host_probe --contained <helper args> --profile-file=PATH --port=N` (Windows only). It reports
  `container=yes|no|unknown`, `profile-file=`, `connect=` (TCP to 127.0.0.1:N) and `spawn=` (the system's
  cmd.exe), each `ok` or `refused:<call>:<win32 error>`, then `end`.
- CI: `.github/actions/windows-tests/action.yml` has a new step, "verify the extension host's AppContainer tests
  actually ran". It runs `vtextension_process_test.exe '[appcontainer]' -s` and
  `vtextension_wamr_helper_test.exe '[appcontainer]' -s`. It fails on a missing binary, on any `skipped`, and on
  a non-zero exit. Because of `-s`, the log always shows the probe's line for each operation and the Hello
  (lockdown, the text of the failed step, the code as hex).
- CMake: `userenv` is linked privately into `vtextension_remote_process`, and `ws2_32` into
  `extension_host_probe` and `vtextension_process_test` on WIN32. `AppContainer_test.cpp` is in process_test on
  every system. `_CONTOUR_SANDBOXED_PLATFORMS` is unchanged (D110), so `contour` stays in-process on Windows.

## Tests (new)

| Case | Where | Systems |
|------|-------|---------|
| `AppContainer.launchCarriesTheIsolation` | process_test | all. The connector's default, explicit None and explicit AppContainer each reach both launches' specs |
| `AppContainer.profileIsEnsuredOnce` `[appcontainer]` | process_test | Windows. Fake API: created once over two `prepare()`s with the same SID; `ALREADY_EXISTS` leads to derive; `E_ACCESSDENIED` gives `Unsupported` with no derive |
| `AppContainer.packageGrantIsIdempotent` `[appcontainer]` | process_test | Windows. A real DACL on a temp directory made protected first, with 0 package ACEs. Two grants leave exactly 1 explicit ACE. A fake `writeDacl` returning ACCESS_DENIED gives `Unsupported`, and the provision logs one line naming the directory, "installer" and S-1-15-2-1 |
| `Sandbox.windowsHelperRunsContained` `[appcontainer]` | process_test | Windows. The probe runs contained: `container=yes`, and the profile file, the loopback connect and the spawn all `refused:`. Under wine it SKIPs and records what the contained launch did |
| `Helper.windowsHelperIsLockedDown` `[appcontainer]` | **wamr_helper_test** (see Deviations) | Windows. The Hello says `LockedDown`. Under wine it says `Unsandboxed`, recorded with `WARN` |

## Wine runs

Tree: `out/clangcl-debug` (preset `linux-to-windows-clangcl-debug`: Debug, Embedded debug format, extensions on,
bundled extensions and GUI off, `CONTOUR_WASM_RUNTIME=wamr`), with its own `WINEPREFIX` in the scratchpad and
`WINEDEBUG=-all`. The build for `vtextension_process_test`, `extension_host_probe`, `contour-extension-host` and
`vtextension_wamr_helper_test` exits 0, with no warning from Contour's own sources.

process_test (five runs):
1. `AppContainer.launchCarriesTheIsolation`: passed (18 assertions).
2. `AppContainer.profileIsEnsuredOnce`: passed (11).
3. `AppContainer.packageGrantIsIdempotent`: passed (17). Wine stores real DACLs, so the protected DACL, the
   added ACE and the idempotence are real here.
4. `Sandbox.windowsHelperRunsContained`: SKIPPED with "wine stubs AppContainers; the contained launch was asked
   for and it failed: the system lacks what it takes …". Wine refuses `CreateAppContainerProfile`, so the
   launcher returns `Unsupported` before `CreateProcessW`. **Whether wine's `CreateProcessW` accepts
   `SECURITY_CAPABILITIES` was therefore not reached, and is not known.**
5. The whole binary: 112 test cases, 110 passed, 2 skipped (the contained probe above, and the intended skip in
   `SharedObject_test`); 928 assertions passed.

helper_test (three runs, on `vtextension_wamr_helper_test`; see Deviations):
1. `Helper.windowsHelperIsLockedDown` with `-s`: passed. `Hello: lockdown=1` (Unsandboxed), "no step of its
   confinement failed", and the WARN "wine: the uncontained helper says Unsandboxed, as it must". This is the
   whole path end to end: `TokenIsAppContainer` false, so `Unsandboxed`, so `AcceptUnsandboxed`.
2. `Helper.wamrHelperSaysCopy`: passed (11).
3. `~[.measure]`: 48 test cases, 47 passed, 1 skipped (bundled extensions are off); 623 assertions passed. The
   contract, Bundled and process cases all run on helpers started uncontained and accepted unsandboxed.

What wine can show: the code compiles cleanly under `-Werror` with clang-cl; the attribute list for a contained
launch builds; the ACL check, the add and the idempotence work; the full Unsandboxed path; and the fake-API
profile logic.

What wine cannot show:
- that an AppContainer isolates files, the network or the registry;
- that the mitigation policies take effect;
- that the child-process policy takes effect;
- that `CreateProcessW` takes `SECURITY_CAPABILITIES`;
- that the ACL grant lets a real AppContainer load the helper and its CRT.

Local workarounds for the cross tree only (nothing committed):
- `out/clangcl-debug/_deps/wamr-src/core/shared/platform/windows/win_file.c` has three `// … \ ` comments
  (a backslash followed by a space). clang-cl reads these as line continuations and swallows code; MSVC does
  not. I edited the local copy of that dependency.
- WAMR's `.asm` needs `ml64`, so `/opt/msvc/bin/x64` was put on PATH.
- lld-link looks for `Pathcch.lib` but the SDK file is `pathcch.lib`. I pointed `LIB` at a scratch directory
  holding a symlink.
- Without these the WAMR helper does not build in the cross tree at all, so before this task
  `vtextension_wamr_helper_test` had never been cross-built. `vtextension_helper_test` (Wasmtime) cannot be
  cross-built here, because there is no Windows Wasmtime in `out/windows-cross-deps`.

## Local checks (Linux, `out/clang-asan`)

- Build of `vtextension_test`, `vtextension_wasmtime_test`, `vtextension_process_test`,
  `vtextension_remote_test`, `vtextension_helper_test`, `vtextension_wamr_helper_test`, `extension_host_probe`
  and `contour-extension-host`: exit 0, 0 warnings.
- `ctest --preset=clang-asan -R 'vtextension|wasm_runtime_containment'`: 8/8 passed.
- `ctest --preset=clang-asan -L lint` (includes `check_workflows`): 34/34 passed.
- `vtextension_process_test` on Linux: all passed, 5536 assertions.
  `AppContainer.launchCarriesTheIsolation` passed (18).
- `scripts/clang-tidy-diff.sh --base HEAD`, with new files added as intent-to-add: 12 translation units clean.
  The Windows-only files are not in the Linux compile database, as the script says.
- I also ran clang-tidy on the Windows translation units through `out/clangcl-debug`'s compile database; CI does
  not. Ignoring the `duplicate-include` false positives clang-cl causes, I fixed every finding in new code (sign
  comparisons, a member initializer, a by-value `shared_ptr`, a redundant RX expression) and the two in
  `Process_win32.cpp` that were there before. Two remain, both shared with the other platforms' files:
  `DescriptorRights.hpp:46` (pro-type-member-init) and `makeSystemSandbox`'s by-value parameter, which is fixed by
  `Sandbox.hpp`.
- `/usr/bin/clang-format` 22 `--dry-run --Werror` on every changed C++ file: clean.
- `editorconfig-checker` on the changed files: clean.
- `scripts/check-spelling.py` (typos 1.48.0): OK.
- `/simplify` was done by hand:
  - `WamrHelper_test` uses `helperIsolation()` instead of a second wine branch.
  - The Hello diagnostic prints `lockdownStepText`.
  - `RecordingLauncher` was dropped from the isolation test: its handle cannot be watched by the fake's wait set.
  - The token check is written twice, once in `Sandbox_win32.cpp` and once in the probe, which is test-only and
    has to stand alone. I kept both.

## What the first Windows CI run (PR #2431, `Windows` job, MSVC, msvc-ci) must show

1. MSVC compiles all of it at `/W4 /WX`. The parts to watch: designated initializers of `SECURITY_CAPABILITIES`;
   lambdas converted to `DWORD (*)() noexcept` inside a `constexpr std::array`; the `AttributeList::set`
   template; `std::ranges::contains`; `std::format` of `uint8_t`.
2. The new step "verify the extension host's AppContainer tests actually ran" passes. With `-s` its log must
   show:
   - `Sandbox.windowsHelperRunsContained`: `container=yes`, and `profile-file=refused:CreateFileW:5`,
     `connect=refused:connect:…` (or `socket`/`WSAStartup`), and `spawn=refused:CreateProcessW:…`, each expected
     to be some `refused:` value.
   - `Helper.windowsHelperIsLockedDown`: `Hello: lockdown=2` (LockedDown).
   If it shows `lockdown=0` (Failed), the failed step text and the HRESULT say which mitigation or the job.
   - `AppContainer.packageGrantIsIdempotent`, which leaves 1 ACE.
   - `AppContainer.profileIsEnsuredOnce`.
3. The `vtextension_process_test` and `vtextension_wamr_helper_test` ctests pass, and so does
   `vtextension_helper_test` where CI builds it. Every process suite there then runs with contained helpers
   held to `RequireLockedDown`. This is the real proof that the contained helper loads (the ACL grant on the
   build's `bin`, the CRT beside it) and that `StrictHandleCheck` does not trip on any handle the host or engine
   misuses.
4. If it is green, D110 can append `Windows` to `_CONTOUR_SANDBOXED_PLATFORMS`.

## Installer follow-up (not in this task)

- The per-machine MSI (WiX, `cmake/CPackWixScope.cmake`) installs under `Program Files`, which already grants
  ALL APPLICATION PACKAGES RX through inheritance. The check finds that grant and writes nothing.
- The per-user MSI installs into a directory the user owns, where the runtime grant can be written.
- So no installer change is needed for either scope today.
- Hardening follow-up: have the WiX sources grant `S-1-15-2-1` read and execute explicitly on the extension
  host's install directory (`PermissionEx`). Then an administrator who tightened the inherited ACLs does not
  break it. The WiX sources are generated through CPack, and that change is not small, so it is listed here
  rather than made.

## Deviations

- **`Helper.windowsHelperIsLockedDown` is in `WamrHelper_test.cpp` (vtextension_wamr_helper_test), not in
  `Helper_test.cpp`.** helper_test needs Wasmtime, which the Windows cross tree does not have, so it could never
  run under wine. Without `Windows` in the sandboxed platforms, Windows defaults to WAMR and its
  `contour-extension-host` is the WAMR helper. `Helper_test.cpp` still checks Windows through
  `thisBuildLockdown()` in `receiveHello()` wherever CI builds it.
- **The "three helper_test runs" were run on wamr_helper_test**, for the same reason.
- **`launchCarriesTheIsolation` uses `FakeProcessLauncher`'s recorded specs, not `RecordingLauncher`.** A
  `RecordingLauncher` wrapped around the fake hands the supervisor `RecordingHandle`s, which the fake's wait set
  cannot watch, so `connect()` fails. That is a limit of the fake that was there before.
- **A contained child's working directory is its executable's directory, not the volume root.** An AppContainer
  generally may not open `C:\`, and process start-up opens its current directory. The ungranted root might have
  failed every contained launch. The uncontained launch keeps the root.
- **The Windows CI verification step went into the first feature commit**, not into a separate `ci:` commit. The
  brief names exactly two commits, and without the step a skipped container case would read as a pass, so I
  treated it as part of the feature.
- **Not being in a job fails the `Container` step** with `HRESULT_FROM_WIN32(ERROR_PROCESS_NOT_IN_JOB)`. The brief
  said "any failure → Mitigations/Container", so I picked the step and the code.

## Concerns

- No AppContainer behaviour has been observed on real Windows yet. Expect a CI iteration. The riskiest parts are:
  - `StrictHandleCheck`: any close or use of an invalid handle in the helper, its CRT or WAMR now raises.
  - Whether the debug CRTs and `vcpkg` DLLs that CI puts next to the binaries are readable once `bin` is
    granted. They inherit the grant, as `SetNamedSecurityInfoW` propagates it.
  - Whether `JOB_LIST` and `SECURITY_CAPABILITIES` together are accepted. I believe they are; Chromium assigns
    its job after creation instead.
- The tests grant ALL APPLICATION PACKAGES RX on the CI build's `bin` directory, which modifies the runner's
  checkout ACL. This is harmless on ephemeral runners, but on a developer's Windows box the tests change that
  directory's DACL once.
- `Unsupported` still says "the system lacks what it takes (on Linux, kernel 5.4 or later)". On Windows the
  separate error-log line about the directory is what explains it. Making the launch-error text name each
  system was out of scope.
- The local cross tree needs the three workarounds above before it can build the WAMR helper with clang-cl. A
  small follow-up could add `ml64` to PATH and the `Pathcch.lib` alias to `cmake/toolchains/windows-clangcl-cross.cmake`;
  the `win_file.c` trailing backslashes are WAMR's to fix upstream.

## Fix round 1

New HEAD `0ed645f58`. I fetched origin and rebased onto `origin/master` `8c3afaa0c`, which has not moved. Nothing pushed.

| Hash | Subject |
|------|---------|
| `1817406a3` | feat(vtextension): the extension host runs in a deny-by-default sandbox on macOS (unchanged) |
| `1c0ef5061` | build: sign the macOS extension host with the one entitlement its compiler needs (item 8 folded in) |
| `a4083b4c1` | feat(vtextension): grant app containers read access to the extension host once (**now first**, item 6) |
| `0ed645f58` | feat(vtextension): the extension host runs in an AppContainer on Windows |

### What changed, by item
1. `windows-tests/action.yml`: the AppContainer step has `if: ${{ !cancelled() }}`, so it runs after a red ctest.
2. **Launch failures say why:**
   - In `LaunchedHelper`:
     - Windows only: `CapturedStandardError`, a delete-on-close temporary file handed to the child as its stderr and read back afterwards.
     - `endOf(launcher, process, stderr)`: it reaps the process and says "it exited with 0x%08X" (or the signal) and what it wrote.
   - `WamrHelper_test` `helloOfHelperIn()` and `AppContainer_test` `probeReportIn()`:
     - `INFO("launch: " << launchErrorText(...))` before each launch `REQUIRE`.
     - A missing Hello or report `FAIL`s with `endOf(...)`, so 0xC0000022 and 0xC0000135 can be recognised.
3. **The ACL tests can now fail:**
   - `packageGrantIsIdempotent`: after the first grant, the second call runs with `apiThatMustNotWrite()`, whose `writeDacl` counts the write and fails. The call must succeed with 0 writes. The single explicit ACE must have flags exactly `OBJECT_INHERIT_ACE`. The test prints the DACL on failure.
   - New `AppContainer.programFilesGrantIsSeen`:
     - The DACL is built in memory (`InitializeAcl` and `AddAccessAllowedAceEx`) and handed to `readDacl`.
     - It has three rows:
       - "both" (`(I)(RX)` and `(I)(OI)(CI)(IO)(GR,GE)`): seen as granted, 0 writes.
       - "itself only": the grant is attempted, 1 write.
       - "its files only": the grant is attempted, 1 write.
     - The two negative rows show the check can fail.
   - Deviation: I first built this on a real child directory of a scratch parent. Wine computes no ACE inheritance when a directory is created (the dump showed the child got wine's default DACL), so that version could not run under wine. The in-memory DACL is deterministic on every system and still goes through the production code.
4. **The containment probe is specific:**
   - The probe runs **uncontained** first, as the positive control. The file and the connect must be `ok`, and `container=no`. On real Windows the spawn must be `refused:CreateProcessW:1816`, the job's process limit.
   - Then it runs contained:
     - `profile-file=refused:CreateFileW:5`.
     - `spawn=refused:CreateProcessW:5`: the child-process policy, not the job's 1816.
     - `connect`: one of `ContainedConnectRefusals`, which are 10013 WSAEACCES, 10060 WSAETIMEDOUT, 10051 WSAENETUNREACH and 10065 WSAEHOSTUNREACH.
   - **Uncertain:** Microsoft documents only that the Windows Filtering Platform's default block filter drops an AppContainer's loopback traffic without a capability (the "Troubleshooting UWP App Connectivity Issues" and "Interprocess communication: Loopback" pages). It does not document the error a caller sees, so I accept that set and the comment says so. WSAECONNREFUSED is excluded, because the uncontained run proves there is a listener.
   - Under wine the uncontained control runs and passes: container=no, the file ok, the connect ok, and the spawn ok, because wine does not hold the job's process limit, so the spawn is only recorded.
5. The grant uses `OBJECT_INHERIT_ACE` only. It reaches the directory's files, not the directories beneath. `grantsReadAndExecute` already required OI for the files and "not inherit-only" for the directory, so it needed no change. The tests check the flags.
6. **The commits are reordered:** the grant commit is now a self-contained part of `AppContainer_win32.{hpp,cpp}`:
   - `AllApplicationPackagesSid`, an `AppContainerApi` with only `readDacl` and `writeDacl`, and `ensurePackagesCanExecute`.
   - Its tests in `AppContainer_test.cpp`: the grant cases only.
   - The CMake source and test lines.

   The launch commit adds the profile functions to the table, the `AppContainerProvision` with its log line, and everything else. Both commits build: commit A was cross-built and ASan-built on its own, and its `[appcontainer]` cases pass under wine (2 cases, 20 assertions). I rebuilt the history with a non-interactive autosquash rebase that dropped the two old Windows commits, then recreated them from a snapshot of the final tree, so HEAD's tree equals the tested final tree.
7. **The uncontained helper is refused:**
   - New `Helper.windowsUncontainedHelperIsRefused` `[appcontainer]` in `WamrHelper_test`:
     - A helper launched with `None` says Hello `Unsandboxed`.
     - A backend with `RequireLockedDown` and `isolation = None` refuses it: its load fails `HostUnavailable`.
     - It also runs under wine, and passes there.
   - To support it, `HelperBackendConfig` gained `isolation`, which defaults to `helperIsolation()`.
   - `Sandbox.systemWithoutOneRunsUnsandboxed` now runs on Windows too, and asserts that the Windows sandbox in this uncontained test process confines nothing and reports `Unsandboxed`.
8. `build.yml`: `sudo log show … > refused.txt || true` inside the retry loop. Folded into the macOS signing commit.
9. **The `Unsupported` text is per system:** `LauncherSystem { Other, Linux, Windows }`, `ThisLauncherSystem`, and a table with one text per system (`UnsupportedTexts`). The Windows row reads "the system lacks what it takes (on Windows, app containers: Windows 10 or later)", and the wine skip message now shows it.

### Checks (HEAD)
- **Tidy-diff** (`--base HEAD~3`): 13 translation units clean. The round's code drew four findings, all fixed: a nested conditional in `endOf`, an unused namespace alias, and two preprocessor conditions now written as a nested `#ifdef` rather than C++23 `#elifdef`, for MSVC's sake.
- **Formatting and spelling:** clang-format 22 `--dry-run --Werror`, `editorconfig-checker` and typos are clean.
- **Builds:** the ASan build (all vtextension targets) and the clang-cl cross build (process_test, probe, helper, wamr_helper_test) both exit 0 with 0 warnings.
- **ctest:** `-R 'vtextension|wasm_runtime_containment'` passes 8/8, and `-L lint` (with `check_workflows`) passes 34/34. `vtextension_remote_test "Sandbox.*"` passes all 89 assertions.
- **Wine, process_test:**
  - `launchCarriesTheIsolation`: passed (18 assertions).
  - `profileIsEnsuredOnce`: passed (11).
  - `packageGrantIsIdempotent`: passed (19).
  - `programFilesGrantIsSeen`: passed (6).
  - `Sandbox.windowsHelperRunsContained`: the uncontained control passes, then the case skips with the Windows "unsupported" text.
  - The whole binary: 113 test cases, 111 passed, 2 skipped; 949 assertions.
- **Wine, wamr_helper_test:**
  - `[appcontainer] -s`: 2 cases, 26 assertions, both helpers say Hello `lockdown=1` (Unsandboxed).
  - `Helper.wamrHelperSaysCopy`: passed (11).
  - `~[.measure]`: 49 test cases, 48 passed, 1 skipped (bundled extensions off); 637 assertions.

### Still open
- Real-Windows behaviour remains unproven until CI runs. The connect error set (item 4) is the least certain expectation.
- I could not observe whether wine accepts `SECURITY_CAPABILITIES`: wine refuses the profile first.

## Fix round 2

New HEAD is `5798ba3f6`. I fetched and autosquashed onto `origin/master` `8c3afaa0c`. Nothing pushed.

| Hash | Subject |
|------|---------|
| `1817406a3` | feat(vtextension): the extension host runs in a deny-by-default sandbox on macOS (unchanged) |
| `1c0ef5061` | build: sign the macOS extension host with the one entitlement its compiler needs (unchanged) |
| `616ae93b7` | feat(vtextension): grant app containers read access to the extension host once (item 3; message reworded) |
| `5798ba3f6` | feat(vtextension): the extension host runs in an AppContainer on Windows (items 1, 2, 4, 5; message reworded) |

### What changed, by item

1. **The child is reaped before its stderr is read.**
   - `endOf()` now takes a `StandardErrorReader` (`std::function<std::string()>`). It reaps the process first and then calls the reader.
   - The two callers pass `[&] { return standardError->text(); }`, so a write the child made as it ended is in the file before it is read.
2. **Stderr is shown on any unclean exit.** The new `checkEndedCleanly(launcher, process, reader)` replaces the exit-0 `CHECK` in `helloOfHelperIn` and `probeReportIn`. When the status is not `Exited 0`, it records the ending in hex together with the captured stderr, read after the reap, and then checks. A helper that says Hello and then exits nonzero now shows why.
3. **The grant no longer propagates.** It is `OBJECT_INHERIT_ACE | NO_PROPAGATE_INHERIT_ACE`, so it covers only the files directly in the helper's directory. A subdirectory no longer gets an inherit-only copy.
   - The test expects exactly those flags.
   - `grantsReadAndExecute` does not compare NP, so it needed no change: an OI entry with or without NP covers the directory's own files.
   - Nothing the contained helper loads lives in a subdirectory. The C runtime, the vcpkg DLLs and the engine all sit beside the executable in `bin` and in the install directory, so I found no reason to keep propagation.
   - The doc comment and the grant commit's message now say what is really granted.
4. **The test listener accepts every connection.**
   - `LoopbackListener` listens with `SOMAXCONN` and accepts on a `std::jthread`, closing each connection at once. The destructor closes the socket, which ends the blocked `accept`, then joins the thread and only then calls `WSACleanup`. Under wine it shuts down cleanly.
   - So the uncontained control's connection no longer fills a backlog slot, and a contained refusal can only come from the container.
   - Decision: **10061 (WSAECONNREFUSED) is added to `ContainedConnectRefusals`.**
     - UWP loopback blocks are widely reported as 0x8007274D, which is WSAECONNREFUSED.
     - Microsoft documents only that the default WFP block filter drops such traffic, not which error the caller sees.
     - With an accepting listener, and an uncontained run that reaches it, a refusal is the container's.
     - The accepted set is now 10013, 10061, 10060, 10051 and 10065. The comment says this set is based on reports, not on documentation.
5. **A grant the user may not write has its own error and its own text.** This is data-driven, one row in each table:
   - `LaunchError::NotGranted` and its `launchErrors()` row. The text reads: "its directory does not let it run contained, and only its installer can grant that".
   - `HostRefusal::HelperNotGranted`, with a `refusalSource` case and a `RefusalPersistenceRows` row. It is Transient, because a reinstall may grant the access while Contour runs.
   - `daclErrorOf(ERROR_ACCESS_DENIED)` now returns `NotGranted`, and the provision writes its directory-naming log line for that error.
   - `ProcessHostConnector.lastingRefusalsStartNoOtherHost` has a row for the new reason.
   - The refusal line now reads "The extension host cannot be started: its directory does not let it run contained, and only its installer can grant that. Extensions do not run."
   - `Unsupported` keeps its per-system text and now covers only a refused profile.
   - This lives in the launch commit. The grant commit on its own still returns `Unsupported`, because the launch-error table row it would need sits next to the per-system text that the launch commit introduces.

### Checks (HEAD `5798ba3f6`)
- **Grant commit on its own (`616ae93b7`):** the cross build and the ASan build of `vtextension_process_test` both exit 0. Under wine the `[appcontainer]` cases pass: 2 cases, 20 assertions.
- **Builds:** the ASan build (all vtextension targets) and the clang-cl cross build both exit 0, with 0 warnings.
- **ctest:** `-R 'vtextension|wasm_runtime_containment'` passes 8/8. `-L lint` (with `check_workflows`) passes 34/34.
- **Static checks:**
  - Tidy-diff (`--base HEAD~3`): 15 translation units clean.
  - clang-format 22 `--dry-run --Werror`: clean. This caught the realigned `HostRefusal` comments, which were then formatted.
  - editorconfig and typos: clean.
- **Wine, process_test:**
  - `launchCarriesTheIsolation`: passed (18 assertions).
  - `profileIsEnsuredOnce`: passed (11).
  - `packageGrantIsIdempotent`: passed (19).
  - `programFilesGrantIsSeen`: passed (6).
  - `Sandbox.windowsHelperRunsContained`: the uncontained control passes against the accepting listener, then the case skips.
  - The whole binary: 113 test cases, 111 passed, 2 skipped; 964 assertions.
- **Wine, wamr_helper_test:**
  - `[appcontainer] -s`: 2 cases, 26 assertions.
  - `Helper.wamrHelperSaysCopy`: passed (11).
  - `~[.measure]`: 49 test cases, 48 passed, 1 skipped; 637 assertions.

### Still open
- Real-Windows behaviour can only be shown by CI.
- The contained connect error is accepted from a set, as decided above.
