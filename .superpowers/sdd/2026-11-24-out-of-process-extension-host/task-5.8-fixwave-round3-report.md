# Task 5.8 fix wave — review round 3 report

Base `790ce3259` → **head `9aefa2abe`** (local, not pushed; base `origin/master` = `8c3afaa0c`). Both changes are
folded into their owning commits. The autosquash rebase ran clean, and the tree it produced equals the tested one.

- Item 1 → `a5f1295dd` fix(vtextension): unlink a WAMR execution environment under its thread cluster's lock
- Items 2 and 3 → `9aefa2abe` fix(vtextension): a host short of memory as it starts is retried, and a lacking
  sandbox is a warning

## 1. Patch applied through `git apply` (Critical)

`cmake/ApplyPatchOnce.cmake` now runs `git apply` and never `patch`.

1. `git apply --check --reverse` succeeds → the patch is already applied: say "Already applied" and do nothing.
2. Otherwise `git apply --check` succeeds → apply it, and say "Applied".
3. Otherwise the tree holds part of the patch, or something else → stop with a FATAL_ERROR naming the patch and the
   tree.

`git apply` asks nothing, so a run without a terminal answers nothing. To keep `git apply` from treating an
enclosing repository's root as the root of the patch's paths, git runs with `GIT_CEILING_DIRECTORIES` set to the
tree's parent. That situation arises in a build directory inside a checkout, for a tree that is not its own
repository. CPM's WAMR clone is its own repository either way.

The WAMR fetch now finds git with `find_package(Git REQUIRED)` (CPM clones with git already) and passes
`GIT_EXECUTABLE`. The patch-program search, including the Git-for-Windows `patch.exe` fallback, is gone.

Tests:

- New `cmake/ApplyPatchOnceTest.cmake`, registered as the lint gate `check_apply_patch_once_selftest` (label
  `lint`, since `CONTOUR_LINT_GATES` gets the label). It builds a scratch file and a two-hunk patch, and runs the
  script with stdin from the null device:
  - **unapplied** → exit 0, both hunks in;
  - **applied** → exit 0, file unchanged, "Already applied" in the output;
  - **half-applied** (first hunk only) → non-zero exit, file unchanged.
- The self-test fails when it should: changing one expectation made it exit 1.
- Under ctest its work directory lies inside this checkout's build tree, so the ceiling is exercised too.
- Run directly in `/tmp`: `rc=0`.
- Run on the real, already patched ASan `_deps/wamr-src` with `</dev/null`: "Already applied", `rc=0`.
- The ASan reconfigure re-ran the patch step, because the command had changed (stamp rewritten). It exited 0, and the
  tree holds the patch once.

## 2. Error numbers per system (Minor)

`ResourceErrorMarkers` is now two tables chosen by `#ifdef _WIN32`:

| System | Markers |
|---|---|
| POSIX | "Cannot allocate memory", `os error 12)`, "out of memory" |
| Windows | the texts of `ERROR_NOT_ENOUGH_MEMORY`, `ERROR_OUTOFMEMORY` and `ERROR_COMMITMENT_LIMIT`, `os error 8)`, `os error 14)`, `os error 1455)`, "out of memory" |

The classifier test now checks each system's own numbers. On POSIX, "(os error 8)" (ENOEXEC) and "(os error 14)"
(EFAULT) stay `RuntimeUnavailable`. On Windows, "(os error 12)" (`ERROR_INVALID_ACCESS`) stays
`RuntimeUnavailable`. The header doc now says "this system's" markers.

## 3. Wiring

The step between a Wasmtime error and the classifier is now `detail::extensionErrorOf(wasmtime_error const*)`,
declared in `WasmtimeEngine.hpp` with a forward declaration, so the header does not pull in `wasmtime.h`.
`compilePriming` and `runPriming` map every `wasmtime_error_t` through it.

New test `WasmtimeEngine.primingErrorsAreReadForMemory`:

- a real error made with `wasmtime_error_new` and carrying this system's out-of-memory message → `OutOfMemory`;
- a parse error → `RuntimeUnavailable`;
- null → `RuntimeUnavailable`.

**Gap, for the PR's risk section:** nothing drives a real priming failure through `compilePriming` or `runPriming`
themselves. Wasmtime cannot be made to fail an allocation there without an injection seam into the engine. The calls
are one line each. The rest of the path, from `prepareThread()` returning `OutOfMemory` to the transient
`OutOfResources` refusal, is covered end to end by `RemoteBackend.hostShortOfMemoryAsItPrimesIsRetried`.

## Verification

| Check | Result |
|---|---|
| ASan build, 11 vtextension targets | exit 0, 0 warnings |
| ASan suites (`~[.measure]`) | vtextension 486/26719; wasmtime 67 (64 + 3 skip)/1201; process 140 (139 + 1 skip)/5582; remote 302/12382; remote_wamr 55 (54 + 1 skip)/655; helper 161 (160 + 1 skip)/3916; wamr_helper 62/1237 |
| `ctest --preset=clang-asan -R 'vtextension\|wasm_runtime_containment'` | 9/9 |
| `ctest -L lint` | **35/35** (34 + `check_apply_patch_once_selftest`) |
| patch states (unapplied / applied / half-applied), no terminal | pass / pass / refused |
| `clang-tidy-diff.sh --base fw-pre` | 75 TUs clean |
| clang-format 22 `--dry-run -Werror` / editorconfig-checker | clean / clean |

## Concerns

- I did not run the BSD hosts, macOS or FreeBSD, where the old script misbehaved. The fix removes `patch` entirely,
  so nothing there depends on BSD `patch` any more.
- The real-allocation gap described in item 3.
- Commits are still not built one at a time (deferred item 2).
