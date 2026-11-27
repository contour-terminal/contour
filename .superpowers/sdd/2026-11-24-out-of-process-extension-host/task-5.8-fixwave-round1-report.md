# Task 5.8 fix wave — review round 1 report

Base `75eb4d4a7` → **head `4491235ab`** (local, not pushed; base `origin/master` = `8c3afaa0c`). Autosquash ran
clean; the tree after it equals the tree tested below (`git diff` empty).

Tail of the branch now:

| Commit | |
|---|---|
| `dbde5a7f7` ci: run the arm legs on pull requests labelled for them | item 8 rewrap folded in |
| `1ffc38d3f` fix(vtextension): lock WAMR's process-wide state around every call that changes it | (comment pointing at the new test, in the next commit) |
| `99c905bb6` feat(vtextension): Contour says what an extension host's confinement failed with, and went without | |
| `cc810ccb7` feat(vtextension): the extension host on FreeBSD maps no memory both writable and executable | |
| `a39001183` **fix(vtextension): unlink a WAMR execution environment under its thread cluster's lock** | item 1 (new) |
| `4491235ab` **fix(vtextension): a host short of memory as it starts is retried, and a lacking sandbox is a warning** | items 3, 5, 6 (new) |

`75eb4d4a7` (primingDone) is gone (item 2). Items 4 and 7 are folded into the AppContainer launch commit and
"locks down in a fixed order" respectively.

## 1. WAMR exec-env / cluster race — real (a data race, not a use-after-free); fixed by a WAMR patch

Against the WAMR we build (`_deps/wamr-src`, tag `25bd7eb63e82`, `WAMR_BUILD_THREAD_MGR=1`):

- Every `wasm_runtime_create_exec_env` makes a cluster of its own (`thread_manager.c:223 wasm_cluster_create`),
  inserted into the global `cluster_list` under `cluster_list_lock` (`:255-260`).
- Walker: `wasm_set_exception` (`wasm_runtime_common.c:3091`), reached by every call (`wasm_runtime_clear_exception`
  in `WamrInstance::call`) and every trap, calls `wasm_clusters_search_exec_env` (`thread_manager.c:470-488`): holds
  `cluster_list_lock`, and for each cluster `cluster->lock` while it walks `cluster->exec_env_list`
  (`:452-466`, `bh_list_first_elem`, `node->module_inst`).
- Destroyer: `wasm_exec_env_destroy` (`wasm_exec_env.c:200-218`) calls `wasm_cluster_del_exec_env` **without**
  `cluster->lock` (its own comment: "we don't acquire cluster->lock because the cluster will be destroyed"), which
  `bh_list_remove`s the env (`thread_manager.c:427`) — a write to the list's links while the walker reads them under
  the lock the writer does not take. That is the data race.
- No use-after-free: the cluster is freed in `wasm_cluster_destroy` (`:336-363`) only after it takes
  `cluster_list_lock` (`:342`), which the walker holds for its whole walk, and the env itself is freed after that
  (`wasm_exec_env_destroy_internal`). The walker sees B's env or not; either way it compares `module_inst` and moves
  on. Benign in practice, undefined behaviour formally, and ThreadSanitizer reports it.

Can it be fixed on our side? `WAMR_BUILD_THREAD_MGR=0` is not possible: `wasm_runtime_terminate()` (our interrupt)
needs it (`ContourThirdParties.cmake` checks an installed WAMR for exactly that). Our process-wide lock held shared
for each outermost call and exclusive for exec-env destruction was tried in the first fix wave: TSan reported
lock-order inversions (host functions take other mutexes during a call) — a real deadlock risk. So: a minimal
WAMR patch, applied by CPM (`PATCHES`):

- `cmake/patches/wamr-unlink-exec-env-under-cluster-lock.patch`: in `wasm_cluster_del_exec_env_internal`, when
  `can_destroy_cluster` (only `wasm_cluster_del_exec_env`, whose sole caller `wasm_exec_env_destroy` holds no lock),
  unlink and read `len` under `cluster->lock`, then destroy outside it. The other caller
  (`wasm_cluster_destroy_spawned_exec_env`, `can_destroy_cluster=false`) already holds the lock and is untouched.
  An installed (system) WAMR goes without it; the comment in `ContourThirdParties.cmake` says why that is benign.
- Test `Wamr.callsAndTrapsRaceUnloads` `[threading]` (vtextension_test): 4 threads × 100 rounds load, instantiate,
  call `idle`, call `trap` (`unreachable`), drop — calls and traps racing other threads' exec-env destruction.
  - TSan **unpatched**: 2 of 3 runs report races in `bh_list_remove` / `bh_list_first_elem` (exit 66).
  - TSan patched: 10 of 10 runs clean (together with `Wamr.registrationsRaceLoads`).
- The patch applied in all three trees on reconfigure (ASan, TSan, and the clang-cl cross tree, where it stacks on the
  local `win_file.c` workaround).

## 2. primingDone dropped
`75eb4d4a7` removed (`git reset` of the branch tip before anything else). Measurements kept from the fix-wave
report: idle VmRSS 17.2–17.3 MiB with and without the release; anonymous 1.3 → 1.2 MiB; the growth since phase 4 is
file-backed compiler code.

## 3. Bounded step mask
`proto::LastLockdownStep` beside `LockdownStep` (`Pdu.hpp`); `LockdownSteps.cpp` static-asserts the table ends at
it (and that every row sits at its index), so a new step not named there fails to compile. `HostRefusals.cpp`
static-asserts `to_underlying(LastLockdownStep) < 32`; `unavailable()` ignores values past it.

## 4. NotGranted: asked again, said once
`AppContainerProvision::prepare` asks the system at every launch until granted; `_ungrantableDirectories` only keeps
the line from being said twice. Test: second launch asks again (3 writes), refused, still 1 line; after the "ACL
fix" (the fake write turns into the system's) the next launch succeeds (4 writes), still 1 line, and is not asked
again. Wine: `AppContainer.*` 4 cases / 63 assertions passed; whole process_test 115 (113 + 2 skipped) / 1002.

## 5. Priming OutOfMemory is transient
`lockdownFailureRefusal(LockdownFailure)` with a table `TransientLockdownFailures` (one row: `Priming` +
`ExtensionError::OutOfMemory` → `HostRefusal::OutOfResources`, static-asserted transient). `HelperIncarnation`'s
`refusalOf` uses it for every `Failed` Hello; everything else stays `LockdownFailed` (lasting). Tests:
`HostRefusals.primingOutOfMemoryDoesNotLast`, and a `starved` row in `RemoteBackend.refusedHelloIsLoggedOncePerReason`
(connector hears `OutOfResources`).

## 6. Degraded sandbox is a warning
`HostRefusalLog::Writer` takes `LineSeverity {Warning, Error}`; refusals are errors, `unavailable()` a warning.
`errorLogWriter()` became `logWriter()`: errors to `errorLog`, warnings to a new category `vtextension.warning`,
enabled by default (core-cpp has no warning severity, and is not ours to edit). Windows' `AppContainerProvision`
diagnostics are wrapped as errors. Test: `HostRefusals.unavailableStepIsLoggedOnce` checks the severity.

## 7. ExecutorThread flake
The test (renamed `startReturnsOnceTheThreadRuns`) no longer reads a counter the body may not have written yet:
`start()` counts its latch down just before the body runs. The body counts down a latch the test waits for, with
a 10 s bound. TSan: 50 of 50 runs passed.

## 8. Formatting
`priming()` went with item 2. `_unavailable` / `_logged` members carry their comments on the line above.
`build.yml` concurrency comment rewrapped (the one-line matrix `include:` expression stays as it was). clang-format
22 `--dry-run -Werror` clean on every changed C++ file (it caught `DefaultBackend.cpp` after the rename);
editorconfig-checker clean; actionlint OK.

## Verification

| Check | Result |
|---|---|
| ASan build, all 11 vtextension targets | exit 0, 0 warnings |
| ASan suites (`~[.measure]`) | vtextension 486/26719; wasmtime 65 (62 + 3 skip)/1191; process 140 (139 + 1 skip)/5582; remote 301/12375; remote_wamr 55 (54 + 1 skip)/655; helper 161 (160 + 1 skip)/3916; wamr_helper 62/1237 |
| `ctest --preset=clang-asan -R 'vtextension\|wasm_runtime_containment'` | 9/9 |
| `ctest -L lint` | 34/34 |
| TSan `ctest -R vtextension` | 7/7, 0 ThreadSanitizer reports |
| TSan item 1 | unpatched 2/3 runs race; patched 10/10 clean |
| TSan item 7 | 50/50 |
| wine (item 4) | AppContainer.* 4/63; process_test 115 (113 + 2 skip)/1002 |
| `clang-tidy-diff.sh --base fw-pre` | 68 TUs clean |

## Concerns
- Commits were not built one by one (the per-commit pass is the deferred item 2). The splits were made so that
  each fixup only uses what its target already has: the writer-signature change lives entirely in `4491235ab`,
  the AppContainer fixup keeps the old writer shape.
- The WAMR patch is ours to carry until upstream fixes it; worth an upstream report. A system-installed WAMR keeps
  the (benign) race.
- `vtextension.warning` is a new log category in Contour's category list, enabled by default.
