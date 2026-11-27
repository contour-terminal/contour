# 5.8 CI round 3: report

**Base:** ee4d5ba28. **Head:** 2f64305cd. Not pushed.

## Commits

| Commit | What |
|---|---|
| `3ef95a025` (was b5a7c0acd) | fix(vtextension): a contained extension host's environment names its container's own folders |
| `3d281cf73` (was ee4d5ba28, content unchanged) | fix(vtextension): an instance refused for its answer's size is gone before the refusal is sent |
| `2f64305cd` (new) | feat(vtextension): an extension host the loader ends is logged with why |

`3ef95a025` now also holds the delay-load fix, the startup-imports guard, and review items 2 and 3. Its message
gains a paragraph on the delay-load.

The tree after the autosquash is identical to the tree before it. No commit before `2f64305cd` refers to
`exitStatusText` or `KnownExitCodes`, so every commit builds on its own.

## Item 1: 0xC0000142 (STATUS_DLL_INIT_FAILED)

### Root cause

**The import tables.** `llvm-readobj --coff-imports` on the cross-built binaries gave:

| Binary | Static imports |
|---|---|
| `extension_host_probe.exe` | ADVAPI32, KERNEL32, MSVCP140, VCRUNTIME140(_1), ucrtbase, WS2_32, **USERENV**, **ole32** |
| `contour-extension-host.exe` (production) | ADVAPI32, api-ms-win-core-path, bcrypt, KERNEL32, ntdll, WS2_32, the C++ and C runtimes; **no ole32, userenv or user32** |

**The probe gained ole32 in this branch.** Round 1 (ci2) added `ole32` to `vtextension_remote_process` for
the `CoTaskMemFree()` that frees `GetAppContainerFolderPath()`'s folder. The probe links that library because
its `--parent` mode uses the system launcher. So every executable that links the launcher imports ole32 at
startup, including the probe when it is started *contained*.

**This was the first contained start.** Before that change, the contained probe never got past
CreateProcessW: error 203 in runs 8484 and c130. The run that first started it (785859) is the run where it
died in the loader. No older Windows log shows a contained start.

**ole32 loads user32.** ole32 (OLE, its window-based STA message loop, clipboard, drag and drop) imports
user32. user32's process initialisation connects the process to its window station and desktop.

**The CI runner's window station admits no app container.** Microsoft Learn, "Window Station and Desktop
Creation", says that a non-interactive service's window station (`Service-0x…$\default`) has a DACL granting
only the service's own account. The GitHub runner runs jobs as a service.

An AppContainer token passes an access check only if the DACL also admits its package SID or ALL APPLICATION
PACKAGES, which that DACL does not. user32's initialisation therefore fails, and the loader ends the process
with STATUS_DLL_INIT_FAILED (0xC0000142) before main. It is not 0xC0000135 or 0xC0000022: the DLL was found
and readable, and its initialisation failed.

**Other hypotheses, ruled out:**

- *DLLs from the build directory.* An unreadable DLL gives 0xC0000135 or 0xC0000022, not 0xC0000142. And the
  probe's non-system imports are the C runtime, which the uncontained run loads from the same place.
- *The environment block.* None of the probe's static imports reads a variable at initialisation. user32 and
  ole32 were the only DLLs with an initialisation that can fail this way.

### Fix (in `3ef95a025`)

`vtextension_remote_process` links `userenv` and `ole32` as before, and now also declares for its consumers:

```cmake
target_link_libraries(vtextension_remote_process INTERFACE delayimp)
target_link_options(vtextension_remote_process INTERFACE /DELAYLOAD:userenv.dll /DELAYLOAD:ole32.dll)
```

After a rebuild the probe imports ADVAPI32, KERNEL32, the runtimes and WS2_32 at startup. USERENV and ole32
appear under DelayImport only. They load on the first call, which only a launcher preparing a container makes,
in the parent.

### Why removal rather than a desktop of its own

The brief offered a Chromium-style alternate desktop (and window station) granting the container SID. I
judged it not worth adding:

- user32 was reachable only through something removable: the launcher's own link dependencies.
- The production host never imports it.
- A desktop object per launch, extra LaunchSteps and RAII handles would add machinery to grant a contained
  process something it has no use for: the extension host draws nothing.

What keeps it that way is a test, not the desktop.

### Guard

New test `AppContainer.containedExecutablesLoadNoWindowStation`, Windows only, in `vtextension_process_test`:

- It parses the PE import directory of `contour-extension-host.exe` and `extension_host_probe.exe` from the
  file. These are startup imports; delay-loads are not counted.
- It CHECKs that `kernel32.dll` is present, which proves the parse worked.
- It CHECKs that no import is in the deny table `WindowStationLibraries`: user32, gdi32, ole32, oleaut32,
  shell32, shlwapi, comctl32.
- `vtextension_process_test` now depends on `contour-extension-host`. `VTEXTENSION_HELPER_PATH` is defined
  for it on WIN32.

**The guard has teeth.** With the `/DELAYLOAD` line removed and the probe relinked, the test FAILED on
`ole32.dll`. With the line restored it passed (96 assertions).

**Limitation:** the guard reads first-level imports only. A future system DLL that pulled user32 in
transitively would not be seen. The table names the known carriers.

### If it recurs: the message names the cause (`2f64305cd`)

`ProcessLauncher.hpp` gains a `KnownExitCodes` table:

- 0xC0000022: access denied
- 0xC0000135: DLL not found
- 0xC0000142: DLL init failed; "contained, usually one that connects to a window station the app container
  may not use"

It also gains `exitStatusText(ExitStatus)`:

- `signal N` for a signal;
- `exit code N` for a non-negative exit code;
- `exit code 0xXXXXXXXX[: reason]` for a negative one (an NTSTATUS).

POSIX exit codes are 0 to 255, so they never collide with the table.

Where it is used:

- **The supervisor's log line** now reads, for example, "An extension host ended unexpectedly (exit code
  0xC0000142: …)". It used to print `-1073741502`.
- **The tests' `endOf()`** (`LaunchedHelper.cpp`) uses it too, so the CI message now says why.

A new test, `ProcessLauncher.exitStatusSaysWhyWhereItKnows`, has rows for an exit code, a signal, an unknown
NTSTATUS and 0xC0000142.

No LaunchStep is added: the launch succeeds, and the failure is an exit. So the refusal table is unchanged.

### Does this affect interactive desktops?

Not for the probe, and not for the production host.

- **Interactive sessions.** WinSta0\Default grants ALL APPLICATION PACKAGES; UWP apps, which are
  AppContainers, use user32 there. Even the old probe would most likely have started.
- **Service and SSH sessions.** A Contour daemon run as a service, or a session started over OpenSSH, sits in
  a non-interactive service window station. There, any contained process loading user32 fails exactly like
  CI. Contour's own extension host imports nothing that loads user32, so it starts there too.
- **Keeping it so.** The new guard test holds both binaries to that.

## Item 2: tolerant path compare

`isSamePath(a, b)`:

- calls `CompareStringOrdinal(…, TRUE)` on the wide strings;
- ignores exactly one trailing `\` on either side;
- is exact otherwise. A path beneath the other is not equal, and neither is a doubled trailing backslash.

`checkContainerVariables` uses it to compare the probe's value with the expected folder.

New test `AppContainer.pathsCompareAsWindowsComparesThem`, with 8 rows:

- equal;
- case-folded;
- a trailing `\` on either side;
- a subfolder;
- `AC\AC`;
- a double trailing `\`;
- empty.

## Item 3: the value line must be present

`valueOf` now returns `std::optional<std::string>`, which is empty when the line is missing.
`checkContainerVariables` REQUIREs the `value-NAME=` line for every row before comparing; uncontained, it
CHECKs the value is empty.

The other callers compare `optional == "…"`, so a missing line fails them too. `connect` uses
`value_or("(no line)")`.

## Tests run

- **Windows cross build** (`out/clangcl-debug`, clang-cl, lld-link, /opt/msvc): built
  vtextension_process_test, extension_host_probe, vtextension_wamr_helper_test and contour-extension-host.
  Exit 0, no warnings. The import tables were checked as above.
- **Under wine 11** (own prefix):
  - The focused set (`[appcontainer]`, `AppContainer.*`, `ProcessLauncher.exitStatusSaysWhyWhereItKnows`,
    `LaunchSpelling.*`): 14 cases, 13 passed, 1 skipped (contained half: wine stubs AppContainers), 227
    assertions.
  - `vtextension_process_test`: 127 cases, 124 passed, 3 skipped, 1162 assertions.
  - `vtextension_wamr_helper_test "~[.measure]"`: 49 cases, 48 passed, 1 skipped.
  - The guard's negative experiment: FAILED on ole32 without `/DELAYLOAD`, passed with it.
- **Linux ASan:** the vtextension suites and the probe built warning-free.
  - `ctest -R "vtextension|apply_patch_once"`: 8 of 8 passed.
  - `ctest -L lint`: 35 of 35 passed.
- **clang-tidy diff:** clean (Linux 6 TUs; Windows 6 TUs, with `readability-duplicate-include` off as
  before).
- **Format:** clang-format 22 (`/usr/bin/clang-format`) `--dry-run --Werror` and editorconfig-checker are
  clean.

## What only real Windows (CI) can prove

That the contained probe now starts on the runner, reports, and that its three variables equal the container
folder. Wine stubs AppContainers and grants every window station, so it cannot reproduce 0xC0000142.

## Concerns

- **The cause is inferred, not observed.** It rests on the documented service window-station DACL, the import
  tables, and the timing (ole32 arrived in the same change that first let the probe start). If CI still shows
  0xC0000142, the log now names it. The next suspect would then be a transitive user32 load through a DLL
  outside the deny table. userenv is already delay-loaded, so it is excluded. The fallback would be the
  desktop-per-launch design.
- **The guard's scope.** It covers first-level imports of the two binaries only.
- **`delayimp`.** It is linked through an INTERFACE dependency under `if(WIN32)`. This works with MSVC
  link.exe and lld-link. A MinGW toolchain would need a different spelling, but none is configured.
