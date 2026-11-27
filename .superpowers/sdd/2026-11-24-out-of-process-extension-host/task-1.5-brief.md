### Task 1.5: Our WASI preview-1 shim, shared by every engine

**Files:**
- Create: `src/vtextension/wasm/wasi/Preview1Shim.hpp`, `Preview1Shim.cpp`, `Preview1Shim_test.cpp`
- Modify: `src/vtextension/wasm/wasmtime/WasmtimeBackend.cpp` (define the shim functions in each linker)
- Modify: `src/vtextension/wasm/testing/BackendContract.hpp` (new case `everyAllowedWasiFunctionWorks`)

**Interfaces:**
- Produces:
  ```cpp
  namespace vtextension::wasm::wasi {
  /// Injected sources, so tests are deterministic.
  struct ShimEnvironment { std::function<uint64_t(uint32_t clockId)> clock; std::function<void(std::span<std::byte>)> random; };
  enum class Errno : uint16_t { Success = 0, Badf = 8, Fault = 21, Inval = 28, Nosys = 52 /* p1 values */ };
  enum class ShimOutcome : uint8_t { Returned, Exit };  // Exit: proc_exit, the engine traps
  struct ShimResult { ShimOutcome outcome; Errno error; };
  struct ShimFunction { std::string_view name; FunctionSignature signature;
                        ShimResult (*call)(ShimEnvironment const&, std::span<std::byte> memory, std::span<uint64_t const> args); };
  /// One row per AllowedWasiFunctions entry, same order; a static_assert checks the names match.
  extern std::span<ShimFunction const> const Preview1Functions;
  }
  ```
  Semantics: no arguments and no environment (sizes 0); fds 0–2 exist as character devices; `fd_write`
  reports every byte written and writes nowhere; `fd_prestat_get` → `Badf`; `fd_seek` on 0–2 → offset 0;
  `clock_*` from `ShimEnvironment.clock`; `random_get` from `.random`; `sched_yield` → `Success`;
  `proc_exit` → `Exit`. Every guest pointer is bounds-checked against `memory` → `Fault`.

- [ ] **Step 1: Write the unit tests.** In `Preview1Shim_test.cpp`, one `TEST_CASE` per function, named
  `"Preview1Shim.<function>"`, against a 64 KiB `std::vector<std::byte>`. Include
  `"Preview1Shim.outOfBoundsPointerFaults"` (each pointer-taking function with `address = size - 2`
  → `Errno::Fault`), and `"Preview1Shim.tableMatchesAllowList"`, which checks the names equal
  `AllowedWasiFunctions` in order.
- [ ] **Step 2: Run; expect a compile failure.** Run: `.../vtextension_test "Preview1Shim.*"`.
- [ ] **Step 3: Implement `Preview1Shim.cpp`.** Implement it as a constexpr-built table. Each function
  reads and writes guest memory through one bounds-checked helper,
  `std::expected<std::span<std::byte>, Errno> range(std::span<std::byte>, uint32_t address, uint32_t size)`.
- [ ] **Step 4: Bind it in Wasmtime.** For each row, `wasmtime_linker_define_func` under
  `wasi_snapshot_preview1`. The callback fetches the memory from the caller, calls the row, and turns
  `Exit` into a trap. The composition passes a `ShimEnvironment` from real clocks and `getrandom` (DI:
  through `WasmtimeConfig`, with a test default).
- [ ] **Step 5: Add the contract case `everyAllowedWasiFunctionWorks`.** Add a hand-assembled module
  importing all 15 functions with one export per function, each calling its import with in-bounds
  arguments and returning the errno. The case asserts the expected errno per function (proc_exit:
  `CallFailure::Trap`). Register it in the suite.
- [ ] **Step 6: Run the shim tests and the whole suite on both engines.** Run:
  `.../vtextension_test "Preview1Shim.*" "[wasm-contract]"` and
  `.../vtextension_wasmtime_test "[wasm-contract]"`.
  Expected: PASS, including `allowListedWasiIsSatisfied` and `wasiStdioGoesNowhere` on Wasmtime.
  If WAMR fails `everyAllowedWasiFunctionWorks`, record the difference in "Phase-1 facts" and assert the
  WAMR value where it is a valid p1 answer.
- [ ] **Step 7: Commit.** `git commit -s -m "feat(vtextension): our own WASI preview-1 functions, identical under every engine"`

