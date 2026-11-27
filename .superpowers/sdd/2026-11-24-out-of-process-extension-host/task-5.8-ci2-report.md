# 5.8 CI round 2: report

Base 8484a0694. New head **b5732de14**. Not pushed. The two review minors were folded into their owning
commits by fixup and autosquash. The tree after the rebase is byte-identical to the tree before it. Two new
commits sit on top:

- `7842a084d fix(vtextension): a contained extension host is given the variables Windows points into its container`
- `b5732de14 fix(vtextension): an instance refused for its answer's size is gone before the refusal is sent`
- Folded into `77695a0c7` (was 598181d55): the ApplyPatchOnceTest fix and the shared wine check.
- Folded into `8e84e41b5` (was 67239ad27): makeJob's error capture and the `_diagnostics` doc comment.

## 1. Windows contained launch, ERROR_ENVVAR_NOT_FOUND (203)

### Root cause

The environment block encoder was **not** malformed:

- An empty block was already `L"\0\0"`.
- Every entry was NUL-terminated, and the block had a final NUL.
- `CREATE_UNICODE_ENVIRONMENT` was set.
- Contained and uncontained launches used the same encoder, through one call site.

The evidence is in the CI log. The same probe started **uncontained**, with the same block, and it ran: it
reported `spawn=refused:CreateProcessW:1816`. Only the contained launch failed with 203. So the block is
well-formed, and the failure is specific to the AppContainer.

Microsoft's "Launch an AppContainer" (learn.microsoft.com/windows/win32/secauthz/implementing-an-appcontainer)
says that CreateProcess reroutes **LOCALAPPDATA, TEMP and TMP** into the container's profile folder
(`…\Packages\<moniker>\AC`, `…\AC\Temp`). Its sample passes a NULL environment, so it inherits a block that
holds all three. Ours held `SystemRoot` alone (`defaultHelperEnvironment()`). The system's reroute looks
those variables up in the block it was given, and fails with STATUS_VARIABLE_NOT_FOUND, which reaches the
caller as ERROR_ENVVAR_NOT_FOUND (203). ERROR_ENVVAR_NOT_FOUND has no other plausible source in a
CreateProcessW that succeeds uncontained with the same block.

The exact kernelbase lookup is inferred, not documented. What is documented is that these three are
rerouted. What is measured is that the block works uncontained and fails contained.

### SystemRoot

SystemRoot was already passed. `defaultHelperEnvironment()` has always returned it, because without it a
process cannot load system DLLs or initialize Winsock. Nothing about that changes.

### Fix (7842a084d)

- **New `process/LaunchSpelling_win32.{hpp,cpp}`.** It is pure and Windows-only, and holds `wideOf`,
  `appendArgument` and `environmentBlock`, which moved out of the anonymous namespace in Process_win32.cpp.
  It also holds `ContainerRedirectedVariables = {LOCALAPPDATA, TEMP, TMP}`, a data table, and
  `containedEnvironment(environment, lookup)`. That function adds each redirected variable the requested
  environment lacks (names compared ignoring case), using this process's value through an injected lookup.
  The system then replaces that value with the container's folder.
- **The block's sort order** is now `CompareStringOrdinal(…, TRUE)`: ordinal, ignoring case, locale-free,
  as Windows documents for environment blocks. It replaces `towupper`, which depends on the CRT locale.
- **`Process_win32.cpp`:**
  - A contained launch encodes `containedEnvironment(spec.environment, processVariable)`. An uncontained
    launch encodes `spec.environment` as before.
  - Both encode through the single `environmentBlock` call.
  - `processVariable` reads any variable through GetEnvironmentVariableW, with a size query, so it is not
    capped at MAX_PATH. `defaultHelperEnvironment()` now uses it too.
  - The file-head comment, the `ProcessIsolation::AppContainer` doc and the file list in CMakeLists.txt
    are updated.
- **Why the parent's values, not the container folder path.** If the system computes the container path
  from LOCALAPPDATA in the block, pre-setting the container path would double it. Passing the parent's
  values is exactly what the documented NULL-environment sample hands it.

### Tests

New `process/LaunchSpelling_test.cpp`, Windows only, in `vtextension_process_test`. It has 6 cases, 22
assertions:

- An empty environment gives `L"\0\0"`.
- A table of rows:
  - one entry;
  - unsorted entries sorted ignoring case;
  - ordinal-on-uppercase order (`a` < `_X`);
  - an empty value;
  - UTF-8 spelled as UTF-16.
- Non-UTF-8 input is refused.
- `containedEnvironment` keeps a `temp` that is already present, adds LOCALAPPDATA, and leaves out TMP
  when the lookup has none.
- The whole contained block for SystemRoot plus the three variables, sorted.
- `appendArgument` quoting rows.

## 2. ARM: `HostServer.callablesPastTheCapFailTheInstantiation`

### Root cause: a race in the product, not ABI or `char`

In `serve(Instantiate)`, the instance went into `_instances` before the answer (correct: live before it is
announced). The over-cap path was wrong:

1. `sendOrRefuse` sent the **refusal** first.
2. A `.transform` then interrupted the instance and erased it.

The test's parent receives the refusal and at once sends `Call{instanceId=1}` on lane 0. If lane 0's
executor thread looks the instance up before the control thread erases it, it finds the instance and runs
the call. There is no violation, the server keeps serving, and `exitWithinDeadline()` returns `nullopt`.
Catch prints that `optional` as `{?}`, which is the `{?} == 0` in the log.

This was not a timing deadline: the case takes 15 ms on x86_64.

`char` signedness was checked: the codec, transport and HostServer use no plain `char`.

The case `releaseDropsTheInstance` avoids the same window on purpose, with a synchronizing `createRuntime(2)`.

### Evidence

No aarch64 toolchain or qemu-user is installed here, and nothing was installed. Instead:

- Against the **old** code, I injected `sleep_for(100ms)` between sending the refusal and erasing the
  instance. On x86_64 ASan the case then fails exactly as on ARM: `CHECK( parent.exitWithinDeadline() ==
  ServerExit::ProtocolViolation )`, `{?} == 0`.
- With the fix and the same 100 ms delay placed after the refusal is sent, it passes.
- Both experiments were reverted. Neither is in any commit.

### Fix (b5732de14)

- `sendOrRefuse` gains an overload taking a `withdraw` invocable. It runs only on the over-cap path, before
  the refusal is written. The three-argument form forwards with a no-op.
- The Instantiate path passes the interrupt-and-erase as `withdraw`, so a parent acting on the refusal finds
  nothing.
- The Load and MemoryRead callers are unchanged. Load's state is control-thread-only, and MemoryRead
  publishes nothing.

### Test

The existing case is now correct by construction. There is no seam to force the interleaving
deterministically without a test hook in the product, so I added no new case. The delay experiment above is
the proof.

## 3. Review minors

**a. `cmake/ApplyPatchOnceTest.cmake`**

- Now `if(ARGC GREATER 3 AND ARGV3 STREQUAL "CLONE")`.
- The clone case sets `core.autocrlf true`, then deletes the file and checks it out again, so the tree has
  CRLF line ends as on a Windows runner. It then checks:
  - the patch still applies;
  - the content matches;
  - the CRLF line ends are kept (read as HEX, because `file(READ)` as text drops the `\r`).
- The git steps became a small `_git()` helper.
- **Deviation:** I used repo config, not `-c core.autocrlf=true` on the clone commands. A `-c` on the
  clone's git commands would not reach the `git apply` that ApplyPatchOnce.cmake runs inside the clone;
  repo config does.
- Verified locally: `git apply` on the CRLF tree succeeds and keeps CRLF.

**b. Wine checks.** The five copies are now one header-only
`src/vtextension/wasm/testing/Wine.hpp`, `vtextension::wasm::testing::runsOnWine()`. Header-only, so no
link-graph constraint. The five were:

- `WasmtimeEngine_test`
- `SharedObject_test`
- `GuestMemoryMapping_test`
- `ProcessLauncher_test`'s inline check
- `process::testing::runsOnWine` in LaunchedHelper; its declaration and definition are removed

Callers updated: AppContainer_test, WamrHelper_test, HelperComposition.cpp, LaunchedHelper.cpp.

**c. makeJob.** It now returns `std::expected<OwnedHandle, LaunchRefusal>` and captures GetLastError()
before the local job handle closes, on both failure paths. The caller returns `job.error()`.

The other two lines the review named are already correct:

- `copyOf` (old line 248) closes nothing between the failure and the caller's GetLastError().
- AttributeList::create's failure only frees heap memory.

I left both unchanged.

**d. `_diagnostics`** has a doc comment.

## Verification

- **Linux ASan:** the vtextension suites build warning-free, and `ctest -R "vtextension|apply_patch_once"`
  passes 8 of 8.
  - `vtextension_remote_test`: 302 cases, 12382 assertions, all passed.
  - `vtextension_process_test`: 140 cases, 139 passed, 1 skipped.
  - `ctest -L lint`: 35 of 35 passed.
- **Windows cross build** (`out/clangcl-debug`, clang-cl and lld-link, /opt/msvc): built
  vtextension_process_test, extension_host_probe, vtextension_wasmtime_test, vtextension_remote_wamr_test,
  vtextension_wamr_helper_test and contour-extension-host. Exit 0, no warnings.
- **Under wine 11** (own prefix, `WINEDEBUG=-all`):

  | Run | Result |
  |---|---|
  | `vtextension_process_test "LaunchSpelling.*"` | 6 cases, 22 assertions, passed |
  | `vtextension_process_test` | 124 cases, 121 passed, 3 skipped |
  | `vtextension_wasmtime_test` | 65 cases, 64 passed, 1 skipped |
  | `vtextension_remote_wamr_test` | 53 cases, 627 assertions, passed |
  | `vtextension_wamr_helper_test "~[.measure]"` | 49 cases, 48 passed, 1 skipped |

- **clang-tidy diff** (`scripts/clang-tidy-diff.sh --base HEAD`):
  - Linux tree: 10 TUs clean.
  - Windows tree: 13 TUs clean.
  - In the cross tree, `readability-duplicate-include` fires on unchanged headers in every TU (for example
    ExtensionError.hpp and WasmRuntime.hpp in an untouched AppContainer_win32.cpp). It is a false positive
    of the clang-cl cross setup, so I ran with that one check off.
- **Format:** `/usr/bin/clang-format` 22.1.8 `--dry-run --Werror` is clean on every changed C++ file.
  editorconfig-checker is clean.

## What only real Windows and real aarch64 can prove

- **Windows:** that a contained launch with LOCALAPPDATA, TEMP and TMP now starts. Wine accepted the old
  block and stubs AppContainers, so the encoder test proves only the block's shape. If the system needs a
  further variable (USERPROFILE, say), the next CI run will show 203 again. The refusal line names the step
  and code, and adding a row to `ContainerRedirectedVariables` is the whole fix. Real Windows must also show
  that the reroute replaces the parent's values: the probe should see `…\Packages\<moniker>\AC` paths, not
  the user's.
- **aarch64:** that `callablesPastTheCapFailTheInstantiation` passes. The mechanism is reproduced on x86_64
  by the injected delay, and the fix closes it by ordering, not by timing.

## Concerns

- The 203 root cause is strongly supported (same block, uncontained works; the docs name the three
  rerouted variables) but not observed on a real kernel. It needs the next Windows CI run.
- I folded into commits 2 and 4 back only. The two product fixes are new commits on top, because their
  owners (fb93a94ed and 03147b69e) lie 11 and 131 commits back under many later edits of the same files.
