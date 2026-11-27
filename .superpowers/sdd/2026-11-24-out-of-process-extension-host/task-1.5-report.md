# Task 1.5 report: our WASI preview-1 shim, shared by every engine

Status: DONE. Commit `c9b6169d1 feat(vtextension): our own WASI preview-1 functions, identical under every engine`
on `feature/out-of-process-extension-host` (not pushed).

## What changed

**New, engine-independent (in the `vtextension` library proper, no engine header):**
- `src/vtextension/wasm/wasi/Preview1Shim.hpp/.cpp`
  - `ClockId`, `ShimEnvironment { clock, random }`, `Errno`, `ShimOutcome`, `ShimResult { outcome, error }`,
    `ShimCall`, `ShimFunction { Preview1Function preview1; ShimCall call; }`.
  - The table is a `constexpr std::array` built by `consteval shim(name, call)`, which copies each row's
    name and signature out of `Preview1Functions` (Preview1Signatures.hpp) via `findPreview1Function` — a
    misspelt name is a compile error. A `static_assert` checks the names equal `AllowedWasiFunctions` in order.
    `Preview1Signatures.hpp` is untouched; it stays the one signature table (R2).
  - Exposed as `shimFunctions()` (span) and `findShimFunction(name)`.
  - Every guest access goes through `range(memory, address, size) -> expected<span<byte>, Errno>` (Fault);
    functions chain with `and_then`/`transform`. Multi-output functions (`args_sizes_get`, ...) check every
    range before writing any, so a fault leaves memory untouched.
  - Semantics: as the brief; plus — `fd_close` on 0–2 → Success and the stream stays usable, ≥3 → Badf;
    `fd_fdstat_get` → filetype character device (2), flags 0, rights_base = fd_seek|fd_write (`0b0100'0100`),
    inheriting 0; `fd_seek` whence > 2 → Inval; `fd_write` sums buffer lengths, each buffer bounds-checked
    (Fault), total > u32 → Inval; `clock_*`: realtime/monotonic from the environment (resolution 1 ns),
    process/thread CPU-time → **Nosys** (valid p1 ids the host withholds), any other id → Inval (data table
    `Clocks`); `fd_prestat_*` → Badf without touching memory (no descriptor has a prestat).
- `src/vtextension/wasm/wasi/SystemEnvironment.hpp/.cpp` — `systemEnvironment()`: `system_clock` /
  `steady_clock`; random via `getrandom` (Linux, EINTR loop), `arc4random_buf` (BSD/macOS),
  `BCryptGenRandom` (Windows; `bcrypt` linked privately on WIN32). A refusing source → `std::terminate`
  (documented: never quietly predictable bytes).
- `src/vtextension/CMakeLists.txt` — sources, headers, test file.

**Wasmtime (`src/vtextension/wasm/wasmtime/`):**
- `WasiBindings`/`unimplementedWasiFunction` removed. `makeLinker` defines every `shimFunctions()` row
  under `wasi_snapshot_preview1` with `callShimFunction` (env = the row; `const_cast` for Wasmtime's
  `void*`, commented). The callback reads args (i32 zero-extended, i64 as is), the caller's exported
  `memory` (none → empty span → pointers fault), calls the row with `StoreData::wasiEnvironment`, writes
  the errno result; `Exit` → `wasmtime_trap_new("proc_exit")`, nothing stashed, so the instance reports
  `CallFailure::Trap`. `wasmtime_linker_define_wasi` is never used.
- DI: `WasmtimeConfig::wasiEnvironment` (default `wasi::systemEnvironment()`), moved into
  `WasmtimeRuntime::_wasiEnvironment`; `StoreData` gains `wasiEnvironment` (a reference into the runtime,
  which every instance outlives through module → runtime).
- `engineConfig` takes the config by const&; config moved, not copied, through the backend.

**Contract (`src/vtextension/wasm/testing/BackendContract.hpp`):**
- `EveryWasiFunctionModule` (WAT in the doc comment): imports all 15, one export per function named after
  it, in-bounds arguments at address 64, returns the errno. `fd_close` targets fd 9 (closing a real stdio
  slot under WAMR could reach the shared null-device handle); prestat functions ask fd 3.
- `EveryWasiFunctionOutcomes` table (reuses `wasi::ShimResult`), `static_assert`ed against the allow list.
- `everyAllowedWasiFunctionWorks` — a fresh instance per function; proc_exit → `CallFailure::Trap`.
  Registered in `CONTOUR_WASM_BACKEND_CONTRACT` (all platforms).

**Extra test:** `WasmtimeBackend.wasiReadsTheConfiguredEnvironment` (`[wasmtime]`) — a deterministic
`ShimEnvironment` in `WasmtimeConfig` reaches the guest (clock value and random bytes read back from
memory). `WasmtimeTraits` gained `kind()` so this test shares the binary's one kind state.

## Module bytes
Assembled and validated with a scratch tool linking the pinned
`out/clang-asan/_deps/wasmtime-src/lib/libwasmtime.a` (`wasmtime_wat2wasm` + `wasmtime_module_validate`),
which also emitted the byte array.

## Deviations
1. **Name: `shimFunctions()` / `findShimFunction()`, not `extern ... Preview1Functions`.** `Preview1Functions`
   already names the signature table in the same namespace; and a non-constexpr global const would trip
   the naming rule (ConstantCase camelBack) — a function returning the span avoids both.
2. **`ShimFunction` holds `Preview1Function preview1`, not `std::string_view name; FunctionSignature
   signature`.** `FunctionSignature` holds vectors and cannot sit in a constexpr table; the row carries
   name and signature from the one signature table (`preview1.name`, `preview1.signature()`).
3. **`Errno : uint8_t` and `ClockId : uint8_t`** (brief: `uint16_t`). clang-tidy `performance-enum-size`
   is an error otherwise; every p1 errno fits a byte. The wire widths are noted in the doc comments.
4. **`range()` takes a `uint64_t size`** (brief: `uint32_t`), so `iovsLength * 8` cannot overflow.
5. **`fd_write` total > u32 → `Inval`**, `fd_close` on 0–2 → `Success`; the brief does not specify these.
6. **Production default is in `WasmtimeConfig`'s member initializer**; the contract traits use it (WAMR's
   contract can only run on real clocks too); deterministic environments are used by the shim unit tests and
   the new `[wasmtime]` DI test.

## TDD evidence
- **RED:** `vtextension_test` with the test file registered and no implementation: configure failed —
  `Cannot find source file` (Preview1Shim.cpp/SystemEnvironment.cpp, headers absent).
- **GREEN:** `vtextension_test "Preview1Shim.*"` → All tests passed (384 assertions in 19 test cases).
- **Contract RED by mutation:** with `callShimFunction` made to trap (the old stub behaviour),
  `everyAllowedWasiFunctionWorks` and `wasiStdioGoesNowhere` on Wasmtime: `test cases: 2 | 0 passed |
  2 failed`. Restored → both pass.

## Test results (real counts, clang-asan)
| Run | Result |
|---|---|
| `vtextension_test "Preview1Shim.*,[wasm-contract]"` (WAMR) | 787 assertions in 49 test cases, all passed |
| `vtextension_test` (whole WAMR suite) | 4504 assertions in 311 test cases, all passed |
| `vtextension_wasmtime_test "~WasmContract.interruptStopsLoop*"` (contract + `[wasmtime]`) | 481 assertions in 35 test cases, all passed — incl. `allowListedWasiIsSatisfied`, `wasiStdioGoesNowhere`, `everyAllowedWasiFunctionWorks` |
| `ctest --preset=clang-asan -R vtextension --timeout 180` | 1/2 passed: vtextension_test passed; vtextension_wasmtime_test **Timeout** (interruptStopsLoop hang, owned by 1.6) |

WAMR passed `everyAllowedWasiFunctionWorks` with no Traits hook; recorded under "### Phase-1 facts" (Task 1.5)
in the local plan, including which behaviours are deliberately *not* asserted across engines.

## Tooling
- clang-tidy, 0 diagnostics:
  `nice -n 19 clang-tidy -p out/clang-asan --quiet src/vtextension/wasm/wasi/Preview1Shim.cpp src/vtextension/wasm/wasi/SystemEnvironment.cpp src/vtextension/wasm/wasi/Preview1Shim_test.cpp src/vtextension/wasm/wasmtime/WasmtimeBackend.cpp src/vtextension/wasm/wasmtime/WasmtimeInstance.cpp src/vtextension/wasm/wasmtime/WasmtimeBackend_test.cpp src/vtextension/wasm/wamr/WamrBackend_test.cpp`
  (headers via HeaderFilterRegex; first run found performance-enum-size ×2 and
  readability-use-concise-preprocessor-directives ×4, fixed).
- `-Werror` build clean (`vtextension_test`, `vtextension_wasmtime_test`, `vtextension_public_headers_test`).
- `/usr/bin/clang-format --dry-run -Werror`, `editorconfig-checker`, `scripts/check-wasm-runtime-containment.py`: clean.

## Self-review notes
- Lifetimes: callback env points into a static constexpr table; `StoreData::wasiEnvironment` references the
  runtime, which instances outlive via module → runtime. `config.wasiEnvironment` is moved only after
  `engineConfig(config, …)` has used `config`.
- `callShimFunction` is noexcept; a throwing `ShimEnvironment` function would terminate — documented on
  `ShimEnvironment` (must not throw).

## Concerns
- **Lockdown and `getrandom`.** `systemEnvironment()` terminates if the random source refuses (e.g. a seccomp
  filter denying `getrandom`). The helper's lockdown must allow `getrandom` (Linux) / BCrypt (Windows), or
  the environment must be built from a pre-lockdown source.
- **Windows/macOS paths are uncompiled here** (only the Linux branch was built); `bcrypt` linkage and the
  `arc4random_buf` branch are untested locally.
- **ctest still times out** on vtextension_wasmtime_test until 1.6 (interruptStopsLoop).
- The shim is stateless: `fd_close` does not remove a standard stream. Documented and tested; differs from
  WAMR (not asserted in the contract).
