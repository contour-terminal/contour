# 5.8 ci3: fix round 1 report

**Base:** 2f64305cd. **Head:** c2d8d12e5. Not pushed.

## Commits

Every fix is folded into the commit that introduced the code (fixup, then autosquash). The tree after the
autosquash is identical to the fixup head.

| Commit | Change | Items folded in |
|---|---|---|
| `2ff29f0a2` (was 3ef95a025) | fix(vtextension): a contained extension host's environment names its container's own folders | 1, 2, 4, 5; its message is reworded for the run-time lookup |
| `1bf54e25c` (was 3d281cf73) | the ARM fix | none; content unchanged |
| `c2d8d12e5` (was 2f64305cd) | feat(vtextension): an extension host the loader ends is logged with why | 3 |

`1bf54e25c` does not refer to anything `c2d8d12e5` adds, so each commit builds on its own.

## 1. LNK4199: delay-loading replaced by a run-time lookup (Important)

The review offered two fixes: a `/DELAYLOAD` on each target, or another way that both link.exe and lld-link
accept without a warning. I took another way.

### The change

**The system implementation of `AppContainerApi`** (`systemAppContainerApi()`, `AppContainer_win32.cpp`)
looks up its few non-advapi32 functions at run time. The lookup happens once, the first time a launcher
prepares a container. `ContainerFunctions` holds:

- `CreateAppContainerProfile`, `DeriveAppContainerSidFromAppContainerName` and `GetAppContainerFolderPath`,
  from `userenv.dll`;
- `CoTaskMemFree`, from `combase.dll`, which is CoTaskMemFree's real home; ole32 forwards to it.

Details:

- Both DLLs are loaded with `LoadLibraryExW(…, LOAD_LIBRARY_SEARCH_SYSTEM32)`.
- The function types are `decltype(&::Fn)`. `decltype` is unevaluated, so nothing is imported.
- A function that is not found makes its API entry return `HRESULT_FROM_WIN32(ERROR_PROC_NOT_FOUND)`. That
  flows into the existing refusals: `Profile` and `ContainerFolder`.
- The `TaskMemory` deleter calls the looked-up `CoTaskMemFree`. clang-analyzer flagged a possible null call
  there, so the deleter checks for null. Only `folderPath` allocates, and only after both functions were
  found.

**CMake:** the `userenv`/`ole32` link, the `delayimp` INTERFACE link and the INTERFACE `/DELAYLOAD` options are
all removed from `vtextension_remote_process`. A comment says why no library is linked.

### Why this and not a per-target function

- **No linker option exists, so LNK4199 cannot occur** on any target, with either linker.
- **No target list to keep in sync.** A per-target `/DELAYLOAD` has to name exactly the executables that pull
  `AppContainer_win32.obj` from the static library. In the cross tree those are the probe,
  `vtextension_process_test` and `vtextension_wamr_helper_test`. On CI they also include contour.exe, and any
  test that reaches `makeSystemProcessLauncher()`. Every new executable would have to be added to that list.
- **It fits the existing seam.** The run-time lookup lives behind the injected `AppContainerApi`, so tests
  keep faking it. It uses the same idiom as `GuestMemoryMapping_win32.cpp` (`VirtualAlloc2`) and
  `ConPty.cpp`.

### How I verified that no LNK4199 can arise

lld-link stays silent on an unused `/DELAYLOAD`, so the absence of a warning there proves nothing. Instead I
looked at what the binaries import.

- **Before:** `llvm-readobj --coff-imports` on every executable in the cross tree. Only
  `extension_host_probe.exe`, `vtextension_process_test.exe` and `vtextension_wamr_helper_test.exe` had
  USERENV/ole32 delay imports. `contour-extension-host.exe`, `-hostile`, `vtextension_remote_test`,
  `vtextension_wasmtime_test` and others had none, yet received the INTERFACE `/DELAYLOAD`. Those are exactly
  the targets where link.exe warns.
- **After:** no executable imports userenv or ole32 at all, neither statically nor delay-loaded. Examples:
  - `extension_host_probe.exe`: WS2_32, ADVAPI32, KERNEL32 and the C/C++ runtimes.
  - `vtextension_process_test.exe` and `vtextension_wamr_helper_test.exe`: the same, without WS2_32 for the
    latter.
  - `contour-extension-host.exe`: unchanged.
- **Conclusion:** with no `/DELAYLOAD` option and no reference to those libraries' symbols left, link.exe has
  nothing to warn about.

## 2. The startup-import guard moved (minor)

- It is now `process/StartupImports_test.cpp`: the PE parser, the `WindowStationLibraries` table, and
  `StartupImports.containedExecutablesLoadNoWindowStation`.
- It is compiled on WIN32 into **both** helper suites:
  - `vtextension_helper_test` checks `contour-extension-host` (the Wasmtime build) and the probe.
  - `vtextension_wamr_helper_test` checks its own host and the probe. Its host is `contour-extension-host-wamr`
    where that twin is built, or `contour-extension-host` where the build runs WAMR. It now depends on
    `extension_host_probe` and gets `VTEXTENSION_PROBE_PATH` on WIN32.
- `vtextension_process_test` loses its dependency on the host and the `VTEXTENSION_HELPER_PATH` define, so it
  stays engine-free.

**Correction (round 2):** an earlier version of this report said the guard does not run on Windows CI. That
was wrong.

- The `msvc-ci` test preset has `stopOnFailure: true`. Run 785859 stopped at test 53 of 79, after
  `vtextension_process_test` failed. That is why `vtextension_wamr_helper_test` was missing from its log.
- The workflow's `[appcontainer]` verification step runs `vtextension_wamr_helper_test`, which includes the
  guard's `[appcontainer]` case.

So the guard does run on Windows CI, in that suite.

## 3. `LostExitStatus` has its own text (minor)

- `exitStatusText()` returns "its exit status was lost" for `LostExitStatus`, before any number formatting.
- The table test has a row for it.

## 4. `Row::same` replaced by an enum class (minor)

- `enum class PathsAre : uint8_t { Different, Same }`, with the negative case at zero.
- The rows say `.expected = PathsAre::Same` or `PathsAre::Different`.

## 5. The probe names its window station (diagnostic)

- `windowStationOf(container)` (`LaunchProbe.cpp`) runs **only uncontained**; contained it returns
  `not-asked`. It loads user32 at run time (`LoadLibraryExW`, System32 only) and calls
  `GetProcessWindowStation` and `GetUserObjectInformationW(UOI_NAME)` through `GetProcAddress`. user32 never
  enters the probe's import table, and the guard still passes.
- The report gains a `window-station=` line.
- `Sandbox.windowsHelperRunsContained` REQUIREs the line in the uncontained run and `WARN`s it, so it is
  printed on every run, pass or fail. Under wine it printed `WinSta0`. On the GitHub runner it should print
  `Service-0x…$`, which would confirm the premise.

## Tests

- **Windows cross build:** `vtextension_process_test`, `extension_host_probe`, `vtextension_wamr_helper_test`
  and `contour-extension-host`. Exit 0, no warnings. `vtextension_helper_test` is not configured in this tree,
  because its runtime is WAMR.
- **Under wine:**

  | Run | Result |
  |---|---|
  | `vtextension_process_test` | 126 cases, 123 passed, 3 skipped, 1068 assertions |
  | `[appcontainer]` | 4 cases, 3 passed, 1 skipped; the WARN printed `WinSta0` |
  | `[appcontainer],AppContainer.*` | 6 cases, 5 passed, 1 skipped |
  | `vtextension_wamr_helper_test "~[.measure]"` | 50 cases, 49 passed, 1 skipped |
  | `vtextension_wamr_helper_test "StartupImports.*"` | 96 assertions, all passed |

- **Linux ASan:** the vtextension suites build warning-free. `ctest -R "vtextension|apply_patch_once"` passed
  8 of 8, and `-L lint` passed 35 of 35.
- **clang-tidy diff:** clean on both trees (Linux 4 TUs, Windows 6 TUs), after the deleter's null check.
- **Format:** clang-format 22 and editorconfig-checker are clean.

## Concerns

- **combase.dll** is loaded only in the parent, on its first container launch. It is the documented home of
  `CoTaskMemFree` on Windows 8 and later, the floor for AppContainers.
- **The 0xC0000142 cause is still inferred.** The next Windows CI log should show the runner's window station
  name in the uncontained run.
