# 5.8 CI round 2: review round 2 report

**Base:** 785859fbf. **New head:** ee4d5ba28. Nothing is pushed. The branch was rebased past the pushed
head, so it needs a force-push.

All of round 2 is folded into the contained-environment commit, now **b5a7c0acd** (was df749af5a). Its
commit message is updated for the empty-folder refusal and the exact-value check. The tree after the
autosquash is byte-identical to the fixup head. The ARM fix commit above it is unchanged in content (now
ee4d5ba28).

## 1. Exact folder values, not a prefix test (Important)

**Probe** (`testing/LaunchProbe.cpp`):
- These are removed: its own `GetAppContainerFolderPath` call (`ownContainerFolder`, which read
  TokenAppContainerSid), the `liesUnder` prefix test and the `folder-NAME=` verdict lines.
- For each `ContainerVariables` row, it now reports only `value-NAME=<value>`, which is empty if the
  variable is not set.
- The probe no longer includes `AppContainer_win32.hpp`.

**Test** (`AppContainer_test.cpp`):
- `expectedContainerFolder()` computes the folder in the parent, apart from the launcher's provision:
  `systemAppContainerApi().deriveSid(AppContainerName)`, which is
  DeriveAppContainerSidFromAppContainerName, then `folderPath`. It REQUIREs a non-empty folder.
- `checkContainerVariables(report, folder)` CHECKs each `value-NAME` for exact string equality with
  `folder / row.subfolder`, or with `folder` itself where the subfolder is empty, spelled in UTF-8.
  - Contained: the expected folder.
  - Uncontained: `std::nullopt`, so every value must be empty.
- Exact equality already implies that no value is the parent's own LOCALAPPDATA, TEMP, TMP or USERPROFILE.
  The round-1 inequality checks (`isSamePath`, `core::LiveEnvironment`) were therefore removed as
  redundant.

## 2. Minor 2 (the probe's own folder lookup)

Closed by item 1: that code is gone.

## 3. Minor 3: CoTaskMemFree ownership (`AppContainer_win32.cpp`)

- A new `TaskMemory` type (a `unique_ptr` with a `TaskMemoryFree` deleter) owns the folder string.
- It takes ownership straight after `GetAppContainerFolderPath` returns, before the failure check, and
  before the path is built.
- If the call succeeds but leaves a null pointer, the result is an empty path, which the provision then
  refuses (item 5).

## 4. Minor 4: `alignas` on the token buffer

Not applicable: item 1 removed that code.

## 5. Minor 5: an empty folder is refused at `ContainerFolder`

- `AppContainerProvision::prepare` chains `.and_then` on `folderPath`. A folder named empty becomes
  `HRESULT_FROM_WIN32(ERROR_PATH_NOT_FOUND)`, refused as `{Io, LaunchStep::ContainerFolder, that HRESULT}`.
  The refusal table logs it like any other at that step.
- The doc comment of `prepare` says so.
- New test section in `AppContainer.profileIsEnsuredOnce`: "its folder named empty: refused alike".

## Verification

**Windows cross build** (`out/clangcl-debug`): built `vtextension_process_test`, `extension_host_probe`,
`vtextension_wamr_helper_test` and `contour-extension-host`. Exit 0, no warnings.

The first build failed on `-Wunused-function`: one call site had not been switched to the new expected
folder. The wine run that followed it used the stale binary, which still printed `folder-*`, and failed.
I fixed the call site and rebuilt; every run below is on the fixed build.

**Under wine 11** (own prefix):

| Run | Result |
|---|---|
| `vtextension_process_test "[appcontainer]"` | 4 cases, 3 passed, 1 skipped (contained half, as wine stubs AppContainers), 76 assertions |
| `vtextension_process_test` | 124 cases, 121 passed, 3 skipped, 1051 assertions |
| `vtextension_wamr_helper_test "~[.measure]"` | 49 cases, 48 passed, 1 skipped |

The uncontained exact-empty checks ran under wine and passed. The contained exact-folder checks run only on
real Windows.

**Linux ASan:** the build is warning-free.
- `ctest -R "vtextension|apply_patch_once"`: 8 of 8 passed.
- `ctest -L lint`: 35 of 35 passed.

**clang-tidy diff:** clean on both trees (Windows 4 TUs, with `readability-duplicate-include` off as
before; Linux 2 TUs).

**Format:** clang-format 22 (`/usr/bin/clang-format`) `--dry-run --Werror` and editorconfig-checker are
clean on every changed file.

## What only real Windows can prove

- That the contained probe's three values equal the folder the parent derives, exactly.
- That `DeriveAppContainerSidFromAppContainerName` and `GetAppContainerFolderPath` work for the profile the
  launch created.

## Concerns

- If Windows' own reroute rewrites these variables differently from what we set (a doubled path, for
  example), the exact CHECK fails on the next Windows run and names the value. That is the intended signal,
  not a flake.
- The branch needs a force-push.
