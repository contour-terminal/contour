# Task 1.3 report: WasmtimeBackend (runtimes, modules, linkage, generations)

Status: DONE (Step 4 cases green; the 15 instance cases fail as expected until 1.4).

## Commits (branch feature/out-of-process-extension-host, not pushed)

| SHA | Subject |
|---|---|
| f25bdada8 | test(vtextension): one set of feature modules for the contract and the WAMR probe |
| ad9d60955 | docs(vtextension): say what the engine feature table's switches mean |
| 1e63d04d4 | build: create contour::wasmtime only once a Wasmtime is resolved |
| 4987865ac | test(vtextension): hold every engine to refusing the features Wasmtime enables by default |
| a5185317e | build: hold Wasmtime to the same containment as WAMR |
| a4805b923 | feat(vtextension): a Wasmtime backend that loads and describes modules |

## Item 0 (carry-ins)

**a. Shared feature modules.** `src/vtextension/wasm/testing/FeatureModules.hpp` now holds every feature
module plus the `FeatureModule` struct and the `FeatureModules` table. `BackendContract.hpp` and
`cmake/WamrTerminateProbe.cpp` both include it. The probe's `try_run` gets
`CMAKE_FLAGS "-DINCLUDE_DIRECTORIES=${PROJECT_SOURCE_DIR}/src"` and `RUN_OUTPUT_VARIABLE`. The probe walks
the table: on the first module WAMR gets wrong it prints the feature name and exits 10 (accepted an Off
feature) or 11 (refused an On feature). The configure reason then names it, e.g. "it loads a module
using simd, which extensions may not use". Before this change the probe checked 4 Off modules and only
extended-const for On. It now checks every module in the table.
- **Deviation:** the header includes `EngineFeatures.hpp` and `<span>` as well as `<array>`/`<cstdint>`. That
  way the feature-to-bytes table lives in this one header and the probe makes its On/Off decision from
  the same table. `EngineFeatures.hpp` is itself std-only, so the header still has no dependencies. The
  probe still builds as C++20.
- I verified the probe by hand, since the configure only runs it for a system WAMR:
  `clang++ -std=c++20 -fsanitize=address,undefined -Isrc -I<wamr include> cmake/WamrTerminateProbe.cpp libiwasm.a`
  run against the in-tree WAMR exits 0. I ran this both before and after the new modules were added.

**b. EngineFeatures docs.** `FeatureSwitch` now says it records whether *Wasmtime* can be told to refuse a
feature, and that WAMR is held to the table by its build options instead. `Configurable` means
"Wasmtime's C API has a switch for it". The `RelaxedSimdDeterministic` enumerator is now documented as a
behaviour setting rather than a feature.

**c. CMake.** The `|> Extension host engine:` value now starts in the same column as its neighbours
(column 53 in the string, checked). `contour::wasmtime` is created only once a Wasmtime is resolved. If
CPM returns an empty `wasmtime_SOURCE_DIR` (CPM_USE_LOCAL_PACKAGES modes), the build falls back to WAMR
with the reason "CPM resolved Wasmtime 48.0.10 without fetching its archive". The find_package branch
also creates the target only when it succeeds. Configure output: `Extension host engine: Wasmtime 48.0.10`.

## Off features whose Wasmtime 48.0.10 default is ON (measured)

A scratch program built default engines (`wasm_config_new()` with no setters) from the in-tree
`libwasmtime.a` and loaded one module per feature:

- **Accepted by default:** gc, exceptions, memory64, function-references, relaxed-simd, simd, threads,
  tail-call, multi-memory.
- **Refused by default:** wide-arithmetic, custom-page-sizes, stack-switching.

With every one of those features switched off, all of them were refused, and extended-const was still
accepted. Before I embedded the new modules, I checked each one against an engine with its feature on,
so a refusal can only come from the switch and never from a malformed module.

New modules in `FeatureModules.hpp`: Gc, Exceptions, Memory64, FunctionReferences, WideArithmetic,
CustomPageSizes, StackSwitching and RelaxedSimd. I assembled them with `wasmtime_wat2wasm` and stripped
the name sections. Five Off features have no module, because no core module can be refused for them.
The table's doc comment explains this:
- the four component-model rows concern components, not core modules;
- relaxed-simd-deterministic is a behaviour setting;
- branch hints are a custom section, which engines ignore.

**WAMR:** it already refuses all eight new modules. `refusesDisabledFeatures - WamrTraits` went from 12 to
48 assertions and all pass, so WAMR's build options did not need changing.

## Task

- `wasi/Preview1Signatures.hpp` (R2) holds the constexpr `Preview1Functions` table: name, parameter slots,
  count, and `Preview1Result {Nothing, Errno}`. A `static_assert` checks that the names equal
  `AllowedWasiFunctions` in order. It also provides `findPreview1Function()` and
  `Preview1Function::signature()`. It is listed in `vtextension_HEADERS`.
- **`wasm/wasmtime/`:**
  - `WasmtimeBackend.hpp` (public) declares `TrapMode`, `WasmtimeConfig`, `KindState`, `makeKindState` and
    `makeWasmtimeBackend`, matching the brief.
  - `WasmtimeHandles.hpp` declares the unique_ptr handles from the brief, plus `FuncTypeHandle`.
  - `WasmtimeRuntime.hpp` (internal) defines `KindState`, `WasmtimeRuntime`, `WasmtimeModule` and `linkageOf`.
  - `WasmtimeBackend.cpp` contains the backend, KindState, thread guards, the engine config and the linker.
  - `WasmtimeModule.cpp` contains the deleters, load/describe and linkage.
  - `PreparedGeneration.cpp`, `CMakeLists.txt` and `WasmtimeBackend_test.cpp` complete the directory.
- **Feature switches:** `FeatureSetters` holds all 22 Wasmtime setters. A consteval `static_assert` checks
  that the table equals, in order, the `Configurable` rows of `EngineFeatures`, and that no row lacks a
  setter. Each engine is configured by setting every row from its state. A `#error` fires if the Wasmtime
  headers lack COMPILER/GC/THREADS/PARALLEL_COMPILATION/COMPONENT_MODEL(_ASYNC), because some setters are
  `#ifdef`-guarded in `config.h`.
- **Engine configuration:** one engine per runtime, plus:
  - `max_wasm_stack = limits.stackBytes`
  - `epoch_interruption = true`
  - `signals_based_traps = (trapMode == Signals)`
  - `cranelift_opt_level = SPEED`
  - `parallel_compilation = false`
  - `macos_use_mach_ports = false`
- **Linker:** one per runtime. It defines each HostFunction as an all-i32, no-result function. The callback
  forwards the arguments to the `IHostContext*` that is the store's data, and it is `noexcept`.
- **R3 (epoch deadline):** two comments say where the per-call deadline must be set: the `engineConfig()`
  epoch line, and the `WasmtimeModule::instantiate` stub. This task never enters guest code.
- **Linkage:** an import is satisfied if it matches a host binding by module, name, i32×N parameters and no
  results, or if it is a WASI import whose preview-1 signature matches. Import or export kinds the
  interface cannot name (a tag) fail the load as `InvalidComponent`.
- **Generations:** the generation increments when the live-runtime count goes from 0 to 1 (under a mutex).
  - The `thread_local` stamp is `{KindState const*, generation}`, so two KindStates in one process cannot
    confuse each other.
  - Guards nest: the outermost guard clears the stamp only if the stamp is still its own.
  - `capabilities()` returns `Unreported`.
- **Build:** `vtextension_wasmtime_test` uses the shared test main (`src/vtextension/test_main.cpp`) and
  `contour_test_trace`. It links `vtextension`, `vtextension_wasmtime`, `contour::wasmtime`, `core::testing`
  and Catch2, and is registered as a ctest. The subdirectory is added when `CONTOUR_HAVE_WASMTIME` is ON.
- `scripts/check-wasm-runtime-containment.py` is now data-driven through a `BACKENDS` table. Wasmtime's
  headers (`wasmtime.h`, `wasm.h`, `wasi.h`, `wasmtime/`) and `contour::wasmtime` are confined to
  `wasm/wasmtime/`. Self-test: 27 cases OK. Gate passes.

## Deviations / decisions

1. **Internal header added.** `WasmtimeRuntime.hpp` was not in the brief's file list. `KindState`'s
   definition and the runtime and module classes are shared between the two `.cpp` files and
   `PreparedGeneration.cpp`, so they needed a header; it follows the `WamrRuntime.hpp` pattern.
2. **Test-only accessor.** `KindState::preparedGeneration()` is static and returns the calling thread's
   stamp, of whichever kind.
3. **Instance stub.** `instantiate()` returns `RuntimeUnavailable`, with a comment, and nothing more is
   implemented for instances.
4. **Host callback is real code already.** The callback reaches the host through store data =
   `IHostContext*`. A host failure becomes a plain trap; 1.4 decides how to label it `HostFailure`.
5. **CMake fallback.** In the CPM-without-archive case the build falls to WAMR, as ruled. It does not try
   the find_package target.
6. **Conservative subdirectory guard.** The `add_subdirectory(wasm/wasmtime)` is skipped when
   `CONTOUR_WASM_RUNTIME == wasmtime`, which cannot happen today. This avoids a double add later.

## TDD evidence

- **RED:** I added the test file, the CMake and empty `.cpp` stubs, then built `vtextension_wasmtime_test`.
  The link failed with
  `undefined reference to vtextension::wasm::wasmtime::makeWasmtimeBackend(std::shared_ptr<KindState>, WasmtimeConfig)`.
- **GREEN:** after implementing, each Step 4 case passes. Assertions per case:

  | Case | Assertions |
  |---|---|
  | loadRejectsNonModule | 3 |
  | describesImportsAndExports | 18 |
  | unknownImportIsUnsatisfied | 4 |
  | wrongSignatureImportIsUnsatisfied | 5 |
  | threadGuardNeedsARuntime | 4 |
  | threadGuardNests | 4 |
  | generationFollowsBringUp | 5 |
  | twoBackendsAgree | 3 |
  | refusesDisabledFeatures | 48 |

  All nine together: 94 assertions in 9 test cases. Also passing:
  - acceptsEnabledFeatures (21), allowListedWasiIsSatisfied (33), moduleCrossesThreads (5) and
    installsNoSignalHandlers;
  - `[wasmtime]`: 33 assertions in 2 test cases.
- **Mutation check of the new Wasmtime-only case:** I forced `wasmtime_config_wasm_memory64_set(cfg, true)`
  after the table loop. `refusesWhatWasmtimeEnablesByDefault` then FAILED with "feature: memory64". The
  code was restored afterwards.

## Test results (real counts)

- `vtextension_wasmtime_test` (whole binary): 30 test cases, 15 passed and 15 failed; 274 assertions,
  259 passed. All 15 failures are instance cases:
  - callsExport, instantiateLeavesReactorToCaller, memoryViewIsBoundsChecked;
  - hostCallbackReceivesArguments(InOrder), hostCallbackErrorFails;
  - twoRuntimesShareHostImports, olderRuntimeOutlivesNewer;
  - memoryCapRefusesGrowth/InitialSize, unboundedRecursionTraps;
  - interruptStopsLoop, interruptIsFinal, instanceCrossesThreads, wasiStdioGoesNowhere.

  These are expected until 1.4.
- `vtextension_test` (WAMR): all tests passed, 4053 assertions in 290 test cases. Its contract alone
  passed 304 assertions in 28 test cases after the move.
- `ctest --preset=clang-asan -R vtextension`: 1 of 2 passed. vtextension_test passed;
  vtextension_wasmtime_test fails because of the expected instance cases.
- `ctest --preset=clang-asan -R 'containment|core-cpp-vendored'`: 3 lint tests passed.

## clang-tidy (off in the build tree; run by hand, LLVM 22.1.8)

`nice -n 19 clang-tidy -p out/clang-asan --quiet src/vtextension/wasm/wasmtime/{WasmtimeBackend.cpp,WasmtimeModule.cpp,PreparedGeneration.cpp,WasmtimeBackend_test.cpp} src/vtextension/wasm/wamr/WamrBackend_test.cpp`
reported 0 warnings. The repository's HeaderFilterRegex covers `vtextension/`, so the new headers,
`FeatureModules.hpp` and `BackendContract.hpp` were checked too. I re-ran it on `WasmtimeBackend.cpp`
after the `noexcept` amend: 0 warnings. The build is `-Werror`-clean. `/usr/bin/clang-format` and
`editorconfig-checker` were run on every changed file.

## Link sets

- **Linux x86_64:** the static `libwasmtime.a` with `pthread dl m` links and runs under ASan+UBSan.
- **macOS and Windows:** unverified, and left as 1.1 wrote them; I found no evidence either way.

## Concerns

- A system Wasmtime built without default features now fails to compile through the `#error`, instead of
  falling back to WAMR at configure time. A configure-time check of `wasmtime/conf.h` would be gentler;
  it is not done here.
- The contract cannot exercise a relaxed-SIMD-only refusal, because SIMD is Off too. The RelaxedSimd module
  is there so that an engine accepting SIMD would still have to refuse it.
- The registered `vtextension_wasmtime_test` ctest is red until 1.4. That is expected, but anyone running
  `ctest -R vtextension` on this branch in the meantime will see it fail.
- Not checked here: whether `signals_based_traps=false` alone keeps Wasmtime 48 from installing any signal
  handler. `installsNoSignalHandlers` passes, but it creates no store or instance; re-check it in 1.4.

---

# Fix round 1 (review: 3 Important, Minors 4 and 6)

Commits:
- d0bc39031 fix(vtextension): keep the Wasmtime tests off the process's signals, and refuse a zero stack
- 3087e16d9 build: take an installed Wasmtime only if it has the features the backend switches
- c83163af0 build: match only Wasmtime's own headers in the containment gate

## 1. The test binary no longer installs Wasmtime's signal handlers

- **Change:** `defaultEngineAccepts` in `WasmtimeBackend_test.cpp` now builds its engine with
  `wasm_config_new()`, setting only `signals_based_traps(false)` and `macos_use_mach_ports(false)`. Feature
  defaults are left as Wasmtime has them.
- **New check:** `refusesWhatWasmtimeEnablesByDefault` records SIGSEGV's `sa_sigaction` before and after and
  CHECKs that they are equal (POSIX only).
- **RED:** I temporarily restored `wasm_engine_new()`.
  `vtextension_wasmtime_test "WasmtimeBackend.refusesWhatWasmtimeEnablesByDefault"` then reported
  `WasmtimeBackend_test.cpp:103: FAILED: CHECK( after...sa_sigaction == before...sa_sigaction )`, with
  33 assertions, 1 failed.
- **GREEN, random order:**
  `vtextension_wasmtime_test --order rand --rng-seed {1,2,4} "WasmContract.installsNoSignalHandlers - WasmtimeTraits,[wasmtime]"`
  passed for each seed: all tests passed, 56 assertions in 4 test cases.

## 2. An installed Wasmtime without default features now falls back to WAMR

- **Feature list:** `cmake/ContourThirdParties.cmake` defines `_WASMTIME_REQUIRED_FEATURES` (COMPILER GC
  THREADS PARALLEL_COMPILATION COMPONENT_MODEL COMPONENT_MODEL_ASYNC).
- **Check function:** `ContourThirdParties_WasmtimeMissingFeatures(target var)` runs one
  `try_compile(... SOURCE_FROM_CONTENT ... NO_CACHE)` per feature against `<wasmtime.h>`, with the
  `WASMTIME_FEATURE_*` macro checked by `#ifndef`/`#error`. It uses `CMAKE_TRY_COMPILE_TARGET_TYPE
  STATIC_LIBRARY`, so nothing is linked.
- **find_package branch:** it calls the check. If any feature is missing, it sets `_wasmtime_reason` to
  "the installed Wasmtime X was built without features it needs: GC, THREADS" and creates no target, so
  the build falls back to WAMR.
- **Backstop:** the `#error` in `WasmtimeBackend.cpp` is kept.
- **Verification:** this machine has no system Wasmtime, so I used a scratch CMake project. It includes
  the function text extracted from the file and defines two IMPORTED INTERFACE targets:
  - the in-tree headers gave `full: missing=[]`;
  - a copy of the headers with `WASMTIME_FEATURE_GC` and `WASMTIME_FEATURE_THREADS` removed from
    `conf.h` gave `stripped: missing=[GC;THREADS]`.
- The real configure is unchanged: `Extension host engine: Wasmtime 48.0.10`, via CPM.

## 3. The WASI signature check is now tested, as a contract case

- **New case:** `WasmContract.wrongSignatureWasiIsUnsatisfied` in `BackendContract.hpp` loads `fd_seek`
  imported as `(param i32 i32 i32 i32) (result i32)`, an i32 offset where preview 1 has i64. It expects
  `Linkage::Unsatisfied`.
- **Exported memory:** the module also exports a memory. Without it, WAMR refuses the whole load ("a
  module with WASI apis must export memory by default") before it ever reaches linkage.
- **WAMR agrees:** `vtextension_test "WasmContract.wrongSignatureWasiIsUnsatisfied - WamrTraits"` passed,
  4 assertions. WAMR fails to resolve `fd_seek` against its `(iIi*)i` registration and leaves it
  unlinked. So this is a contract case, and both backends are held to it.
- **RED (Wasmtime):** I temporarily removed `&& function->signature() == import.signature` from
  `linkageOf`. The case then reported `BackendContract.hpp:501: FAILED`, with 4 assertions, 1 failed.
- **GREEN:** with the check restored, 4 assertions pass.

## 4. A zero stack is refused (Minor)

- **Change:** `WasmtimeRuntime::create` returns `RuntimeUnavailable` when `limits.stackBytes == 0`, before
  any configuration is built. The doc is updated to match.
- **New test:** `WasmtimeBackend.refusesAZeroStack`.
- **RED:** before the guard, the process aborted with
  `panicked at crates/c-api/src/engine.rs:37:38: ... max_wasm_stack size cannot be zero`.
- **GREEN:** passes.

## 6. Containment regex (Minor)

- **Change:** the Wasmtime pattern is now `#include\s*[<"]((wasmtime|wasm|wasi)\.h[>"]|wasmtime/)`.
- **New self-test case:** "a .hpp of the same stem is not Wasmtime" (`<wasm.hpp>` is not flagged).
- `check-wasm-runtime-containment.py --self-test` reports 28 cases OK, and the gate passes.

Minor 5 (KindState address reuse) is left alone, as instructed.

## Tests run after the fixes

| Command | Result |
|---|---|
| `vtextension_wasmtime_test "[wasmtime]"` | 3 test cases passed, 36 assertions |
| The 9 Step 4 cases | passed, 94 assertions |
| `vtextension_wasmtime_test` (whole binary) | 32 test cases: 17 passed, 15 failed |
| `vtextension_test` (WAMR) | all passed: 4057 assertions in 291 test cases |
| `ctest --preset=clang-asan -R 'vtextension\|containment'` | 3 of 4 passed |

The 15 whole-binary failures are the same instance cases as before; every failure is
`REQUIRE( instance.has_value() )`. In ctest, both containment gates and vtextension_test pass;
vtextension_wasmtime_test fails only on those instance cases.

clang-tidy:
`nice -n 19 clang-tidy -p out/clang-asan --quiet src/vtextension/wasm/wasmtime/WasmtimeBackend.cpp src/vtextension/wasm/wasmtime/WasmtimeBackend_test.cpp src/vtextension/wasm/wamr/WamrBackend_test.cpp`
reported 0 warnings. clang-format and editorconfig-checker are clean on every changed file.
