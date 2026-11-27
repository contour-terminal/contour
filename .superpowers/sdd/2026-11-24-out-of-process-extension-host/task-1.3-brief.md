### Task 1.3: `WasmtimeBackend` — runtimes, modules, linkage, generations

**Files:**
- Create: `src/vtextension/wasm/wasmtime/{CMakeLists.txt,WasmtimeBackend.hpp,WasmtimeBackend.cpp,WasmtimeHandles.hpp,WasmtimeModule.cpp,WasmtimeBackend_test.cpp}`
- Modify: `src/vtextension/CMakeLists.txt` (add `wasm/wasmtime` when `CONTOUR_HAVE_WASMTIME`, beside the
  selected backend; the new test binary `vtextension_wasmtime_test` links `vtextension`,
  `vtextension_wasmtime`, Catch2 and `src/testing`)
- Modify: `scripts/check-wasm-runtime-containment.py` (allow `wasm/wasmtime/` to include `wasmtime.h`)

**Interfaces:**
- Consumes: `EngineFeatures` (1.2), `contour::wasmtime` (1.1).
- Produces:
  ```cpp
  namespace vtextension::wasm::wasmtime {
  enum class TrapMode : uint8_t { Software, Signals };  // Software: no signal handler, explicit bounds checks
  struct WasmtimeConfig { TrapMode trapMode = TrapMode::Software; };
  /// What all backends of the kind share (generation, preparedness), created once by the composition root.
  class KindState;
  [[nodiscard]] std::shared_ptr<KindState> makeKindState();
  [[nodiscard]] std::unique_ptr<IWasmBackend> makeWasmtimeBackend(std::shared_ptr<KindState> kind,
                                                                  WasmtimeConfig config);
  }
  ```
  `WasmtimeHandles.hpp`: `std::unique_ptr` aliases with deleters for `wasm_engine_t`, `wasmtime_store_t`,
  `wasmtime_module_t`, `wasmtime_linker_t`, `wasmtime_error_t`, `wasm_trap_t`.

- [ ] **Step 1: Instantiate the contract suite.** In `WasmtimeBackend_test.cpp`:
  ```cpp
  struct WasmtimeTraits
  {
      [[nodiscard]] static std::unique_ptr<IWasmBackend> make()
      {
          static auto const kind = makeKindState();
          return makeWasmtimeBackend(kind, WasmtimeConfig { .trapMode = TrapMode::Software });
      }
  };
  CONTOUR_WASM_BACKEND_CONTRACT(WasmtimeTraits)
  ```
  Also add `src/vtextension/wasm/wasmtime/PreparedGeneration.cpp`, defining `testing::preparedGeneration()`
  for this test binary from a test-only `KindState` accessor.
- [ ] **Step 2: Build and run; expect failures.** Run:
  `out/clang-asan/src/vtextension/wasm/wasmtime/vtextension_wasmtime_test "[wasm-contract]"`.
  Expected: it fails to link (`makeWasmtimeBackend` undefined).
- [ ] **Step 3: Implement the backend, runtime and module.**
  - `KindState` holds an atomic generation and the live-runtime count. Thread preparation is trivial: a
    `thread_local` generation stamp, which `enterThread` sets and `isCurrentThreadPrepared` compares.
  - `createRuntime(limits, hostFunctions)` creates **one `wasm_engine_t` per runtime**, configured from
    `EngineFeatures` (each ON/OFF setter), `max_wasm_stack = limits.stackBytes`,
    `epoch_interruption = true`, `signals_based_traps = (trapMode == Signals)`,
    `cranelift_opt_level = speed`, `parallel_compilation = false`, `macos_use_mach_ports = false`. It also
    creates one linker per runtime that defines each `HostFunction` as an all-i32, no-result function.
  - `load(bytes)`: `wasmtime_module_new`; on error → `InvalidComponent`. Describe imports and exports
    from `wasmtime_module_imports/exports`. An import is `Satisfied` if it matches a host function by
    module, name and signature, or is a WASI function in `AllowedWasiFunctions` with its p1 signature
    (signatures from a table in `wasi/Preview1Shim.hpp`, Task 1.5).
  - `capabilities()` returns `MemoryGrowthReporting::Unreported`.
  - The bring-up generation increments when the live-runtime count goes 0 → 1.
- [ ] **Step 4: Run the load/describe/generation cases.** Run:
  `... "WasmContract.loadRejectsNonModule" "WasmContract.describesImportsAndExports" "WasmContract.unknownImportIsUnsatisfied" "WasmContract.wrongSignatureImportIsUnsatisfied" "WasmContract.threadGuardNeedsARuntime" "WasmContract.threadGuardNests" "WasmContract.generationFollowsBringUp" "WasmContract.twoBackendsAgree" "WasmContract.refusesDisabledFeatures"`.
  Expected: PASS. Instance cases still fail.
- [ ] **Step 5: Commit.** `git commit -s -m "feat(vtextension): a Wasmtime backend that loads and describes modules"`

