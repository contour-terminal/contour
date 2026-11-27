# Task 1.2 report: the engine feature table, enforced by both engines

Status: DONE_WITH_CONCERNS (one deviation from the Global Constraints: extended-const is On; see below).

Commit: `afcf521bf feat(vtextension): one table of WebAssembly features extensions may use`

## What changed

- `src/vtextension/wasm/EngineFeatures.hpp` (new): `EngineFeature : uint8_t` (the brief's 15 enumerators in the
  brief's order, then `ExtendedConst`, `FunctionReferences`, `RelaxedSimdDeterministic`, `WideArithmetic`,
  `BranchHinting`, `CustomPageSizes`, `StackSwitching`, `ComponentModelAsync`, `ComponentModelAsyncBuiltins`,
  `ComponentModelAsyncStackful`, `ComponentModelMap`), `FeatureState { Off, On }`, `FeatureRow { feature, state,
  name, featureSwitch }`, `EngineFeatures` (one row per enumerator, in enum order), `featureRow(EngineFeature)`.
  - Extra column `FeatureSwitch { Configurable, AlwaysOn }`: AlwaysOn marks the rows Wasmtime 48's C API has no
    setter for (sign-extension, saturating float-to-int, mutable globals, extended-const).
  - `static_assert` (consteval): one row per enumerator in order, non-empty names, every AlwaysOn row is On.
  - Every `wasmtime_config_wasm_*_set` in `out/clang-asan/_deps/wasmtime-src/include/wasmtime/config.h` (22 PROPs)
    maps to a row, so Task 1.3 can drive every setter from this table. The extra rows are Off.
  - Listed in `src/vtextension/CMakeLists.txt` headers.
- `src/vtextension/wasm/testing/BackendContract.hpp`: 11 hand-assembled modules (WAT in the doc comment), a
  `FeatureModules` table `{feature, bytes}`, and the helper `checkFeatureModules<T>(FeatureState)`. Two contract
  cases are registered in `CONTOUR_WASM_BACKEND_CONTRACT`: `refusesDisabledFeatures` (SIMD, threads/shared memory,
  tail call, multi-memory) and `acceptsEnabledFeatures` (bulk-memory, sign-ext, sat-float-to-int,
  mutable-global export, multi-value, reference-types, extended-const). Each module runs in its own
  `DYNAMIC_SECTION(row.name)` with `INFO("feature: " …)`. Ruling R1: `REQUIRE_FALSE(loaded.has_value())` comes
  before `loaded.error()` is compared.
- `cmake/ContourThirdParties.cmake`: WAMR CPM options pin every feature switch to the table:
  `BULK_MEMORY 1`, `REF_TYPES 1`, `EXTENDED_CONST_EXPR 1`, `SIMD 0`, `SHARED_MEMORY 0`, `TAIL_CALL 0`,
  `MULTI_MEMORY 0`, `MEMORY64 0`, `GC 0`, `STRINGREF 0`, `EXCE_HANDLING 0`. Only SIMD and EXTENDED_CONST_EXPR
  change behaviour; the others restate WAMR 2.4.5's defaults so an upstream default change cannot widen the set.
  `ContourThirdParties_WamrTerminates` has two new exit codes, each with a reason: 10 ("it loads modules using
  SIMD, threads, tail calls or multiple memories") and 11 ("it refuses extended constant expressions …").
- `cmake/WamrTerminateProbe.cpp`: after the signal checks, it loads the four disabled-feature modules (any load →
  exit 10) and the extended-const module (refused → exit 11). The module bytes are the contract's, checked equal
  with a script.

## Module validation

There is no `wasm-tools`, `wasm-validate` or `wat2wasm` on PATH. I built a scratch tool that links the pinned
`libwasmtime.a` (48.0.10). It assembles each WAT with `wasmtime_wat2wasm` and validates it with
`wasmtime_module_validate` under two configs: "all" (every feature on) and "ours" (the table's states). All 11 are
valid under "all". Under "ours", the four disabled modules fail with the expected feature errors ("SIMD support is
not enabled", "threads must be enabled for shared memories", "tail calls support is not enabled", "multiple
memories"), and the seven enabled modules pass. The bytes in the header are that tool's output.

## TDD

RED. I ran this after adding the table and the cases, with the WAMR options unchanged:
```
WasmContract.refusesDisabledFeatures:  BackendContract.hpp:539: FAILED  feature: simd
  test cases: 1 | 0 passed | 1 failed   assertions: 15 | 14 passed | 1 failed
WasmContract.acceptsEnabledFeatures:   BackendContract.hpp:536: FAILED  feature: extended-const
  test cases: 1 | 0 passed | 1 failed   assertions: 21 | 20 passed | 1 failed
```
WAMR refused threads, tail call and multi-memory by default. It accepted SIMD (`WAMR_BUILD_SIMD` defaults to 1)
and refused extended-const (defaults to 0).

The probe built against the old `libiwasm.a` exited 10, so a WAMR like the old one would be rejected. During this
step it also caught a byte-count slip in my first probe SIMD array, which I fixed and checked against the
contract's bytes.

GREEN, after the CPM options:
```
refusesDisabledFeatures: All tests passed (16 assertions in 1 test case)
acceptsEnabledFeatures:  All tests passed (21 assertions in 1 test case)
```
The probe against the new `libiwasm.a` exits 0.

Full runs:
- `vtextension_test`: All tests passed (4021 assertions in 290 test cases).
- `[wasm-contract]`: 304 assertions in 28 test cases.
- `ctest --preset=clang-asan -R vtextension`: 1/1 passed.

Checks:
- `clang-tidy -p out/clang-asan src/vtextension/wasm/wamr/WamrBackend_test.cpp`: no first-party diagnostics.
  `ENABLE_TIDY` is OFF in this tree, so I ran it by hand; HeaderFilterRegex covers vtextension headers.
- `clang-format --dry-run` and `editorconfig-checker` are clean on the changed files.

## Deviations

1. **extended-const is On.** It is not in the Global Constraints. Verified empirically: Wasmtime 48 accepts
   extended-const with every switch off, and its C API has no setter for it. WAMR refused it. The only way to get
   parity without a pre-validator was to make it On and build WAMR with `WAMR_BUILD_EXTENDED_CONST_EXPR 1`. The
   probe now also requires a system WAMR to accept it (code 11). If the owner wants it Off, that needs a
   pre-validation pass in the world check, because Wasmtime cannot refuse it.
2. **Extra column `featureSwitch`** on `FeatureRow`. It defaults to `Configurable`, so the brief's three-field
   designated initializers still work.
3. **Added `acceptsEnabledFeatures`** next to the brief's `refusesDisabledFeatures`. Parity has two directions,
   and this case is what found the extended-const gap.
4. **Pinned WAMR's other feature options explicitly** (to their current defaults), beyond the one option the test
   showed was needed.

## Concerns

- The full WAMR rebuild prints WAMR's own third-party C warnings (`wasm_application.c`: misleading-indentation,
  parentheses-equality). They are not first-party and the build exited 0, but they show up whenever WAMR rebuilds.
- `relaxed-simd-deterministic` is a determinism flag rather than a feature. It is Off here and only meaningful if
  relaxed SIMD is ever turned On.
- `/simplify` was not run inside this agent; it is left to the orchestrator's task-end step.
- Only the clang-asan tree was built. No macOS or Windows check of the probe change; it is POSIX-agnostic code
  apart from the existing `#ifndef _WIN32` blocks.
