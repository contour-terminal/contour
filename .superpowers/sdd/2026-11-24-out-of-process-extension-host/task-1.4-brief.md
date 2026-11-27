### Task 1.4: Instances, calls, host functions, memory, limits

**Files:**
- Create: `src/vtextension/wasm/wasmtime/WasmtimeInstance.cpp`
- Modify: `src/vtextension/wasm/wasmtime/WasmtimeBackend.cpp`

**Interfaces:**
- Consumes: 1.3.
- Produces: the full `IWasmInstance` for Wasmtime (interrupt excepted; see Task 1.6).

- [ ] **Step 1: Confirm the failing cases.** Run:
  `... "WasmContract.callsExport" "WasmContract.instantiateLeavesReactorToCaller" "WasmContract.memoryViewIsBoundsChecked" "WasmContract.hostCallbackReceivesArguments" "WasmContract.hostCallbackReceivesArgumentsInOrder" "WasmContract.hostCallbackErrorFails" "WasmContract.twoRuntimesShareHostImports" "WasmContract.olderRuntimeOutlivesNewer" "WasmContract.memoryCapRefusesGrowth" "WasmContract.memoryCapRefusesInitialSize" "WasmContract.unboundedRecursionTraps" "WasmContract.moduleCrossesThreads" "WasmContract.instanceCrossesThreads"`.
  Expected: FAIL.
- [ ] **Step 2: Implement.**
  - **One store per instance.** The store data points at the instance's `IHostContext&`.
    `wasmtime_store_limiter(store, limits.memoryBytes, -1, 1, -1, 1)`. An initial memory above the cap →
    `OutOfMemory`.
  - **Host function trampoline:** read the i32 arguments into a fixed `std::array<uint32_t, 5>` and call
    `context.callHost(index, span)`. On error, stash the `ExtensionError` in the store data and return a
    trap; `call()` maps that trap to `CallFailure::HostFailure`.
  - `call()` maps traps: a stashed host error → `HostFailure`; an epoch/interrupt trap → `Interrupted`;
    anything else → `Trap`.
  - `memory(address, size)` looks up the exported memory, bounds-checks against
    `wasmtime_memory_data_size`, and returns a span. `_initialize` is never run; it stays an ordinary
    export.
  - The runtime and module are kept alive by `shared_ptr`s from the instance, matching the WAMR ownership.
- [ ] **Step 3: Run the cases.** Expected: PASS.
- [ ] **Step 4: Commit.** `git commit -s -m "feat(vtextension): instantiate and call Wasmtime modules under the host's limits"`

