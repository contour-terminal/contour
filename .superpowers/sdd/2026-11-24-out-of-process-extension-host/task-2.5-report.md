# Task 2.5 report — `makeDefaultBackend(BackendConfig)` + remote composition

Status: DONE (with minor concerns, below). Base 7e77b3019.

## Commits
- 6f68f2cb7 fix(vtextension): reuse a held memory copy unless another one wrote into it (item 0)
- f6230198e refactor: configure the extension runtime at every composition root

## Item 0a — CopyPathMemory
- `src/vtextension/wasm/remote/CopyPathMemory.{hpp,cpp}`: new private `settledBytes(begin, end)` computes, per byte, what `flush()` would leave there (latest fetched range that *changed* the byte, else the first holder's fetched value). `view()` serves the latest range that holds the whole request and whose bytes there equal the settled bytes; otherwise it fetches and overlays only settled bytes (previously every byte of every earlier range was overlaid).
- "Written" = differs from the range's snapshot — the same definition `flush()` uses, so view and flush agree.
- Fixes besides the alternating pattern: (1) a later range no longer serves bytes an earlier range wrote after the later fetch (it used to: latest containing range won unconditionally); (2) a refetch no longer copies a later range's *unchanged* (stale) bytes over an earlier range's newer write.
- Tests (`CopyPathMemory_test.cpp`): `alternatingOverlappingViewsFetchEachRangeOnce` (64 rounds of view(0,8)/view(4,8) with writes outside the overlap, cap 16 bytes = exactly two ranges: 2 fetches, never refused; flush correct); `earlierRangeWrittenAfterALaterFetchIsSeen`. Both failed before the change (verified), pass after; `overlappedRangeIsNotServedStale` and the other 4 still pass (7/7, 191 assertions).
- Limit, inherent to the design: alternating views that each *write into the overlap* still refetch each time (needed so the later writer lands last at flush), so such a pattern can still reach the cap.

## Item 0b
- `RemoteBackend.cpp`, `RemoteModule::instantiate` declaration: Doxygen explaining `InvalidComponent` vs `HostUnavailable` for a respawned host's description mismatch (HostUnavailable invites retrying in a new host; the module as now described is not the one checked against its world; host is still terminated and the next instantiate respawns and checks again).

## Main task
- `wasm/DefaultBackend.hpp`: `[[nodiscard]] std::unique_ptr<IWasmBackend> makeDefaultBackend(BackendConfig config);`, includes `BackendConfig.hpp`, no default.
- `wasm/wamr/DefaultBackend.cpp`: ignores the config (D16) by moving it into a local `dropped` — see deviations.
- Roots: `ContourGuiApp.cpp` (named local `backendConfig` in `extensionRuntime()`), `cli/ContourApp.cpp` (new `makeDaemonExtensionBackend()` with named local `backendConfig`, used by `DaemonExtensionResources::backend`), `bench-headless.cpp`, `vthost/SessionHost_test.cpp`, `testing/Harness.hpp`, `testing/RunnerHarness.hpp`, and all `vtextension/*_test.cpp` sites. `grep -rn "wasm::makeDefaultBackend()" src` → none (only a CMake comment).
- New `src/vtextension/wasm/remote/testing/RemoteDefaultBackend.cpp`: one binary-wide `RemoteKindState`; null `config.connector` → `InProcessHostConnector` over `testing::makeHostedEngine()` (Wasmtime, software traps), per R15; `wasm::testing::preparedGeneration()` → `remote::preparedGeneration()`.
- `wasm/remote/CMakeLists.txt`: `vtextension_remote_test` also compiles Bundled/ControlImports/Engine/ExtensionHost/ExtensionWorkerPool/Instance `_test.cpp` and `RemoteDefaultBackend.cpp`; guest deps (echo, hostile, sleeper, spawner, contour_bundled_extensions) and `VTEXTENSION_TEST_MODULE_DIR` under `CONTOUR_WASM_GUESTS`; links `glaze::glaze` privately; ctest runs it with `"~[in-process]"`.

### What the remote composition surfaced (and how it was handled)
- Harness defect, not a RemoteBackend bug: `RunningGuest`, `Instance_test`'s `outcomeOf` and malicious-clock case, `ControlImports_test`'s instantiate case, `ManualScheduler` (RunnerHarness/Bundled) and `Engine.deadline.endToEnd` called into instances on an unguarded thread, relying on WAMR's rule that the thread which brought the runtime up counts as prepared (`ThreadEnv::isInitializingThread`). The contract (`enterThread()` doc; `BackendContract` checks) doesn't promise that, and Wasmtime/Remote don't do it. Fixed by preparing threads: new `testing::preparedThread(backend)` helper in Harness.hpp; `RunningGuest` holds a guard (member after `_engine`, before module/instance); `ManualScheduler` now takes the backend (`ManualScheduler scheduler { *backend }`) and runs each job under `enterThread()` like a pool lane; Engine test enters the thread itself.
- `[in-process]` set — one case: `CompiledExtension.instantiationHooks.engineRun` (split out of `instantiationHooks`): `__post_instantiate`, `__wasm_call_ctors` without WASI, `_initialize\0`, the WAMR backend's private `_initialize` alias. These are refused because of what WAMR runs/misreads while instantiating (`wamr/HiddenExports.hpp`); Wasmtime runs none, so the modules instantiate. The generic "two export sections" case stays in the untagged test and passes remotely. Recorded in the plan's "### Phase-2 facts" (local, not committed).

## Tests (all run)
- `CopyPathMemory.*`: 7/7 pass (191 assertions).
- `vtextension_remote_test "~[in-process]" "~[.measure]"`: All tests passed, 3265 assertions in 178 test cases (25 `[guest]`, 14 `[bundled]`, guests run through RemoteBackend). The binary has no `[.measure]` cases.
- `vtextension_test` (all): 12282 assertions in 363 test cases pass.
- `ctest --preset=clang-asan -R 'vtextension|bench_headless'`: 100% passed, 14/14.
- `vthost_test "*xtension*"`: 171 assertions in 21 test cases pass.
- Builds (clang-asan, -j 4 niced, under the lock): `contour`, `vthost_test`, `vtextension_test`, `vtextension_remote_test`, `bench-headless` — clean under -Werror.

## clang-tidy
`nice -n 19 clang-tidy -p out/clang-asan --quiet <file>` on CopyPathMemory.cpp, CopyPathMemory_test.cpp, RemoteBackend.cpp, RemoteDefaultBackend.cpp, wamr/DefaultBackend.cpp, Instance_test.cpp, Engine_test.cpp, ControlImports_test.cpp, Bundled_test.cpp (pulls in Harness/RunnerHarness/ManualScheduler), cli/ContourApp.cpp, ContourGuiApp.cpp → no diagnostics. clang-format (/usr/bin, 22) and editorconfig-checker clean.

## Deviations
- Harness changes (thread guards, `ManualScheduler(IWasmBackend&)`) beyond the brief's file list: needed so the suites honour the contract; the alternative was tagging ~13 cases `[in-process]` for a harness assumption, which the brief says to keep minimal.
- WAMR `makeDefaultBackend` uses `auto const dropped = std::move(config);`: an unnamed/unused by-value parameter trips `performance-unnecessary-value-param`, and `std::ignore = std::move(config)` trips `performance-move-const-arg`; no NOLINT allowed.
- Remote test's ctest entry adds `"~[in-process]"` (necessary for `ctest -R vtextension` to pass).

## Concerns
- The `[in-process]` case is really WAMR-specific, not in-process-specific: when Wasmtime becomes the in-process default it will fail in `vtextension_test` too and needs an engine-feature gate (e.g. via `EngineFeatures`) rather than a tag.
- `RemoteBackend_test.cpp` keeps its own static `RemoteKindState` beside RemoteDefaultBackend's; two kinds in one binary are fine per D29 but are not "one kind per process".
- CopyPathMemory: alternating views with writes into the overlap still grow the held bytes (see 0a).
- Performance: CopyPathMemory `view()` now computes settled bytes per call, O(ranges x view size); copy path only, ranges cleared each call. No measurement taken.

## Fix round 1

Commits:
- 1931d68ce fix(vtextension): keep copied guest memory in a page shadow
- dade38107 fix(vtextension): refuse instantiation hooks before any engine sees them
- e2ed5e691 test(vtextension): give the large-output render a deadline it can meet

### 1. CopyPathMemory page shadow (R16)
- `wasm/remote/CopyPathMemory.{hpp,cpp}` rewritten; public interface kept (`view`/`flush`/`clear`; `flush()` is now non-const because it refreshes the snapshot). RemoteBackend is untouched.
- `PageBytes = 4096`. Shadow and snapshot: one `std::make_unique_for_overwrite<std::byte[]>(memoryBytes)` each (default-initialised, never written until a page is fetched → lazily committed on Linux/macOS/BSD; Windows commits the allocation up front against the commit limit, pages stay demand-zero; documented at the class). `memoryBytes` is clamped to 4 GiB. Per-page `PageState {Absent, Fetched}` plus a list of pages fetched since `clear()` (so `clear()` and `flush()` only touch those).
- `view`: overflow-safe bound (`address + size` in size_t vs `memoryBytes`), one fetch per maximal run of absent pages, span into the shadow at the same offset. No per-view allocation besides the fetch result itself (item 3).
- `flush`: pages sorted, whole-page equality fast path, one MemoryWrite per maximal run of changed bytes (runs continue across adjacent fetched pages), address order; flushed runs copied into the snapshot.
- Tests (`CopyPathMemory_test.cpp`, 9 cases): flushEmitsOnlyChangedRuns (incl. second flush empty), viewsOfHeldPagesFetchNothing, overlappingViewsAreTheSameBytes (same address; earlier stale-bytes scenario both directions), noWriteIsLost (the reviewer's scenario → memory ends 9), alternatingOverlappingViewsFetchOnce (64 rounds, writes into the overlap, 1 fetch), adjacentChangesFlushAsOneWrite (across a page boundary → one write), viewAcrossPages (read runs split by a held page; after clear one 2-page read), refusesWhatLiesPastTheMemory (limit, address space, max values, failing host). Old range-mechanics tests deleted/replaced.

### 2. Instantiation hooks, engine-neutral
- Choice: decide in engine-neutral code, from the bytes, before any engine sees the module. New `wasm/InstantiationHooks.hpp`: `RefusedExportNames` = `__post_instantiate`, `__wasm_call_ctors`, `~initialize`; `isRefusedExportName()` also refuses any name holding a NUL. New `binary::exportNames(bytes)` in `WasmBinary.hpp`. `CompiledExtension::load` refuses `InvalidComponent` when the export section is unreadable or names a refused export (beside the existing `hasStartFunction` byte check).
- Why bytes, not engine descriptions: an engine reading C strings reports `_initialize\0` as `_initialize`; the bytes are the only engine-independent truth. Why `__wasm_call_ctors` unconditionally (not only without WASI): that is what the WAMR backend already refused (`isInstantiationHook` on every module), so no module that loaded before is refused now.
- `wamr/HiddenExports.hpp` gains a static_assert that every private alias is in the neutral refused set; WAMR's own refusal stays as a backstop.
- `CompiledExtension.instantiationHooks` is one untagged test again with all five cases and the promptness bound, passing on WAMR (vtextension_test) and remote/Wasmtime (vtextension_remote_test). No `[in-process]` tag remains; the remote ctest entry no longer filters. New tests: `WasmBinary.exportNames`, `InstantiationHooks.isRefusedExportName`.
- Behaviour change: junk bytes now fail `InvalidComponent` without reaching the host (previously an unreachable host could make them `HostUnavailable`).

### TSan finding
- Under TSan, `Instance.hostile.hugeOutput`'s generous render (2 MiB copied out of the host) returned DeadlineExceeded (50 ms default call deadline). Verified pre-existing by rebuilding the TSan binary with the 7e77b3019 CopyPathMemory: same failure. The case now sets its own 5 s deadline (it tests the output limit, not deadlines). Separate commit.

### Results
- ASan: `vtextension_test` 365 cases / 12297 assertions pass; `vtextension_remote_test` (no filter) 178 cases / 3281 assertions pass; `ctest --preset=clang-asan -R 'vtextension|bench_headless'` 14/14.
- TSan (`out/clang-tsan`, existing tree, -j 4 niced): `vtextension_remote_test` 178 cases / 3281 assertions pass, no ThreadSanitizer warnings.
- clang-tidy (`nice -n 19 clang-tidy -p out/clang-asan --quiet <file>`): CopyPathMemory.cpp, CopyPathMemory_test.cpp, CompiledExtension.cpp, WasmBinary_test.cpp, Instance_test.cpp, wamr/HiddenExports.cpp — clean (two `readability-math-missing-parentheses` fixed). clang-format 22 and editorconfig-checker clean.

### Concerns
- Each remote instance reserves 2 × memoryBytes (64 MiB at the default) of address space; committed only as fetched on POSIX, committed up front on Windows.
- A copy-path render of megabytes takes longer than the 50 ms default deadline under TSan; phase 4's shared memory removes the copy.

## Fix round 2

Commits:
- 8b65322e7 fix(vtextension): snapshot only the memory pages the parent fetched
- 3ce6b5120 test(vtextension): name the constructor hook case for what is refused

1. Snapshot per fetched page (R16): the full-size `_snapshot` buffer is gone. `_slots` (`std::vector<uint32_t>`, one per page, `NoSlot` = `numeric_limits<uint32_t>::max()` for absent) indexes `_snapshots`, a `std::vector<std::byte>` arena grown by one page per fetched page; `clear()` resets the fetched pages' slots and `_snapshots.clear()` keeps capacity, so steady-state views allocate nothing for snapshots. `snapshotOf(page)` gives a page's span (last page may be short); `flush()` copies each emitted run back per spanned page. The per-page `PageState` vector is replaced by the slots. Test hook `snapshotBytes()`; new test `CopyPathMemory.snapshotsOnlyTheFetchedPages` (64-page memory: 3 pages fetched → 3 pages of snapshot; held-page view adds none; flush doesn't grow it; clear → 0; refetch of one page → one page). Each instance now reserves memoryBytes (32 MiB default) of address space for the shadow, plus a page per fetched page.
2. `MaxCopiedBytes` doc (RemoteBackend.cpp) now says it is the largest memory the protocol carries and so the limit no view reaches past.
3. `Instance_test` case relabelled `"__wasm_call_ctors"` (refused regardless of WASI).

Results (ASan): `vtextension_test` 366 cases / 12313 assertions; `CopyPathMemory.*,CompiledExtension.*` 17 cases pass; `vtextension_remote_test` 178 cases / 3281 assertions; `ctest -R vtextension` 3/3. clang-tidy clean on CopyPathMemory.cpp, CopyPathMemory_test.cpp, RemoteBackend.cpp; clang-format and editorconfig-checker clean.
