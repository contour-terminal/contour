# Task 1.4 report: Wasmtime instances, calls, host functions, memory, limits

Status: DONE. All 13 Step-1 cases pass. `interruptIsFinal` passes too. Three cases are still open:
- `interruptStopsLoop` **hangs** and is owned by 1.6 (see Concerns).
- `wasiStdioGoesNowhere` fails and is owned by 1.5.

## Commits (branch feature/out-of-process-extension-host, not pushed)

| SHA | Subject |
|---|---|
| 0dfbafa8a | build: keep the Wasmtime features the backend needs in one list |
| 6a96a4a89 | feat(vtextension): instantiate and call Wasmtime modules under the host's limits |

## Item 0: one list of required Wasmtime features

- `_WASMTIME_REQUIRED_FEATURES` in `cmake/ContourThirdParties.cmake` is now the only list.
- `ContourThirdParties_WasmtimeFeatureChecks(<out> <feature>...)` turns the features into C++ source: it
  includes `<wasmtime.h>` and has one `#ifndef WASMTIME_FEATURE_X` / `#error "...lacks WASMTIME_FEATURE_X."`
  per feature. Two things use this:
  - **The probe.** `ContourThirdParties_WasmtimeUnusableReason(<target> <version> <out>)` replaces
    `..._WasmtimeMissingFeatures`. It first try_compiles a plain `#include <wasmtime.h>`. If that fails,
    the reason is "the headers of the installed Wasmtime X cannot be compiled". Otherwise it compiles one
    check per feature, and the reason is "...was built without features it needs: GC, THREADS".
  - **The backstop.** `src/vtextension/wasm/wasmtime/CMakeLists.txt` uses `file(CONFIGURE)` to generate
    `<build>/generated/vtextension/wasm/wasmtime/WasmtimeRequiredFeatures.hpp` from the same list.
    `WasmtimeBackend.cpp` includes it where the hand-written `#if !defined(...) #error` used to be.
- **Verified in a scratch CMake project.** It contains the extracted functions and three IMPORTED
  INTERFACE targets:
  - full in-tree headers gave `full: []`;
  - headers with GC and THREADS removed from `conf.h` gave `...without features it needs: GC, THREADS`;
  - a `wasmtime.h` with garbage appended gave `the headers ... cannot be compiled`.
  - The generated backstop compiled against the stripped headers gives two `#error`s, naming GC and
    THREADS. Against the full headers it compiles.
- The real configure still says `Extension host engine: Wasmtime 48.0.10`.

## Task

**New: `src/vtextension/wasm/wasmtime/WasmtimeInstance.cpp`** (`WasmtimeInstance` and
`WasmtimeModule::instantiate`)
- **One store per instance.** The store's data is a `StoreData { IHostContext& context;
  std::optional<ExtensionError> hostFailure; }`. It is held by `unique_ptr` and declared before the store,
  so it outlives it.
- **Limiter.** `wasmtime_store_limiter(store, min(memoryBytes, INT64_MAX), -1, 1, -1, 1)`.
- **Initial size above the cap.** Exported memories are checked before instantiation
  (`initialPages * 64 KiB > memoryBytes`) and give `OutOfMemory`, as WAMR does.
- **Instantiation.** It runs through the runtime's shared linker (`wasmtime_linker_instantiate`). An error
  or a trap gives `InvalidComponent`, and a null store gives `RuntimeUnavailable`. `_initialize` is never
  run; it stays an ordinary export.
- **Exports.** Function exports whose parameters and results are all i32 are looked up once. The first
  exported memory is kept.
- **`call()`:**
  - checks the sticky interrupt flag;
  - clears the stash only at call depth 0, so re-entrant host calls work;
  - copies the cells into a `std::vector<wasmtime_val_t>`;
  - sets the epoch deadline to max (R3);
  - calls `wasmtime_func_call`;
  - on success, writes the results back.
- **Failure mapping** (`failure()`), checked in this order:
  1. sticky interrupt → `Interrupted`;
  2. a stashed host error → `HostFailure`;
  3. `wasmtime_trap_code == WASMTIME_TRAP_CODE_INTERRUPT` → `Interrupted`;
  4. anything else → `Trap`.

  No trap message is parsed.
- **`memory()`.** A size of 0 gives an empty span. Otherwise it bounds-checks against
  `wasmtime_memory_data_size`.
- **`interrupt()`.** Sets the sticky flag only. The comment at `armEpochDeadline` marks where the
  interrupt's deadline replaces "max" (R3).
- **Ownership.** The instance holds `shared_ptr<WasmtimeModule const>` (now `enable_shared_from_this`),
  which holds the runtime. Destruction order is: store, then data, then module, then runtime.

**`WasmtimeBackend.cpp`**
- **Trampoline.** The arguments go into a `std::array<uint32_t, MaxHostParameters>`. A failure is stashed
  in `StoreData::hostFailure`, and the trampoline returns a trap.
- **R2 seam.** `WasiBindings` is a constexpr table of `{name, wasmtime_func_callback_t}` rows. A
  `static_assert` checks it against `wasi::Preview1Functions` (same names, same order). Every row is
  `&unimplementedWasiFunction`, which traps. `makeLinker` defines each row with its preview-1 signature.
  1.5 swaps rows one at a time.
- **`defineFunction()`** now holds the linker-definition code that host functions and WASI share. It
  returns `std::expected<void, ExtensionError>`.
- **Parameter cap.** `create()` refuses a host function that takes more than `MaxHostParameters`, with
  `RuntimeUnavailable`.

**`WasmtimeModule.cpp`**
- `functionType(FunctionSignature)` now builds every function type. It reuses the `ValueKinds` table, and
  `hostFunctionType` is gone.
- The instantiate stub is removed.

**Headers**
- `WasmtimeRuntime.hpp` gains `StoreData`, `engine()`/`linker()` accessors, `functionType`, and the
  updated `create()` doc.
- `WasmtimeBackend.hpp` gains a public `MaxHostParameters`, plus doc saying that refused growth makes
  memory.grow return -1.

## Deviations

1. **MaxHostParameters is 17, not 5.** The brief says 5, "per the existing contract". The contract doc
   said "five", but `wamr::MaxHostParameters` is 17, and real host tables need more than 5:
   `ControlImports` has `pane-split` with 13 parameters. A cap of 5 would refuse the control-imports
   runtime. The fixed array is therefore `std::array<uint32_t, 17>`, still allocation-free. I corrected
   the stale "five" in `WasmRuntime.hpp`.
2. **Only all-i32 function exports are callable.** `function()` does not find an export with i64, f32,
   f64 or v128 parameters or results. Every world function is all-i32. WAMR's cell model would instead
   pass an i64 as two cells. This is documented at `function()`.
3. **`call()` allocates two small vectors per call** (arguments and results). Using
   `wasmtime_func_call_unchecked` over a scratch buffer would avoid this, but it is subtler under
   re-entrancy. The host-call path (the trampoline) allocates nothing.
4. **A non-exported memory above the cap** fails instantiation through the limiter as `InvalidComponent`,
   not `OutOfMemory`. The C API offers no way to tell that refusal apart without parsing the error text.
   Exported memories (every real guest) give `OutOfMemory`.
5. **Extra tests**, in `WasmtimeBackend_test.cpp`:
   - `WasmtimeBackend.hostFunctionArities` covers arities 0, 1 and 17, assembled at test time with
     `wasmtime_wat2wasm`, and checks that 18 is refused.
   - `WasmtimeBackend.hostFunctionCallsBackIn` has a host function call back into its own instance (the
     allocation path), then fail. The outer call fails as `HostFailure` and the nested call returns 5.

## TDD evidence

- **RED** (before implementing), the 13 Step-1 cases: 13 test cases, 1 passed, 12 failed; 61 assertions,
  12 failed. moduleCrossesThreads already passed from 1.3. Every failure was `REQUIRE( instance.has_value() )`.
- **GREEN:** the same 13 cases pass, 138 assertions in 13 test cases.
- **R3 mutation.** With `armEpochDeadline` made a no-op, `WasmContract.callsExport` FAILED at
  `BackendContract.hpp:660 REQUIRE( instance->call(...) )`, which confirms that Wasmtime's default
  deadline of 0 traps.
  - Removing only the per-call arm, and keeping the one at instantiate, still passes: the deadline is
    absolute and persists in the store.
  - The per-call arm is kept as the spot 1.6 replaces.
- **Depth-guard mutation.** I cleared the stash on every call, nested calls included. All tests still
  passed. In Wasmtime a host function stashes its failure only after its nested call has returned, so no
  ordering exercises the guard. It is kept for parity with WAMR, and the test comment was corrected
  rather than claiming to cover it.
- **The new `[wasmtime]` tests were written alongside the code.** No separate RED run was made for them.

## Test results (real counts)

| Run | Result |
|---|---|
| `vtextension_wasmtime_test '[wasm-contract]~"WasmContract.interruptStopsLoop - *",[wasmtime]'` | 33 test cases: 32 passed, 1 failed; 404 assertions, 403 passed. The only failure is `wasiStdioGoesNowhere` (fd_write is a trapping stub until 1.5) |
| `interruptIsFinal`, `hostFunctionArities`, `hostFunctionCallsBackIn` | 46 assertions in 3 test cases, all passed |
| `WasmContract.interruptStopsLoop - WasmtimeTraits` | hangs (`timeout 20` gives exit 124): `spin` never reaches a deadline set to max |
| `vtextension_test` (WAMR) | all passed: 4057 assertions in 291 test cases |
| `ctest --preset=clang-asan -R vtextension --timeout 180` | 1 of 2 passed. vtextension_test passed; vtextension_wasmtime_test hit the **Timeout** (interruptStopsLoop) |
| `scripts/check-wasm-runtime-containment.py` | passes |

## Tooling

- **clang-tidy** (off in the tree; run by hand) reported 0 warnings:
  `nice -n 19 clang-tidy -p out/clang-asan --quiet src/vtextension/wasm/wasmtime/{WasmtimeInstance,WasmtimeBackend,WasmtimeModule,WasmtimeBackend_test}.cpp`.
  HeaderFilterRegex covers the touched headers.
- The `-Werror` build is clean.
- `/usr/bin/clang-format -i` and `editorconfig-checker` are clean on every changed file.

## Concerns

- **ctest hangs until its timeout.** `interruptStopsLoop` hangs rather than fails, so
  `ctest -R vtextension` sits on vtextension_wasmtime_test until ctest's timeout (default 1500 s; I used
  `--timeout 180`). That follows from R3 (deadline at max), and 1.6 removes it. Until then, run the binary
  with the filter above.
- **Thread-safety is not checked here.** Instances cross threads (instanceCrossesThreads passes under
  ASan). Nothing was run under TSan; the run rules allow only the asan tree.
- **Signal handlers with stores.** `installsNoSignalHandlers` still passes, but it creates no store.
  Whether instantiating and calling installs anything with `signals_based_traps=false` was not measured
  separately. Every contract case now runs real stores in the same process, and the SIGSEGV check in
  `refusesWhatWasmtimeEnablesByDefault` still passes. That is indirect evidence only.
- **A host binding under `wasi_snapshot_preview1`** now collides with the WASI rows, so the linker refuses
  the runtime. No host table does this.
