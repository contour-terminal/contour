# 5.8 — first real Windows CI run: report

Status: DONE_WITH_CONCERNS. Base 9aefa2abe, new head **8484a0694** (not pushed). The tree after the rebase is
byte for byte the tree that was tested (`508ad606…`).

## 1. `WasmtimeEngine.signalTrapsPastTheOpenBytes`: the test was wrong, the bound holds

### Root cause

The test's `GrantedObject` committed the **whole** object through the parent's view
(`mapping->commit(0, bytes)` in `mapped()`). Windows commits the pages of a pagefile-backed `SEC_RESERVE`
section **for the section, not for a view**. So every page of the object was committed in the helper's view
too, and nothing closed the bytes past the open one. Under signal traps those closed pages are all that stops
a store past `memory.size` (Wasmtime checks against the reservation), so the store went through.

Evidence:
- CI log: only "past the open bytes" fails. "past the object" passes: that store is sent to address 0 and
  faults there.
- The project already states and tests that Windows commits for the section:
  `SharedObject.pagesCommittedThroughOneViewAreSeenThroughAnother` ("Windows commits a reserved section's pages
  for the section, so any view may touch them"), and it skips under wine for the same reason. That case did
  not run on this CI run: ctest stopped after 51 of 78 tests, and the `[appcontainer]` step ran only that tag.
- Microsoft Learn (CreateFileMapping2, `SEC_RESERVE`): committed pages "cannot be freed or decommitted with
  VirtualFree". That fits commit being a property of the section. Page State: a reserved page "is not
  accessible". The docs never say outright that the commit is cross-view; that comes from Windows Internals
  and from the project's own test.
- Production commits exactly what it granted: `SharedGuestMemory::create` commits `initialBytes`, and `grow`
  commits up to the acknowledged size. `SharedGuestMemory.commitFollowsAcknowledgement` already checks this
  with VirtualQuery. So in production the committed pages are the open pages, and the bound holds. Only the
  test's parent over-committed.

The brief says wine passes this test. **It does not.** It fails under wine at base too: I rebuilt with the
base test and it failed the same way. The reason is a different wine defect. A raw store into a
`MEM_RESERVE` page of a section view does **not** fault under wine. An SEH probe showed `faults=0`, and
VirtualQuery still reported `MEM_RESERVE` / `PAGE_NOACCESS` afterwards. So wine cannot show this bound at all.

### Changes (folded into 5f3c4e026, which introduced the case, and 01c3799e3)

- `src/vtextension/wasm/wasmtime/WasmtimeEngine_test.cpp`: `GrantedObject` now commits what Contour would:
  - `provision()` commits `[0, initialBytes)` through the parent's view.
  - A new member `answering()` commits a granted growth through the parent before the helper opens it.
  - It is pinned in place (its growth requests hold `this`), and every call site uses `object.answering(...)`.
  - `signalTrapsPastTheOpenBytes` SKIPs under wine, with the reason ("wine lets a store into a reserved page of
    a section's view through, unlike Windows").
- `src/vtextension/wasm/remote/transport/GuestMemoryMapping_test.cpp`: new Windows-only case
  `GuestMemoryMapping.closedPagesAreThoseNoViewCommitted` (skipped under wine). The parent commits one page,
  the helper's next page is `MEM_RESERVE`. The parent then commits that page and it reads `MEM_COMMIT` in the
  helper's view, while the page after it stays reserved. This pins the dependency on real Windows.
- `GuestMemoryMapping_win32.cpp` file head: the old comment said "whether Windows commits for the section or
  for each view". It now says the commit is the section's, and that the closed pages stay closed only because
  Contour commits no more than it granted. `SharedGuestMemory::grow` gets the matching comment.

## 2. `Sandbox.windowsHelperRunsContained`: "the system refused" is now traceable

### Root cause: not determinable off Windows

"the system refused" is `LaunchError::Io`, the catch-all. It drops which step failed and the Win32 code.
Candidates, each reaching `Io` and none distinguishable from the log:
- `UpdateProcThreadAttribute` refusing `SECURITY_CAPABILITIES` or `CHILD_PROCESS_POLICY`.
- `CreateProcessW` failing with a code outside `launchErrorOf()`'s table. `ERROR_ACCESS_DENIED` maps to
  `NotFound`, so it is not that one.
- Reading or writing the directory's DACL failing with a code other than `ERROR_ACCESS_DENIED`.
- `SetEntriesInAclW`.

My leading guess, unconfirmed: the new process gets a console (`CREATE_NO_WINDOW`), and its `conhost.exe`
may be blocked by `PROCESS_CREATION_CHILD_PROCESS_RESTRICTED`. Chromium starts its sandboxed targets with
`DETACHED_PROCESS`. I did **not** change the flags on a guess; the next CI run will name the step and code.
Microsoft Learn's UpdateProcThreadAttribute page documents the policy but says nothing about consoles.

The dependency-DLL theory is not the cause either way: a missing or unreadable DLL fails in the child's
loader after CreateProcess has succeeded, not as a launch error.

### Changes (folded into fb93a94ed, the AppContainer commit)

- New `process/LaunchRefusal_win32.{hpp,cpp}`, data-driven:
  - `LaunchStep` (17 steps) with a `LaunchSteps` row table (text, and whether its code is Win32 or HRESULT).
    `static_assert`s check that each row sits at its enumerator's index.
  - `DetailedLaunchErrors` = {`Unsupported`, `Io`}: the errors whose text cannot say what was refused.
  - `LaunchRefusal { error, step, code }` and `launchRefusalLine()`. The line names the step, the code and
    FormatMessageW's words, e.g. "The extension host could not be started: Windows refused to create its
    process (error 87, The parameter is incorrect.): the system refused."
- `Process_win32.cpp`:
  - The launch body is now `start()`, returning `LaunchRefusal` at every system call.
  - `launch()` maps it to `LaunchError` and logs each new (step, code) pair once, for the errors in
    `DetailedLaunchErrors`.
  - New factory `makeWin32ProcessLauncher(AppContainerApi, diagnostics)` in `process/Process_win32.hpp`.
    `makeSystemProcessLauncher()` calls it with Contour's error log.
- `AppContainer_win32`: `ensureAppContainer`, `ensurePackagesCanExecute` and `prepare` now return
  `LaunchRefusal`. The profile failure keeps its HRESULT.
- `AppContainer_test.cpp`:
  - `probeReportIn` and the wine branch launch through a recording launcher. A failed contained launch now
    prints the refusal line next to "launch: the system refused".
  - New cases: `LaunchRefusal.lineNamesTheStepAndTheCode` (Win32 decimal, HRESULT hex), and
    `LaunchRefusal.refusalALaunchErrorLosesIsSaidOnce` (a non-UTF-8 argument gives `Io` with step Spelling,
    code 1113, logged once over two launches; a missing executable, `NotFound`, logs nothing).
  - The three `.error() == LaunchError` checks now compare whole refusals where the step is known.
- Under wine the refusal line already works. The skip now reads "… -- The extension host could not be
  started: Windows refused to create or find its app container profile (error 0x80004001, Not implemented.)".

## 3–5. Round-3 minors

- `.gitattributes`: `cmake/patches/*.patch -text`, with a comment (folded into 598181d55).
- `cmake/ApplyPatchOnceTest.cmake`: a fourth case where the tree is its own git clone (`git init` + commit;
  user, email and gpgsign passed with `-c`). It must be patched, and left alone once patched (also 598181d55).
  The script runner is factored into `_apply`. It reports "4 states checked".
- The `_WIN32` halves of the out-of-memory classifier: `WasmtimeEngine.resourceErrorsAreOutOfMemory` and
  `WasmtimeEngine.primingErrorsAreReadForMemory` were cross-built and pass under wine (inside the
  `vtextension_wasmtime_test` run below). WAMR's Windows half (`WamrThreadEnv.cpp`) compiles in the cross build;
  it has no table test.

## Tests run

Linux, `clang-asan`:

| Run | Result |
|---|---|
| `vtextension_wasmtime_test` | 67 cases, 64 passed, 3 skipped (the signal-trap cases skip in instrumented builds) |
| `vtextension_process_test` | 140 cases, 139 passed, 1 skipped |
| `vtextension_remote_test "SharedGuestMemory.*"` | 4 cases, 40 assertions, all passed |
| ctest: `check_apply_patch_once_selftest` + both suites | 3 of 3 passed |

Linux, `clang-release`, uninstrumented, so the signal-trap cases run:

| Run | Result |
|---|---|
| `vtextension_wasmtime_test` | all passed, 67 cases, 1253 assertions |

Windows cross build:
- Tree `out/clangcl-debug` (clang-cl, lld-link, `/opt/msvc`; Wasmtime 48.0.10 fetched).
- `PATH` has `/opt/msvc/bin/x64`, and `LIB` points at a scratch `Pathcch.lib` alias (local only, as in 5.7).
- Targets: wasmtime_test, process_test, extension_host_probe, contour-extension-host(-hostile),
  wamr_helper_test, remote_wamr_test. Exit 0, no warnings.

wine 11, own prefix, `WINEDEBUG=-all`:

| Run | Result |
|---|---|
| `vtextension_wasmtime_test.exe` | 65 cases, 64 passed, 1 skipped (open-bytes trap, wine) |
| `vtextension_process_test.exe` | 118 cases, 115 passed, 3 skipped |
| `vtextension_process_test.exe "[appcontainer]"` | 4 cases, 3 passed, 1 skipped (contained probe, wine) |
| `vtextension_process_test.exe "LaunchRefusal.*,GuestMemoryMapping.*,SharedObject.*"` | 18 cases, 16 passed, 2 skipped |
| `vtextension_remote_wamr_test.exe` | all passed, 53 cases, 627 assertions |
| `vtextension_wamr_helper_test.exe "~[.measure]"` | 49 cases, 48 passed, 1 skipped |
| `vtextension_wamr_helper_test.exe "[appcontainer]"` | 2 cases, 26 assertions, all passed |

clang-format 22 (`/usr/bin/clang-format`) and editorconfig-checker are clean on every changed file. Only this
lane's wineserver was killed (`WINEPREFIX=… wineserver -k`).

## What only real Windows can prove

- That the open-bytes store now traps. Wine cannot show it: wine does not fault on reserved view pages.
- That `closedPagesAreThoseNoViewCommitted` holds, i.e. that the commit is cross-view.
- The actual step and code of the contained launch's refusal. The next CI run's
  `Sandbox.windowsHelperRunsContained` failure will print it beside "launch: the system refused", and the
  wamr helper's `[appcontainer]` case will log it to Contour's error log. Whether it then passes depends on
  what it names (the `DETACHED_PROCESS` theory above, or another).

## Deviations and concerns

- The brief's premise that "wine passes both" is wrong for test 1. Wine fails it at base too, for an unrelated
  wine defect, hence the wine skip.
- Test 2 is diagnosed, not fixed. Without a Windows box the refusal cannot be named; changing the flags
  speculatively would have been a guess shipped into the sandbox's launch path.
- The rebase hit two conflicts in `Process_win32.cpp`:
  - At fb93a94ed, the factory call had to use that commit's `errorLogWriter()`.
  - At 8484a0694, I took the final version.
  
  The intermediate commit fb93a94ed was not built on its own; only the final tree was built and tested.
- The owning commits' messages were not reworded to mention the refusal logging or the test's commit model.
- Other tests (`HostServer_test`, `HandleGrant_test`, `SandboxProbe_test`) still commit whole objects through
  the parent. None of them relies on a closed page trapping, so I left them.
