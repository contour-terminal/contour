# Task 1.7 — Phase 1 gate: report

Status: DONE (steps 1, 2, 3, 5; step 4 /code-review left to the controller).

## Commit

- `38b58d1ec test(vtextension): time a bundled extension's render on each backend`
  - `src/vtextension/wasm/testing/RenderMeasurement.hpp` (new): `measureRender()` (Engine → CompiledExtension::load
    [timed apart] → Instance::create + load → 1 warm-up render → N timed renders cycling through given sizes;
    median/p90/fastest), `reportRender()`, and `measureBundledRender(backend, name)` running both workloads on
    plasma (SKIPs where the guests are not built). Clock injected (`core::IClock&`), `core::defaultSteadyClock()`
    at the call site.
  - `src/vtextension/wasm/wamr/WamrBackend_test.cpp`: hidden `Wamr.measureRender` `[.measure]`.
  - `src/vtextension/wasm/wasmtime/WasmtimeBackend_test.cpp`: hidden `Wasmtime.measureRender` `[.measure]`.
  - `src/vtextension/wasm/wasmtime/CMakeLists.txt`: with CONTOUR_WASM_GUESTS, vtextension_wasmtime_test depends
    on `contour_bundled_extensions` and gets `VTEXTENSION_TEST_MODULE_DIR` (same dir vtextension_test uses).
  - `src/vtextension/CMakeLists.txt`: header listed in vtextension_test sources.

## Performance (out/clang-release, Ryzen 9 9950X3D, machine shared with another build session)

Real workload: bundled `plasma.wasm`, full extension-host call path, 1000 timed calls after a warm-up, three runs.

| Workload | WAMR median/call (3 runs) | Wasmtime median/call (3 runs) | Speed-up |
|---|---|---|---|
| steady `render(120, 1)` (brief's workload; pixels cached by plasma) | 38.2 / 38.4 / 38.3 µs | 2.8 / 2.8 / 2.7 µs | **13.7×** |
| uncached (120/119 alternating → pixels re-encoded every call) | 229.6 / 230.9 / 228.6 µs | 12.4 / 12.4 / 12.4 µs | **18.5×** |

Compile time (once, excluded from per-call): Wasmtime 57–59 ms per load of plasma (Cranelift, parallel
compilation off, Software trap mode); WAMR 0.9–1.0 ms. Instantiate + `load` ≤ 0.1 ms on both. p90 within 3%
of median throughout. Speed-up is far above the 3× threshold.

Implication for phase 2+: Wasmtime moves the cost from the call to the load — ~58 ms per module compile is
the number that matters for extension start-up and hot reload; the serialized-module cache
(`wasmtime_module_serialize/deserialize`) is the obvious lever if it shows up.

## Sanitizers

- ASan: `ctest --preset=clang-asan -R vtextension` → 2/2 pass. vtextension_test 317 cases / 4558 assertions;
  vtextension_wasmtime_test 45 cases / 969 assertions; `[wasm-contract]` 34 cases / 437 assertions per backend.
- TSan (`out/clang-tsan`, both test binaries built and run in full, no suppressions/exclusions): 317 + 45 cases
  pass (17 `[threading]` cases in vtextension_test), 0 ThreadSanitizer reports.

## Coverage (out/clang-coverage, only the two test targets built; both suites run; llvm-cov gcov)

- `src/vtextension/wasm/wasmtime/`: 476/496 lines = 96.0% (WasmtimeBackend.cpp 94.8% of 193,
  WasmtimeInstance.cpp 97.0% of 165, WasmtimeModule.cpp 96.4% of 138, WasmtimeRuntime.hpp 100%).
- `src/vtextension/wasm/wasi/`: 152/155 lines = 98.1% (Preview1Shim.cpp 100% of 133, SystemEnvironment.cpp
  86.4% of 22).

## Risk assessment

- Low for production: the commit only adds a hidden test case per suite, a test-only header and a test build
  dependency. The Wasmtime test target now builds the bundled extensions first where the guests are on, which
  lengthens a cold build of that target (cargo) — no effect where guests are off.
- The Wasmtime backend itself (Phase 1) is still linked by nothing but its tests; it is green under ASan and
  TSan with the contract suite shared with WAMR.
- Measurement caveats: one machine, shared with another build session (load avg ~3 on 32 threads); run-to-run
  spread < 1%. Wasmtime measured in Software trap mode (the backend default); Signals mode would likely be
  faster still and was not measured.

## Deviations / judgment calls

- Added a second ("uncached") workload beside the brief's `render(120, 1)`: plasma caches its encoded pixels
  per size, so the steady frame alone understates compute-heavy extension work. Both are reported; the brief's
  workload is the headline.
- Where guests are not built the measure case SKIPs rather than falling back to a synthetic TestModules loop
  (both trees used here have the guests).
- clang-release configured fresh in this worktree (PACE_SCHEDULER=127.0.0.1:6674); only the two test targets
  built in release, tsan and coverage trees. Third-party WAMR C warnings in the release log are from `_deps`
  (not first-party; build exit 0).
