### Task 2.7: Phase 2 gate

- [ ] **Step 1: ASan/LSan.** `ctest --preset=clang-asan -R vtextension` → 3/3 binaries pass (`vtextension_test`, `vtextension_wasmtime_test`, `vtextension_remote_test`), `[wasm-contract]` on all three, `Bundled` on both compositions; no LSan report.
- [ ] **Step 2: TSan.** `cmake --preset clang-tsan && nice -n 19 ionice -c 3 cmake --build --preset clang-tsan -j 4 --target vtextension_test vtextension_remote_test`; run `out/clang-tsan/src/vtextension/vtextension_test "[threading]"` and `out/clang-tsan/src/vtextension/wasm/remote/vtextension_remote_test "~[.measure]"` → no reports, no suppressions.
- [ ] **Step 3: Whole-callback IPC overhead.** Hidden `Remote.measureRender` (`[.measure]`) in `vtextension_remote_test` via `wasm/testing/RenderMeasurement.hpp`, same workloads as 1.7 (steady `render(120, 1)`, uncached 120/119); `out/clang-release`, remote test target only; three runs each. Report median per call vs Wasmtime in-process (2.8 µs steady / 12.4 µs uncached) and one round trip (`callsExport`'s export, 10 000 calls). Record in "Phase-2 facts". No threshold here (gate 9.2 applies to the orchestrator at 6.5).
- [ ] **Step 4: Coverage.** `clang-coverage` preset, the three test targets only; line coverage of `wasm/remote/`, `wasm/host/`, `ExtensionRegistry.cpp`.
- [ ] **Step 5: Phase review (R8).** Code-reviewer agent on the worktree diff `38b58d1ec..HEAD`; address every finding.
- [ ] **Step 6: Phase summary.** Performance (Step 3), risk (D9, D10, D20 named), coverage (Step 4).

### Phase-2 facts

*(Filled in by Tasks 2.5 and 2.7.)*

Task 2.5 — the remote composition of the real-guest suites (`vtextension_remote_test`, `"~[in-process]"`; 178 cases, 25 of them `[guest]`, 14 `[bundled]`):
- `[in-process]` set: empty (fix round 1). The instantiation-hook names (`__post_instantiate`, `__wasm_call_ctors`, NUL-holding names, WAMR's `~initialize` alias) are refused engine-neutrally in `CompiledExtension::load` from the module bytes (`wasm/InstantiationHooks.hpp`, `binary::exportNames`), so every engine gives `InvalidComponent` at once; the remote ctest entry no longer filters.
- CopyPathMemory is a page shadow (R16): 4 KiB pages, one lazily committed buffer + snapshot of `memoryBytes`; one MemoryRead per run of absent pages.
- Under TSan, `Instance.hostile.hugeOutput`'s 2 MiB render over the copy path exceeded the 50 ms call deadline (old and new CopyPathMemory alike); the case now raises its own deadline.
- Not tagged, fixed instead: the harnesses relied on WAMR treating the thread that brought its runtime up as prepared (`ThreadEnv::isInitializingThread`), which the contract does not promise and neither Wasmtime nor RemoteBackend does. `RunningGuest` and `Instance_test`'s `outcomeOf`/malicious-clock case and `ControlImports_test`'s instantiate case hold a guard (`testing::preparedThread`); `ManualScheduler` (now constructed with the backend) runs each job under `enterThread()`, as a pool lane does; `Engine.deadline.endToEnd` enters the thread itself.
- `testing::preparedGeneration()` maps to `remote::preparedGeneration()` in `RemoteDefaultBackend.cpp`; that file composes `makeRemoteBackend` over one binary-wide `RemoteKindState` and, when `config.connector` is null (R15), an `InProcessHostConnector` over `testing::makeHostedEngine()` (Wasmtime, software traps).
- The binary has no `[.measure]` cases, so `"~[.measure]"` changes nothing there.
- The WAMR `makeDefaultBackend(BackendConfig)` drops its config into a local: an unused by-value sink trips `performance-unnecessary-value-param`, and `std::ignore = std::move(...)` trips `performance-move-const-arg`.

---

## Phase 3 — The executable, its supervisor and its lifecycle

