# Phase 1 fix wave: report

Base `38b58d1ec`. All nine findings are fixed. The work is in five commits, all `git commit -s`, with no trailers:

| Commit | Subject | Findings |
|---|---|---|
| `c9c1c00d1` | fix(vtextension): hold every table to one cap on both engines | 1 |
| `b22903ccc` | refactor(vtextension): tighten the runtime interface both engines share | 7, 8, 9 (code + comment) |
| `f1ac1f92f` | fix(vtextension): build the Wasmtime feature switches without constant addresses | 4 |
| `6fecac62b` | build: keep Wasmtime's C++ headers inside its backend too | 6 |
| `6f4f5ed56` | build: resolve the extension runtimes as CPM's local-package modes ask | 2, 3, 5, 9 (summary) |

## Finding 1: table limits (Important)

### Design

- **`WasmRuntime.hpp`**
  - `RuntimeLimits` stays a plain aggregate.
  - New field `uint32_t tableElements = MaxTableElements;`, documented. A table whose initial size is above it fails to instantiate with `OutOfMemory`. `table.grow` past it returns -1, as `memory.grow` does past `memoryBytes`.
  - New `MaxTableElements = 1024`. This is the most WAMR lets a small table grow to: max(2 x initial, `WASM_TABLE_MAX_SIZE` = 1024), and it cannot be raised. `createRuntime()` on both backends refuses a higher cap with `RuntimeUnavailable`. The interface doc says so.
  - New `MaxTables = 1`. The finding asked for Wasmtime's limiter to use tables = 1. With reference-types On, WAMR would accept a second table, so both engines now refuse a module that defines more than one, with `OutOfMemory`. Without this, the per-table cap bounds nothing: each extra table costs the guest 3 bytes of module.
  - I checked every built extension (`out/clang-asan/extensions/.../*.wasm`, 9 modules). Each defines exactly one table with initial == max (40–60), so the new rules reject none of them.
- **`WasmBinary.hpp`** (pure)
  - `TableSectionId`, `TableDeclaration`, `tableDeclarations(module)` and `TableShape { count, largestInitialElements, fitsWithin() }` with `tableShape(module)`.
  - Typed-reference, table64, shared and initializer-expression forms are `Malformed`. Each needs a feature that is Off in EngineFeatures.
- **Wasmtime**
  - The module computes `tableShape` at load (`InvalidComponent` if unreadable).
  - `WasmtimeInstance::create` checks it before instantiating: `OutOfMemory`, the same pattern as the unexported-memory check.
  - The limiter is now `wasmtime_store_limiter(store, memoryCap, tableElements, 1, MaxTables, 1)`.
  - `WasmtimeRuntime::create` refuses `tableElements > MaxTableElements`.
- **WAMR**
  - WAMR has no table-cap option.
  - New `wamr/TableCaps.{hpp,cpp}` (pure, internal, in the same style as HiddenExports): `capTableMaxima(bytes, cap)` gives every defined table a declared maximum ≤ cap before WAMR loads the module. Only the table section is rewritten, and only if a table needs it. Other sections are copied byte for byte. A second table section is `Malformed`.
  - With a declared maximum M ≤ 1024, WAMR's `adjust_table_max_size` leaves max = min(M, max(2·init, 1024)) = M. That is exactly what Wasmtime's limiter enforces.
  - `loadModule` stores `tableShape`. `instantiateModule` refuses before WAMR allocates (`OutOfMemory`). `WamrRuntime::create` refuses `tableElements > MaxTableElements`.
- **Every `RuntimeLimits` builder now sets the field explicitly:**
  - the contract's `Limits` (`.tableElements = 64`, below the maximum so a backend is seen to honour the cap it is given);
  - the five capped-memory builders in `BackendContract.hpp`;
  - `WamrBackend_test.cpp:120`;
  - `WasmtimeBackend_test.cpp:342`;
  - `Engine.cpp` (`wasm::MaxTableElements`).

### Contract cases (written first)

- **`tableGrowthIsCapped`.** Module `(table 40 funcref)` with no max. 40 is more than half the cap of 64, so WAMR's native 2 x initial / 1024 rule would let it grow past the cap. `grow(24)` returns 40. `grow(1)` returns -1. `grow(0xFFFFFFFF)` returns -1. `size` is 64.
- **`tableInitialSizeIsCapped`.** Module `(table 65 funcref)` fails to instantiate with `OutOfMemory`.
- **`tableCountIsCapped`.** Module `(table 1 funcref) (table 1 funcref)` fails to instantiate with `OutOfMemory`.
- **`tableCapAboveMaximumIsRefused`.** `tableElements = 1025` makes `createRuntime` fail with `RuntimeUnavailable`.

The modules were assembled with a scratch `wat2wasm` linking the pinned `libwasmtime` (`wasmtime_wat2wasm`).

**Unit tests:**
- `TableCaps.capTableMaxima`: 12 cases, covering unchanged, declared cap, lowered max, initial above cap left alone, section growth with neighbours copied, two tables, second section, typed ref, table64, truncated, and not a module.
- `WasmBinary.tableDeclarations` (8 cases) and `WasmBinary.tableShape`.

### RED, before any enforcement (field and constants only)

```
== vtextension_test (WAMR)
tableGrowthIsCapped              test cases:  1 |  0 passed | 1 failed   (grow(1) at 64 returned 64, not 0xffffffff)
tableInitialSizeIsCapped         test cases: 1 | 1 failed
tableCountIsCapped               test cases: 1 | 1 failed
tableCapAboveMaximumIsRefused    test cases: 1 | 1 failed
onlyI32ExportsAreOffered         test cases: 1 | 1 failed               (finding 8)
== vtextension_wasmtime_test
tableGrowthIsCapped              test cases:  1 |  0 passed | 1 failed
tableInitialSizeIsCapped         test cases: 1 | 1 failed
tableCountIsCapped               test cases: 1 | 1 failed
tableCapAboveMaximumIsRefused    test cases: 1 | 1 failed
onlyI32ExportsAreOffered         All tests passed (8 assertions in 1 test case)
```

WAMR's behaviour before the fix: it enforced its own cap of 1024 (or 2 x initial), not the configured one. It took a 65-element and a two-table module, and it accepted a cap of 1025.

### GREEN

On both binaries:

- `WasmContract.table*`: all tests passed, 26 assertions in 4 test cases.
- `TableCaps.*`: 12 assertions (WAMR suite).
- `WasmBinary.*`: 53 assertions in 6 test cases.

## Finding 2: CPM local-package modes

- The Wasmtime block in `cmake/ContourThirdParties.cmake` was restructured. In these cases an installed Wasmtime is looked for with `find_package` and probed first:
  - without CPM;
  - with no archive row for the platform;
  - when `CPM_USE_LOCAL_PACKAGES` or `CPM_LOCAL_PACKAGES_ONLY` is set.
- The pinned archive is fetched only when no usable Wasmtime is resolved and `CPM_LOCAL_PACKAGES_ONLY` is off.
- During that one `CPMAddPackage`, `CPM_USE_LOCAL_PACKAGES` is turned off (saved and restored). Otherwise CPM's own lookup would hand back an installed Wasmtime that the probe just rejected.
- A rejected installed Wasmtime is reported with a status line ("Installed Wasmtime rejected (...); fetching the pinned archive instead.").

**Verification.** No Wasmtime is installed system-wide, so I used a scratch CMake project in the scratchpad (`cmk/`):

- It includes the real CPM.cmake (`out/clang-asan/cmake/CPM.cmake`) and the Wasmtime block, extracted verbatim from the old file (`git show HEAD:`) and from the new one.
- A scratch prefix holds three fake installed Wasmtimes. Each has a `wasmtimeConfig.cmake` and a version file for 48.0.10, over a copy of the pinned headers:
  - `full`;
  - `nowat`: `conf.h` without `WASMTIME_FEATURE_WAT`;
  - `nogc`: without `WASMTIME_FEATURE_GC`.
- Fetch scenarios pass `-DCPM_wasmtime_SOURCE=<out/clang-asan/_deps/wasmtime-src>`, so the archive is not downloaded again.

| block | scenario | result |
|---|---|---|
| old | `CPM_LOCAL_PACKAGES_ONLY=ON`, none installed | **rc=1**, `CMake Error at CPM.cmake:824: CPM: wasmtime not found via find_package(wasmtime 48.0.10)` (RED) |
| old | `CPM_USE_LOCAL_PACKAGES=ON`, full installed | rc=0 but `Extension host engine: WAMR (CPM resolved Wasmtime 48.0.10 without fetching its archive)`: discarded (RED) |
| new | `CPM_LOCAL_PACKAGES_ONLY=ON`, none | rc=0, `not available (CPM_LOCAL_PACKAGES_ONLY is ON and no Wasmtime 48 is installed)`, WAMR fallback |
| new | `CPM_LOCAL_PACKAGES_ONLY=ON`, full | rc=0, `system package (Wasmtime 48.0.10)` |
| new | `CPM_LOCAL_PACKAGES_ONLY=ON`, nogc | rc=0, `not available (the installed Wasmtime 48.0.10 was built without features it needs: GC)` |
| new | `CPM_USE_LOCAL_PACKAGES=ON`, full | rc=0, `system package (Wasmtime 48.0.10)`: probed and used |
| new | `CPM_USE_LOCAL_PACKAGES=ON`, nogc | rc=0, "Installed Wasmtime rejected (... GC); fetching the pinned archive instead.", then CPM, prebuilt |
| new | `CPM_USE_LOCAL_PACKAGES=ON`, none | rc=0, CPM fetch |
| new | `CONTOUR_USE_CPM=OFF`, full / none | system package / `not available (CONTOUR_USE_CPM is OFF and no Wasmtime 48 is installed)` |
| new | default CPM (full installed, no local vars) | CPM fetch (the installed one is not consulted, as before) |

The real tree also reconfigures cleanly (`cmake --preset clang-asan -DPACE_SCHEDULER=127.0.0.1:6674`) and still takes the CPM path: `CPM: Adding package wasmtime@48.0.10`.

## Finding 3: WAT for the tests

I chose to gate the test target, which is the second option the finding offered. "Add WAT to the required list for the test target" would have made a testing build reject a production-usable Wasmtime, so the engine choice would depend on whether tests are on. Gating keeps production free of WAT.

- `_WASMTIME_TEST_FEATURES WAT` sits next to the required list, with a comment.
- The probe loop was factored out as `ContourThirdParties_WasmtimeMissingFeatures(target var features...)`. `..._WasmtimeUnusableReason` uses it for the required list.
- An installed, usable Wasmtime also sets `CONTOUR_WASMTIME_MISSING_TEST_FEATURES`. It is empty for the pinned archive.
- In `src/vtextension/wasm/wasmtime/CMakeLists.txt`, when that list is non-empty, `vtextension_wasmtime_test` is not created. A status line names the missing features.

**Verification:**
- The scratch scenarios `uselocal-nowat` and `nocpm-nowat` both give `system package (Wasmtime 48.0.10) missingTest=[WAT]`. `full` gives `[]`.
- The gating condition, run as `cmake -P` over both values, gives `[] test target built` and `[WAT] skipped: lacks WAT`.
- The real tree still builds and runs `vtextension_wasmtime_test`.

## Finding 4: dllimport addresses in a constexpr table

`FeatureSetters` is now `auto const featureSetters` at namespace scope. clang-tidy's naming rule requires camelBack for a non-constexpr constant.

On Windows, a dllimport address is not a constant expression. The table is then dynamically initialized before `main()`, which is legal, and it is only read after `main()`, when an engine is configured. So there is no init-order hazard.

The consistency check (`featureSettersMatchTheTable`) can no longer be a `static_assert` over a non-constant table. It is now a plain function, and `engineConfig()` runs `Require(featureSettersMatchTheTable())` before it configures each engine:
- It costs about 25 row comparisons per runtime creation.
- Every Wasmtime test configures engines through this path, so a mismatch would fail the suite at once.
- The guarantee is unchanged; it is now caught at run time instead of compile time.

**Syntax check with the Windows cross toolchain.** I ran `clang-cl --target=x86_64-pc-windows-msvc /winsysroot /opt/msvc /std:c++latest /EHsc /Zs` with the tree's include paths. `LIBWASM_STATIC` was not defined, so `WASM_API_EXTERN` is `__declspec(dllimport)`.
- On the previous version of the file:
  ```
  WasmtimeBackend.old.cpp(97,20): error: constexpr variable 'FeatureSetters' must be initialized by a constant expression
  WasmtimeBackend.old.cpp(141,19): error: static assertion expression is not an integral constant expression
  ```
  exit 1 (RED, which reproduces the finding).
- On the new file: exit 0, no diagnostics.

## Finding 5: stale WAMR rejection messages

**`ContourThirdParties_WamrTerminates`**
- It now looks reasons up in a table, `_WAMR_PROBE_REASONS`, of `<exit code>|<reason>` rows for the probe's codes 2–11. `@FEATURE@` is replaced by the feature the probe names.
- Cases with a reason of their own:
  - compile failure: "the probe does not compile and link against it";
  - cross-compiling without an emulator: "it cannot be probed when cross-compiling without an emulator";
  - an unknown exit: "the probe ended unexpectedly (<code>)".
- Codes 2–5 used to be reported as "did not stop a call". They now say that the runtime does not initialize, load, instantiate, or set up a call.

**Messages**
- The fatal error now states the whole rule: thread manager, no hardware bounds checks or blocking-op wakeup, and exactly the features EngineFeatures.hpp allows. It also says either "The installed WAMR was rejected: <reason>." or "None is installed."
- `_wamr_rejected` now holds the bare reason. The third-party summary still appends "; system WAMR rejected: ...".
- The status line no longer claims "building WAMR from source instead" when CPM is off.

**Verification.** I used a scratch project in the scratchpad (`wamr/`) with the extracted WAMR block, old and new, `CONTOUR_USE_CPM=OFF`, and fake `iwasm::vmlib` packages:
- a stub library whose `wasm_runtime_init` fails (exit 2);
- a stub that loads every module, so it accepts simd (exit 10);
- a package with no headers, so the probe does not compile.

- **Old block:** all three gave "wasm_runtime_terminate() did not stop a call (built without WAMR_BUILD_THREAD_MGR?)" for init/noheaders. The fatal error named only THREAD_MGR and bounds checks, without the reason.
- **New block:**
  - `init`: "The installed WAMR was rejected: its runtime does not initialize."
  - `load`: "... rejected: it loads a module using simd, which extensions may not use."
  - `noheaders`: "... rejected: the probe does not compile and link against it."
  - Each comes with the full rule text.

## Finding 6: containment lint misses `.hh`

- The pattern is now `(wasmtime|wasm|wasi)\.hh?[>"]`, and the docstring lists the C++ API headers.
- Three self-test cases were added: `<wasmtime.hh>`, `"wasm.hh"`, `<wasmtime/store.hh>`.
- `<wasm.hpp>` is still not matched; the existing case covers it.

| | Result |
|---|---|
| RED, new cases before the fix | `FAILED: Wasmtime's C++ header wasmtime.hh`, `FAILED: ... wasm.hh`, `2 of 31 cases failed` |
| GREEN | `31 cases OK`; the tree lint reports "every WebAssembly runtime is contained" |

## Finding 7: `ShimEnvironment` could be built unusable

`ShimEnvironment` is now a class.
- Its constructor `ShimEnvironment(Clock clock, Random random)` takes both callbacks. The aliases are `Clock = std::function<uint64_t(ClockId)>` and `Random = ...`.
- The constructor checks `Require(_clock && _random)`. An empty callback is a programmer error, so it ends the process at construction instead of later, inside a noexcept guest call.
- Accessors: `clock(ClockId)` and `random(span)`. `clock` takes `ClockId` instead of `uint32_t`.

Call sites updated: `Preview1Shim.cpp` (it now passes the `ClockId` directly), `SystemEnvironment.cpp` (`readClock(ClockId)`), `Preview1Shim_test.cpp`, and `WasmtimeBackend_test.cpp`.

I did not add a test for the refusal. `core::detail::fail` is `noexcept` and aborts, so it cannot be observed in-process with Catch2. The existing shim and Wasmtime WASI tests cover the new API.

## Finding 8: `function()` narrowing

- New `FunctionSignature::isAllI32()` in the interface.
- `IWasmInstance::function()` now documents that only all-i32 exports are offered, and why: `call()` passes u32 cells, and every export the host calls is all-i32.
- WAMR now filters to these exports as well. Wasmtime uses the shared predicate; its local `allI32` helper is removed.
- New contract case `onlyI32ExportsAreOffered`. Module: `wide (i64)->i64` and `narrow (i32)->i32`. `function("wide")` gives nothing; `narrow` works.

| Engine | Before the fix | After |
|---|---|---|
| WAMR | RED: `CHECK_FALSE( instance->function("wide").has_value() )` failed | GREEN, 8 assertions |
| Wasmtime | already passed (8 assertions) | GREEN, 8 assertions |

## Finding 9: cleanups

- **`MaxHostParameters`**
  - It is now a single constant in `WasmRuntime.hpp`, with the Canonical ABI rationale.
  - It was removed from `WasmtimeBackend.hpp` and `WamrRuntime.hpp`. The backends' unqualified uses resolve to `wasm::MaxHostParameters`, and the tests now name `vtextension::wasm::MaxHostParameters`.
  - The `HostFunction` doc no longer names WAMR's copy.
- **`EngineFeatures.hpp` comment.** It now says that Wasmtime is configured from the table by the backend, and that WAMR's build options are checked against it by the backend contract, which names features by their row's name. It no longer mentions a world check.
- **Configure summary**
  - The variable is renamed `CONTOUR_EXTENSION_HOST_ENGINE_STRING` → `CONTOUR_WASMTIME_STRING`.
  - The line now reads `|> Wasmtime for the extension host: Wasmtime 48.0.10 (available; only its own tests use it so far)`, or `not available (<reason>)`.
  - The status line is `Wasmtime for the extension host: ...`.
  - The line above it, `|> WebAssembly runtime: wamr`, still names what extensions actually run on.

## Final gates

- Build: `flock -o /tmp/contour-extension-host.build.lock nice -n 19 ionice -c 3 cmake --build --preset clang-asan -j 4 --target vtextension_test vtextension_wasmtime_test`. Exit 0 with no warnings (`-Werror`).
- `out/clang-asan/src/vtextension/vtextension_test` (WAMR): **All tests passed (4618 assertions in 325 test cases)**, 13.4 s.
- `out/clang-asan/src/vtextension/wasm/wasmtime/vtextension_wasmtime_test`: **All tests passed (1003 assertions in 50 test cases)**.
- `ctest --preset=clang-asan -R 'vtextension|check_wasm_runtime_containment'`: **100% tests passed, 0 tests failed out of 4**. These are `check_wasm_runtime_containment`, `check_wasm_runtime_containment_selftest`, `vtextension_test` and `vtextension_wasmtime_test`.
- clang-tidy, run by hand over every `.cpp` changed in the series (15 files). It reported no warnings or errors, with the same checks as the pedantic build (`.clang-tidy`, `WarningsAsErrors`):
  ```
  clang-tidy -p out/clang-asan --quiet $(git diff --name-only 38b58d1ec..HEAD -- '*.cpp')
  ```
  The headers were linted through `HeaderFilterRegex`. During the work this caught `FeatureSetters` → `featureSetters` (readability-identifier-naming).
- `/usr/bin/clang-format --dry-run -Werror` on every changed `.cpp`/`.hpp`: clean. `editorconfig-checker` on every changed file: clean.

## Deviations and concerns

1. **`MaxTables = 1` is a new module-shape rule beyond the two cases the finding names.** It follows from the finding's tables = 1 for Wasmtime: WAMR must agree or the engines diverge. All nine bundled or test extensions have one table. A future toolchain that emits a second table (for example wasm-bindgen's externref table) would be refused on both engines with `OutOfMemory`.
2. **WAMR enforces growth by rewriting the table section before load.** The rewrite assumes `WASM_TABLE_MAX_SIZE` ≥ 1024, which is WAMR's default and the value in our CPM build. A system WAMR built with a smaller value would cap growth below `tableElements`, which is stricter, never looser. Every module whose tables all declare a maximum within the cap is passed through byte for byte. That includes all of today's extensions.
3. **Finding 3 gates the test target** rather than requiring WAT; the reasoning is under finding 3 above.
4. **Finding 4 moves the table/switch consistency check** from a `static_assert` to a `Require` at engine configuration. The finding chose the non-constexpr table, which makes this unavoidable.
5. **Finding 7's refusal is a `Require`, which aborts the process.** It has no in-process test. The finding's other option, a `RuntimeUnavailable` from `WasmtimeRuntime::create`, would have left the type constructible in an unusable state.
6. The local plan doc (`docs/superpowers/plans/...md:146,149`) still mentions the old `Extension host engine:` status line. I did not touch it, per the run rules.
7. **Commit `b22903ccc` bundles three findings:** the shim API (7), `function()` narrowing (8), and the constants and comment cleanups (9).

---

## Re-review fixes (I1, M1, M2, size re-check)

Commit: `19124c9e6` fix(vtextension): read a module's tables once, and as strictly as Wasmtime.

### I1: trailing bytes in the table section (Important)

**Changes in `WasmBinary.hpp`:**
- `tableDeclarations(module, maxTables)` returns `Malformed` unless the reader is at the end of the table section after the last table.
- It also returns `Malformed` for a second table section. That check used to live in `capTableMaxima`.

**Tests:**
- New contract case `tableSectionWithTrailingBytesIsRefused`. Its module, `TrailingBytesTableModule`, is the reviewer's example: table section `04 05 01 70 00 02 ff`. `IWasmRuntime::load` must fail with `InvalidComponent` on both engines.
- New unit cases:
  - `WasmBinary.tableDeclarations` "bytes after the last table" → `Malformed`.
  - `TableCaps.capTableMaxima` "bytes after the last table" → `Malformed`. The test now reads the tables and rewrites the module the way the backend does.

### M1: tables read once, count refused early

**Single read.**
- `tableDeclarations(module, maxTables)` is the only table reader. It checks the count before it reserves space or reads any entry: `TooMany`, a new `BinaryError`.
- `HiddenExports` maps `TooMany` to `Malformed`. Its reader never produces it.
- `TableShape`, `tableShape()` and `fitsWithin()` are gone. Modules store only `largestInitialElements(tables)`.
- `capTableMaxima(module, tables, cap)` takes the declarations already read and does not parse the table section again. It still splits the module into sections, which are few, to find the table section.

**Count check moves to load.** More than `MaxTables` tables is now refused when the module is loaded (`InvalidComponent`), on both backends, before the engine reads the module:
- Wasmtime: before `wasmtime_module_new`.
- WAMR: before `wasm_runtime_load`.

Before this fix the count was checked at instantiate (`OutOfMemory`). It moved because refusing early was the request, and because both engines must refuse at the same step. WAMR now refuses on the count alone at load. If Wasmtime still loaded the module and refused it only at instantiate, the two would disagree on the step and the error.

As a result:
- `tableCountIsCapped` now expects a load refusal with `InvalidComponent`.
- The `MaxTables` doc in `WasmRuntime.hpp` is updated.
- The instance checks keep only the initial-size cap (`OutOfMemory`).
- The Wasmtime store limiter keeps `tables = MaxTables` as a backstop.

**Unit cases.** `WasmBinary.tableDeclarations` adds:
- "more tables than are read, refused by the count alone": count 3, then `ff ff ff ff` junk, gives `TooMany`. This shows no entry is read.
- "a second table section".

`TableCaps` adds "more tables than are read".

`WasmBinary.tableShape` is replaced by `WasmBinary.largestInitialElements`.

### M2: one-table note

A comment on the ReferenceTypes row in `EngineFeatures.hpp` says that reference-types is On, but `MaxTables` in `WasmRuntime.hpp` limits every engine to one table. The `MaxTables` doc points back to it.

### Size re-check

After the rewrite, WAMR's `loadModule` refuses a module whose size no longer fits a `uint32_t` (`InvalidComponent`). Declaring a maximum can grow the module, and WAMR cannot take a module larger than that. This has no test: it needs a module of more than 4 GiB.

### RED

Taken with the contract cases added and before any code change:

```
== vtextension_test (WAMR)
tableCountIsCapped                       test cases: 1 | 1 failed
tableSectionWithTrailingBytesIsRefused   test cases: 1 | 1 failed     (WAMR loaded the junk-trailing module)
== vtextension_wasmtime_test
tableCountIsCapped                       test cases: 1 | 1 failed
tableSectionWithTrailingBytesIsRefused   All tests passed (3 assertions in 1 test case)   (Wasmtime already refused it)
```

### GREEN

Build:

```
flock -o /tmp/contour-extension-host.build.lock nice -n 19 ionice -c 3 cmake --build --preset clang-asan -j 4 --target vtextension_test vtextension_wasmtime_test
```

Exit 0, no warnings.

Targeted cases:

| Filter | vtextension_test (WAMR) | vtextension_wasmtime_test |
|---|---|---|
| `WasmContract.table*` | 27 assertions in 5 test cases | 27 assertions in 5 test cases |
| `TableCaps.*` | 14 assertions | — |
| `WasmBinary.*` | 52 assertions in 6 test cases | — |
| `HiddenExports*` | 76 assertions in 10 test cases | — |

Full binaries:
- `vtextension_test`: **All tests passed (4620 assertions in 326 test cases)**, 13.3 s.
- `vtextension_wasmtime_test`: **All tests passed (1004 assertions in 51 test cases)**.

ctest:

```
ctest --preset=clang-asan -R 'vtextension|check_wasm_runtime_containment'
```

**100% tests passed, 0 tests failed out of 4.**

Lint and formatting:
- clang-tidy, run by hand over the commit's changed `.cpp` files plus `WasmtimeBackend.cpp`: no warnings or errors.
  ```
  clang-tidy -p out/clang-asan --quiet src/vtextension/wasm/WasmBinary_test.cpp \
    src/vtextension/wasm/wamr/{HiddenExports,TableCaps,TableCaps_test,WamrInstance,WamrModule}.cpp \
    src/vtextension/wasm/wasmtime/{WasmtimeInstance,WasmtimeModule,WasmtimeBackend}.cpp
  ```
- `clang-format --dry-run -Werror` and `editorconfig-checker` on the changed files: clean.

### Note

The contract no longer has a case where a module with two tables loads and then fails at instantiate. That behaviour is gone by design: the count is now refused at load.
