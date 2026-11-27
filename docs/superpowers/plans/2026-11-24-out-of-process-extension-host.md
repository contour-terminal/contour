# Out-of-Process Extension Host Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Run every extension's guest code compiled (Wasmtime) inside its own sandboxed
`contour-extension-host` process, so that extensions get several times faster, and a crash, hang or
exploit stays inside the one extension that caused it.

**Architecture:** A new parent-side `RemoteBackend` implements the existing `IWasmBackend` seam by
forwarding over per-lane channels to one helper process per extension. The helper drives an
`IHostedEngine`: Wasmtime, or WAMR where Wasmtime is unavailable. Guest memory is a parent-created shared
object whose size the parent controls. A supervisor thread owns every process. Each helper locks itself
down with the platform's OS sandbox before it reads guest code.

**Tech Stack:** C++23, Catch2, Wasmtime C API (pinned LTS, prebuilt `c-api` archive), WAMR 2.4.5,
CMake presets, seccomp-BPF + Landlock, Capsicum, Seatbelt, AppContainer.

**Spec:** `docs/superpowers/specs/2026-11-24-out-of-process-extension-host-design.md` (revision 3,
approved 2026-11-24). Section numbers (§5.4) refer to it. Executors read both documents. Neither is ever
referenced from a commit message or from source, and neither is committed.

## How this plan is layered

Phase 1 is step level and executable now. Phases 2–6 are task level. Each opens with "Task N.0: expand
this phase to step level", which waits on the previous phase's gate, because later phases depend on facts
earlier ones produce (Wasmtime API details, measured latencies). Task N.0 may start while the previous
gate builds; implementation may not.

| Phase | Delivers | Default composition after it |
|---|---|---|
| 1 | `WasmtimeBackend` in-process (tests only), our WASI shim, the shared engine feature table | unchanged (in-process WAMR) |
| 2 | Protocol, `HostServer`, `RemoteBackend` over a socketpair (copy-path memory); identity, registry, breaker, new errors | unchanged |
| 3 | `contour-extension-host` executable, supervisor, launcher, process handles, interrupt wake, backstop, timeouts, off-thread teardown | unchanged (out-of-process selectable, unsandboxed: tests only) |
| 4 | Parent-created shared memory, parent-authoritative growth, single-fetch decoding | unchanged |
| 5 | Sandboxes (Linux, FreeBSD, macOS, Windows), `WamrEngine` in the helper, sanitizer rows | **out-of-process** on each platform as its sandbox lands |
| 6 | Compile cache, packaging, lint rules, docs, release note, the measurement gates | out-of-process everywhere |

## Global Constraints

- Work happens in the worktree `/home/christianparpart/projects/contour-extension-host`, branch
  `feature/out-of-process-extension-host`, based on `origin/master`. Never in the main checkout.
- The user-facing name is `contour-extension-host`. User-facing text (docs, `--help`, log messages a user
  reads, config keys) never says "wasm", "WAMR" or "Wasmtime". Internal code may.
- The GUI and daemon processes never link an engine, map executable memory, or install a signal handler
  (from phase 5 on, for the default composition).
- Wasmtime: the current LTS, v48.x (every 12th major is LTS), prebuilt `c-api` archive, pinned by version
  and SHA-256 per platform, linked statically into the helper only. Never `wasmtime_linker_define_wasi`.
- Engine features ON: MVP, bulk-memory, sign-extension, saturating float-to-int, mutable globals,
  multi-value, reference-types. OFF: GC, exceptions, threads, memory64, multi-memory, tail calls, SIMD
  (incl. relaxed), component model. One table, shared by both engines and the world check.
- Allowed WASI p1 functions: exactly `wasm/WasiAllowList.hpp` (15 on master).
- Limits (unchanged): 32 MiB memory, 128 KiB stack, 50 ms call deadline (status line); modules ≤ 16 MiB;
  host-call payload ≤ 64 KiB; `faultsBeforeDisable` 3 within `faultWindow` 60 s.
- New timeouts (`BackendConfig`): `Load` 10 s, `Instantiate` 1 s, handshake + lockdown 2 s, interrupt
  grace ≈ 50 ms. Lane channels ≤ 4 (one per worker lane). Compile cache LRU 256 MiB. PDU artifact cap
  4 × `maxModuleBytes`.
- Never edit `vendor/core-cpp`.
- AGENT.md: C++23, range-`for` only, `std::expected` with a per-subsystem error enum,
  `enum class : uint8_t` (zero = negative/absent case) instead of `bool`, DI for every OS resource, clock
  and process (no test waits on a real timer), complete constructors, behaviour as table rows,
  `.hpp` + `#pragma once`, Doxygen on public API, `[[nodiscard]]`, one declaration per line.
- Zero warnings: `-Werror`, clang-tidy clean, no `NOLINT`, no `#pragma` mutes.
- CI portability: no `| std::ranges::to<>()`, no `std::views::enumerate` (use `core::views::enumerate`).
- Builds: one heavy build at a time; `nice -n 19 ionice -c 3 cmake --build --preset clang-asan -j 6`.
  Serialize with `flock -o`. Tests: `ctest --preset=clang-asan`. Unset every `CONTOUR_*` environment
  variable in test shells.
- Format C++ with `/usr/bin/clang-format` (22). Run `editorconfig-checker` before pushing.
- Every commit: `git commit -s`, conventional subject, no AI attribution trailers, never mentions the
  plan or spec. Host build changes and packaging changes go in separate commits.
- After each task: `/simplify`. After each phase: `/code-review`, all findings addressed. Each phase
  summary reports performance impact, risk and coverage.

## Review Focus

The five failure modes most likely to bite a user that no single task's happy path exercises:

1. **A helper dies while the parent serves a long host call** (`run-command`, up to 5 s). The call must fail
   `HostCrashed` once the host call returns, the lane must be freed, and nothing may deadlock. Pinned by
   `"helper death during CallHost frees the lane"` (Task 3.4).
2. **Ten panes, one crashing extension.** One death must count one breaker fault, not ten. It must
   respawn once, not ten times, and the other extensions keep running. Pinned by
   `"one death counts once across panes"` (Task 2.6) and `"ten panes respawn one helper"` (Task 3.4).
3. **Closing a pane or quitting while a helper hangs.** The GUI thread must not wait. Pinned by
   `"teardown never blocks the owner thread"` (Task 3.5).
4. **A user `.wasm` built with a disabled feature** (SIMD, threads, tail calls). It must be refused at load
   with the same error under both engines, and the log must name the feature. Pinned by contract case
   `refusesDisabledFeatures` (Task 1.2).
5. **A config reload while a helper is starting, or the last pane closing during `instantiate()`.** There must
   be no use-after-free, no orphaned helper, and the reload must reset the breaker. Pinned by
   `"registry drop races instantiate"` and `"reload resets breaker"` (Task 2.6).

## File Structure

```
src/vtextension/
  wasm/
    EngineFeatures.hpp            the ON/OFF feature table (phase 1), consumed by both engines + world check
    wasi/Preview1Shim.hpp/.cpp    engine-independent WASI p1 functions over a memory span (phase 1)
    wasmtime/                     WasmtimeBackend (phase 1) → WasmtimeEngine (phase 5); links Wasmtime
      CMakeLists.txt  WasmtimeBackend.hpp/.cpp  WasmtimeHandles.hpp  WasmtimeInstance.cpp
      WasmtimeModule.cpp  WasmtimeBackend_test.cpp
    remote/                       parent side (phases 2–4); links no engine
      RemoteBackend.hpp/.cpp  ExtensionHostSupervisor.hpp/.cpp  BackendConfig.hpp
      proto/  Pdu.hpp  PduTable.hpp  Codec.hpp/.cpp  Codec_test.cpp
      SharedMemory_{linux,freebsd,macos,win32}.cpp  ProcessLauncher_{posix,win32}.cpp
    host/                         helper side (phases 2–5)
      IHostedEngine.hpp  HostServer.hpp/.cpp  Sandbox.hpp  Sandbox_{linux,freebsd,macos,win32}.cpp
  ExtensionRegistry.hpp/.cpp      above the seam (phase 2)
src/contour-extension-host/main.cpp   the executable (phase 3)
cmake/ContourThirdParties.cmake   Wasmtime resolution (phase 1)
```

---

## Phase 1 — Wasmtime in-process, behind the contract suite

### Task 1.1: Pin Wasmtime and verify the C API facts the design relies on

**Files:**
- Modify: `cmake/ContourThirdParties.cmake` (new block after the WAMR block)
- Modify: `CMakeLists.txt:170-185` (new internal flag)
- Modify: this plan, section "Phase-1 facts" (appended, local only)

**Interfaces:**
- Produces: CMake target `contour::wasmtime` (INTERFACE, linking the static `libwasmtime.a`, its include
  dir, and the platform's system libs: `pthread dl m` on Linux, `ws2_32 advapi32 userenv ntdll shell32
  ole32 bcrypt` on Windows). The variable `CONTOUR_HAVE_WASMTIME` (ON when resolved).

- [ ] **Step 1: Find the LTS version and archive names.** Open
  `https://github.com/bytecodealliance/wasmtime/releases`. Take the newest `v48.0.x` and record the
  `c-api` archive names for x86_64-linux, aarch64-linux, x86_64-macos, aarch64-macos and x86_64-windows
  (msvc), with their SHA-256 (download and `sha256sum` each). If v48 is not marked LTS, stop and report.
- [ ] **Step 2: Verify the API surface in the archive's headers.** Grep `include/wasmtime/*.h` for each
  symbol and record "present / absent / differently named" in "Phase-1 facts":
  `wasmtime_config_host_memory_creator_set`, `wasmtime_config_epoch_interruption_set`,
  `wasmtime_store_epoch_deadline_callback`, `wasmtime_context_set_epoch_deadline`,
  `wasmtime_engine_increment_epoch`, `wasmtime_config_signals_based_traps_set`,
  `wasmtime_config_macos_use_mach_ports_set`, `wasmtime_config_max_wasm_stack_set`,
  `wasmtime_store_limiter`, `wasmtime_module_serialize`, `wasmtime_module_deserialize`,
  `wasmtime_config_parallel_compilation_set`, `wasmtime_config_cranelift_opt_level_set`, and one setter
  per feature in Global Constraints (`wasmtime_config_wasm_*_set`). An absent symbol that the design
  needs (memory creator, epoch, signals-based traps) stops the phase: report to the owner.
- [ ] **Step 3: Add the CMake resolution.** With `CONTOUR_USE_CPM=ON` and a pinned archive for the
  host platform: `CPMAddPackage(NAME wasmtime URL <archive> URL_HASH SHA256=<hash> DOWNLOAD_ONLY YES)`,
  then define `contour::wasmtime`. Otherwise `find_package(wasmtime 48 QUIET)`. Otherwise
  `CONTOUR_HAVE_WASMTIME=OFF`, with `message(STATUS "Extension host engine: WAMR (<reason>)")`. Print the
  outcome in the summary block next to `|> WebAssembly runtime:`.
- [ ] **Step 4: Configure and check.** Run:
  `cmake --preset clang-asan -B out/clang-asan 2>&1 | grep -i 'wasmtime\|Extension host engine'`.
  Expected: a line showing Wasmtime 48.x resolved. `ls out/clang-asan/_deps/wasmtime-src/lib/libwasmtime.a`
  exists.
- [ ] **Step 5: Commit.**
  `git commit -s -m "build: fetch a pinned Wasmtime for the extension host"`

### Task 1.2: The engine feature table, enforced by both engines

**Files:**
- Create: `src/vtextension/wasm/EngineFeatures.hpp`
- Modify: `src/vtextension/wasm/testing/BackendContract.hpp` (new case + new modules, macro list)
- Modify: `cmake/ContourThirdParties.cmake` (WAMR CPM options for disabled features, if a test shows
  WAMR accepts one)

**Interfaces:**
- Produces:
  ```cpp
  namespace vtextension::wasm {
  enum class EngineFeature : uint8_t { BulkMemory, SignExtension, SaturatingFloatToInt, MutableGlobals,
      MultiValue, ReferenceTypes, Gc, Exceptions, Threads, Memory64, MultiMemory, TailCalls, Simd,
      RelaxedSimd, ComponentModel };
  enum class FeatureState : uint8_t { Off, On };
  struct FeatureRow { EngineFeature feature; FeatureState state; std::string_view name; };
  /// The one table both engines configure from and the world check reports by name.
  inline constexpr auto EngineFeatures = std::to_array<FeatureRow>({ /* states per Global Constraints */ });
  }
  ```

- [ ] **Step 1: Write the failing contract case.** In `BackendContract.hpp` add four hand-assembled
  modules, each needing one OFF feature: `SimdModule` (a `v128.const` in a function body),
  `ThreadsModule` (a shared memory), `TailCallModule` (`return_call`), `MultiMemoryModule` (two memories).
  Add:
  ```cpp
  template <typename T>
  void refusesDisabledFeatures()
  {
      auto const backend = T::make();
      auto const runtime = createRuntime(*backend);
      auto const guard = enterThread(*backend);
      for (auto const bytes : { std::span<uint8_t const>(SimdModule), std::span<uint8_t const>(ThreadsModule),
                                std::span<uint8_t const>(TailCallModule), std::span<uint8_t const>(MultiMemoryModule) })
          CHECK(runtime->load(toBytes(bytes)).error() == ExtensionError::InvalidComponent);
  }
  ```
  Register it in `CONTOUR_WASM_BACKEND_CONTRACT`.
- [ ] **Step 2: Run against WAMR.** Run:
  `out/clang-asan/src/vtextension/vtextension_test "WasmContract.refusesDisabledFeatures"`.
  Expected: it either passes, or fails naming the modules WAMR accepts.
- [ ] **Step 3: Make WAMR refuse them.** For each module WAMR accepted, add the matching CPM option
  (`WAMR_BUILD_SIMD 0`, `WAMR_BUILD_TAIL_CALL 0`, `WAMR_BUILD_SHARED_MEMORY 0`,
  `WAMR_BUILD_MULTI_MEMORY 0`). Create `EngineFeatures.hpp` with the table. A system WAMR that accepts a
  disabled feature is rejected by the existing probe path: add the check to `WamrTerminateProbe.cpp`.
- [ ] **Step 4: Run the case again.** Expected: PASS.
- [ ] **Step 5: Commit.** `git commit -s -m "feat(vtextension): one table of WebAssembly features extensions may use"`

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

### Task 1.6: Sticky interruption by epoch, with no ticker

**Files:**
- Modify: `src/vtextension/wasm/wasmtime/WasmtimeInstance.cpp`, `WasmtimeBackend.cpp`
- Modify: `src/vtextension/wasm/wasmtime/WasmtimeBackend_test.cpp`

**Interfaces:**
- Produces: `IWasmInstance::interrupt()` for Wasmtime. Internal:
  `std::atomic<InterruptState> _interrupt` with `enum class InterruptState : uint8_t { Running, Interrupted }`.

- [ ] **Step 1: Write a Wasmtime-only test.** `TEST_CASE("Wasmtime.idleInstanceNeverWakes")`: run an
  instance with an export that loops 10⁶ iterations, and count epoch-callback invocations through a
  test-only counter on `KindState`. Expected count: 0 when no `interrupt()` happened.
- [ ] **Step 2: Run the interrupt cases; expect failure.** Run:
  `... "WasmContract.interruptStopsLoop" "WasmContract.interruptIsFinal" "Wasmtime.idleInstanceNeverWakes"`.
- [ ] **Step 3: Implement.**
  - Every `call()` checks `_interrupt` first; if `Interrupted`, it fails `Interrupted` without entering
    the guest. Otherwise it sets the store's epoch deadline to 1 tick.
  - `interrupt()` stores `Interrupted`, then calls `wasmtime_engine_increment_epoch` on the runtime's
    engine. It is wait-free: no lock, no allocation.
  - The epoch callback returns a trap when its instance's flag is `Interrupted`. Otherwise it extends the
    deadline by 1 tick and continues. Another instance's interrupt bumps a shared engine epoch, which is
    why the callback must check its own flag.
- [ ] **Step 4: Run the cases.** Expected: PASS.
- [ ] **Step 5: Commit.** `git commit -s -m "feat(vtextension): interrupt Wasmtime calls by epoch, never polling while idle"`

### Task 1.7: Phase 1 gate

- [ ] **Step 1: Run the full suites under ASan.** Run:
  `ctest --preset=clang-asan -R 'vtextension'`. Expected: all pass, including both contract suites.
- [ ] **Step 2: Run the threaded cases under TSan.** Run:
  `cmake --preset clang-tsan && nice -n 19 ionice -c 3 cmake --build --preset clang-tsan -j 6 --target vtextension_wasmtime_test`,
  then `out/clang-tsan/src/vtextension/wasm/wasmtime/vtextension_wasmtime_test "[wasm-contract]"`.
  Expected: no reports.
- [ ] **Step 3: Measure the speed-up in-process.** In a `clang-release` tree, add a hidden
  `[.measure]` case in `WasmtimeBackend_test.cpp` that runs a bundled extension's `render(120, 1)`
  1000 times. Use `plasma` (the heaviest on master) if `CONTOUR_WASM_GUESTS` is on, otherwise the
  `TestModules` loop. Report the median per call for WAMR and for Wasmtime, three runs each. Record the
  numbers in "Phase-1 facts". There is no pass threshold, but a speed-up below 3× is reported to the owner
  before phase 2 starts.
- [ ] **Step 4: Run `/code-review` on the phase diff and address every finding.**
- [ ] **Step 5: Write the phase summary** (performance numbers, risk, coverage of `wasm/wasmtime/` and
  `wasm/wasi/` via the `clang-coverage` preset).

### Phase-1 facts

*(Filled in by Tasks 1.1, 1.5 and 1.7.)*

**Task 1.1 — Wasmtime pin (verified 2026-11-24).**

- LTS policy (upstream `docs/stability-release.md`): every major divisible by 12 is LTS, 24 months; two LTS
  lines supported at once (today 36 and 48). Newest LTS patch: **v48.0.10** (2026-11-19). Newest overall:
  v51.0.0. v48 is the current LTS, as the spec assumed.
- Pinned archives (`wasmtime-v48.0.10-<name>`), SHA-256 from `sha256sum`, identical to GitHub's asset digest:

  | Platform | Archive | SHA-256 |
  |---|---|---|
  | Linux x86_64 | `x86_64-linux-c-api.tar.xz` | `5fc9a0b8c825ae7e2730f41f6260ec38556236f28bcd34fbb9b76e15c9f56e27` |
  | Linux aarch64 | `aarch64-linux-c-api.tar.xz` | `7be1929dd01708212668696cf55b224b4c82bd562bc231596122563e2eb32a3d` |
  | macOS x86_64 | `x86_64-macos-c-api.tar.xz` | `3bd16df4a8beedb510e8b92a9a3063da8b4187a6ebbc4f363b25397d436ef232` |
  | macOS aarch64 | `aarch64-macos-c-api.tar.xz` | `868e59b586157b2497d5cd3004a588dde85c8542b4ff2366acc069a7da5d6927` |
  | Windows x86_64 (MSVC) | `x86_64-windows-c-api.zip` | `2c5cd1a231c0b6aee80ecc700d68946bca4afaa09b728f6f8c43db64207ae682` |

- Archive layout (single top dir, stripped by CPM): `include/{wasm.h,wasi.h,wasmtime.h,wasmtime/*.h,*.hh}`,
  `lib/libwasmtime.a` (68 MiB, x86_64-linux) + `lib/libwasmtime.so` (30 MiB); Windows `lib/wasmtime.lib`
  (static, 79 MiB) + `wasmtime.dll` + `wasmtime.dll.lib`. Every archive also carries a `min/` build
  (`min/include`, `min/lib`) — Windows `min/lib/wasmtime.lib` is 2.5 MiB: a runtime-only build without
  the compiler, a candidate for a deserialize-only helper later.
- Default archive's `include/wasmtime/conf.h` enables: PROFILING, WAT, CACHE, PARALLEL_COMPILATION, WASI,
  WASI_HTTP, LOGGING, COREDUMP, ADDR2LINE, DEMANGLE, THREADS, GC, GC_DRC, GC_NULL, ASYNC, CRANELIFT, WINCH,
  DEBUG_BUILTINS, POOLING_ALLOCATOR, COMPONENT_MODEL, COMPONENT_MODEL_ASYNC, PULLEY. Our config switches the
  wasm features off at runtime; the code is still linked.
- Static linking on Windows: `wasm.h` uses `__declspec(dllimport)` unless `LIBWASM_STATIC` is defined;
  `wasi.h` unless `WASI_API_EXTERN` is predefined. `contour::wasmtime` defines `LIBWASM_STATIC` and
  `WASI_API_EXTERN=` there.
- API surface (x86_64-linux headers, and `nm` confirms each as a `T` symbol in `libwasmtime.a`). All
  **present**, none renamed. Most setters are declared via `WASMTIME_CONFIG_PROP(void, <name>, <type>)` in
  `wasmtime/config.h`, so a plain grep for the full name only finds them in the `.hh` wrappers:
  - `wasmtime_config_host_memory_creator_set(wasm_config_t*, wasmtime_memory_creator_t*)` — present
    (config.h:637). `wasmtime_memory_creator_t { void* env; wasmtime_new_memory_callback_t new_memory;
    void (*finalizer)(void*); }`; the config copies the struct, `new_memory` must be thread-safe.
  - `wasmtime_config_epoch_interruption_set(bool)` — present (PROP, config.h:167).
  - `wasmtime_store_epoch_deadline_callback(store, wasmtime_error_t* (*)(wasmtime_context_t*, void*,
    uint64_t* epoch_deadline_delta, wasmtime_update_deadline_kind_t*), void* data, void (*finalizer)(void*))`
    — present (store.h:254). Kinds: `WASMTIME_UPDATE_DEADLINE_CONTINUE` 0, `..._YIELD` 1 (needs async).
  - `wasmtime_context_set_epoch_deadline(context, uint64_t ticks_beyond_current)` — present (store.h:224).
  - `wasmtime_engine_increment_epoch(engine)` — present.
  - `wasmtime_config_signals_based_traps_set(bool)` — present (PROP, config.h:551).
  - `wasmtime_config_macos_use_mach_ports_set(bool)` — present (PROP, config.h:538).
  - `wasmtime_config_max_wasm_stack_set(size_t)` — present (PROP, config.h:182).
  - `wasmtime_store_limiter(store, int64_t memory_size, int64_t table_elements, int64_t instances,
    int64_t tables, int64_t memories)` — present (store.h:111); negative = default.
  - `wasmtime_module_serialize(const wasmtime_module_t*, wasm_byte_vec_t*)` — present (module.h:110).
  - `wasmtime_module_deserialize(wasm_engine_t*, const uint8_t*, size_t, wasmtime_module_t**)` — present
    (module.h:126).
  - `wasmtime_config_parallel_compilation_set(bool)` — present (PROP, config.h:363).
  - `wasmtime_config_cranelift_opt_level_set(wasmtime_opt_level_t)` — present (PROP, config.h:399);
    `WASMTIME_OPT_LEVEL_NONE/SPEED/SPEED_AND_SIZE`, default SPEED.
  - Feature setters, all `(wasm_config_t*, bool)`, present: `wasm_bulk_memory`, `wasm_multi_value`,
    `wasm_reference_types` (ON); `wasm_gc`, `wasm_exceptions`, `wasm_threads`, `wasm_memory64`,
    `wasm_multi_memory`, `wasm_tail_call`, `wasm_simd`, `wasm_relaxed_simd`, `wasm_component_model` (OFF).
    Also present (headers only, not nm-checked) and to be switched OFF explicitly so the table is the whole truth:
    `wasm_function_references` (GC depends on it), `wasm_relaxed_simd_deterministic`, `wasm_wide_arithmetic`,
    `wasm_branch_hinting`, `wasm_custom_page_sizes`, `wasm_stack_switching`, `wasm_component_model_async`,
    `..._async_stackful`, `..._map`, `..._more_async_builtins`.
  - **No setter exists** for sign-extension, saturating float-to-int and mutable globals: Wasmtime
    always accepts them. They are ON in the table anyway, so nothing is lost, but the table's ON rows for
    these three have no Wasmtime switch to drive.
- CMake: `contour::wasmtime` (INTERFACE: `include/` as SYSTEM, the static lib, `pthread dl m` on Linux,
  `ws2_32 advapi32 userenv ntdll shell32 ole32 bcrypt` on Windows, nothing extra on macOS — unverified
  until a macOS link in Task 1.3). `CONTOUR_HAVE_WASMTIME` is a plain (non-cache) variable, initialised OFF
  in `CMakeLists.txt`, set ON in `cmake/ContourThirdParties.cmake`; it is defined only for the top-level
  scope and below.

**Task 1.5 — WASI shim vs WAMR's libc-wasi (measured 2026-11-25).**

- `everyAllowedWasiFunctionWorks` passes on WAMR unchanged: for the in-bounds calls the contract makes,
  WAMR's built-in libc-wasi (stdio bound to the null device, no preopens) returns the same errno as our
  shim for all 15 functions — `fd_close(9)`, `fd_prestat_get(3)`, `fd_prestat_dir_name(3)` → `badf` (8);
  `fd_seek(1, 0, set)` on the null device → success; `proc_exit` → `CallFailure::Trap`. No Traits hook
  was needed.
- Not asserted across engines (they may differ, and the contract does not depend on it): `fd_close` on a
  standard stream (WAMR closes its table entry; the shim keeps the stream), `fd_seek` on a standard stream
  (WAMR seeks the null device; the shim always reports offset 0), the CPU-time clocks (shim: `nosys`;
  WAMR: whatever the host clock gives), and `fd_fdstat_get` rights.

**Task 1.7 — Phase 1 gate (measured 2026-11-25, head 38b58d1ec).**

- Workload: the real bundled `plasma.wasm` (CONTOUR_WASM_GUESTS on; not synthetic), via the full extension
  host path (`Engine` → `CompiledExtension::load` → `Instance::create` → `load({})` → `render`), one warm-up
  render, then 1000 timed renders, `core::defaultSteadyClock()` around each call, median of the sorted samples.
  Helper `src/vtextension/wasm/testing/RenderMeasurement.hpp`; hidden cases `Wamr.measureRender`
  (vtextension_test) and `Wasmtime.measureRender` (vtextension_wasmtime_test), tag `[.measure]`.
  Wasmtime runs at the backend default (`TrapMode::Software`); WAMR is the fast interpreter (no AOT/JIT).
- plasma caches its Sixel pixels per surface size, so `render(120, 1)` repeated is its *steady* frame (palette
  + copy). A second workload alternates 120/119 columns, so every call re-encodes the pixels (*uncached* frame,
  the expensive first frame after a resize).
- `out/clang-release` (Release, clang), AMD Ryzen 9 9950X3D, load avg ~3 (another session
  was building); three runs each, median per call:

  | Workload | WAMR run 1/2/3 | Wasmtime run 1/2/3 | Ratio (median of runs) |
  |---|---|---|---|
  | steady `render(120, 1)` | 38.2 / 38.4 / 38.3 µs | 2.8 / 2.8 / 2.7 µs | **13.7×** |
  | uncached (120/119 alternating) | 229.6 / 230.9 / 228.6 µs | 12.4 / 12.4 / 12.4 µs | **18.5×** |

  p90 within 3% of the median for every run. Compilation (`CompiledExtension::load`), once per run, excluded
  from per-call timing: Wasmtime 57–59 ms (Cranelift, parallel compilation off), WAMR 0.9–1.0 ms (load only).
  Instantiate + `load` ≤ 0.1 ms on both. Speed-up is well above the 3× threshold.
- ASan (`ctest --preset=clang-asan -R vtextension`): 2/2 pass — vtextension_test 317 cases / 4558
  assertions, vtextension_wasmtime_test 45 cases / 969 assertions; `[wasm-contract]` 34 cases / 437 assertions
  on each backend.
- TSan (`out/clang-tsan`, full binaries, no suppressions): vtextension_test 317 cases (17 `[threading]`) and
  vtextension_wasmtime_test 45 cases all pass, **0 ThreadSanitizer reports**.
- Coverage (`out/clang-coverage`, only the two test targets built, both suites run, `llvm-cov gcov`):
  `wasm/wasmtime/` .cpp 476/496 lines = **96.0%** (WasmtimeBackend.cpp 94.8% of 193, WasmtimeInstance.cpp 97.0%
  of 165, WasmtimeModule.cpp 96.4% of 138; WasmtimeRuntime.hpp 100%); `wasm/wasi/` .cpp 152/155 = **98.1%**
  (Preview1Shim.cpp 100% of 133, SystemEnvironment.cpp 86.4% of 22).

---

## Phase 2 — Protocol, `HostServer`, `RemoteBackend`; identity, registry, breaker

Phase-2 decisions (controller rulings, ledger R10):
- D1. Execution order 2.1 → 2.4 → 2.2 → 2.3 → 2.5 → 2.6 → 2.7 (seam types exist before anything implements them); numbering kept.
- D2. `wasm/remote/proto/` (codec, std only) + `wasm/remote/transport/` (Channel, PduStream) = `vtextension_remote_wire`; `wasm/remote/` = `vtextension_remote`; `wasm/host/` = `vtextension_host`. None links an engine; all always built; `vtextension_remote_test` only where Wasmtime is.
- D3. Own wire primitives shaped like `vthost/proto/Wire.hpp`, not shared (vtextension must not depend on vthost).
- D4. Frame = varint length, varint kind, body; body consumed exactly. Unknown kind, wrong direction or wrong channel is fatal `Malformed` (parent and helper ship together).
- D5. 15 PDU kinds, one table row each; one `Release` PDU with a target enum; every status a u8 (0 = ok, else value + 1).
- D6. Caps: `PduLimits` 16 MiB modules / 32 MiB memory (static_assert against `ExtensionLimits {}`); `Loaded` ≤ 4 × `maxModuleBytes`, entry counts ≤ the module's own declared counts (`WasmBinary::declaredCounts`); `Call`/`CallResult` ≤ 64 values; `CallHost` ≤ 17 arguments.
- D7. Declared frame length checked against the largest cap before buffering, then against the row's cap once the kind is read.
- D8. Handshake = helper's first control PDU `Hello{version, memoryMode, lockdown}`, `ProtocolVersion = 1`; mismatch or `lockdown == Failed` → `HostUnavailable` + terminate; `Unsandboxed` accepted until phase 5.
- D9. Load/CreateRuntime/Instantiate/Release on control (parent mutex). Calls, host calls and copy-path memory PDUs on the calling thread's lane (deviates from spec §5.4 wording; phase 4 replaces the copy path for Wasmtime).
- D10. A thread takes a lane slot on first lane use while holding a guard, returns it with its outermost guard. A fifth concurrent lane user fails `CallFailure::Trap` and logs an error.
- D11. Instantiate on control; a host call during instantiation (start function) fails → `InvalidComponent`.
- D12. Copy-path memory: overlay earlier overlapping cached ranges on fetch; at flush write only changed bytes, in fetch order; cache dropped at every `Call`/`HostResult`; capped per instance at the memory limit.
- D13. Phase-2 interrupt: parent sticky flag + `IHostHandle::interrupt(instanceId)`; in-process = direct call under a brief server mutex, no I/O. Shared page + wake pipe in Task 3.4.
- D14. `IHostConnector` / `IHostHandle` / `HostConnection` are what phase 3's supervisor implements; `BackendConfig.connector` replaces the plan's launcher (phase 3 wraps the launcher in a connector).
- D15. `BackendConfig` in `wasm/BackendConfig.hpp`, namespace `vtextension::wasm` (names no engine; DefaultBackend.hpp includes it).
- D16. WAMR's `makeDefaultBackend(BackendConfig)` ignores the config in phase 2; every root passes `wasm::BackendConfig {}` explicitly, no default argument.
- D17. `HostServer::run()` is the control thread (jthread owned by `InProcessHost`), one jthread per lane; `stop()` interrupts every live instance, then shuts every channel; `~InProcessHost` stops and joins on the dropping thread (off-thread teardown is Task 3.5).
- D18. `ModuleIdentity{std::string key}`; helpers shared per `RemoteRuntime` by key; empty key → own helper.
- D19. An incarnation goes alive → dead exactly once (atomic exchange) on EOF, stream error or violation, then is terminated; the module keeps its bytes and lazily respawns at the next instantiate (handshake, CreateRuntime, Load; descriptions must match).
- D20. Breaker per registry entry via `CompiledExtension` (`shared_ptr<HostBreaker>`); `Instance` reports faults; admissions carry an epoch, a fault counts only if its epoch is current, then the epoch advances. Bounded undercount in one race. Runner's per-pane counting unchanged.
- D21. `CallFailure::HostCrashed` → `ExtensionError::HostUnavailable` in `Instance`.
- D22. Registry: weak refs keyed by `RegistryKey{hostId, world, configDigest, moduleDigest}` (`core::Sha256`); concurrent loads merged through a `shared_future`; entry matches only on the same engine; lives in `ExtensionRuntime`, uses the pool's clock; `ExtensionRuntime::configurationReloaded()` resets every breaker (called from `ContourGuiApp::onConfigurationReloaded` and the daemon's reload).
- D23. Config digest = SHA-256 over length-prefixed fields: `runCommand`, then config pairs sorted by key.
- D24. `ExtensionWorld` has only `StatusLine` on master.
- D25. Fuzzing: fixed-seed mutation case in CI; optional libFuzzer target behind `VTEXTENSION_FUZZING` (OFF).
- D26. Codec/Channel tests in `vtextension_test`; HostServer/RemoteBackend/contract tests in `vtextension_remote_test`, which in 2.5 also runs real-guest suites via `RemoteDefaultBackend.cpp`.
- D27. 2.2 renames `WasmtimeBackend` → `WasmtimeEngine` (files too); `makeWasmtimeEngine` returns `unique_ptr<IHostedEngine>`; `memoryMode()` = `Copy` until phase 4; `MemoryMode` lives in `proto/Pdu.hpp`.
- D28. Channel: POSIX `socketpair(AF_UNIX, SOCK_STREAM|SOCK_CLOEXEC)` (macOS: fcntl + `SO_NOSIGPIPE`), sends with `MSG_NOSIGNAL`; Windows overlapped named-pipe pair, random name, `PIPE_REJECT_REMOTE_CLIENTS`. Blocking I/O, `shutdown()` unblocks; bounded writes in phase 3.
- D29. Thread-local "prepared" and lane stamps keyed by a per-kind serial, not an address (closes the deferred 1.3 minor).

Builds: `nice -n 19 ionice -c 3 cmake --build --preset clang-asan -j 4 --target <t>` (R5), one at a time, under the build lock; test shells unset every `CONTOUR_*` variable.

### Task 2.1: PDU vocabulary and codec (`remote/proto/`), channels and framed streams (`remote/transport/`)

**Files:**
- Create: `src/vtextension/wasm/remote/proto/{Wire.hpp,Pdu.hpp,PduTable.hpp,Codec.hpp,Codec.cpp,Codec_test.cpp,Codec_fuzz.cpp}`
- Create: `src/vtextension/wasm/remote/transport/{Channel.hpp,Channel_posix.cpp,Channel_win32.cpp,PduStream.hpp,PduStream.cpp,Channel_test.cpp,PduStream_test.cpp}`
- Create: `src/vtextension/wasm/remote/CMakeLists.txt` (target `vtextension_remote_wire`: proto + transport, links only `ws2_32` on Windows; option `VTEXTENSION_FUZZING` OFF → `vtextension_remote_fuzz` with `-fsanitize=fuzzer`, Clang only)
- Modify: `src/vtextension/CMakeLists.txt` (`add_subdirectory(wasm/remote)` before the backend; the three `_test.cpp` join `vtextension_test`, which links `vtextension_remote_wire`)
- Modify: `src/vtextension/wasm/WasmBinary.hpp` (`ImportSectionId = 2`, `declaredCounts()`)

**Interfaces:**
- Produces:
  ```cpp
  namespace vtextension::wasm::remote::proto {
  inline constexpr uint32_t ProtocolVersion = 1;
  inline constexpr std::size_t MaxCallValues = 64;
  inline constexpr std::size_t MaxHostParameters = 17;      // == wasmtime::MaxHostParameters (static_assert there)
  enum class MemoryMode : uint8_t { Copy, Shared };
  enum class LockdownState : uint8_t { Failed, Unsandboxed, LockedDown };
  enum class Direction : uint8_t { ToHelper, ToParent };
  enum class ChannelKind : uint8_t { Control, Lane };
  enum class ReleaseTarget : uint8_t { Instance, Module, Runtime };
  enum class PduKind : uint8_t { Hello = 1, CreateRuntime, RuntimeCreated, Load, Loaded, Instantiate, Instantiated,
      Release, Call, CallHost, HostResult, CallResult, MemoryRead, MemoryData, MemoryWrite };
  enum class MemoryStatus : uint8_t { OutOfBounds, Ok };
  struct PduLimits { uint64_t maxModuleBytes = 16 MiB; uint64_t maxMemoryBytes = 32 MiB; };
  struct WireHostFunction { std::string module; std::string name; uint32_t parameterCount = 0; };
  struct Hello { uint32_t version; MemoryMode memoryMode; LockdownState lockdown; };
  struct CreateRuntime { uint32_t runtimeId; uint64_t memoryBytes; uint64_t stackBytes; std::vector<WireHostFunction> functions; };
  struct RuntimeCreated { uint32_t runtimeId; std::optional<ExtensionError> error; };
  struct Load { uint32_t runtimeId; uint32_t moduleId; std::vector<std::byte> bytes; };
  struct Loaded { uint32_t moduleId; std::optional<ExtensionError> error;
                  std::vector<ImportDescription> imports; std::vector<ExportDescription> exports; };
  struct Instantiate { uint32_t moduleId; uint32_t instanceId; };
  struct Instantiated { uint32_t instanceId; std::optional<ExtensionError> error; std::vector<uint32_t> callableExports; };
  struct Release { ReleaseTarget target; uint32_t id; };
  struct Call { uint32_t instanceId; uint32_t exportOrdinal; std::vector<uint32_t> argv; };
  struct CallHost { uint32_t functionIndex; std::vector<uint32_t> arguments; };
  struct HostResult { std::optional<ExtensionError> error; };
  struct CallResult { std::optional<CallFailure> failure; std::vector<uint32_t> argv; };
  struct MemoryRead { uint32_t instanceId; uint32_t address; uint32_t size; };
  struct MemoryData { MemoryStatus status; std::vector<std::byte> bytes; };
  struct MemoryWrite { uint32_t instanceId; uint32_t address; std::vector<std::byte> bytes; };
  using Pdu = std::variant<Hello, CreateRuntime, RuntimeCreated, Load, Loaded, Instantiate, Instantiated, Release,
                           Call, CallHost, HostResult, CallResult, MemoryRead, MemoryData, MemoryWrite>;
  enum class DecodeError : uint8_t { NeedMoreData, MalformedVarint, OverCap, UnknownKind, WrongDirection,
                                     WrongChannel, Truncated, TrailingBytes, BadEnum };
  struct PduRow { PduKind kind; ChannelKind channel; Direction direction;
                  uint64_t (*sizeCap)(PduLimits const&);
                  std::expected<Pdu, DecodeError> (*decode)(std::span<uint8_t const> body); };
  extern std::span<PduRow const> const PduTable;            // one row per PduKind, in kind order
  [[nodiscard]] PduKind kindOf(Pdu const& pdu) noexcept;
  [[nodiscard]] std::vector<uint8_t> encodeFrame(Pdu const& pdu);
  struct DecodedFrame { Pdu pdu; std::size_t consumed; };
  [[nodiscard]] std::expected<DecodedFrame, DecodeError> decodeFrame(std::span<uint8_t const> buffer,
      Direction arriving, ChannelKind channel, PduLimits const& limits);
  }
  namespace vtextension::wasm::remote::transport {
  enum class ChannelError : uint8_t { Closed, Io };
  class Channel;   // move-only; send(span) / receiveSome(span) -> expected<size_t>; shutdown() noexcept, thread-safe
  struct ChannelPair { Channel parent; Channel helper; };
  [[nodiscard]] std::expected<ChannelPair, ChannelError> makeChannelPair();
  enum class StreamError : uint8_t { Closed, Io, Malformed, OverCap };
  class PduStream {  // owns a Channel and a receive buffer; one reader+writer thread
  public:
      PduStream(Channel channel, ChannelKind kind, Direction arriving, PduLimits limits);
      [[nodiscard]] std::expected<void, StreamError> send(Pdu const& pdu);
      [[nodiscard]] std::expected<Pdu, StreamError> receive();
      void shutdown() noexcept;
  };
  }
  namespace vtextension::wasm::binary {
  struct DeclaredCounts { uint32_t imports = 0; uint32_t exports = 0; };
  [[nodiscard]] std::expected<DeclaredCounts, BinaryError> declaredCounts(std::span<std::byte const> module);
  }
  ```
  The table (body sizes in bytes; M = `maxModuleBytes`, Mem = `maxMemoryBytes`):

  | Kind | Channel | Direction | Cap |
  |---|---|---|---|
  | Hello | Control | ToParent | 16 |
  | CreateRuntime | Control | ToHelper | 64 KiB |
  | RuntimeCreated | Control | ToParent | 16 |
  | Load | Control | ToHelper | M + 64 |
  | Loaded | Control | ToParent | 4 × M |
  | Instantiate | Control | ToHelper | 32 |
  | Instantiated | Control | ToParent | M |
  | Release | Control | ToHelper | 16 |
  | Call | Lane | ToHelper | 16 + 5 × MaxCallValues |
  | CallHost | Lane | ToParent | 16 + 5 × MaxHostParameters |
  | HostResult | Lane | ToHelper | 8 |
  | CallResult | Lane | ToParent | 16 + 5 × MaxCallValues |
  | MemoryRead | Lane | ToHelper | 32 |
  | MemoryData | Lane | ToParent | Mem + 16 |
  | MemoryWrite | Lane | ToHelper | Mem + 32 |

  Encoding: varints for u32/u64, one byte per enum, status `0` = ok / `value + 1`, byte strings and vectors as varint count + elements, reserves through a `boundedReserveCount<T>` like `vthost/proto/Wire.hpp`. Frame: `varint length` (kind + body), `varint kind`, body (D7).

- [ ] **Step 1: Write the codec tests.** In `Codec_test.cpp`:
  - `"PduCodec.tableHasOneRowPerKind"`: `PduTable.size() == 15`, `PduTable[i].kind == PduKind(i + 1)`.
  - `"PduCodec.roundTrip"`: one `DYNAMIC_SECTION` per kind with a populated sample; `decodeFrame(encodeFrame(p), row.direction, row.channel, {})->pdu == p` and `consumed == frame.size()`.
  - `"PduCodec.everyPrefixNeedsMoreData"`: each sample's frame truncated at every `n < size` → `NeedMoreData`, never another error, never a value.
  - `"PduCodec.overCapIsRejectedFromTheHeader"`: per kind, a header declaring `cap + 1` body bytes and no body → `OverCap` (not `NeedMoreData`).
  - `"PduCodec.lyingBodyIsMalformed"`: inner count exceeding the remainder → `Truncated`; one extra byte → `TrailingBytes`; enum byte out of range → `BadEnum`; kind `0` and `16` → `UnknownKind`.
  - `"PduCodec.directionAndChannelAreChecked"`: `Call` as `ToParent` → `WrongDirection`; `Load` on `Lane` → `WrongChannel`.
  - `"PduCodec.mutatedFramesNeverCrash"`: 10 000 frames from `std::mt19937 { 0x5eed }` (byte flips, truncations, splices); each decode returns a value or an error.
  - In `WasmBinary_test.cpp`: `"WasmBinary.declaredCounts"` on `DescribedModule` → `{ .imports = 2, .exports = N }`.
- [ ] **Step 2: Write the transport tests.** `Channel_test.cpp`: `"Channel.pairCarriesBytesBothWays"`, `"Channel.shutdownUnblocksReceive"` (a jthread blocked in `receiveSome` returns `Closed` after `shutdown()`), `"Channel.sendToClosedPeerFails"` (POSIX: SIGPIPE disposition unchanged, send → `Closed`), `"Channel.endsAreCloseOnExec"` (POSIX, `FD_CLOEXEC`). `PduStream_test.cpp`: `"PduStream.roundTripsOverAPair"`, `"PduStream.overCapFailsWithoutWaitingForTheBody"` (header only → `OverCap` at once), `"PduStream.peerCloseIsClosed"`, `"PduStream.garbageIsMalformed"`.
- [ ] **Step 3: Build; expect compile failure.** `nice -n 19 ionice -c 3 cmake --build --preset clang-asan -j 4 --target vtextension_test`.
- [ ] **Step 4: Implement the codec.** `Wire.hpp`: standalone `Writer`/`Reader`, LEB128 (max 5 bytes u32, 10 bytes u64 → `MalformedVarint`) (D3). `Codec.cpp`: one encode and one decode function per kind; `PduTable` a `constexpr std::array` with a static_assert on row order; each body ends with `reader.atEnd()` or `TrailingBytes` (D4). `declaredCounts` reads the import/export section vector counts, 0 when absent.
- [ ] **Step 5: Implement the transport.** POSIX: `socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC)` (macOS: + `fcntl(FD_CLOEXEC)` + `SO_NOSIGPIPE`); `send` with `MSG_NOSIGNAL`, looping on partial writes and `EINTR`; `shutdown()` = `::shutdown(fd, SHUT_RDWR)`; close only in the destructor. Windows: `CreateNamedPipeW(L"\\\\.\\pipe\\contour-extension-host-<pid>-<serial>-<random>", PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED | FILE_FLAG_FIRST_PIPE_INSTANCE, PIPE_TYPE_BYTE | PIPE_REJECT_REMOTE_CLIENTS, 1, …)` + `CreateFileW(…, FILE_FLAG_OVERLAPPED)`; overlapped I/O waited on; `shutdown()` sets a flag and `CancelIoEx`. `PduStream::receive` grows its buffer until `decodeFrame` stops returning `NeedMoreData`; other decode errors map to `Malformed`/`OverCap`.
- [ ] **Step 6: Add the fuzz target.** `Codec_fuzz.cpp` defines `LLVMFuzzerTestOneInput`, decoding the input as each (direction, channel) pair; built only with `-DVTEXTENSION_FUZZING=ON` on Clang.
- [ ] **Step 7: Run.** `out/clang-asan/src/vtextension/vtextension_test "PduCodec.*" "Channel.*" "PduStream.*" "WasmBinary.*"` → PASS.
- [ ] **Step 8: Commit.** `git commit -s -m "feat(vtextension): a bounded wire protocol and channels for the extension host process"`

### Task 2.2: `IHostedEngine` and `HostServer`

Runs after 2.4 (D1).

**Files:**
- Create: `src/vtextension/wasm/host/{CMakeLists.txt,IHostedEngine.hpp,HostServer.hpp,HostServer.cpp,InProcessHost.hpp,InProcessHost.cpp,HostServer_test.cpp}`
- Rename (git mv): `wasm/wasmtime/WasmtimeBackend.{hpp,cpp}` → `WasmtimeEngine.{hpp,cpp}`, `WasmtimeBackend_test.cpp` → `WasmtimeEngine_test.cpp`; update includes and the containment script's paths
- Create: `src/vtextension/wasm/remote/HostConnector.hpp` (interfaces only)
- Create: `vtextension_remote_test` in `wasm/remote/CMakeLists.txt` (only `if(TARGET vtextension_wasmtime)`; links `vtextension vtextension_remote_wire vtextension_host vtextension_wasmtime contour::wasmtime core::testing Catch2::Catch2 contour_test_trace`)
- Modify: `src/vtextension/CMakeLists.txt` (`add_subdirectory(wasm/host)`; Wasmtime directory before `wasm/remote`)

**Interfaces:**
- Consumes: 2.1 (`proto::*`, `transport::*`), 2.4 (`ModuleIdentity`, `CallFailure::HostCrashed`).
- Produces:
  ```cpp
  namespace vtextension::wasm::host {
  /// A backend a host process drives. The helper's own interface: the parent never sees one.
  class IHostedEngine: public IWasmBackend {
  public:
      [[nodiscard]] virtual remote::proto::MemoryMode memoryMode() const noexcept = 0;
  };
  struct HostServerConfig { remote::proto::PduLimits limits {}; remote::proto::LockdownState lockdown = remote::proto::LockdownState::Unsandboxed; };
  enum class ServerExit : uint8_t { ProtocolViolation, PeerClosed, Stopped };
  class HostServer {
  public:
      HostServer(IHostedEngine& engine, std::vector<remote::transport::Channel> lanes,
                 remote::transport::Channel control, HostServerConfig config);
      /// Sends Hello, starts one executor jthread per lane, serves control on the calling thread until EOF,
      /// a violation or stop(); then stops, joins the executors, releases every instance, module, runtime.
      [[nodiscard]] ServerExit run();
      /// Thread-safe, idempotent: interrupts every live instance, shuts every channel down.
      void stop() noexcept;
      /// Interrupts one instance; a brief mutex over the instance map, no I/O. Unknown id: ignored.
      void interrupt(uint32_t instanceId) noexcept;
  };
  using EngineFactory = std::function<std::unique_ptr<IHostedEngine>()>;
  class InProcessHostConnector final: public remote::IHostConnector {   // tests and in-process measurement
  public:
      InProcessHostConnector(EngineFactory engines, HostServerConfig config);
      [[nodiscard]] std::expected<remote::HostConnection, ExtensionError> connect() override;
      [[nodiscard]] std::size_t connects() const noexcept;     // test hooks
      [[nodiscard]] std::size_t liveHosts() const noexcept;
      std::size_t terminateAll() noexcept;                      // simulated crash of every live host
  };
  }
  namespace vtextension::wasm::wasmtime {
  [[nodiscard]] std::unique_ptr<host::IHostedEngine> makeWasmtimeEngine(std::shared_ptr<KindState> kind, WasmtimeConfig config);
  }
  namespace vtextension::wasm::remote {
  inline constexpr std::size_t MaxLaneChannels = 4;       // == MaxExtensionWorkers (static_assert in RemoteBackend.cpp)
  class IHostHandle {   // one helper incarnation's process (phase 2: an InProcessHost)
  public:
      virtual void interrupt(uint32_t instanceId) noexcept = 0;  // never blocks on the helper
      virtual void terminate() noexcept = 0;                      // the helper stops serving; channels close
      virtual ~IHostHandle() = default;                           // phase 2: terminate + join
  };
  struct HostConnection { transport::Channel control; std::vector<transport::Channel> lanes; std::unique_ptr<IHostHandle> handle; };
  class IHostConnector { public: virtual std::expected<HostConnection, ExtensionError> connect() = 0; virtual ~IHostConnector() = default; };
  }
  ```

- [ ] **Step 1: Rename the Wasmtime backend into an engine.** `WasmtimeBackend` → `WasmtimeEngine : host::IHostedEngine`, `memoryMode()` → `MemoryMode::Copy`; `makeWasmtimeBackend` → `makeWasmtimeEngine`; traits and `PreparedGeneration.cpp` follow. Run `vtextension_wasmtime_test "[wasm-contract]"` (PASS); commit: `git commit -s -m "refactor(vtextension): the Wasmtime backend becomes an engine a host process drives"`.
- [ ] **Step 2: Write the server tests** (`HostServer_test.cpp`; a `ScriptedParent` fixture holds the parent ends as `PduStream`s and runs `HostServer::run()` on a jthread over `makeWasmtimeEngine(kind, { TrapMode::Software })`, with the contract's modules):
  - `"HostServer.helloComesFirst"`: first control PDU is `Hello{ ProtocolVersion, MemoryMode::Copy, LockdownState::Unsandboxed }`.
  - `"HostServer.loadDescribes"`: CreateRuntime, `Load(DescribedModule)` → `Loaded` imports/exports equal an in-process `runtime->load(...)`'s.
  - `"HostServer.callRoundTrip"`: Instantiate, `Call` on lane 0 → `CallResult{ nullopt, expected argv }`.
  - `"HostServer.callHostNests"`: guest calls `pair(7, 9)` → `CallHost{ 1, {7, 9} }`; the test sends a nested `Call` before `HostResult{}` and gets that call's `CallResult` first, then the outer one's.
  - `"HostServer.memoryOpsOnTheLane"`: `MemoryWrite` then `MemoryRead` of the same range, idle and parked in `CallHost` → the written bytes; out of bounds → `MemoryStatus::OutOfBounds`.
  - `"HostServer.malformedFrameEndsTheConnection"`: garbage on lane 0 → `run()` returns `ProtocolViolation`; every parent end reads `StreamError::Closed`.
  - `"HostServer.stopInterruptsARunningGuest"`: `Call` an infinite loop, then `stop()` → `run()` returns `Stopped` within 1 s (`std::future::wait_for`).
  - `"HostServer.releaseDropsTheInstance"`: after `Release{Instance, id}`, a `Call` on that id → `ProtocolViolation`.
  - `"HostServer.hostCallDuringInstantiateFails"`: a start function calling `ping` → `Instantiated{ error = InvalidComponent }` (D11).
- [ ] **Step 3: Build; expect link failure.** `nice -n 19 ionice -c 3 cmake --build --preset clang-asan -j 4 --target vtextension_remote_test`.
- [ ] **Step 4: Implement `HostServer` and `InProcessHost`.**
  - **Maps:** runtime/module/instance ids → `shared_ptr`s of engine objects under one mutex, never held across I/O. An id never created or reused → `ProtocolViolation` (the parent assigns ids, D19).
  - **Executor per lane** loops on `receive()`. `Call`: (re-)enter the engine's guard if `!isCurrentThreadPrepared()`, mark the instance busy (atomic; busy on another lane is a violation), set `thread_local LaneExecutor* currentLane` for the call's duration. `MemoryRead`/`MemoryWrite`: served through `IWasmInstance::memory` on this thread. Anything else: violation.
  - **`LaneHostContext::callHost`:** no current lane → `RuntimeUnavailable`; otherwise send `CallHost` and loop: serve `MemoryRead`/`MemoryWrite`, recurse on `Call`, return on `HostResult`.
  - **Control thread:** sends `Hello`, serves `CreateRuntime` (keeps its own copy of the function strings), `Load`, `Instantiate` (`callableExports` = ordinals `instance->function(name)` resolves), `Release`.
  - **Teardown:** `stop()` → `interrupt()` every live instance, then `shutdown()` every stream.
  - **`InProcessHost` (D17):** members engine, `HostServer`, `std::jthread` running `run()`, in that order; `IHostHandle::terminate` = `server.stop()`; destructor = `terminate()` then join.
- [ ] **Step 5: Run.** `out/clang-asan/src/vtextension/wasm/remote/vtextension_remote_test "HostServer.*"` → PASS.
- [ ] **Step 6: Commit.** `git commit -s -m "feat(vtextension): serve extension modules over channels from a host"`

### Task 2.3: `RemoteBackend` over an in-process `HostServer` (copy-path memory)

**Files:**
- Create: `src/vtextension/wasm/remote/{RemoteBackend.hpp,RemoteBackend.cpp,RemoteKindState.hpp,HelperIncarnation.hpp,HelperIncarnation.cpp,CopyPathMemory.hpp,CopyPathMemory.cpp,RemoteBackend_test.cpp,CopyPathMemory_test.cpp}`
- Create: `src/vtextension/wasm/BackendConfig.hpp` (D15)
- Modify: `src/vtextension/wasm/remote/CMakeLists.txt` (target `vtextension_remote`: links `vtextension vtextension_remote_wire`; tests join `vtextension_remote_test`; `CopyPathMemory_test.cpp` joins `vtextension_test`)

**Interfaces:**
- Consumes: 2.1, 2.2 (`IHostConnector`, `InProcessHostConnector`, `makeWasmtimeEngine`), 2.4.
- Produces:
  ```cpp
  namespace vtextension::wasm {
  struct HostTimeouts { std::chrono::milliseconds load { 10'000 }; std::chrono::milliseconds instantiate { 1'000 };
                        std::chrono::milliseconds handshake { 2'000 }; std::chrono::milliseconds interruptGrace { 50 }; };
  struct BackendConfig {
      std::filesystem::path helper;                         ///< The helper executable (phase 3); unused before.
      HostTimeouts timeouts {};                             ///< Enforced by the supervisor (phase 3).
      std::shared_ptr<remote::IHostConnector> connector;    ///< How helper incarnations are started; null: in-process.
  };
  }
  namespace vtextension::wasm::remote {
  class RemoteKindState;   // generation, live runtimes, lane slots [MaxLaneChannels], per-kind serial id (D29)
  [[nodiscard]] std::shared_ptr<RemoteKindState> makeRemoteKindState();
  [[nodiscard]] std::unique_ptr<IWasmBackend> makeRemoteBackend(std::shared_ptr<RemoteKindState> kind, BackendConfig config);
  /// Test-only: the generation the calling thread's outermost remote guard was entered for; 0 if none.
  [[nodiscard]] uint64_t preparedGeneration() noexcept;
  }
  ```
  Internal: `HelperIncarnation` (control `PduStream` + mutex, `MaxLaneChannels` lane streams, `IHostHandle`, `std::atomic<IncarnationState>` with `enum class IncarnationState : uint8_t { Dead, Alive }`, id counters, `void markDead() noexcept` terminating the handle exactly once); `HelperSlot` (per `ModuleIdentity.key` within a `RemoteRuntime`: current incarnation, respawn counter); `CopyPathMemory`: `std::optional<std::span<std::byte>> view(uint32_t address, uint32_t size)`, `flush() -> std::vector<proto::MemoryWrite>`, `clear()`.

- [ ] **Step 1: Instantiate the contract suite** in `RemoteBackend_test.cpp`:
  ```cpp
  struct RemoteTraits
  {
      [[nodiscard]] static std::unique_ptr<IWasmBackend> make()
      {
          static auto const kind = makeRemoteKindState();
          static auto const engineKind = wasmtime::makeKindState();
          auto connector = std::make_shared<host::InProcessHostConnector>(
              [] { return wasmtime::makeWasmtimeEngine(engineKind, { .trapMode = wasmtime::TrapMode::Software }); },
              host::HostServerConfig {});
          return makeRemoteBackend(kind, BackendConfig { .connector = std::move(connector) });
      }
  };
  CONTOUR_WASM_BACKEND_CONTRACT(RemoteTraits)
  ```
- [ ] **Step 2: Write the remote-only tests** (an `InProcessHostConnector` the test keeps):
  - `"RemoteBackend.sameIdentitySharesAHelper"`: two loads with `ModuleIdentity{ "a" }` → `connects() == 1`; `{ "b" }` → `2`; two loads with `ModuleIdentity {}` → `+2` (D18).
  - `"RemoteBackend.helperDeathFailsCallsHostCrashed"`: instantiate, `terminateAll()`, then `call(...).error() == CallFailure::HostCrashed`, `memory(0, 4) == std::nullopt`, a second call fails `HostCrashed` with `connects()` unchanged.
  - `"RemoteBackend.instantiateRespawnsLazily"`: after the death, `module->instantiate(ctx)` succeeds, `connects() == 2`, the new instance calls correctly.
  - `"RemoteBackend.writesFlushBeforeCall"`: `memory(a, 4)` written `{1,2,3,4}`, then an export returning `load32(a)` → `0x04030201`.
  - `"RemoteBackend.writesFlushBeforeHostResult"`: write through `memory()` inside `callHost`; the guest reads it after the host function returns.
  - `"RemoteBackend.overlappingViewsSeePendingWrites"`: view A = [a, a+8) written, view B = [a+4, a+12) reads A's bytes; after the flush both writes land (D12).
  - Scripted connector (the test thread plays the helper): `"RemoteBackend.versionMismatchIsHostUnavailable"` (`Hello{ ProtocolVersion + 1, … }` → `load(...).error() == HostUnavailable`); `"RemoteBackend.lockdownFailureIsHostUnavailable"` (`LockdownState::Failed` → same); `"RemoteBackend.malformedPduKillsTheHelper"` (garbage on the lane during a call → `HostCrashed`, handle saw `terminate()` once); `"RemoteBackend.overCapPduKillsTheHelper"` (`MemoryData` header declaring Mem + 17 → same); `"RemoteBackend.describedMoreThanDeclaredKillsTheHelper"` (`Loaded` with one import more than `declaredCounts` → `HostUnavailable`).
  - `"RemoteBackend.fifthLaneThreadFailsItsCall"`: 4 threads park inside host calls; a 5th thread's `call` fails `CallFailure::Trap` (D10).
  - `"RemoteBackend.interruptIsWaitFreeOfTheHelper"`: interrupt during a guest loop returns while the server's lanes are busy; the call fails `Interrupted`.
- [ ] **Step 3: Build; expect link failure.** Same command as 2.2.
- [ ] **Step 4: Implement.**
  - **`RemoteKindState`** like Wasmtime's `KindState`: generation counts 0 → 1 transitions of live runtimes; trivial guards remembering their generation; `RuntimeUnavailable` while no runtime is up; thread-local stamps keyed by the kind's serial (D29); lane slot taken on first lane use under a guard, freed by the outermost guard's destructor (D10).
  - **`RemoteRuntime::load(identity, bytes)`:** find or connect the `HelperSlot` for `identity.key` (empty key: always a new slot). Connect = `connector->connect()`, read `Hello`, send `CreateRuntime`; connect error, mismatch or `Failed` → `HostUnavailable` and terminate the handle. Then `Load`; the module keeps `bytes`, identity and slot; `Loaded` is checked against `declaredCounts(bytes)`.
  - **`RemoteModule::instantiate`:** dead incarnation → respawn (connect, CreateRuntime, Load; the new `Loaded` must equal the first or `InvalidComponent`), then `Instantiate` on control; the `RemoteInstance` holds the incarnation `shared_ptr`.
  - **`RemoteInstance::call`:** (1) `Interrupted` if the flag is set, `HostCrashed` if dead; (2) send every `CopyPathMemory::flush()` write as `MemoryWrite`, then `clear()`; (3) `Call` on the thread's lane; (4) loop — `CallHost`: run `context.callHost`, flush + clear, send `HostResult`; `CallResult`: check `argv.size()`, copy results back; (5) any `StreamError` or unexpected kind → `markDead()` → `HostCrashed`.
  - **`memory()`:** size 0 → empty span, no I/O; else `CopyPathMemory::view` (sends `MemoryRead` on the lane only for an uncached range); cache above the runtime's `memoryBytes` → `nullopt`.
  - **`interrupt()`:** store the sticky flag, then `handle->interrupt(instanceId)` (D13).
  - **Destructors:** `Release` on control while alive, errors ignored; the last incarnation reference destroys its handle (D17).
  - **Flush algorithm (D12):** each fetched range keeps a snapshot; on a new range, copy the current bytes of every earlier overlapping range into it, then snapshot it; `flush()` walks ranges in fetch order and emits one `MemoryWrite` per maximal run of bytes differing from the snapshot.
- [ ] **Step 5: Unit-test `CopyPathMemory`** (`CopyPathMemory_test.cpp`, in `vtextension_test`): `"CopyPathMemory.flushEmitsOnlyChangedRuns"` (one 2-byte write for a 16-byte view whose bytes 3–4 changed), `"CopyPathMemory.laterViewWins"`.
- [ ] **Step 6: Run.** `out/clang-asan/src/vtextension/wasm/remote/vtextension_remote_test "[wasm-contract]" "RemoteBackend.*"` and `out/clang-asan/src/vtextension/vtextension_test "CopyPathMemory.*"` → PASS, including `installsNoSignalHandlers`; LSan silent.
- [ ] **Step 7: Commit.** `git commit -s -m "feat(vtextension): a backend that runs extensions in a separate host over channels"`

### Task 2.4: Contract changes at the seam

Runs right after 2.1 (D1).

**Files:**
- Modify: `src/vtextension/wasm/WasmRuntime.hpp`, `src/vtextension/ExtensionError.hpp` (text table row), `src/vtextension/CompiledExtension.{hpp,cpp}`, `src/vtextension/Instance.cpp` (`guestFailure`), `src/vtextension/ExtensionHost.cpp` (`prepareExtensions` passes the identity), the WAMR and Wasmtime `load` implementations, `testing/FakeWasmBackend.hpp`, `wasm/testing/BackendContract.hpp`, and every `CompiledExtension::load` / `runtime.load` caller (grep)

**Interfaces:**
- Produces:
  ```cpp
  namespace vtextension::wasm {
  /// Which host a module runs in. Modules with equal keys share one; an empty key gets a host of its own.
  /// In-process backends ignore it.
  struct ModuleIdentity { std::string key; bool operator==(ModuleIdentity const&) const = default; };
  enum class CallFailure : uint8_t { Trap, Interrupted, MemoryGrowthRefused, HostFailure,
                                     HostCrashed /* the host running the instance died; every later call fails so */ };
  class IWasmRuntime { /* … */ [[nodiscard]] virtual std::expected<std::shared_ptr<IWasmModule>, ExtensionError>
      load(ModuleIdentity identity, std::vector<std::byte> bytes) = 0; };
  }
  namespace vtextension {
  enum class ExtensionError : uint8_t { /* … */ ModuleTooLarge, HostUnavailable /* appended */ };
  // CompiledExtension::load(Engine& engine, wasm::ModuleIdentity identity, std::vector<std::byte> bytes);
  }
  ```
  `HostUnavailable` text: `"its host process is unavailable"` (user-visible; names no engine). `Instance::guestFailure`: `HostCrashed` → `HostUnavailable` (D21). `FakeWasmBackend` gains `[[nodiscard]] std::size_t loads() const noexcept` and `[[nodiscard]] wasm::ModuleIdentity lastIdentity() const`.

- [ ] **Step 1: Write the failing tests.** `"CompiledExtension.passesHostUnavailableThrough"` (fake `load` returns `HostUnavailable` → `CompiledExtension::load(...).error() == HostUnavailable`; today `InvalidComponent`); `"CompiledExtension.passesIdentityToTheRuntime"` (`fake.lastIdentity() == ModuleIdentity{ "x" }`); `"Instance.hostCrashedIsHostUnavailable"` (an export returning `CallFailure::HostCrashed` → `render(...).error() == HostUnavailable`); `"ExtensionError.everyErrorHasText"` covers the new row.
- [ ] **Step 2: Build; expect compile failure.** `… --target vtextension_test vtextension_wasmtime_test`.
- [ ] **Step 3: Change the seam and its implementers.** WAMR and Wasmtime take `ModuleIdentity` and ignore it. `CompiledExtension::load` passes only `HostUnavailable` through, the rest stays `InvalidComponent`. Contract helpers pass `ModuleIdentity {}`. `prepareExtensions` passes `ModuleIdentity { spec.hostId }` until 2.6 replaces it with the registry key.
- [ ] **Step 4: Update callers mechanically.** Remaining call sites pass `wasm::ModuleIdentity {}` (tests) or the spec's identity.
- [ ] **Step 5: Run.** `out/clang-asan/src/vtextension/vtextension_test` and `out/clang-asan/src/vtextension/wasm/wasmtime/vtextension_wasmtime_test` → all PASS.
- [ ] **Step 6: Commit.** `git commit -s -m "feat(vtextension): name a module's host at the runtime seam, and report a crashed one"`

### Task 2.5: `makeDefaultBackend(BackendConfig)`

**Files:**
- Modify: `src/vtextension/wasm/DefaultBackend.hpp` (includes `wasm/BackendConfig.hpp`), `wasm/wamr/DefaultBackend.cpp` (ignores the config, D16)
- Modify composition roots: `src/contour/ContourGuiApp.cpp` (GUI), `src/contour/cli/ContourApp.cpp` (daemon resources), `src/vtbackend/bench-headless.cpp`, `src/vthost/SessionHost_test.cpp`, `src/vtextension/testing/{Harness.hpp,RunnerHarness.hpp}`, and every `wasm::makeDefaultBackend()` in `vtextension/*_test.cpp` (grep)
- Create: `src/vtextension/wasm/remote/testing/RemoteDefaultBackend.cpp` (defines `makeDefaultBackend(BackendConfig)` → `makeRemoteBackend` over an `InProcessHostConnector` + Wasmtime when `config.connector` is null, and `wasm::testing::preparedGeneration()` → `remote::preparedGeneration()`)
- Modify: `wasm/remote/CMakeLists.txt` (`vtextension_remote_test` also compiles `Bundled_test.cpp`, `Instance_test.cpp`, `ExtensionHost_test.cpp`, `Engine_test.cpp`, `ExtensionWorkerPool_test.cpp`, `ControlImports_test.cpp` and `RemoteDefaultBackend.cpp`, with `VTEXTENSION_TEST_MODULE_DIR` and the guest dependencies copied from `vtextension_test`; links `glaze::glaze` privately)

**Interfaces:**
- Consumes: `BackendConfig` (2.3).
- Produces: `[[nodiscard]] std::unique_ptr<IWasmBackend> makeDefaultBackend(BackendConfig config);` — no default argument.

- [ ] **Step 1: Change the declaration; expect compile failures at every root.** `… --target contour vtextension_test bench-headless vthost_test`.
- [ ] **Step 2: Update each root** to `vtextension::wasm::makeDefaultBackend(vtextension::wasm::BackendConfig {})`. GUI and daemon roots keep a named local `BackendConfig` so phase 3 has one place to fill the helper path and launcher.
- [ ] **Step 3: Add the remote composition to `vtextension_remote_test`.** Cases depending on in-process internals that fail there are tagged `[in-process]` and excluded on that binary with `~[in-process]`, each with a one-line reason in "Phase-2 facts".
- [ ] **Step 4: Run.** `ctest --preset=clang-asan -R 'vtextension|bench_headless'` and `out/clang-asan/src/vtextension/wasm/remote/vtextension_remote_test "~[in-process]" "~[.measure]"` → PASS (Bundled cases run real guests through `RemoteBackend`).
- [ ] **Step 5: Commit.** `git commit -s -m "refactor: configure the extension runtime at every composition root"`

### Task 2.6: `ExtensionRegistry` and the per-extension breaker

**Files:**
- Create: `src/vtextension/ExtensionRegistry.hpp`, `ExtensionRegistry.cpp`, `ExtensionRegistry_test.cpp` (vtextension_test)
- Create: `src/vtextension/wasm/remote/RegistryRemote_test.cpp` (vtextension_remote_test)
- Modify: `CompiledExtension.{hpp,cpp}` (breaker), `Instance.{hpp,cpp}` (admission + reporting), `ExtensionHost.{hpp,cpp}` (`prepareExtensions` takes the registry), `SessionExtensions.{hpp,cpp}` (`SessionExtensionServices::registry`, `ExtensionRuntime` owns one, `configurationReloaded()`), `src/contour/ContourGuiApp.cpp` (`onConfigurationReloaded`), the daemon's reload in `src/vthost/` (grep), `CMakeLists.txt` (sources)

**Interfaces:**
- Consumes: 2.4 (`ModuleIdentity`, `HostUnavailable`), 2.3/2.5 for the remote tests.
- Produces:
  ```cpp
  namespace vtextension {
  enum class ExtensionWorld : uint8_t { StatusLine };
  enum class BreakerState : uint8_t { Closed, Open };
  struct HostAdmission { uint64_t epoch = 0; };
  /// Counts an extension's host deaths, once each however many panes saw it (D20). Thread-safe.
  class HostBreaker {
  public:
      HostBreaker(core::IClock& clock, std::size_t faultsBeforeDisable, std::chrono::seconds faultWindow);
      [[nodiscard]] std::expected<HostAdmission, ExtensionError> admit() noexcept;   // HostUnavailable while Open
      void recordHostFault(HostAdmission admission) noexcept;   // counts iff admission.epoch is current; then ++epoch
      void reset() noexcept;                                    // Closed, no faults, ++epoch
      [[nodiscard]] BreakerState state() const noexcept;
      [[nodiscard]] std::size_t faultCount() const noexcept;    // within faultWindow
  };
  struct RegistryKey { std::string hostId; ExtensionWorld world; std::string configDigest; std::string moduleDigest;
                       bool operator==(RegistryKey const&) const = default; };
  [[nodiscard]] RegistryKey registryKeyOf(ExtensionSpec const& spec, ExtensionWorld world);   // SHA-256, hex (D23)
  [[nodiscard]] wasm::ModuleIdentity moduleIdentityOf(RegistryKey const& key);  // hostId, world, configDigest; not the module
  class ExtensionRegistry {
  public:
      ExtensionRegistry(core::IClock& clock, ExtensionLimits limits);
      /// The live entry for @p key on @p engine, or loads one (once, however many ask at once: D22).
      [[nodiscard]] std::expected<std::shared_ptr<CompiledExtension>, ExtensionError> acquire(
          Engine& engine, RegistryKey const& key, std::vector<std::byte> bytes);
      void resetBreakers() noexcept;
      [[nodiscard]] std::size_t liveEntries() const;
  };
  // CompiledExtension::load(Engine&, wasm::ModuleIdentity, std::vector<std::byte>, std::shared_ptr<HostBreaker> breaker /* nullable */);
  // CompiledExtension::admitHost() -> std::expected<HostAdmission, ExtensionError>;  recordHostFault(HostAdmission) noexcept
  // prepareExtensions(ExtensionHostConfig, std::expected<std::shared_ptr<Engine>, ExtensionError>, ExtensionRegistry* registry /* nullable */);
  // ExtensionRuntime::configurationReloaded() noexcept { _registry.resetBreakers(); }
  }
  ```

- [ ] **Step 1: Write the breaker and registry tests** (`ExtensionRegistry_test.cpp`; `core::ManualClock`, `FakeWasmBackend`):
  - `"ExtensionRegistry.breakerOpensAtTheLimit"`: 3 faults, each with a fresh admission, within 60 s → `state() == Open`, `admit().error() == HostUnavailable`.
  - `"ExtensionRegistry.faultsAgeOut"`: fault, `clock.advance(61s)`, fault, fault → `Closed`.
  - `"ExtensionRegistry.staleAdmissionCountsOnce"`: 10 × `recordHostFault(sameAdmission)` → `faultCount() == 1`.
  - `"reload resets breaker"`: open it, `registry.resetBreakers()` → `Closed`, `admit()` succeeds, `recordHostFault(oldAdmission)` leaves `faultCount() == 0`.
  - `"rebuild with unchanged bytes reuses the entry"`: two `SessionExtensions::rebuild` with the same declared extensions via a `QueuedOwner` → `fake.loads() == 1`, `registry.liveEntries() == 1`; changed bytes → `loads() == 2`.
  - `"ExtensionRegistry.differentConfigGetsItsOwnEntry"`: same path, different `ConfigMap` → two entries, two different `fake.lastIdentity()` keys.
  - `"registry drop races instantiate"` (`[threading]`): 200 rounds; acquire `first`, a jthread drops it while the test thread acquires `second` and runs `Instance::create(engine, **second, …)`; `REQUIRE(second)`, `CHECK(instance)`, after the join `CHECK(registry.liveEntries() <= 1)`. TSan-clean.
- [ ] **Step 2: Write the remote tests** (`RegistryRemote_test.cpp`; `InProcessHostConnector`, a real statusline test module):
  - `"one death counts once across panes"`: 10 `Instance`s of one entry, `connector.terminateAll()`; every `render({10, 1})` → `HostUnavailable`; then `breaker.faultCount() == 1`, `connector.connects() == 1`; the next `Instance::create` → `connects() == 2`.
  - `"breaker open fails fast without spawning"`: 3 deaths, each followed by a new instance; then `Instance::create(...).error() == HostUnavailable` with `connects()` unchanged.
  - `"registry drop races instantiate (remote)"`: as Step 1 over the real helper; afterwards `connector.liveHosts() == 0` once every reference is gone.
- [ ] **Step 3: Build; expect compile failures.** `… --target vtextension_test vtextension_remote_test`.
- [ ] **Step 4: Implement.**
  - `HostBreaker`: mutex, `std::deque<SteadyTimePoint>` pruned like `Runner::fault`, epoch, state; Open until `reset()`.
  - `CompiledExtension` holds a nullable `shared_ptr<HostBreaker>`; without one `admitHost()` always succeeds.
  - `Instance::create` admits first (open → `HostUnavailable` before instantiating) and keeps the admission; every `HostUnavailable` from instantiate or `callExport` reports it once per instance; Runner's own counting unchanged (D20).
  - `ExtensionRegistry`: one mutex around `RegistryKey` → loading (`std::shared_future<std::expected<std::shared_ptr<CompiledExtension>, ExtensionError>>`) or live (`std::weak_ptr`); an entry whose engine is not `&engine` is a miss; expired entries swept on every acquire; loads run outside the lock.
  - `prepareExtensions` uses `registry->acquire(engine, registryKeyOf(spec, ExtensionWorld::StatusLine), …)` when non-null. `ExtensionRuntime` builds `ExtensionRegistry { _pool.clock(), limits }`, declared before `_engines` so entries die before the engine provider.
  - `ContourGuiApp::onConfigurationReloaded` and the daemon's reload call `configurationReloaded()` on the started runtime, if any.
- [ ] **Step 5: Run.** `out/clang-asan/src/vtextension/vtextension_test "ExtensionRegistry.*" "reload resets breaker" "rebuild with unchanged bytes reuses the entry" "registry drop races instantiate" "[threading]"` and `out/clang-asan/src/vtextension/wasm/remote/vtextension_remote_test "one death counts once across panes" "breaker open fails fast without spawning" "registry drop races instantiate (remote)"` → PASS.
- [ ] **Step 6: Run the whole suites.** `ctest --preset=clang-asan -R vtextension` → PASS.
- [ ] **Step 7: Commit.** `git commit -s -m "feat(vtextension): load each extension once per process, behind a breaker for its host"`

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

Task 2.7 — the gate (head 6b9e33c9e; measurement case committed there):
- ASan/LSan: `ctest --preset=clang-asan -R vtextension` 3/3 (vtextension_test 384 cases, vtextension_wasmtime_test 52, vtextension_remote_test 181); `[wasm-contract]` 40 cases on each of the three; `[bundled]` 14 cases on both compositions; no LSan report (the only suppressions are the project-wide GUI/driver ones in cmake/lsan-suppressions.txt).
- TSan: `vtextension_test "[threading]"` 21 cases, `vtextension_remote_test "~[.measure]"` 181 cases; zero reports, no suppressions.
- Measurement (out/clang-release, plasma, 1000 timed renders, three runs; medians): remote steady 18.5 / 18.9 / 19.1 µs (p90 ~20 µs), uncached 29.0 / 29.4 / 28.9 µs; Wasmtime in-process re-run alongside: steady 3.3 / 2.8 / 3.4 µs, uncached 12.5 / 12.6 / 13.1 µs (Phase 1: 2.8 / 12.4). So the process boundary adds ~16 µs to a render, steady or uncached alike (~6.5x steady, ~2.3x uncached). Bare round trip (`add`, 10 000 calls): median 3.4 / 3.2 / 3.3 µs, p90 ~4 µs, fastest 1.7 µs, 33 ms in all. A render therefore costs about five round trips' worth over the boundary (call + host calls + copy-path memory reads; inference, not counted). Host = in-process HostServer over real channels on Wasmtime (software traps), so a real process adds scheduling across processes on top. Load ~60 ms on both (compile-dominated); start 0.3 ms remote vs 0.1 ms.
- Coverage (clang-coverage, the three suites, line coverage): RemoteBackend.cpp 94.0% of 299, RemoteKindState.cpp 92.0% of 75, HelperIncarnation.cpp 87.7% of 73, CopyPathMemory.cpp 96.0% of 101, proto/Codec.cpp 99.6% of 271, transport/Channel_posix.cpp 90.0% of 60, transport/PduStream.cpp 95.6% of 45, host/HostServer.cpp 95.1% of 385, host/InProcessHost.cpp 97.8% of 89, host/ExecutorThread.cpp 80.0% of 35 (move-assignment and the thread-creation failure unreached), ExtensionRegistry.cpp 95.1% of 103; all .cpp together 94.9% of 1536. Uncovered lines are error paths (connector failure, send failures other than OverCap).
- Windows cross build: CMake needs `-DCMAKE_MSVC_DEBUG_INFORMATION_FORMAT=Embedded` with the debug preset: WAMR enables ASM, and clang-cl as ASM compiler does not know the default `ProgramDatabase` format (configure error in _deps/wamr-src).
- Windows under wine (R12): `vtextension_test` cannot cross-build. WAMR picks its platform from `CMAKE_HOST_SYSTEM_NAME` (linux → `pthread.h` not found) unless `-DWAMR_BUILD_PLATFORM=windows`; with that, WAMR's `core/shared/platform/windows/win_file.c:1298` has a `// \??\ ` comment ending in backslash-space, which clang splices into the next line (MSVC does not): `error: expected identifier`. So WAMR on Windows builds with MSVC only. The transport/codec suites were built instead as a local, uncommitted target (test_main + Codec_test + Channel_test + PduStream_test over `vtextension_remote_wire`, out/clangcl-debug, `linux-to-windows-clangcl-debug` with `CONTOUR_EXTENSIONS=ON`, `CONTOUR_EXTENSIONS_BUNDLED=OFF`, `CONTOUR_FRONTEND_GUI=OFF`).
- That build found a real Windows break: `Channel_win32.cpp` (and the test) `#define NOMINMAX` over the build's `-DNOMINMAX` → `-Wmacro-redefined` / MSVC C4005 under -Werror. Fixed in d0cf0568b.
- Under wine 11.20 (Staging), own prefix: 22/22 cases, 7321 assertions, five runs green (`Channel.*`, `PduStream.*`, `PduCodec.*`; `Channel.shutdownEndsThePeersStream` and `Channel.endsAreNotInheritable` included; `Channel.endsAreCloseOnExec` is POSIX-only). Wine is not Windows: evidence, not proof.

---

## Phase-3 decisions

Proposed for a controller ruling (as R10 did for D1–D29). Each names the choice and, after the dash, why.

- D30. New directory `src/vtextension/wasm/remote/process/` (namespace `vtextension::wasm::remote::process`), target `vtextension_remote_process`: launcher, process handles, exit wait set, supervisor, `ProcessHostConnector`, `HostTimers`, `LaneWatch` (namespace `remote`), helper location. Links only `vtextension_remote_wire core::base core::platform`; `vtextension_remote` and `vtextension_host` link it — it stays std-only, so it cross-builds and runs under wine without vtbackend.
- D31. `vtextension_remote_wire` gains what both processes need: `Channel::adopt`, `Channel::trySend`, `ControlPage`, `WakeSender`/`WakeReceiver`, `HelperArguments` — the helper must not link the launcher's library to read its own arguments.
- D32. `src/contour-extension-host/` holds `main.cpp` only and is built iff `TARGET vtextension_wasmtime` (the WAMR engine in the helper is Task 5.3). The engine comes from a neutral `wasm/host/HelperEngine.hpp` (`makeHelperEngine()`), defined in `wasm/wasmtime/HelperEngine.cpp` (target `vtextension_wasmtime_helper_engine`) — the containment lint's rule 3 forbids Wasmtime headers outside `wasm/wasmtime/`, as phase 2's `HostedEngine` probe already respects.
- D33. Helper trap mode: `TrapMode::Signals` in plain builds, `TrapMode::Software` when `CONTOUR_SANITIZE` or `CONTOUR_COVERAGE` is set (compile definition on `vtextension_wasmtime_helper_engine`) — sanitizer runtimes own SIGSEGV; Task 5.2 inherits this.
- D34. `main` is a thin call to `host::runExtensionHost(arguments, engines)` (`wasm/host/HostProcess.{hpp,cpp}`): parse, adopt, bind to parent, serve through `HostProcess` (server + interrupt listener), map `HostProcessExit` to an exit code through one table (codes 0 and 70–75, never 1/23/66, which ASan/LSan/TSan use). `InProcessHost` is rebuilt on `HostProcess` — the in-process tests then drive the same interrupt path as the real helper, under TSan.
- D35. Handles reach the helper by inheritance, named in argv: `--control=H --lanes=H,H,H,H --page=H --wake=H --parent=PID --data-bytes=N`. POSIX child numbers are fixed (control 3, lanes 4–7, page 8, wake 9; `dup2` by the launcher); Windows passes the inherited handle values. `LaunchSpec::arguments` is a function of the child-side values, because on Windows they are known only after duplication. Table-driven parser: unknown, duplicate, missing or malformed → `HostProcessExit::BadArguments` — one row per option, so phase 5 adds a row, not a branch.
- D36. Launch per OS. Linux: `posix_spawn` (glibc's `CLONE_VFORK` path: no `fork` of a large, sanitized, multi-threaded parent) with `posix_spawn_file_actions_addclosefrom_np(10)`, `addchdir_np("/")`, `POSIX_SPAWN_SETSIGDEF|SETSIGMASK`, then `pidfd_open`; a kernel without pidfd (< 5.3) → `LaunchError::Unsupported` (Landlock in phase 5 needs 5.13 anyway). FreeBSD: `pdfork(PD_CLOEXEC)`, the child only `dup2`/`closefrom(10)`/`chdir`/`execve`. macOS: `posix_spawn` with `POSIX_SPAWN_CLOEXEC_DEFAULT` + `dup2` actions + `addchdir_np`, exit through kqueue `EVFILT_PROC`; the CLOEXEC window on the parent's ends is documented (spec §5.3). Windows: `CreateProcessW` with `PROC_THREAD_ATTRIBUTE_HANDLE_LIST` (inheritable duplicates, closed after the call) and `PROC_THREAD_ATTRIBUTE_JOB_LIST` (job: `KILL_ON_JOB_CLOSE`, `ACTIVE_PROCESS_LIMIT` 1, `PROCESS_MEMORY` = `ResourceCaps::dataBytes`, `DIE_ON_UNHANDLED_EXCEPTION`) — the job applies from the first instruction, no suspended window.
- D37. Parent-death binding and POSIX caps are applied by the helper itself. `bindToParent(parentPid)` runs first in `runExtensionHost`: Linux `prctl(PR_SET_PDEATHSIG, SIGKILL)` then `getppid() == parentPid` (else `ParentGone`) — the binding is to the thread that ran `posix_spawn`, the supervisor thread (D42), exactly as a pre-exec `prctl` would be; FreeBSD `procctl(PROC_PDEATHSIG_CTL)` + the same check; macOS the check only (no equivalent, documented); Windows nothing (the job). Caps (`RLIMIT_NPROC = 0` on Linux and FreeBSD, `RLIMIT_DATA` on Linux) are applied by a new `HostServerConfig::confine` hook that `HostServer::run()` calls after every lane's thread and the listener started, before `Hello` — on Linux `RLIMIT_NPROC` counts threads, so caps applied pre-exec would stop the helper from starting its own executors; phase 5's lockdown does NOT plug into this hook: no_new_privs and Landlock are per-thread and must precede thread creation (spec §5.6 step 2), so they go into `HostConfinement::beforeThreads` (phase-3 fix wave), which `runExtensionHost` calls right after `bindToParent` and before the endpoints are adopted, the engine is made or any thread starts; it already applies R26 (`hardenProcess()`: RLIMIT_CORE 0, Linux PR_SET_DUMPABLE 0, FreeBSD PROC_TRACE_CTL_DISABLE, Windows WerSetFlags NO_UI). Only seccomp with TSYNC (step 4) runs after the threads, in `confine`. A `Failed` from `beforeThreads` makes `Hello` state `Failed` whatever `confine` returns.
- D38. Cap mechanism (the measurement the plan assigned to 3.0, decided by the kernels' semantics and confirmed by a measurement in 3.7). Linux `RLIMIT_DATA`: it counts private writable mappings, not `PROT_NONE` reservations or guard regions, so Wasmtime's per-memory reservation costs nothing until committed; the executor stacks (4 × `ExecutorStackBytes` = 40 MiB, the carried 2.2 minor) do count. Default `ResourceCaps::dataBytes = 1 GiB`; 3.7 measures `VmData` and the default stands only with ≥ 4× headroom. FreeBSD `RLIMIT_AS`: sized in Task 5.4 against WAMR (no helper runs there before it). macOS: no cap (`RLIMIT_AS`/`RLIMIT_DATA` do not bound `mmap`), documented. Windows: the job's memory limit. Sanitizer builds apply neither `RLIMIT_NPROC` nor `RLIMIT_DATA` (shadow memory; LSan's stop-the-world clones), and the helper says so once on stderr.
- D39. Environment: empty on POSIX, only `SystemRoot` on Windows (`defaultHelperEnvironment()`); test compositions append the sanitizer option variables (`ASAN_OPTIONS`, `LSAN_OPTIONS`, `UBSAN_OPTIONS`, `TSAN_OPTIONS`) from one table, when set. stdin/stdout are the null device; stderr is inherited (diagnostics until Task 5.2's descriptor). Working directory `/`; on Windows the root of the helper's volume.
- D40. `IProcessHandle { id, kill, tryReap, waitable }`: `kill()` is idempotent and does nothing once `tryReap()` returned a status (on macOS a `kill(pid)` after reaping could hit a reused pid); the destructor neither kills nor waits. `IExitWaitSet` (watch, unwatch, `wait(deadline)`, wait-free `wake()`) is made by the launcher — a handle and the set that waits on it are one platform: Linux `poll` over an eventfd + pidfds; FreeBSD `poll` over a pipe + process descriptors; macOS kqueue `EVFILT_USER` + `EVFILT_PROC NOTE_EXIT`; Windows `WaitForMultipleObjects` over an event + ≤ 63 process handles, any beyond polled with `tryReap()` every 100 ms.
- D41. Ownership: `ExtensionHostSupervisor::start()` returns `std::unique_ptr` owning one `std::jthread`; connectors and handles hold a `std::shared_ptr<SupervisorMailbox>` (command queue + wait-set wake), never the supervisor — so no last reference can be dropped on the supervisor's own thread and join it. After the supervisor stopped, posts are dropped and `launch` fails `LaunchError::Stopped` (→ `HostUnavailable`). Its destructor kills every helper, reaps what exits within `ShutdownReapBudget` (100 ms; plus `teardownGrace` before the kill when that is set), then joins — bounded.
- D42. Every launch runs on the supervisor thread (required on Linux for the death-signal binding; uniform elsewhere). `SupervisorMailbox::launch` blocks its caller — a build or worker thread, never the owner thread (asserted in 3.3) — on a future.
- D43. Non-call timeouts are RAII watches: `IHostHandle::watch(HostStep, budget) -> StepWatch`. `HelperIncarnation::request(pdu, step)` arms one around each control exchange: `Hello` + `CreateRuntime` under `handshake`, `Load` under `load`, `Instantiate` under `instantiate`. Budgets come only from `BackendConfig::timeouts`. Expiry: the supervisor kills, calls the attachment's `onDeath` (→ `markDead`, channels shut), the blocked receive returns `Closed`, the request fails `HostUnavailable`. Closes R14 (a start function that never returns) and the control-channel half of the 2.7 header-stall note.
- D44. Lane I/O stays blocking, bounded by the backstop instead of a deadline-carrying I/O path: the pool watchdog interrupts at the call deadline, the grace timer kills, the kill shuts every channel, the blocked send or receive returns `Closed`, the call fails `HostCrashed`. That is §5.4's guarantee ("the write fails, the supervisor kills the helper, the call fails `HostCrashed`") with one mechanism; it deviates from §5.4's wording ("non-blocking with the remaining deadline"). Copy-path fetches are lane frames too (D49), so a `MemoryData` that stalls after its header (deferred 2.7 minor) is bounded the same way.
- D45. Fake time: the supervisor takes `core::IClock&` and passes absolute deadlines to the wait set; `FakeProcessLauncher`'s wait set ignores real time and returns only when woken; the test hook `supervisor.sync()` returns once everything posted before it was processed, so `clock.advance(…); supervisor.sync();` is deterministic. No timeout test waits on a real timer except the real-helper cases named in 3.3–3.6, which shrink their budgets to ≤ 200 ms.
- D46. Control page: 4096 bytes = `InterruptSlotCount` (1024) `std::atomic<uint32_t>` flags (`static_assert(std::atomic<uint32_t>::is_always_lock_free)`). Linux and FreeBSD ≥ 13: `memfd_create(MFD_CLOEXEC|MFD_ALLOW_SEALING)`, sealed `SHRINK|GROW|SEAL`; macOS: `shm_open` of `/contour-eh-<pid>-<serial>` with `O_EXCL`, unlinked at once; Windows: a pagefile-backed section. The parent only stores to it, never reads it back, so a helper scribbling over it cannot affect the parent. The incarnation allocates one slot per instance (`InterruptSlots`), sends it in `Instantiate.interruptSlot` (wire addition, `ProtocolVersion = 2`) and clears it before reuse; exhaustion fails `instantiate` with `RuntimeUnavailable` — not a host fault, so no breaker count. Phase 4's memory objects may reuse the POSIX/Windows creation code.
- D47. The "wake pipe" is a one-byte AF_UNIX `SOCK_STREAM` socketpair on POSIX, sent with `MSG_DONTWAIT|MSG_NOSIGNAL` (macOS: `SO_NOSIGPIPE`) — a pipe write to a gone reader raises SIGPIPE, and the parent installs no handler; on Windows an inheritable auto-reset event (`SetEvent` never blocks). `WakeResult::Pending` = buffer full = a wake is already queued.
- D48. In the helper, an `InterruptListener` thread (not the control thread, which blocks on the control channel) waits on the wake receiver and a local stop signal, then scans the page and calls `HostServer::interruptSlot(slot)` for every raised slot → the engine's sticky `interrupt()`, which increments the Wasmtime epoch (Task 1.6). Started through `ExecutorThread::start` (fallible, `std::expected`). Replaces D13's direct in-process call.
- D49. Grace backstop: one `LaneWatch` per lane (atomics only: a frame stack of instance ids up to `MaxCallDepth + 1`, a per-frame parked-in-`CallHost` mark, the armed depth, an epoch), owned by the incarnation in a `std::shared_ptr<LaneWatchSet>` and handed to the supervisor in `HostAttachment`. `interrupt()` = raise the page slot, signal the wake, `LaneWatchSet::interrupt(instanceId)`, `mailbox.nudge()`: atomics and one non-blocking write, no lock, no clock. The supervisor timestamps with its own clock, keeps each armed lane's grace in `HostTimers` (paused while the innermost frame is parked in `CallHost`, reset when the epoch moves), re-observes every `interruptGrace / 10` only while some lane is armed (an idle helper costs no wake-up), and kills on expiry. Covers R7's epoch lost-wakeup window.
- D50. Death observation: the wait set reports an exit → `tryReap()` → the attachment's `onDeath` (captures `std::weak_ptr<HelperIncarnation>`, calls `markDead()`), so an idle crashed helper is reaped and marked at once rather than at the next call; an `attach` arriving after the death fires at once. The `terminate()` that `markDead` then posts is a no-op on a reaped process (D40).
- D51. Teardown: module and instance destructors never take the control mutex blocking. `Release` goes into a per-incarnation outbox; the destructor then `try_lock`s the control mutex and `PduStream::trySend`s (new non-blocking send; a partial frame's tail is kept and sent first by the next send); `request()` drains the outbox before and after its exchange, under its own watch. More than `MaxPendingReleases` (4096) queued → `markDead()`. The last incarnation reference drops its `ProcessHostHandle`, which posts `Drop` and returns; the supervisor kills at once, or, with `teardownGrace` set (coverage and sanitizer test compositions: 2 s), waits that long for the EOF-induced exit so `.gcda` files and sanitizer reports get written, then kills; reaping is asynchronous. `InProcessHost` keeps joining (tests only, D17).
- D52. Lockdown acceptance: `BackendConfig::lockdown` of `enum class LockdownRequirement : uint8_t { RequireLockedDown, AcceptUnsandboxed }` (zero = strict), checked in `HelperIncarnation::connect`; every test composition, in-process and real, passes `AcceptUnsandboxed`; the helper reports `Unsandboxed` until phase 5. Opening the breaker at once on `Failed` (spec §5.3) is Task 5.1's: only a sandbox can fail.
- D53. Load failures reach the breaker: `ExtensionRegistry` keeps each key's `HostBreaker` across failed loads (a map keyed by `RegistryKey`, pruned of keys without a live entry by `resetBreakers()`); `acquire` admits first (open → `HostUnavailable`, nothing loaded or spawned), and a `HostUnavailable` load records a fault — spec §5.5 "on expiry: kill, breaker fault", which D20's instance-only counting misses for `Load`.
- D54. Location: `locateExtensionHost(selfExecutable, isExecutable)` over a per-platform candidate table — Unix: `<dir of self>/<bindir→libexecdir>/contour/` (relative path computed by CMake with `file(RELATIVE_PATH)` from `CMAKE_INSTALL_FULL_BINDIR` to `CMAKE_INSTALL_FULL_LIBEXECDIR/contour`), then `<dir of self>/`; macOS bundle: `<dir of self>/../Helpers/`, then `<dir of self>/`; Windows: `<dir of self>/contour-extension-host.exe`. Install: Unix `${CMAKE_INSTALL_LIBEXECDIR}/contour`, Windows `bin` beside `contour.exe`, macOS a POST_BUILD copy into `$<TARGET_BUNDLE_CONTENT_DIR:contour>/Helpers/`. The composition roots are not changed in phase 3 (the default stays in-process WAMR); Task 5.2 calls `locateExtensionHost(vtpty::currentExecutablePath())`.
- D55. Test binaries: `vtextension_process_test` (links only `vtextension_remote_process vtextension_remote_wire core::testing Catch2`; pure units, the launcher and wait set against `extension_host_probe`, the supervisor over `FakeProcessLauncher`; cross-builds for wine); `vtextension_remote_test` gains the `HostProcess`/listener and in-process interrupt cases; new `vtextension_helper_test` (links `vtextension vtextension_remote vtextension_remote_process`, no engine) runs the contract suite and the real-guest suites through the real helper (`wasm/remote/testing/ProcessDefaultBackend.cpp`), the Review Focus process cases, teardown and hostility. Executable paths reach tests as compile definitions from `$<TARGET_FILE:…>` (`VTEXTENSION_HELPER_PATH`, `VTEXTENSION_HOSTILE_HELPER_PATH`, `VTEXTENSION_PROBE_PATH`) with `add_dependencies`.
- D56. Test executables (never installed): `extension_host_probe` (no engine; `--report` writes `key=value` lines — cwd, environment names, open descriptors or the validity of named handles, rlimits — on its control handle and exits at EOF; `--parent` starts a supervisor and a `ProcessHostConnector` to a given helper, prints the helper's pid, and sleeps). `contour-extension-host-hostile` runs the real `HostProcess` behind a frame-splitting relay driven by a `HostileMode` table — no test mode ships in `contour-extension-host`.
- D57. Clean-exit check: real-helper test compositions use `teardownGrace` so helpers exit on EOF; the supervisor counts `uncleanExits` (a non-zero exit or a signal it did not send); each real-helper case asserts 0 at its end — how helper-side ASan/LSan/UBSan/TSan reports fail a test.
- D58. Order 3.1 → 3.2 → 3.3 → 3.4 → 3.5 → 3.6 → 3.7. The real-helper contract suite turns on in 3.4 (it needs cross-process interrupt); `contour-extension-host-hostile` is created in 3.4 with its one row `ignore-interrupt` (owned Review Focus test) and gains the other rows in 3.6.
- D59. R11 follow-through: end of stream across a real process boundary is tested both ways (`"Channel.peerProcessExitIsClosed"`, `"Channel.adoptedEndSeesParentShutdown"`), under wine for Windows.
- D60. `DecodeError`/`EncodeError` are unchanged: the only wire change is `Instantiate.interruptSlot` (row cap 32 still holds three varints), covered by the existing round-trip and cap cases; argv errors have their own `ArgumentError`.
- D61. `StepWatch`, `HostAttachment`, `HostStep` and the extended `IHostHandle` live in `remote/HostConnector.hpp`; `InProcessHost` returns inert watches and calls `onDeath` when its server's `run()` returns, so phase-2 compositions keep working unchanged in behaviour.

Builds: `nice -n 19 ionice -c 3 cmake --build --preset clang-asan -j 4 --target <t>`, one at a time, under the build lock (`flock -o`); test shells unset every `CONTOUR_*` variable. FreeBSD and macOS sources are compiled by CI only; Windows by the local cross toolchain (3.7).

## Phase 3 — The executable, its supervisor and its lifecycle

### Task 3.1: `IProcessLauncher`, process handles, and the handles a helper inherits

**Files:**
- Create: `src/vtextension/wasm/remote/process/{ProcessLauncher.hpp,Process_linux.cpp,Process_freebsd.cpp,Process_macos.cpp,Process_win32.cpp,ProcessLauncher_test.cpp}`
- Create: `src/vtextension/wasm/remote/process/testing/{FakeProcessLauncher.hpp,FakeProcessLauncher.cpp,RecordingLauncher.hpp,LaunchProbe.cpp}`
- Create: `src/vtextension/wasm/remote/transport/{ControlPage.hpp,ControlPage_posix.cpp,ControlPage_win32.cpp,WakeSignal.hpp,WakeSignal_posix.cpp,WakeSignal_win32.cpp,HelperArguments.hpp,HelperArguments.cpp,ControlPage_test.cpp,WakeSignal_test.cpp,HelperArguments_test.cpp}`
- Modify: `src/vtextension/wasm/remote/transport/{Channel.hpp,Channel_posix.cpp,Channel_win32.cpp,Channel_test.cpp}` (`adopt`)
- Modify: `src/vtextension/wasm/remote/CMakeLists.txt` (new sources in `vtextension_remote_wire`; target `vtextension_remote_process` selecting `Process_<os>.cpp` by `CMAKE_SYSTEM_NAME`; `vtextension_process_test` (always with `VTEXTENSION_TESTING`, `add_test`, `-Wno-c2y-extensions` off Windows) and `extension_host_probe`, with `VTEXTENSION_PROBE_PATH="$<TARGET_FILE:extension_host_probe>"`)

**Interfaces:**
- Produces:
  ```cpp
  namespace vtextension::wasm::remote::transport {
  // Channel: takes ownership of an end inherited from the parent.
  [[nodiscard]] static std::expected<Channel, ChannelError> Channel::adopt(NativeHandle handle);

  inline constexpr std::size_t ControlPageBytes = 4096;
  inline constexpr uint32_t InterruptSlotCount = ControlPageBytes / sizeof(std::atomic<uint32_t>);   // 1024
  enum class SharedObjectError : uint8_t { Unsupported, Io, WrongSize };
  /// One page both processes map: a flag per interrupt slot. Move-only; unmapped and closed when destroyed.
  class ControlPage {
  public:
      [[nodiscard]] static std::expected<ControlPage, SharedObjectError> create();                       // parent
      [[nodiscard]] static std::expected<ControlPage, SharedObjectError> adopt(NativeHandle handle);     // helper
      void raise(uint32_t slot) noexcept;          // precondition slot < InterruptSlotCount
      void clear(uint32_t slot) noexcept;
      [[nodiscard]] bool isRaised(uint32_t slot) const noexcept;
      [[nodiscard]] NativeHandle nativeHandle() const noexcept;
  };

  enum class WakeResult : uint8_t { Pending, Sent };        // Pending: a wake is already queued
  enum class WakeWait : uint8_t { Stopped, PeerGone, Woken };
  class WakeSender {   // the parent's end; move-only
  public:
      [[nodiscard]] WakeResult signal() noexcept;           // never blocks, never raises SIGPIPE
      [[nodiscard]] NativeHandle nativeHandle() const noexcept;
  };
  class WakeReceiver { // the helper's end; move-only
  public:
      [[nodiscard]] static std::expected<WakeReceiver, ChannelError> adopt(NativeHandle handle);
      [[nodiscard]] WakeWait wait() noexcept;               // drains every queued wake before it returns Woken
      void stop() noexcept;                                 // thread-safe; a wait() now or later returns Stopped
      [[nodiscard]] NativeHandle nativeHandle() const noexcept;
  };
  struct WakePair { WakeSender parent; WakeReceiver helper; };
  [[nodiscard]] std::expected<WakePair, ChannelError> makeWakePair();

  struct HelperArguments {
      NativeHandle control {};
      std::array<NativeHandle, MaxLaneChannels> lanes {};
      NativeHandle controlPage {};
      NativeHandle wake {};
      int64_t parentProcessId = 0;
      uint64_t dataBytes = 0;                               ///< 0: no cap
      bool operator==(HelperArguments const&) const = default;
  };
  enum class ArgumentError : uint8_t { Unknown, Duplicate, Missing, Malformed };
  [[nodiscard]] std::vector<std::string> formatHelperArguments(HelperArguments const& arguments);
  [[nodiscard]] std::expected<HelperArguments, ArgumentError> parseHelperArguments(std::span<std::string_view const> arguments);
  }

  namespace vtextension::wasm::remote::process {
  using ProcessId = int64_t;
  inline constexpr uint64_t DefaultDataBytes = uint64_t { 1 } << 30;                 // D38
  inline constexpr unsigned KilledExitCode = 0xC0DE'0009;                            // Windows: what kill() passes to TerminateProcess
  enum class LaunchError : uint8_t { NotFound, Resources, Unsupported, Stopped, Io };
  enum class ExitKind : uint8_t { Exited, Signalled };
  struct ExitStatus { ExitKind kind = ExitKind::Exited; int value = 0; bool operator==(ExitStatus const&) const = default; };
  struct ResourceCaps { uint64_t dataBytes = DefaultDataBytes; };                  // Windows: the job's limit; POSIX: --data-bytes
  #ifdef _WIN32
  using Waitable = void*;   ///< The process HANDLE.
  #else
  using Waitable = int;     ///< pidfd (Linux), process descriptor (FreeBSD), pid (macOS, for EVFILT_PROC).
  #endif
  struct LaunchSpec {
      std::filesystem::path executable;
      std::vector<transport::NativeHandle> inherited;                               ///< the child sees them in this order
      std::function<std::vector<std::string>(std::span<transport::NativeHandle const> childHandles)> arguments;
      std::vector<std::pair<std::string, std::string>> environment;                 ///< the child's whole environment
      ResourceCaps caps {};
  };
  class IProcessHandle {   // non-copyable, non-movable; used by one thread (the supervisor's)
  public:
      virtual ~IProcessHandle() = default;                                          ///< neither kills nor waits
      [[nodiscard]] virtual ProcessId id() const noexcept = 0;
      virtual void kill() noexcept = 0;                                             ///< idempotent; nothing once reaped
      [[nodiscard]] virtual std::optional<ExitStatus> tryReap() noexcept = 0;       ///< never blocks
      [[nodiscard]] virtual Waitable waitable() const noexcept = 0;
  };
  class IExitWaitSet {
  public:
      virtual ~IExitWaitSet() = default;
      [[nodiscard]] virtual std::expected<void, LaunchError> watch(uint64_t key, IProcessHandle const& process) = 0;
      virtual void unwatch(uint64_t key) noexcept = 0;
      /// Blocks until a watched process exits, wake() is called, or @p deadline passes (by the clock it was made with).
      /// @return The keys whose process may have exited; empty on a wake or the deadline.
      [[nodiscard]] virtual std::vector<uint64_t> wait(std::optional<core::SteadyTimePoint> deadline) = 0;
      virtual void wake() noexcept = 0;                                             ///< wait-free, any thread
  };
  class IProcessLauncher {
  public:
      virtual ~IProcessLauncher() = default;
      [[nodiscard]] virtual std::expected<std::unique_ptr<IProcessHandle>, LaunchError> launch(LaunchSpec const& spec) = 0;
      [[nodiscard]] virtual std::expected<std::unique_ptr<IExitWaitSet>, LaunchError> makeWaitSet(core::IClock& clock) = 0;
  };
  [[nodiscard]] std::shared_ptr<IProcessLauncher> makeSystemProcessLauncher();
  [[nodiscard]] std::vector<std::pair<std::string, std::string>> defaultHelperEnvironment();   // {} / {SystemRoot}
  }
  namespace vtextension::wasm::remote::process::testing {
  class FakeProcessLauncher;   // launch() records specs and returns FakeProcessHandles; exit(i, status) makes one exit and
                               // wakes the set; kills(), launches(), launchThreads(), lastDeadline(); its wait set blocks
                               // until woken, ignoring real time (D45)
  class RecordingLauncher;     // decorates any launcher: launches(), processIds(), killThreads(); an optional
                               // argument rewrite for launches after the first (used by 3.6)
  }
  ```

- [ ] **Step 1: Write the transport tests.**
  - `HelperArguments_test.cpp`: `"HelperArguments.roundTrip"` (`parseHelperArguments(formatHelperArguments(a)) == a` for POSIX-style values 3–9 and for large Windows-style values); `"HelperArguments.rejects"` as a table — `--bogus=1` → `Unknown`, `--wake` twice → `Duplicate`, no `--wake` → `Missing`, `--lanes=4,5,6` → `Malformed`, `--parent=x` → `Malformed`, `--data-bytes=-1` → `Malformed`.
  - `Channel_test.cpp`: `"Channel.adoptTakesAnInheritedEnd"` (adopt a duplicate of a pair's helper end; bytes flow both ways); `"Channel.adoptRefusesAClosedHandle"` (→ `Io`).
  - `ControlPage_test.cpp`: `"ControlPage.raiseIsSeenThroughASecondMapping"` (adopt a duplicate of `create()`'s handle; `raise(5)` on one → `isRaised(5)` on the other, `isRaised(6)` false; `clear(5)` → false); `"ControlPage.adoptRefusesAWrongSize"` (POSIX: a memfd/shm of 8192 bytes → `WrongSize`); `"ControlPage.isSealed"` (Linux, FreeBSD: `ftruncate(handle, 8192)` fails `EPERM`); `"ControlPage.isNotInherited"` (POSIX `FD_CLOEXEC`; Windows `HANDLE_FLAG_INHERIT` clear).
  - `WakeSignal_test.cpp`: `"WakeSignal.signalWakesTheReceiver"` (a jthread in `wait()` returns `Woken` after one `signal()`); `"WakeSignal.signalNeverBlocks"` (nobody waits: 100 000 `signal()`s inside `std::async`, `wait_for(5s) == ready`; the first returns `Sent`, at least one returns `Pending`; afterwards one `wait()` returns `Woken` and drains all); `"WakeSignal.stopEndsAWait"`; `"WakeSignal.senderGoneIsPeerGone"` (POSIX: drop the sender → `PeerGone`); `"WakeSignal.signalToAGoneReceiverKeepsSigpipeAway"` (POSIX: SIGPIPE at `SIG_DFL`, drop the receiver, `signal()` returns, the process lives).
- [ ] **Step 2: Write the launch probe and the launcher tests.** `LaunchProbe.cpp` (`extension_host_probe --report <helper arguments> [--check-handle=H]…`): parses `HelperArguments`, adopts the control channel, writes `cwd=…`, one `env=NAME` per variable, one `fd=N` per open descriptor (Linux `/proc/self/fd`, else `/dev/fd`) or `handle=H:valid|invalid` per `--check-handle`, then blocks on control until EOF and exits 0. `ProcessLauncher_test.cpp` (each case launches the probe with fresh channels, page and wake, `ResourceCaps {}`):
  - `"ProcessLauncher.childGetsACleanEnvironment"`: the test sets `VTEXTENSION_LEAK=1` in its own environment; `environment = {{"A","1"}}` → the report's `env` lines are exactly `{A}`.
  - `"ProcessLauncher.childRunsAtTheRoot"`: `cwd=/` (Windows: the probe's volume root).
  - `"ProcessLauncher.childInheritsExactlyItsHandles"`: POSIX — the test holds an extra descriptor without `FD_CLOEXEC`; the report's `fd` lines are exactly `{0,1,2,3,4,5,6,7,8,9}`. Windows — an extra inheritable event not in `inherited` is `invalid` in the child, every listed handle `valid`.
  - `"ProcessLauncher.childHandlesHaveFixedNumbers"` (POSIX): the `arguments` callback receives `{3,4,5,6,7,8,9}`.
  - `"ProcessLauncher.exitIsSeenWithoutPolling"`: register with a system wait set, close the parent's control end → `wait(nullopt)` returns `{key}`; `tryReap() == ExitStatus{ Exited, 0 }`.
  - `"ProcessLauncher.killThenReap"`: `kill()` → the set reports the key within `wait(now + 5s)`; POSIX `tryReap() == ExitStatus{ Signalled, SIGKILL }`, Windows `{ Exited, KilledExitCode }` (a named constant the launcher passes to `TerminateProcess`).
  - `"ProcessLauncher.killAfterReapDoesNothing"`: let the probe exit, reap it, then `kill()` twice → returns, and `tryReap()` keeps returning the first status (the handle's reaped flag short-circuits; on macOS that is what keeps a reused pid safe, D40).
  - `"ProcessLauncher.missingExecutableIsNotFound"`: `executable = "/nonexistent/x"` → `LaunchError::NotFound`.
  - `"ProcessLauncher.childIsInAKillOnCloseJob"` (Windows): `IsProcessInJob(child, nullptr) == TRUE`; destroying the handle (and with it the job) without `kill()` ends the child — a separately opened `SYNCHRONIZE` handle signals within 5 s.
  - `"ExitWaitSet.wakeEndsAWait"`: a jthread in `wait(nullopt)` returns an empty list after `wake()`.
  - `"ExitWaitSet.deadlineEndsAWait"`: `wait(now + 20ms)` returns empty (one of the few real-timer cases, D45).
  - `"Channel.peerProcessExitIsClosed"`: the probe exits → the parent's lane end `receiveSome` → `Closed` (D59).
- [ ] **Step 3: Build; expect compile failure.** `nice -n 19 ionice -c 3 cmake --build --preset clang-asan -j 4 --target vtextension_process_test extension_host_probe`.
- [ ] **Step 4: Implement the transport additions.** `Channel::adopt`: POSIX `fstat` + `S_ISSOCK`, set `FD_CLOEXEC`, macOS `SO_NOSIGPIPE`; Windows `GetFileType == FILE_TYPE_PIPE`, clear `HANDLE_FLAG_INHERIT`. `ControlPage` and `WakeSignal` per D46/D47; `WakeReceiver` POSIX polls its socket and an internal `pipe2(O_CLOEXEC)` for `stop()`, reading until `EAGAIN` before it returns `Woken` and treating EOF as `PeerGone`; Windows `WaitForMultipleObjects(wake, stopEvent)`. `HelperArguments`: one row per option (`name`, parse into the field, format from it) in a `constexpr std::array`; handles print as decimal (`std::bit_cast<uintptr_t>` on Windows).
- [ ] **Step 5: Implement the launchers** (D36, D39, D40), one `Process_<os>.cpp` each defining `makeSystemProcessLauncher()` and `defaultHelperEnvironment()`. POSIX children: stdin/stdout from `/dev/null`, stderr inherited; `inherited[i]` → descriptor `3 + i`. Linux: `posix_spawn` with the actions of D36, `pidfd_open(pid, 0)` right after (`ENOSYS` → kill + reap the child, `Unsupported`), `kill` = `pidfd_send_signal(SIGKILL)`, `tryReap` = `waitid(P_PIDFD, …, WEXITED|WNOHANG)`, wait set = `poll` over an eventfd + pidfds. FreeBSD: `pdfork` child per D36 (`_exit(127)` on any failure), `pdkill`, `waitpid(pdgetpid, WNOHANG)`, wait set = `poll` (`POLLHUP` on a procdesc). macOS: `posix_spawn` per D36, `kill(pid, SIGKILL)` guarded by the reaped flag, `waitpid(WNOHANG)`, kqueue wait set. Windows: per D36; the environment block built with `CREATE_UNICODE_ENVIRONMENT`; `TerminateProcess(KilledExitCode)`; `WaitForSingleObject(0)` + `GetExitCodeProcess`. A missing executable is checked before spawning (`NotFound`); `ENOMEM`/`EAGAIN`/`ERROR_NOT_ENOUGH_MEMORY` → `Resources`.
- [ ] **Step 6: Implement the test launchers.** `FakeProcessLauncher` and `RecordingLauncher` as in the interface; every record guarded by one mutex.
- [ ] **Step 7: Run.** `out/clang-asan/src/vtextension/wasm/remote/vtextension_process_test` → PASS (Linux rows; the other OS rows compile here only where guarded, and run in CI and in 3.7 under wine).
- [ ] **Step 8: Commit.** Transport first: `git commit -s -m "feat(vtextension): inheritable channels, an interrupt page and a wake signal for the extension host"`; then the rest: `git commit -s -m "feat(vtextension): launch and reap extension host processes through process handles"`

### Task 3.2: `contour-extension-host` executable

**Files:**
- Create: `src/contour-extension-host/{CMakeLists.txt,main.cpp}`
- Create: `src/vtextension/wasm/host/{HostProcess.hpp,HostProcess.cpp,HostBootstrap.hpp,HostBootstrap_posix.cpp,HostBootstrap_win32.cpp,HelperEngine.hpp,HostProcess_test.cpp}`
- Create: `src/vtextension/wasm/wasmtime/HelperEngine.cpp` (target `vtextension_wasmtime_helper_engine`, D32/D33)
- Create: `src/vtextension/wasm/remote/process/{HelperLocation.hpp,HelperLocation.cpp,HelperLocation_test.cpp}` (D54)
- Create: `src/vtextension/wasm/remote/Helper_test.cpp` and target `vtextension_helper_test` in `wasm/remote/CMakeLists.txt` (only `if(TARGET contour-extension-host)`; links `vtextension vtextension_remote vtextension_remote_process vtextension_remote_wire glaze::glaze core::testing Catch2::Catch2 contour_test_trace`, no engine; `VTEXTENSION_HELPER_PATH`, `add_dependencies`)
- Modify: `wasm/host/{HostServer.hpp,HostServer.cpp,IHostedEngine.hpp (EngineFactory moves here),InProcessHost.hpp,HostServer_test.cpp,CMakeLists.txt}`, `wasm/remote/process/testing/LaunchProbe.cpp` (reports caps), `src/CMakeLists.txt` (`add_subdirectory(contour-extension-host)` after `vtextension`)

**Interfaces:**
- Consumes: 3.1 (`HelperArguments`, `Channel::adopt`, `ControlPage::adopt`, `WakeReceiver::adopt`, `makeSystemProcessLauncher`).
- Produces:
  ```cpp
  namespace vtextension::wasm::host {
  using EngineFactory = std::function<std::unique_ptr<IHostedEngine>()>;          // moved from InProcessHost.hpp
  [[nodiscard]] std::unique_ptr<IHostedEngine> makeHelperEngine();               // HelperEngine.hpp; defined by the engine dir
  // HostServerConfig gains:
  //   std::function<remote::proto::LockdownState()> confine;   ///< After every lane's thread started, before Hello; its
  //                                                            ///< result is Hello's lockdown. Null: `lockdown` is sent.
  enum class HostProcessExit : uint8_t { Served, BadArguments, HandlesUnusable, ParentGone, EngineUnavailable,
                                         ProtocolViolation, ExecutorUnavailable };
  struct HostProcessExitRow { HostProcessExit exit; int code; std::string_view text; };   // text: user-readable, names no engine
  [[nodiscard]] std::span<HostProcessExitRow const> hostProcessExits() noexcept;           // codes 0, 70..75
  [[nodiscard]] int exitCodeOf(HostProcessExit exit) noexcept;
  [[nodiscard]] HostProcessExit hostProcessExitOf(ServerExit exit) noexcept;  // PeerClosed, Stopped → Served
  struct HostEndpoints { remote::transport::Channel control; std::vector<remote::transport::Channel> lanes;
                         remote::transport::ControlPage controlPage; remote::transport::WakeReceiver wake; };
  [[nodiscard]] std::expected<HostEndpoints, HostProcessExit> adoptEndpoints(remote::transport::HelperArguments const& arguments);
  /// A host's whole serving side over its endpoints: the server (3.4 adds the interrupt listener).
  class HostProcess {
  public:
      HostProcess(IHostedEngine& engine, HostEndpoints endpoints, HostServerConfig config);
      [[nodiscard]] ServerExit run();      // once
      void stop() noexcept;                // thread-safe, idempotent
  };
  /// The executable's body: parse, adopt, bind to the parent, make the engine, serve.
  [[nodiscard]] int runExtensionHost(std::span<std::string_view const> arguments, EngineFactory const& engines);
  enum class BootstrapError : uint8_t { ParentGone, Refused };
  [[nodiscard]] std::expected<void, BootstrapError> bindToParent(int64_t parentProcessId) noexcept;     // D37
  enum class CapPolicy : uint8_t { Skip, Apply };
  inline constexpr CapPolicy BuildCapPolicy = /* Skip under CONTOUR_SANITIZE (compile definition), else Apply */;
  [[nodiscard]] remote::proto::LockdownState capResources(uint64_t dataBytes, CapPolicy policy) noexcept;
      // Linux: RLIMIT_NPROC 0, RLIMIT_DATA dataBytes (0: none); FreeBSD: RLIMIT_NPROC 0; macOS, Windows: nothing.
      // Unsandboxed on success, Failed if a setrlimit failed.
  }
  namespace vtextension::wasm::remote::process {
  enum class HelperLocationError : uint8_t { NotFound };
  inline constexpr std::string_view ExtensionHostName = /* "contour-extension-host", ".exe" on Windows */;
  [[nodiscard]] std::expected<std::filesystem::path, HelperLocationError> locateExtensionHost(
      std::filesystem::path const& selfExecutable, std::function<bool(std::filesystem::path const&)> const& isExecutable);
  }
  ```
  `main.cpp`: `return host::runExtensionHost(arguments-as-string_views, &host::makeHelperEngine);` — nothing else.
  Exit table: Served 0, BadArguments 70, HandlesUnusable 71, ParentGone 72, EngineUnavailable 73, ProtocolViolation 74, ExecutorUnavailable 75.

- [ ] **Step 1: Write the server and process tests.**
  - `HostServer_test.cpp`: `"HostServer.confineRunsAfterTheLanesStartAndBeforeHello"` — a counting `startExecutor` and a `confine` that records the count and returns `LockdownState::Failed`: count at confine == `MaxLaneChannels`, the first control PDU is `Hello{ …, lockdown = Failed }`; `"HostServer.noConfineSendsTheConfiguredLockdown"` (unchanged behaviour).
  - `HostProcess_test.cpp` (vtextension_remote_test): `"HostProcess.servesOverAdoptedEndpoints"` — channel pairs, a page and a wake pair; the helper ends' duplicates formatted into `HelperArguments` (`parentProcessId` = own pid); `runExtensionHost(args, testing::makeHostedEngine)` on a jthread → the parent reads `Hello{ ProtocolVersion, MemoryMode::Copy, LockdownState::Unsandboxed }`; closing the control end → the jthread's result is `0`. `"HostProcess.badArgumentsExit"` (`{"--bogus=1"}` → 70, the engine factory never called); `"HostProcess.unusableHandleExits"` (`--control=` a closed descriptor → 71); `"HostProcess.wrongParentExits"` (`parentProcessId` = own pid + 1 on POSIX → 72, before any engine is made); `"HostProcess.exitCodesAreDistinct"` (every row's code unique, none in `{1, 23, 66}`, `Served` is 0).
  - `HelperLocation_test.cpp` (vtextension_process_test), a table over a fake `isExecutable`: Unix `/usr/bin/contour` → `/usr/bin/<rel>/contour-extension-host` (rel from the compile definition `CONTOUR_EXTENSION_HOST_RELATIVE_DIR`); only the same-directory candidate existing → that one; macOS `…/Contour.app/Contents/MacOS/contour` → `…/Contents/Helpers/contour-extension-host`; Windows `C:\Contour\bin\contour.exe` → `…\bin\contour-extension-host.exe`; none → `NotFound`.
  - `Helper_test.cpp` (vtextension_helper_test, the real executable through `makeSystemProcessLauncher()`): `"Helper.saysHelloOverInheritedChannels"` (Hello as above; close control → `ExitStatus{ Exited, 0 }` within 5 s); `"Helper.exitsOnBadArguments"` (→ `{ Exited, 70 }`).
  - `ProcessLauncher_test.cpp`: `"HostBootstrap.capsHold"` (the probe now applies `capResources(64 MiB, BuildCapPolicy)` and reports `nproc=`/`data=`; Linux with `Apply`: `nproc=0`, `data=67108864`; under `Skip`: `SKIP("sanitizer builds apply no caps")`); `"HostBootstrap.wrongParentExits"` (probe with `--parent=1` → `{ Exited, 72 }` on Linux/FreeBSD/macOS).
- [ ] **Step 2: Build; expect compile failure.** `nice -n 19 ionice -c 3 cmake --build --preset clang-asan -j 4 --target vtextension_remote_test vtextension_process_test`.
- [ ] **Step 3: Implement the server hook.** `HostServer::run()` order becomes: start the executors (failure → `ExecutorUnavailable`, nothing sent), `confine()` if set, send `Hello`, serve control.
- [ ] **Step 4: Implement `HostProcess` and `runExtensionHost`.** `runExtensionHost`: parse (`BadArguments`), `bindToParent` (`ParentGone`), `adoptEndpoints` (`HandlesUnusable`: any adopt failure, `lanes.size() != MaxLaneChannels`), engine (`EngineUnavailable` on null), `HostServerConfig{ .confine = [&]{ return capResources(args.dataBytes, BuildCapPolicy); } }`, `HostProcess::run()` → `hostProcessExitOf`. Each non-`Served` exit writes one line `contour-extension-host: <text>` to stderr. Under `CapPolicy::Skip` it writes once `contour-extension-host: resource caps are off in this build`. `bindToParent` and `capResources` per D37/D38 in `HostBootstrap_posix.cpp` (`#if` per OS for `prctl`/`procctl`/`RLIMIT_DATA`) and `HostBootstrap_win32.cpp` (both no-ops returning success / `Unsandboxed`).
- [ ] **Step 5: The executable and its engine.** `vtextension_wasmtime_helper_engine` (`HelperEngine.cpp`: one process-wide `wasmtime::makeKindState()`, `makeWasmtimeEngine(kind, { .trapMode = HelperTrapMode })`, `HelperTrapMode` from the D33 compile definition). `src/contour-extension-host/CMakeLists.txt`: `if(TARGET vtextension_wasmtime)`: `add_executable(contour-extension-host main.cpp)` linking `vtextension_host vtextension_wasmtime_helper_engine contour::wasmtime`; on Windows `WIN32_EXECUTABLE OFF` (console subsystem, no window: the launcher passes `CREATE_NO_WINDOW`). Otherwise a status line: `contour-extension-host is not built: it needs Wasmtime until its WAMR engine lands`.
- [ ] **Step 6: Run.** `nice -n 19 ionice -c 3 cmake --build --preset clang-asan -j 4 --target contour-extension-host vtextension_helper_test vtextension_remote_test vtextension_process_test extension_host_probe`; then `out/clang-asan/src/vtextension/wasm/remote/vtextension_remote_test "HostServer.*" "HostProcess.*"`, `…/vtextension_process_test "HelperLocation.*" "HostBootstrap.*"`, `…/vtextension_helper_test "Helper.*"` → PASS; `ctest --preset=clang-asan -R check_wasm_runtime_containment` → PASS.
- [ ] **Step 7: Commit.** `git commit -s -m "feat(vtextension): the contour-extension-host executable"`
- [ ] **Step 8: Install rules (separate commit).** In `src/contour-extension-host/CMakeLists.txt` (D54): `include(GNUInstallDirs)`; non-Windows non-bundle `install(TARGETS contour-extension-host RUNTIME DESTINATION "${CMAKE_INSTALL_LIBEXECDIR}/contour")`; Windows `install(TARGETS contour-extension-host RUNTIME DESTINATION bin)`; `CONTOUR_MACOS_APP_BUNDLE`: a POST_BUILD copy into `$<TARGET_BUNDLE_CONTENT_DIR:contour>/Helpers/` (the bundle install carries it). `file(RELATIVE_PATH …)` → `CONTOUR_EXTENSION_HOST_RELATIVE_DIR` on `vtextension_remote_process`. Verify: `DESTDIR=<scratchpad>/install cmake --install out/clang-asan` → `<scratchpad>/install/<prefix>/libexec/contour/contour-extension-host` exists. Commit: `git commit -s -m "build: install contour-extension-host beside contour"`

### Task 3.3: `ExtensionHostSupervisor`, the process connector, and the non-call timeouts

**Files:**
- Create: `src/vtextension/wasm/remote/process/{ExtensionHostSupervisor.hpp,ExtensionHostSupervisor.cpp,HostTimers.hpp,HostTimers.cpp,ProcessHostConnector.hpp,ProcessHostConnector.cpp,ExtensionHostSupervisor_test.cpp,HostTimers_test.cpp}`
- Create: `src/vtextension/wasm/testing/ProcessModules.hpp` (`StartSpinModule`: `(func $s (loop (br 0))) (start $s)`, hand-encoded like the contract modules)
- Modify: `src/vtextension/wasm/remote/HostConnector.hpp` (D61), `HelperIncarnation.{hpp,cpp}`, `RemoteBackend.cpp`, `src/vtextension/wasm/BackendConfig.hpp`, `wasm/host/InProcessHost.{hpp,cpp}`, `wasm/remote/testing/RemoteDefaultBackend.cpp`, `RemoteBackend_test.cpp`, `RegistryRemote_test.cpp` (`AcceptUnsandboxed`), `src/vtextension/ExtensionRegistry.{hpp,cpp}`, `ExtensionRegistry_test.cpp`, `SessionExtensions_test.cpp`, `testing/FakeWasmBackend.hpp`, `wasm/remote/Helper_test.cpp`

**Interfaces:**
- Consumes: 3.1, 3.2.
- Produces:
  ```cpp
  namespace vtextension::wasm {
  enum class LockdownRequirement : uint8_t { RequireLockedDown, AcceptUnsandboxed };
  // BackendConfig gains: LockdownRequirement lockdown = LockdownRequirement::RequireLockedDown;  (D52)
  }
  namespace vtextension::wasm::remote {
  enum class HostStep : uint8_t { Handshake, Load, Instantiate };
  /// Disarms its timeout when destroyed. Default-constructed: inert.
  class StepWatch {
  public:
      StepWatch() noexcept = default;
      explicit StepWatch(std::function<void()> disarm) noexcept;
      StepWatch(StepWatch&&) noexcept; StepWatch& operator=(StepWatch&&) noexcept; ~StepWatch();
  };
  struct HostAttachment { std::function<void()> onDeath; };   // 3.4 adds lanes and interruptGrace
  // IHostHandle gains:
  //   [[nodiscard]] virtual StepWatch watch(HostStep step, std::chrono::milliseconds budget) = 0;
  //   virtual void attach(HostAttachment attachment) = 0;   // once, right after the incarnation is made
  struct HelperPolicy { HostTimeouts timeouts {}; LockdownRequirement lockdown = LockdownRequirement::RequireLockedDown; };
  // HelperIncarnation::connect(IHostConnector&, proto::CreateRuntime const&, HelperPolicy const&)
  // HelperIncarnation::request(proto::Pdu const& pdu, HostStep step)   // arms watch(step, policy budget) around it
  }
  namespace vtextension::wasm::remote::process {
  struct TimerKey { uint64_t helper = 0; uint32_t serial = 0; auto operator<=>(TimerKey const&) const = default; };
  /// Deadlines of one thread (the supervisor's); pure, told the time by its caller.
  class HostTimers {
  public:
      void arm(TimerKey key, core::SteadyTimePoint now, core::SteadyDuration budget);
      void pause(TimerKey key, core::SteadyTimePoint now) noexcept;
      void resume(TimerKey key, core::SteadyTimePoint now) noexcept;
      void disarm(TimerKey key) noexcept;
      [[nodiscard]] std::vector<TimerKey> expire(core::SteadyTimePoint now);        // removes what it returns
      [[nodiscard]] std::optional<core::SteadyTimePoint> nextDeadline() const noexcept; // paused timers excluded
      [[nodiscard]] std::size_t armed() const noexcept;
  };
  inline constexpr auto ShutdownReapBudget = std::chrono::milliseconds { 100 };
  struct SupervisorConfig {
      std::shared_ptr<IProcessLauncher> launcher;
      core::IClock* clock = nullptr;                          ///< non-null; outlives the supervisor
      std::chrono::milliseconds teardownGrace { 0 };          ///< D51
  };
  enum class SupervisorError : uint8_t { WaitSetUnavailable, ThreadUnavailable };
  enum class KillReason : uint8_t { Requested, StepTimeout, Grace, Dropped, Shutdown };
  struct SupervisorCounters {
      std::size_t launches = 0; std::size_t reaped = 0; std::size_t deathsObserved = 0; std::size_t uncleanExits = 0;
      std::array<std::size_t, 5> kills {};                    ///< indexed by KillReason
  };
  class SupervisorMailbox;     // shared, thread-safe: the command queue and the wait set's wake (D41)
  /// The parent's grip on one supervised process. Move-only; destroying it posts Drop and returns.
  class SupervisedHelper {
  public:
      [[nodiscard]] ProcessId processId() const noexcept;
      void kill() noexcept;                                                          // posts
      [[nodiscard]] remote::StepWatch watch(remote::HostStep step, std::chrono::milliseconds budget);
      void attach(remote::HostAttachment attachment);                               // posts
      void nudge() noexcept;                                                         // wait-free (used from 3.4)
      ~SupervisedHelper();
  };
  class SupervisorMailbox {
  public:
      /// Launches on the supervisor's thread; blocks the caller until it did. `Stopped` once the supervisor is gone.
      [[nodiscard]] std::expected<SupervisedHelper, LaunchError> launch(LaunchSpec spec);
  };
  class ExtensionHostSupervisor {
  public:
      [[nodiscard]] static std::expected<std::unique_ptr<ExtensionHostSupervisor>, SupervisorError> start(SupervisorConfig config);
      ~ExtensionHostSupervisor();                             // kill all, reap within the budget, join (D41)
      [[nodiscard]] std::shared_ptr<SupervisorMailbox> mailbox() const noexcept;
      void sync();                                            // test hook: everything posted before is processed
      [[nodiscard]] SupervisorCounters counters() const;      // test hook
      [[nodiscard]] std::thread::id threadId() const noexcept;// test hook
      [[nodiscard]] std::size_t liveProcesses() const;        // test hook: launched, not yet reaped
  };
  struct ProcessHostConfig {
      std::filesystem::path helper;
      std::vector<std::pair<std::string, std::string>> environment = defaultHelperEnvironment();
      std::vector<std::string> extraArguments;                ///< appended after the handle arguments (3.4: hostile helper)
      ResourceCaps caps {};
  };
  class ProcessHostConnector final: public remote::IHostConnector {
  public:
      ProcessHostConnector(std::shared_ptr<SupervisorMailbox> supervisor, ProcessHostConfig config);
      [[nodiscard]] std::expected<remote::HostConnection, ExtensionError> connect() override;   // HostUnavailable on any failure
  };
  }
  namespace vtextension::testing { // FakeWasmBackend gains
  //   [[nodiscard]] std::vector<std::thread::id> loadThreads() const;  instantiateThreads() const;
  }
  ```

- [ ] **Step 1: Write the timer tests** (`HostTimers_test.cpp`, vtextension_process_test; explicit time points, no clock): `"HostTimers.expiresAtTheBudget"` (arm at t0 for 10 ms: `expire(t0+9ms)` empty, `expire(t0+10ms) == {key}`, then `armed() == 0`); `"HostTimers.pauseStopsTheClock"` (50 ms, pause at +10, `nextDeadline() == nullopt` while paused, resume at +1010; `expire(+1049)` empty, `expire(+1050) == {key}`); `"HostTimers.disarmIsFinal"`; `"HostTimers.nextDeadlineIsTheEarliest"`.
- [ ] **Step 2: Write the supervisor tests** (`ExtensionHostSupervisor_test.cpp`, vtextension_process_test; `FakeProcessLauncher`, `core::ManualClock`, `sync()` after every `advance`, D45):
  - `"Supervisor.launchRunsOnItsThread"`: `fake.launchThreads() == { supervisor->threadId() }`, `counters().launches == 1`.
  - `"Supervisor.stepTimeoutsKill"`: `DYNAMIC_SECTION` per row `{ Handshake, HostTimeouts {}.handshake (2000 ms) }`, `{ Load, 10000 ms }`, `{ Instantiate, 1000 ms }`: watch, advance budget − 1 ms → `fake.kills() == 0`; advance 1 ms → `fake.kills() == 1`, the attached `onDeath` ran once, `kills[StepTimeout] == 1`.
  - `"Supervisor.disarmedWatchNeverFires"`: destroy the watch, advance 1 h → no kill, `fake.lastDeadline() == std::nullopt`.
  - `"Supervisor.exitIsObservedAndReaped"`: `fake.exit(0, { Exited, 3 })` → `deathsObserved == 1`, `reaped == 1`, `uncleanExits == 1`, `onDeath` once, `liveProcesses() == 0`.
  - `"Supervisor.attachAfterDeathFiresAtOnce"`; `"Supervisor.requestedKillIsNotUnclean"` (`kill()`, then the fake exits `{ Signalled, 9 }` → `uncleanExits == 0`, `kills[Requested] == 1`); `"Supervisor.dropKillsAndReaps"` (destroy the `SupervisedHelper` → `kills[Dropped] == 1`, after the fake exit `reaped == 1`).
  - `"Supervisor.idleNeverWakes"`: one live helper, no watch → after `sync()`, `fake.lastDeadline() == std::nullopt`.
  - `"Supervisor.stopIsBounded"`: three helpers, one whose fake never reports an exit; destroy the supervisor inside `std::async` → `wait_for(1s) == ready`, `fake.kills() == 3`.
  - `"Supervisor.mailboxOutlivesTheSupervisor"`: after destruction, `kill()`, `watch()`, dropping a `SupervisedHelper` → no effect, no crash; `mailbox->launch(...)` → `LaunchError::Stopped`.
- [ ] **Step 3: Write the composition tests.**
  - `ExtensionRegistry_test.cpp` (D53): `"load timeout counts a breaker fault"` — `FakeWasmBackend` whose `load` fails `HostUnavailable`: three `acquire`s → each `HostUnavailable`, then the fourth → `HostUnavailable` with `fake.loads() == 3` (refused before loading); `"reload resets a load breaker"` (after `resetBreakers()`, the next `acquire` loads again: `loads() == 4`); `"InvalidComponent counts no fault"`.
  - `SessionExtensions_test.cpp`: `"status-line load never runs on the owner thread"` — a `QueuedOwner` on the test thread, `rebuild` with two declared extensions, drain → `fake.loadThreads()` and `fake.instantiateThreads()` non-empty and none equals `std::this_thread::get_id()`.
  - `Helper_test.cpp` (real helper; a binary-wide fixture: `ExtensionHostSupervisor` over `RecordingLauncher(makeSystemProcessLauncher())` and `core::defaultSteadyClock()`, `teardownGrace = 2s`, `ProcessHostConnector` with `VTEXTENSION_HELPER_PATH`, the sanitizer variables appended (D39), `makeRemoteBackend` with `LockdownRequirement::AcceptUnsandboxed`; every case ends with `CHECK(supervisor.counters().uncleanExits == 0)`, D57): `"Process.loadsAndCallsThroughARealHelper"` (InstanceModule, `add(2, 3) == 5` on a prepared thread); `"Process.unsandboxedIsRefusedUnlessAccepted"` (`RequireLockedDown` → `load(...).error() == HostUnavailable`, `kills[Requested] == 1`); `"Process.startFunctionThatNeverReturnsIsKilledAtTheInstantiateTimeout"` (R14: `timeouts.instantiate = 200ms`, `StartSpinModule` → `instantiate(...).error() == HostUnavailable` after ≥ 200 ms and < 5 s, `kills[StepTimeout] == 1`; then InstanceModule in the same backend instantiates: `recording.launches() == 2`).
- [ ] **Step 4: Build; expect compile failures.** `nice -n 19 ionice -c 3 cmake --build --preset clang-asan -j 4 --target vtextension_process_test vtextension_test vtextension_remote_test vtextension_helper_test`.
- [ ] **Step 5: Implement `HostTimers`** (a `std::map<TimerKey, Entry>` with remaining budget and running-since; `expire` and `nextDeadline` scan — the table is per helper and small).
- [ ] **Step 6: Implement the supervisor.** The thread loop (the algorithm the signatures leave open):
  1. `clock.refresh()`; take the command queue (mutex, swap): `Launch` (call the launcher, `watch` the handle in the wait set, fulfil the promise), `Watch`/`Disarm` (`HostTimers` with `clock.now()`), `Attach` (store; fire `onDeath` now if already dead), `Kill`/`Drop` (kill unless reaped; `Drop` also forgets the attachment; with `teardownGrace`, `Drop` first arms a teardown timer and kills only when it expires or skips the kill if the process exits first), `Sync` (fulfil after the batch), `Stop`.
  2. For each key the wait set returned: `tryReap()`; on a status: unwatch, count `reaped`, `uncleanExits` unless the status is a kill the supervisor sent or `{ Exited, 0 }`, `deathsObserved`, call `onDeath` once.
  3. `expire(clock.now())`: kill each owner with `KillReason::StepTimeout` (teardown timers: `Dropped`), call `onDeath`.
  4. `wait(nextDeadline())`.
  `Stop`: kill all (`Shutdown`; with `teardownGrace`, wait that long for EOF exits first), wait up to `ShutdownReapBudget` reaping, return. `onDeath` runs on the supervisor thread without the queue mutex held.
- [ ] **Step 7: Implement the connector and handle.** `ProcessHostConnector::connect`: `1 + MaxLaneChannels` channel pairs, `ControlPage::create()`, `makeWakePair()`; `LaunchSpec{ helper, inherited = { control, lanes…, page, wake } helper ends, arguments = formatHelperArguments({…childHandles, parentProcessId = own pid, dataBytes = caps.dataBytes}) + extraArguments, environment, caps }`; `mailbox->launch(spec)`; the helper ends close when the spec goes out of scope; return `HostConnection{ parent ends, std::make_unique<ProcessHostHandle>(std::move(helper), std::move(page), std::move(wakeSender)) }`. `ProcessHostHandle` (internal, in the .cpp): `terminate()` → `helper.kill()`; `watch`/`attach` forward; `interrupt` is a documented no-op until 3.4 (`RemoteInstance`'s sticky flag still fails every later call `Interrupted`; stopping a running guest across the boundary is 3.4's).
- [ ] **Step 8: Wire policy and watches into the incarnation.** `HelperIncarnation::connect` attaches `HostAttachment{ .onDeath = [weak]{ if (auto self = weak.lock()) self->markDead(); } }`, then under `watch(Handshake, timeouts.handshake)` reads Hello (acceptance now also checks `policy.lockdown`) and sends `CreateRuntime`; `request(pdu, step)` arms `watch(step, budget-of(step))` from a `constexpr` step → `HostTimeouts` member table. `RemoteBackend` builds `HelperPolicy{ config.timeouts, config.lockdown }`. `InProcessHost`: inert `StepWatch {}`, `attach` stores `onDeath` and calls it when `run()` returns. Registry per D53.
- [ ] **Step 9: Run.** `out/clang-asan/src/vtextension/wasm/remote/vtextension_process_test "HostTimers.*" "Supervisor.*"`, `out/clang-asan/src/vtextension/vtextension_test "load timeout counts a breaker fault" "reload resets a load breaker" "InvalidComponent counts no fault" "status-line load never runs on the owner thread" "ExtensionRegistry.*"`, `out/clang-asan/src/vtextension/wasm/remote/vtextension_helper_test "Process.*" "Helper.*"`, and the whole `vtextension_remote_test` → PASS.
- [ ] **Step 10: Commit.** `git commit -s -m "feat(vtextension): supervise extension host processes, with timeouts on start, load and instantiate"`

### Task 3.4: Interruption across the process boundary and the backstop

**Files:**
- Create: `src/vtextension/wasm/remote/{LaneWatch.hpp,LaneWatch.cpp,LaneWatch_test.cpp}` (compiled into `vtextension_remote_process`, namespace `vtextension::wasm::remote`; test in vtextension_process_test)
- Create: `src/vtextension/wasm/host/{InterruptListener.hpp,InterruptListener.cpp}`
- Create: `src/vtextension/wasm/remote/testing/ProcessDefaultBackend.cpp`, `src/vtextension/wasm/remote/{ProcessBackend_test.cpp,ReviewFocusProcess_test.cpp}` (vtextension_helper_test)
- Create: `src/contour-extension-host/testing/{HostileHost.cpp,HostileModes.hpp}` (target `contour-extension-host-hostile`, not installed; `VTEXTENSION_HOSTILE_HELPER_PATH`)
- Modify: `wasm/remote/proto/{Pdu.hpp,Codec.cpp,Codec_test.cpp}` (`Instantiate.interruptSlot`, `ProtocolVersion = 2`), `wasm/host/{HostServer.hpp,HostServer.cpp,HostProcess.cpp,InProcessHost.{hpp,cpp},HostServer_test.cpp,HostProcess_test.cpp}`, `wasm/remote/{HostConnector.hpp,HelperIncarnation.{hpp,cpp},RemoteBackend.cpp,RemoteKindState.{hpp,cpp},RemoteBackend_test.cpp}`, `wasm/remote/process/{ProcessHostConnector.cpp,ExtensionHostSupervisor.cpp,ExtensionHostSupervisor_test.cpp}`, `wasm/remote/CMakeLists.txt` (helper_test gains the contract and real-guest suites like remote_test in 2.5, with the guest dependencies and `VTEXTENSION_TEST_MODULE_DIR`)

**Interfaces:**
- Consumes: 3.1 (`ControlPage`, `WakeSender`/`WakeReceiver`), 3.2 (`HostProcess`), 3.3 (supervisor, `HostAttachment`, `SupervisedHelper::nudge`).
- Produces:
  ```cpp
  namespace vtextension::wasm::remote::proto {
  inline constexpr uint32_t ProtocolVersion = 2;
  struct Instantiate { uint32_t moduleId = 0; uint32_t instanceId = 0; uint32_t interruptSlot = 0; };
  }
  namespace vtextension::wasm::remote {
  enum class LaneArming : uint8_t { Disarmed, Armed };
  enum class LanePlace : uint8_t { Idle, InHelper, ServingHost };
  struct LaneObservation { uint64_t epoch = 0; LaneArming arming = LaneArming::Disarmed; LanePlace place = LanePlace::Idle; };
  /// One lane's frames as its thread runs them, for the backstop. Atomics only (D49).
  class LaneWatch {
  public:
      void enter(uint32_t instanceId) noexcept;           // lane thread: a call or a fetch of the instance begins
      void leave() noexcept;                               // lane thread: the innermost frame ends
      void beginHostCall() noexcept;                       // lane thread: the innermost frame parks in CallHost
      void endHostCall() noexcept;
      [[nodiscard]] bool interrupt(uint32_t instanceId) noexcept;   // any thread, wait-free: arms iff a frame of it is on the lane
      [[nodiscard]] LaneObservation observe() const noexcept;      // the supervisor
  };
  class LaneWatchSet {   // MaxLaneChannels lanes
  public:
      [[nodiscard]] LaneWatch& lane(std::size_t index) noexcept;
      [[nodiscard]] bool interrupt(uint32_t instanceId) noexcept;  // any lane
      [[nodiscard]] std::span<LaneWatch const> lanes() const noexcept;
  };
  struct HostAttachment {
      std::function<void()> onDeath;
      std::shared_ptr<LaneWatchSet const> lanes;                   ///< null: no backstop (in-process hosts)
      std::chrono::milliseconds interruptGrace { 50 };
  };
  // IHostHandle: interrupt(uint32_t instanceId) becomes
  //   virtual void interrupt(uint32_t instanceId, uint32_t interruptSlot) noexcept = 0;  // raise, wake, nudge; no lock, no I/O wait
  //   virtual void clearInterrupt(uint32_t interruptSlot) noexcept = 0;                   // before the slot is reused
  /// The incarnation's interrupt slots: one per live instance.
  class InterruptSlots {
  public:
      [[nodiscard]] std::optional<uint32_t> acquire();             // nullopt: all InterruptSlotCount in use
      void release(uint32_t slot) noexcept;
  };
  // RemoteKindState gains: [[nodiscard]] std::size_t lanesInUse() const noexcept;   // test hook
  }
  namespace vtextension::wasm::host {
  // HostServer gains: void interruptSlot(uint32_t slot) noexcept;   // the live instance instantiated with it; else ignored
  class InterruptListener {
  public:
      [[nodiscard]] static std::expected<InterruptListener, std::error_code> start(
          remote::transport::ControlPage const& page, remote::transport::WakeReceiver& wake,
          std::function<void(uint32_t slot)> interruptSlot);
      ~InterruptListener();   // stop() the receiver, join
  };
  }
  ```
  `HostileModes.hpp` (D56): `enum class HostileMode : uint8_t { IgnoreInterrupt /* 3.6 adds the rest */ };` and one `HostileModeRow { HostileMode mode; std::string_view name; }` per mode; `--hostile=<name>` is the only argument the hostile executable adds.

- [ ] **Step 1: Write the lane-watch tests** (`LaneWatch_test.cpp`): `"LaneWatch.interruptArmsOnlyAFrameOnTheLane"` (`enter(7)`; `interrupt(8) == false`, Disarmed; `interrupt(7) == true` → `{ Armed, InHelper }`); `"LaneWatch.leavingTheInterruptedFrameDisarms"` (`enter(7) enter(9) interrupt(9)` → Armed; `leave()` → Disarmed and the epoch moved; `interrupt(7)` arms again); `"LaneWatch.hostCallParksTheInnermostFrame"` (`enter(7) beginHostCall` → ServingHost; nested `enter(9)` → InHelper; `leave` → ServingHost; `endHostCall` → InHelper); `"LaneWatch.deepNestingIsBalanced"` (20 enters, 20 leaves → Idle; frames past `MaxCallDepth + 1` are counted, not recorded); `"LaneWatchSet.interruptRacesFrames"` (`[threading]`: one thread enter/leave 100 000 times, another interrupts; TSan-clean, ends Disarmed).
- [ ] **Step 2: Write the grace tests** (`ExtensionHostSupervisor_test.cpp`, FakeProcessLauncher + ManualClock + `sync()`): `"Supervisor.graceKillsAfterHelperSideTime"` (grace 50 ms: `lane(0).enter(7)`, `set.interrupt(7)`, `nudge()`; advance 49 ms → `kills[Grace] == 0`; advance 1 ms → `kills[Grace] == 1`, `onDeath` once); `"Supervisor.graceIsPausedWhileServingCallHost"` (interrupt; +10 ms; `beginHostCall`; +5000 ms → no kill; `endHostCall`; +39 ms → no kill; +1 ms → kill); `"Supervisor.graceEndsWithTheCall"` (interrupt, +20 ms, `leave()`, +1000 ms → no kill, `fake.lastDeadline() == nullopt`); `"Supervisor.noGraceWithoutAFrame"`; `"Supervisor.armedLaneTicksAtATenthOfTheGrace"` (while armed, `fake.lastDeadline() == now + 5ms`).
- [ ] **Step 3: Write the server, process and backend tests.**
  - `HostServer_test.cpp`: `"HostServer.interruptSlotInterruptsItsInstance"` (Instantiate with slot 5, `Call spin` on lane 0, `interruptSlot(5)` → `CallResult{ Interrupted }`; `interruptSlot(6)` is ignored); `"HostServer.slotOutOfRangeIsAViolation"` (`interruptSlot = InterruptSlotCount` → `ProtocolViolation`); `"HostServer.slotInUseIsAViolation"`.
  - `HostProcess_test.cpp`: `"HostProcess.raisedFlagAndWakeInterrupt"` (in-thread `runExtensionHost`; `Call spin`; parent `page.raise(slot)` + `wake.signal()` → `CallResult{ Interrupted }` within 1 s); `"HostProcess.wakeWithoutAFlagInterruptsNothing"` (signal only → `add` still returns its sum).
  - `Codec_test.cpp`: the `Instantiate` sample carries `interruptSlot = 1023`.
  - `RemoteBackend_test.cpp` (in-process, now through page and wake): `"RemoteBackend.interruptIsWaitFreeOfTheHelper"` unchanged and passing; `"RemoteBackend.slotsAreReused"` (2000 sequential instantiate/drop → all succeed); `"RemoteBackend.slotExhaustionIsRuntimeUnavailable"` (1024 live instances, the 1025th → `RuntimeUnavailable`; a call on the first still works; `connects() == 1`).
- [ ] **Step 4: Write the real-helper suites** (vtextension_helper_test).
  - `ProcessDefaultBackend.cpp`: `makeDefaultBackend(BackendConfig)` composes `makeRemoteBackend` over one binary-wide `RemoteKindState` and, when `config.connector` is null, a binary-wide supervisor + `ProcessHostConnector` as in 3.3's fixture, with `lockdown = AcceptUnsandboxed`; `testing::preparedGeneration()` → `remote::preparedGeneration()`. `ProcessBackend_test.cpp`: `CONTOUR_WASM_BACKEND_CONTRACT(ProcessTraits)`. The real-guest suites join the binary as in 2.5; a case that cannot pass through a real process is tagged `[in-process]`, excluded with `~[in-process]` here, its reason recorded in "Phase-3 facts".
  - `ReviewFocusProcess_test.cpp` (each asserts `uncleanExits == 0` at the end, D57):
    - `"helper death during CallHost frees the lane"`: InstanceModule; `ping` blocks on a latch; on a prepared thread T, `call("ping-then-add", {2, 3})` runs in `std::async`; once inside `ping`, the test kills the helper from outside (`::kill(pid, SIGKILL)` / `TerminateProcess`, pid from `recording.processIds()[0]`), waits until `counters().deathsObserved == 1` (bounded 5 s, `sync()` loop), releases `ping` → the future is ready within 5 s with `error() == CallFailure::HostCrashed`; `counters().kills[Grace] == 0`; after T's guard is gone `kind->lanesInUse() == 0`; then on T, `module->instantiate` → `recording.launches() == 2` and `add(2, 3) == 5`.
    - `"interrupt never blocks on a full pipe"` (POSIX; `timeouts.interruptGrace = 10s`): a lane thread calls `spin`; the test `SIGSTOP`s the helper; 100 000 `instance->interrupt()` inside `std::async` → `wait_for(5s) == ready`; `SIGCONT` → the call returns `CallFailure::Interrupted` within 5 s; `kills[Grace] == 0`. (Windows is covered by `"WakeSignal.signalNeverBlocks"`.)
    - `"helper ignoring interrupt is killed after grace"` (hostile `ignore-interrupt`, grace 50 ms): a lane thread calls `spin`; at t0 `interrupt()` → the call returns `HostCrashed`, `t0 + 50ms ≤ return < t0 + 5s` (steady clock), `kills[Grace] == 1`, `uncleanExits == 0`; the next `instantiate` respawns (`recording.launches() == 2`).
    - `"ten panes respawn one helper"` (a registry and engine over this composition, the statusline test module of `RegistryRemote_test`): 10 `Instance`s of one entry; kill the helper from outside; each `render({10, 1}) == HostUnavailable`; `breaker.faultCount() == 1`; 10 threads released by one latch each `Instance::create` → all succeed; `recording.launches() == 2`; `supervisor.liveProcesses() == 1`; every new instance renders.
- [ ] **Step 5: Build; expect compile failures.** `nice -n 19 ionice -c 3 cmake --build --preset clang-asan -j 4 --target vtextension_process_test vtextension_remote_test vtextension_test contour-extension-host contour-extension-host-hostile vtextension_helper_test`.
- [ ] **Step 6: Implement the helper side.** `Instantiate` carries the slot; `HostServer` keeps slot → instance (released with the instance); `HostProcess::run()` starts the `InterruptListener` (on failure `ExecutorUnavailable`) before the server and stops it after; the listener loop: `wait()` → `Woken`: for each slot in `std::views::iota(0u, InterruptSlotCount)` raised on the page, `interruptSlot(slot)`; `Stopped`/`PeerGone`: return. `InProcessHost` becomes `HostProcess` over in-memory endpoints (it creates the page and the wake pair itself; its `interrupt(id, slot)` raises and signals, D48).
- [ ] **Step 7: Implement the parent side.** `HelperIncarnation` owns `InterruptSlots` and a `std::shared_ptr<LaneWatchSet>`, attached with `policy.timeouts.interruptGrace`; `RemoteModule::instantiate` acquires a slot (none → `RuntimeUnavailable`, no `markDead`); `~RemoteInstance` calls `clearInterrupt(slot)` then releases it. `RemoteInstance::call`/`fetch` bracket their lane exchange with `enter`/`leave`, and `serveHostCall` with `beginHostCall`/`endHostCall`. `interrupt()`: sticky flag, `lanes->interrupt(id)`, `handle->interrupt(id, slot)` (ProcessHostHandle: `page.raise`, `wake.signal`, `helper.nudge()`). Supervisor grace (D49): on every loop turn, for each attached lane set, `observe()` each lane: newly Armed → `arm(Grace)`; epoch moved or Disarmed → `disarm`; `ServingHost` ↔ `InHelper` → `pause`/`resume`; while any is armed, the wait deadline is at most `now + interruptGrace / 10`; expiry → kill `KillReason::Grace` + `onDeath`.
- [ ] **Step 8: The hostile helper, first row.** `HostileHost.cpp`: parses and strips `--hostile=<name>`; for `ignore-interrupt` it hands `runExtensionHost` a wake handle of its own that nobody signals, so the page's flags are never read; otherwise identical to `main`.
- [ ] **Step 9: Run.** `out/clang-asan/src/vtextension/wasm/remote/vtextension_process_test "LaneWatch*" "Supervisor.*"`; `…/vtextension_remote_test` (whole); `…/vtextension_helper_test "~[in-process]"` (contract: 40 cases; the four Review Focus cases; the real-guest suites) → PASS.
- [ ] **Step 10: Commit.** `git commit -s -m "feat(vtextension): interrupt an extension host without waiting on it, and kill one that ignores it"`

### Task 3.5: Teardown off the owner thread

**Files:**
- Modify: `wasm/remote/transport/{Channel.hpp,Channel_posix.cpp,Channel_win32.cpp,PduStream.hpp,PduStream.cpp,Channel_test.cpp,PduStream_test.cpp}` (non-blocking send)
- Modify: `wasm/remote/{HelperIncarnation.hpp,HelperIncarnation.cpp,RemoteBackend.cpp,RemoteBackend_test.cpp}` (release outbox, D51)
- Modify: `wasm/remote/process/{ExtensionHostSupervisor.cpp,ExtensionHostSupervisor_test.cpp,testing/LaunchProbe.cpp}` (teardown grace; `--parent` mode)
- Modify: `src/contour-extension-host/testing/{HostileModes.hpp,HostileHost.cpp}` (rows `stop-reading-after-instantiate`, `ignore-eof`)
- Create: `src/vtextension/wasm/remote/Teardown_test.cpp` (vtextension_helper_test; `VTEXTENSION_PROBE_PATH` also defined there)

**Interfaces:**
- Consumes: 3.3, 3.4.
- Produces:
  ```cpp
  namespace vtextension::wasm::remote::transport {
  // Channel gains: sends what fits without blocking. @return bytes sent; 0 if none fit.
  [[nodiscard]] std::expected<std::size_t, ChannelError> Channel::trySend(std::span<uint8_t const> bytes);
  enum class SendOutcome : uint8_t { WouldBlock, Sent };
  // PduStream gains (a partial frame's tail is kept; every later send, blocking or not, sends it first):
  [[nodiscard]] std::expected<SendOutcome, StreamError> PduStream::trySend(proto::Pdu const& pdu);
  }
  namespace vtextension::wasm::remote {
  inline constexpr std::size_t MaxPendingReleases = 4096;
  // HelperIncarnation: post(Pdu) is replaced by
  void release(proto::Release release) noexcept;   // queues; sends now only if the control mutex is free and the
                                                   // channel takes it without blocking; never blocks
  }
  ```

- [ ] **Step 1: Write the transport tests.** `"Channel.trySendNeverBlocks"` (no reader: `trySend` of 4 KiB chunks until it returns 0, inside `std::async`, `wait_for(5s) == ready`); `"PduStream.trySendKeepsAPartialFrame"` (fill until `WouldBlock`; a reader drains; the next `send()` sends the kept tail first; the reader decodes every PDU sent, in order, nothing `Malformed`).
- [ ] **Step 2: Write the outbox tests** (`RemoteBackend_test.cpp`, scripted helper as in 2.3): `"RemoteBackend.releaseWaitsForTheControlChannel"` (a thread's `Instantiate` request is outstanding; the test thread drops an older instance → the drop returns at once (`std::async`, `wait_for(100ms) == ready`); the scripted helper reads `Release{Instance, id}` after it answers the request and before the next request); `"RemoteBackend.releaseOverflowKillsTheHelper"` (the scripted helper stops reading control; `MaxPendingReleases + 1` instance drops → the handle saw `terminate()` once; every drop returned at once).
- [ ] **Step 3: Write the supervisor tests** (FakeProcessLauncher + ManualClock): `"Supervisor.teardownGraceLetsAHelperExit"` (`teardownGrace = 2s`; drop; the fake exits `{ Exited, 0 }` at +1 s → `kills[Dropped] == 0`, `uncleanExits == 0`); `"Supervisor.teardownGraceThenKills"` (no exit by +2 s → `kills[Dropped] == 1`); `"Supervisor.zeroGraceKillsAtOnce"`.
- [ ] **Step 4: Write the Review Focus cases** (`Teardown_test.cpp`, real executables; `RecordingLauncher` records the thread of every `kill()`):
  - `"teardown never blocks the owner thread"`:
    - Section "closing": hostile `stop-reading-after-instantiate`, `teardownGrace = 0`, `timeouts.load = 30s`. Instantiate three instances from InstanceModule on the test thread (the owner). A worker thread W starts `runtime->load(ModuleIdentity{"same"}, <a 16 MiB module>)`, which blocks in its send under the control mutex (the helper no longer reads). The owner then destroys the three instances, the module, the runtime and the backend inside `std::async` → `wait_for(1s) == ready`. `recording.killThreads()` all equal `supervisor->threadId()`. W's load returns `HostUnavailable` within 5 s (the drop killed the helper). After a `sync()` loop: `kills[Dropped] == 1`, `reaped == 1`.
    - Section "quitting": a helper that `ignore-eof`s (and, on POSIX, a second one `SIGSTOP`ped); destroy the supervisor inside `std::async` → `wait_for(1s) == ready`; both processes are gone afterwards (POSIX: `kill(pid, 0) == -1 && errno == ESRCH` once reaped by the destructor or, if it ran out of budget, by this test as their parent; Windows: their handles signalled).
  - `"no helper outlives its parent"`: Linux `prctl(PR_SET_CHILD_SUBREAPER, 1)` / FreeBSD `procctl(PROC_REAP_ACQUIRE)` once for the binary. Launch `extension_host_probe --parent <helper path> [--hostile=ignore-eof]` through the system launcher; read the helper's pid from it; kill the probe (`SIGKILL` / `TerminateProcess`). `DYNAMIC_SECTION` "benign" and "ignore-eof": the helper is gone within 2 s — Linux/FreeBSD `waitpid(helper, WNOHANG)` as its subreaper; Windows a `SYNCHRONIZE` handle opened before the kill signals. macOS: "benign" polls `kill(pid, 0)` until `ESRCH`; "ignore-eof" is `SKIP("macOS has no parent-death signal; documented")`.
  - `"teardown in coverage builds lets the helper exit"` (`teardownGrace = 2s`): drop the backend → the helper exits `{ Exited, 0 }` (recorded), `kills[Dropped] == 0`.
- [ ] **Step 5: Build; expect failures.** `nice -n 19 ionice -c 3 cmake --build --preset clang-asan -j 4 --target vtextension_test vtextension_process_test vtextension_remote_test extension_host_probe contour-extension-host-hostile vtextension_helper_test`.
- [ ] **Step 6: Implement.** `Channel::trySend`: POSIX `send(MSG_DONTWAIT|MSG_NOSIGNAL)`, `EAGAIN` → 0; Windows: an overlapped `WriteFile`, `GetOverlappedResult` without waiting, still pending → `CancelIoEx` and report what was transferred. `PduStream`: `_unsentTail` drained first by `send` (blocking) and `trySend` (`WouldBlock` while it is not drained). `HelperIncarnation::release` (D51): push under `_outboxMutex` (never held across I/O); then `std::unique_lock{ _controlMutex, std::try_to_lock }` and `trySend` the queue front-to-back until `WouldBlock`; over `MaxPendingReleases` queued → `markDead()`. `request()` sends the queue (blocking) before and after its exchange. `~RemoteModule`/`~RemoteInstance` call `release()`. Supervisor teardown grace and `Stop` per 3.3's algorithm with `teardownGrace`. The probe's `--parent`: start a supervisor + `ProcessHostConnector` (helper path and extra arguments from its argv), `connect()`, print the pid (`SupervisedHelper::processId()`), sleep until killed. Hostile rows: `stop-reading-after-instantiate` (the relay stops reading every parent channel after it forwarded the first `Instantiated`), `ignore-eof` (on the parent's EOF it ignores SIGTERM and sleeps forever).
- [ ] **Step 7: Run.** `out/clang-asan/src/vtextension/wasm/remote/vtextension_process_test "Supervisor.*"`, `…/vtextension_test "Channel.*" "PduStream.*"`, `…/vtextension_remote_test "RemoteBackend.*"`, `…/vtextension_helper_test "teardown never blocks the owner thread" "no helper outlives its parent" "teardown in coverage builds lets the helper exit"` → PASS.
- [ ] **Step 8: Commit.** `git commit -s -m "feat(vtextension): let go of an extension host without waiting for it"`

### Task 3.6: Crash and hostility suite

**Files:**
- Modify: `src/contour-extension-host/testing/{HostileModes.hpp,HostileHost.cpp}` (the remaining rows)
- Modify: `src/vtextension/wasm/remote/process/testing/RecordingLauncher.hpp` (argument rewrite for launches after the first, if 3.1 left it out)
- Create: `src/vtextension/wasm/remote/Hostility_test.cpp` (vtextension_helper_test)

**Interfaces:**
- Consumes: 3.3–3.5.
- Produces: `HostileMode` rows (name → action), each acting once per incarnation, through a relay that splits frames with `proto::decodeFrame` and forwards the raw bytes between the parent's channels and an inner `HostProcess`'s:

  | Mode | Acts when | Action |
  |---|---|---|
  | `abort` | the first lane `Call` arrives | `std::abort()` |
  | `segfault` | the first lane `Call` arrives | POSIX `std::raise(SIGSEGV)` with `SIG_DFL`; Windows `TerminateProcess(self, STATUS_ACCESS_VIOLATION)` |
  | `exit-in-callhost` | the inner server sends its first `CallHost` | forward it, then `_exit(3)` |
  | `stop-reading` | the first lane `Call` arrives | stop reading every parent channel; stay alive |
  | `garbage` | the first lane `Call` arrives | write 64 bytes from `std::mt19937{ 0x5eed }` to that lane instead |
  | `over-cap` | the first `MemoryRead` | answer a `MemoryData` header declaring `maxMemoryBytes + 17` body bytes |
  | `bogus-size` | the first `MemoryRead` | answer a well-formed `MemoryData{ Ok }` with `size - 1` bytes |
  | `stall-after-header` | the first `MemoryRead` | send a valid `MemoryData` header and half its body, then stall |
  | `hang-at-handshake` | start | hold back `Hello` |
  | `hang-at-load` | the first `Load` | hold back `Loaded` |
  | `hang-at-instantiate` | the first `Instantiate` | hold back `Instantiated` |
  | `ignore-interrupt` | (3.4) | |
  | `stop-reading-after-instantiate`, `ignore-eof` | (3.5) | |

- [ ] **Step 1: Write the suite** (`Hostility_test.cpp`). One expectations table, one `DYNAMIC_SECTION` per row; each section builds a fresh supervisor (`teardownGrace = 2s`), a `RecordingLauncher` whose launches after the first drop `--hostile` (so the respawn is benign), `timeouts = { load 200ms, instantiate 200ms, handshake 200ms, interruptGrace 50ms }`, `AcceptUnsandboxed`; a "watchdog" `std::jthread` interrupts the instance 50 ms after a call or fetch starts, as the pool's watchdog would.

  | Mode | Operation (InstanceModule) | Expected | Bound |
  |---|---|---|---|
  | `abort`, `segfault`, `garbage`, `stop-reading` | `call("add", {2, 3})` | `CallFailure::HostCrashed` | 2 s |
  | `exit-in-callhost` | `call("ping-then-add", {2, 3})` | `HostCrashed` | 2 s |
  | `over-cap`, `bogus-size`, `stall-after-header` | `memory(16, 5)` on a prepared thread | `MemoryFailure::HostCrashed` | 2 s |
  | `hang-at-handshake`, `hang-at-load` | `runtime->load(...)` | `ExtensionError::HostUnavailable` | 2 s |
  | `hang-at-instantiate` | `module->instantiate(ctx)` | `HostUnavailable` | 2 s |

  Each section asserts: the expected failure; elapsed (steady clock) < bound; exactly one death before recovery — `counters().deathsObserved == 1` (a supervisor kill is reaped and observed like any exit) and `recording.launches() == 1`; `uncleanExits` equals 1 for `abort`/`segfault`/`exit-in-callhost` (the helper died on its own) and 0 for the rest (the supervisor's kill); lazy recovery — the same module (for the load rows: a new load) then instantiates and `add(2, 3) == 5`, with `recording.launches() == 2`.
- [ ] **Step 2: Write the breaker cases** (Instance level, through `ExtensionRegistry` and `Engine` over this composition, the statusline test module): `"Hostility.oneCrashIsOneFault"` (`abort`: `render(...) == HostUnavailable`, `breaker.faultCount() == 1`, the next `Instance::create` renders); `"Hostility.oneHangIsOneFault"` (`hang-at-instantiate`: `Instance::create(...).error() == HostUnavailable`, `faultCount() == 1`); `"Hostility.oneViolationIsOneFault"` (`garbage`); `"Hostility.repeatedCrashesOpenTheBreaker"` (`abort` on every launch, no rewrite: three create+render rounds → `faultCount() == 3`, `state() == Open`; a fourth `Instance::create` → `HostUnavailable` with `recording.launches() == 3`); `"Hostility.hangingLoadOpensTheBreaker"` (D53: three `acquire`s under `hang-at-load` → a fourth fails without launching).
- [ ] **Step 3: Build; expect failures.** `nice -n 19 ionice -c 3 cmake --build --preset clang-asan -j 4 --target contour-extension-host-hostile vtextension_helper_test`.
- [ ] **Step 4: Implement the rows** (the relay: one thread per parent channel and direction; a row is a predicate on the decoded kind plus an action; holding back = buffering forever).
- [ ] **Step 5: Fix what the suite finds.** A row that fails because the parent accepts what it should refuse (`bogus-size` in particular: `RemoteInstance::fetch` must reject `bytes.size() != size`) is fixed in the parent, each with its own commit, not by weakening the row.
- [ ] **Step 6: Run.** `out/clang-asan/src/vtextension/wasm/remote/vtextension_helper_test "Hostility*"` → PASS; then three more runs (flakiness: every bound held each time).
- [ ] **Step 7: Commit.** `git commit -s -m "test(vtextension): crash and hostility suite for the extension host process"`

### Task 3.7: Phase 3 gate

- [ ] **Step 1: ASan/LSan.** `nice -n 19 ionice -c 3 cmake --build --preset clang-asan -j 4 --target vtextension_test vtextension_wasmtime_test vtextension_remote_test vtextension_process_test vtextension_helper_test contour-extension-host contour-extension-host-hostile extension_host_probe`; `ctest --preset=clang-asan -R 'vtextension|wasm_runtime_containment'` → all pass: five suites, `[wasm-contract]` 40 cases on each of the four compositions (WAMR in-process, Wasmtime direct, remote over an in-process host, remote over the real helper), `[bundled]` on both remote compositions, no LSan report, `uncleanExits == 0` in every real-helper case (D57).
- [ ] **Step 2: TSan.** `cmake --preset clang-tsan && nice -n 19 ionice -c 3 cmake --build --preset clang-tsan -j 4 --target vtextension_test vtextension_remote_test vtextension_process_test vtextension_helper_test contour-extension-host contour-extension-host-hostile extension_host_probe`; run `out/clang-tsan/src/vtextension/vtextension_test "[threading]"`, and `vtextension_remote_test`, `vtextension_process_test`, `vtextension_helper_test "~[.measure]"` from `out/clang-tsan/src/vtextension/wasm/remote/` → zero reports, no suppressions; helpers are TSan-built too, so their reports surface as `uncleanExits` (exit 66).
- [ ] **Step 3: Measurement.** Hidden `Process.measureRender` (`[.measure]`) in vtextension_helper_test through `wasm/testing/RenderMeasurement.hpp`, the 1.7/2.7 workloads (plasma steady `render(120, 1)`, uncached 120/119 alternating), plus `Process.measureRoundTrip` (10 000 `add` calls) and `Process.measureSpawn` (connect → `RuntimeCreated`, 20 incarnations, median). `out/clang-release`, these targets only (`nice -n 19 ionice -c 3 cmake --build --preset clang-release -j 4 --target vtextension_helper_test contour-extension-host`), three runs each. Report the medians against Phase 2's in-process host (steady 18.5 / 18.9 / 19.1 µs, ≈ 18.8 µs; uncached 29.0 / 29.4 / 28.9 µs; round trip 3.2–3.4 µs) and in-process Wasmtime (2.8 / 12.4 µs), naming the helper's trap mode (`Signals` in release, D33; Phase 2 ran `Software`). Also measure the helper's `VmData` (Linux `/proc/<pid>/status`) after 16 plasma instances rendered once; `DefaultDataBytes` stands if it is ≥ 4× that, else raise it and record why (D38). Record everything in "Phase-3 facts". No threshold here (gate 9.2 is Task 6.5's).
- [ ] **Step 4: Windows cross-build + wine** (R12). Configure `linux-to-windows-clangcl-debug` into `out/clangcl-debug` with `-DCMAKE_MSVC_DEBUG_INFORMATION_FORMAT=Embedded -DCONTOUR_EXTENSIONS=ON -DCONTOUR_EXTENSIONS_BUNDLED=OFF -DCONTOUR_FRONTEND_GUI=OFF`; build `vtextension_process_test extension_host_probe` (std-only by D30/D55); run under wine in its own prefix, five runs: launcher (clean environment, handle list, kill-on-close job, `KilledExitCode`), wait set, `ControlPage`, `WakeSignal` (incl. `signalNeverBlocks`), `HelperArguments`, `Channel.peerProcessExitIsClosed`, the supervisor cases. Then try `contour-extension-host` (Wasmtime's MSVC static library) and `vtextension_helper_test`; if either cannot cross-build or run, record why in "Phase-3 facts" (as 2.7 did for WAMR) — evidence, not a blocker. Fix any Windows break found, each its own commit.
- [ ] **Step 5: FreeBSD and macOS.** Their launchers, wait sets and bootstraps compile only in CI: push the branch (owner rule: push when locally green) and record the FreeBSD and macOS jobs' results; failures there are fixed before the phase closes.
- [ ] **Step 6: Coverage.** `clang-coverage` preset, the five test targets and the three executables only; real-helper compositions use `teardownGrace` so helpers write `.gcda` (D51). Line coverage of `wasm/remote/process/*.cpp`, `wasm/remote/LaneWatch.cpp`, `wasm/remote/transport/{ControlPage_posix,WakeSignal_posix,HelperArguments}.cpp`, `wasm/host/{HostProcess,InterruptListener,HostBootstrap_posix}.cpp`, `HelperIncarnation.cpp`, `RemoteBackend.cpp`, `src/contour-extension-host/main.cpp`; name the uncovered lines' kind.
- [ ] **Step 7: Phase review (R8).** A code-reviewer agent on the worktree diff `3abef4e10..HEAD` (Phase 2's end); every finding addressed, scoped re-review until clean.
- [ ] **Step 8: Phase summary.** Performance (Step 3, against 18.8 µs in-process host and 2.8 µs in-process Wasmtime), risk (D36 launch mechanics per OS, D37 caps applied by the helper itself, D44 backstop in place of non-blocking lane I/O, D49 grace accounting by observation, D51 release outbox, the macOS parent-death gap), coverage (Step 6), and what Windows/FreeBSD/macOS verification did and did not prove.

### Phase-3 facts

*(Filled in by Tasks 3.4 and 3.7.)*

Task 3.4 / 3.5 — the real-helper composition (`vtextension_helper_test`): the `[in-process]` set is empty; the
ctest entry runs the whole binary unfiltered (the dead `~[in-process]` filter was removed in 3.5).

Task 3.7 — the gate (head 6e99bc621; its commits: c76fc82a1 hang rows timed from their own watch, d8bedfc2a
measurement cases, 6e99bc621 hostile host crashes without a core dump):
- ASan/LSan (`ctest --preset=clang-asan -R 'vtextension|wasm_runtime_containment'`): 7/7. vtextension_test
  397 cases, vtextension_wasmtime_test 54, vtextension_remote_test 215, vtextension_process_test 77 (1
  skipped: caps under sanitizers), vtextension_helper_test 152 (1 skipped, same reason). `[wasm-contract]`
  is 41 cases (one more than the plan's 40) on each of the four compositions; `[bundled]` 14 on both remote
  compositions; no LSan or ASan report.
- D57 is live, proven by sabotage (reverted, never committed): a heap-use-after-free at the helper's exit, and
  separately 100 leaked allocations, each made `Process.loadsAndCallsThroughARealHelper` fail at
  `uncleanExits == 0` (1 == 0), the helper's ASan/LSan report on stderr. A single leaked `new` in `main` was
  not reported: LSan's scan still found the pointer (conservative), not a gate defect.
- TSan (out/clang-tsan, no suppressions, TSAN_OPTIONS unset): `vtextension_test "[threading]"` 22 cases,
  `vtextension_remote_test` 215, `vtextension_process_test` 77 (1 skipped), `vtextension_helper_test
  "~[.measure]"` 152 (1 skipped); zero reports, TSan-built helpers included.
- Plain builds and core dumps: in release and coverage builds the `abort`/`segfault` hostility rows failed
  on a machine whose `core_pattern` pipes to systemd-coredump: the crashed helper stays alive while its core
  is written, past the 50 ms interrupt grace, so the supervisor kills it first and counts a Grace kill, not
  an unclean exit. Sanitizer runtimes disable core dumps, which is why ASan/TSan never saw it. Fixed in the
  hostile host (RLIMIT_CORE 0 before it crashes). The product helper still dumps core when it crashes: a
  real crash in a plain build is reported as a Grace kill (or observed only after the dump), and its core
  holds guest memory. Whether the helper should set RLIMIT_CORE 0 / PR_SET_DUMPABLE 0 itself belongs with
  the lockdown (Phase 5).
- Measurement (out/clang-release, helper in `TrapMode::Signals` (D33); phase 2's in-process host ran
  `Software`). Plasma, 1000 timed renders, three gate runs (medians):
  - Through the real helper: steady 18.6 / 18.1 / 18.4 µs (≈ 18.4, p90 ~20–21 µs); uncached 39.4 / 40.1 /
    31.5 µs, and four more runs 32.6 / 25.4 / 26.9 / 25.3 µs: noisy (shared machine, load ~2.3); median of the
    seven 31.5 µs. Load 43 ms (Signals mode compiles faster than Software's 58 ms), start 0.3 ms.
  - Same runs, in-process host (phase 2's composition): steady 17.9 / 18.9 / 18.7 µs, uncached 28.2 / 28.1 /
    28.9 µs. In-process Wasmtime: steady 2.8 / 2.8 / 3.2 µs, uncached 12.8 / 12.9 / 12.8 µs.
  - So a real process boundary costs a steady render the same as phase 2's in-process host (~18.5 µs, ~6.5x
    in-process Wasmtime); uncached sits 0–12 µs above it with high run-to-run spread (cross-process
    scheduling of the copy-path reads; not counted).
  - Round trip (`add`, 10 000 calls): 4.4 / 3.3 / 2.6 µs (median 3.3, p90 3.2–6.1, fastest 1.6–3.0 µs);
    in-process host alongside 2.7 / 2.6 / 3.2 µs.
  - Spawn to `RuntimeCreated` (launch, Hello, CreateRuntime; 20 incarnations): median 1.29 / 1.23 / 1.18 ms,
    p90 ≤ 1.7 ms. (ASan build: 3.1 ms.)
  - Helper footprint (/proc/<pid>/status, caps applied, 1 GiB RLIMIT_DATA): idle VmData 41.4 MiB, VmRSS
    10.7–10.9 MiB (the 4 × 10 MiB executor stacks are most of VmData); plasma loaded VmData 45.8 MiB, VmRSS
    21.6 MiB; 16 plasma instances rendered once VmData 63.1 MiB, VmRSS 22.6 MiB (VmHWM the same). Identical
    across runs.
  - D38: 1 GiB / 63.1 MiB = 16.2x ≥ 4x: `DefaultDataBytes` stays 1 GiB. (Per-instance growth is ~1.1 MiB of
    VmData; the cap binds at roughly 900 such instances in one helper.)
- Windows (R12; `linux-to-windows-clangcl-debug` into out/clangcl-debug, `-DCMAKE_MSVC_DEBUG_INFORMATION_FORMAT=
  Embedded -DCONTOUR_EXTENSIONS=ON -DCONTOUR_EXTENSIONS_BUNDLED=OFF -DCONTOUR_FRONTEND_GUI=OFF`, -Werror):
  `vtextension_process_test`, `extension_host_probe`, `contour-extension-host` (links Wasmtime's MSVC
  static library with clang-cl/lld-link: yes), `contour-extension-host-hostile` and `vtextension_helper_test`
  build clean. Under wine 11.20 (Staging), own prefix, mingw `zlib1.dll` on WINEPATH, debug CRT DLLs in
  out/clangcl-debug/bin (as 3.1):
  - `vtextension_process_test.exe`: 5 runs, 70 cases / 470 assertions each, all green (launcher: clean
    environment, exact handle list, kill-on-close job; wait set; ControlPage; WakeSignal incl.
    `signalNeverBlocks`; HelperArguments; `Channel.peerProcessExitIsClosed`; 29 Supervisor cases).
  - `vtextension_helper_test.exe "~[.measure]"`: 3 runs, 124 cases (122 passed, 2 skipped: no bundled/Rust
    guests in this configuration), 2882 assertions; the contract and the hostility suite through the real
    Windows helper. `[hostility]` again after the core-dump commit: 7 cases / 1375 assertions.
  - Under wine (debug, not comparable): round trip 65.8 µs, spawn to RuntimeCreated 38.6 ms. Wine is not
    Windows: evidence, not proof.
- Coverage (out/clang-coverage, the five suites, helpers write `.gcda` via `teardownGrace`; llvm-cov gcov):
  lines — ExtensionHostSupervisor.cpp 95.9% of 458, HostTimers.cpp 100% of 47, PosixLaunch.cpp 86.0% of 136,
  Process_linux.cpp 86.9% of 84, ProcessHostConnector.cpp 95.6% of 68, HelperLocation.cpp 100% of 9,
  LaneWatch.cpp 100% of 62, ControlPage_posix.cpp 72.1% of 86, WakeSignal_posix.cpp 80.7% of 140,
  HelperArguments.cpp 95.8% of 95, HostProcess.cpp 94.8% of 97, InterruptListener.cpp 71.4% of 35,
  HostBootstrap_posix.cpp 92.0% of 25, HelperIncarnation.cpp 94.1% of 186, RemoteBackend.cpp 95.0% of 378,
  contour-extension-host/main.cpp 100% of 4; all together 91.9% of 1910 lines, functions 333 of 348 (95.7%).
  Uncovered: syscall-failure branches and their errno mapping (memfd/mmap/socketpair/posix_spawn/pidfd_open
  failing); RAII move-assignment/move-construction (InterruptListener, ControlPage, WakeSender/Receiver);
  EINTR retries; the supervisor's degraded polling after the wait set broke (816–818) and its start-up
  failures; `exitStatusOfWait` (the FreeBSD/macOS reap path, compiled on Linux, unused there). Worth a test
  later: the helper's `ProtocolViolation` and `HandlesUnusable` exits (HostProcess.cpp 103/109/120 — no test
  sends the helper a malformed parent PDU), the listener's `WakeWait::PeerGone`, HelperIncarnation's
  connect-then-refused path terminating the handle (86–88) and the outbox flush that finds the channel
  blocked and marks the incarnation dead (245).
- Not verified here: FreeBSD (`pdfork` launcher, process-descriptor wait set, `procctl` death signal,
  `RLIMIT_AS` unset until 5.4) and macOS (`posix_spawn` + kqueue `EVFILT_PROC`, `shm_open` control page,
  `SO_NOSIGPIPE`, no parent-death binding) compile and run only in CI; the branch was not pushed from the gate
  (run rules), so their jobs' results are outstanding. The core-dump finding above also applies to FreeBSD and
  macOS CI runners that collect cores (the hostile host's fix is POSIX-wide).

---

## Phase 4 — Shared guest memory

### Task 4.1: Carried minors
**Files:**
- Create: `wasm/BuildInstrumentation.hpp`, `remote/process/Process_linux.hpp`.
- Modify: `process/{Process_linux,ProcessLauncher.hpp,ProcessHostConnector,PosixLaunch,Process_win32,ExtensionHostSupervisor,ProcessLauncher_test}`, `remote/HostRefusals{.hpp,.cpp,_test}`, `host/{HostBootstrap.hpp,_posix,_win32,HostProcess.hpp,.cpp,_test}`, `wasmtime/HelperEngine.cpp`, three CMakeLists.

**Produces:**
- `BuildInstrumentation` and `ThisBuildInstrumentation`.
- `LaunchErrorRow{LaunchError error; string_view text; optional<HostRefusal> refusal}`, `launchErrors()`.
- `refusalLine(HostRefusal) -> std::string`.
- `PidfdWaits`, `probePidfdWaits()`, `makeLinuxProcessLauncher(PidfdWaits)`.
- `HardeningStep`, `HardeningRow{step, text, bool(*apply)() noexcept}`, `hardeningSteps()`.
- `hardenProcess(span<HardeningRow const>) -> expected<void, HardeningStep>`, `hardenProcess()`, `hardeningStepText()`.
- `BuildCapPolicy` derived from `ThisBuildInstrumentation`.

- [ ] **Step 1: Write the tests.**
  - `"ProcessLauncher.unsupportedPidfdWaitsRefuseEveryLaunch"` (Linux): `→ LaunchError::Unsupported`; with the helper ends dropped, the parent control end `receiveSome` → `Closed` (no child).
  - `"ProcessLauncher.thisKernelWaitsOnPidfds"`: `== Supported`.
  - `"LaunchErrors.oneRowPerError"`.
  - `"HostRefusals.launchLinesSayWhatTheLauncherSays"`: `refusalLine(r) == format(prefix, row.text)` and `launchErrorText(e) == row.text`; `Stopped` has no refusal.
  - `"HostRefusals.everyRefusalHasALine"`: none names wasm/WAMR/Wasmtime.
  - `"HostBootstrap.hardeningNamesTheFirstFailedStep"`: fake rows 2 and 3 fail → `error() == rows[1].step`, all three ran.
  - `"HostProcess.failedHardeningSaysWhichStep"`: Hello `Failed`; the step's text is non-empty.
  - `"BuildInstrumentation.drivesCapsAndStderr"`: `BuildCapPolicy` and `defaultHelperStandardError()` match the instrumentation; asan tree → `Sanitizers`.
- [ ] **Step 2: Build** `vtextension_process_test vtextension_remote_test`; expect compile failures.
- [ ] **Step 3: Implement M2** (D64).
- [ ] **Step 4: Implement M3** (D65).
- [ ] **Step 5: Implement hardening rows** (D66).
- [ ] **Step 6: Implement the instrumentation switch** (D63). Add `CONTOUR_BUILD_INSTRUMENTATION=$<IF:$<BOOL:${CONTOUR_SANITIZE}>,2,$<IF:$<BOOL:${CONTOUR_COVERAGE}>,1,0>>` on `vtextension_remote_wire` and delete the old three definitions.
- [ ] **Step 7: Run** `vtextension_process_test "ProcessLauncher.*" "LaunchErrors.*" "HostRefusals.*" "BuildInstrumentation.*"` and `vtextension_remote_test "HostBootstrap.*" "HostProcess.*"` → PASS. Grep for the three old macros → none; ctest → PASS.
- [ ] **Step 8: Commit** (four commits):
  - `refactor(vtextension): inject whether the kernel waits on pidfds into the launcher`
  - `refactor(vtextension): phrase each launch failure once`
  - `feat(vtextension): say which hardening step of the extension host failed`
  - `refactor(vtextension): one switch says how a build is instrumented` (CMake and code are inseparable here).

### Task 4.2: Shared objects, one creation path
**Files:**
- Create: `transport/{OwnedHandle.hpp,SharedObject.hpp,SharedObject_posix.cpp,SharedObject_win32.cpp,SharedObject_test.cpp}`. The test goes in `vtextension_process_test` (it runs under wine).
- Modify: `transport/ControlPage{.hpp,_posix,_win32,_test}`, `Channel_{posix,win32}` (`closeNativeHandle`), CMake.

**Produces:**
- `OwnedHandle` (`get`, `release`, `operator bool`).
- `MaxPageBytes = 16 KiB`, `constexpr fitsSharedObject(object, expected)`.
- `SharedObjectUse { GuestMemory, InterruptPage }`, `MapAccess`.
- `createSharedObject(bytes, use) -> expected<OwnedHandle, SharedObjectError>`.
- `sealSharedObject(handle, use)`.
- `checkSharedObjectSize(handle, expected)` (POSIX).
- `SharedMapping::map(handle, bytes, access)`, `bytes()`, `commit(from, to)` (Windows `VirtualAlloc MEM_COMMIT` through the view; no-op elsewhere).
- `isControlPageSize` is removed.

- [ ] **Step 1: Write the tests.**
  - `"SharedObject.sizeMayBeRoundedUpToTheLargestPage"`:
    - accepted: `fits(16384, 4096)` (the Rosetta case), `fits(32 MiB, 32 MiB)`;
    - refused: `fits(16385, 4096)`, `fits(4095, 4096)`, `fits(32 MiB + 16 KiB, 32 MiB)`.
  - `"SharedObject.writesAreSeenThroughASecondMapping"`.
  - `"SharedObject.isSealedAgainstResizing"` (Linux/FreeBSD): `ftruncate` → EPERM; seals are `SHRINK|GROW|SEAL`, no `FUTURE_WRITE` for `GuestMemory`.
  - `"SharedObject.isNotInherited"`.
  - `"SharedObject.checkRefusesAWrongSize"`.
  - `"SharedObject.reservedSectionCommitsOnlyWhatIsAsked"` (Windows `VirtualQuery`).
  - Delete `ControlPage.sizeMayBeRoundedUpToThePage`; the other ControlPage cases are unchanged.
- [ ] **Step 2: Build** `vtextension_process_test`; expect failure.
- [ ] **Step 3: Implement POSIX** (creation moves out of `ControlPage_posix`).
- [ ] **Step 4: Implement Windows** (`SEC_RESERVE` for `GuestMemory`; `VirtualQuery` size check on map).
- [ ] **Step 5: Rebuild ControlPage on SharedObject**; POSIX adopt uses `checkSharedObjectSize` (fixes M1).
- [ ] **Step 6: Run** `vtextension_process_test "SharedObject.*" "ControlPage.*"` → PASS; ctest → PASS.
- [ ] **Step 7: Commit** (two):
  - `fix(vtextension): accept a shared object rounded up to any page a parent may use`
  - `feat(vtextension): reserved shared objects whose pages Contour commits`

### Task 4.3: Handing an object to the helper; protocol v3
**Files:**
- Create: `transport/HandleGrant{.hpp,_posix,_win32}`.
- Modify:
  - `transport/{Channel,PduStream}` and their tests.
  - `proto/{Pdu.hpp,PduTable.hpp,Codec.cpp,Codec_test.cpp}`.
  - `process/{ProcessLauncher.hpp,Process_*.cpp,ExtensionHostSupervisor,ProcessHostConnector,testing/FakeProcessLauncher,testing/LaunchProbe.cpp,ProcessLauncher_test}`.
  - `remote/HostConnector.hpp`, `host/InProcessHost`.

**Produces:**
- Proto:
  - `ProtocolVersion = 3`, `WasmPageBytes`, `memoryObjectBytes()`.
  - `MemoryGrant`, `Instantiate::memory`.
  - `GrowStatus`, `Grow`, `Grown` (kinds 16/17; the variant static_assert names `Grown`).
- POSIX `Channel`:
  - `sendWithHandle(bytes, NativeHandle)`.
  - `receiveSomeWithHandles(buffer, vector<OwnedHandle>&)`.
- Grants:
  - `HandleTarget` with `currentProcess()` and, on Windows, `ofProcess(OwnedHandle)`.
  - `HandleGrant{u64 wireValue; optional<NativeHandle> attached}`.
  - `grantHandle(target, object)`.
  - `claimGrantedHandle(PduStream&, wireValue) -> expected<OwnedHandle, StreamError>`.
- `PduStream`: `send(pdu, grant)`, `takeAttachedHandle()`. Only (Control, ToHelper) streams accept attachments, at most one.
- Process side:
  - `IProcessHandle::handleTarget() -> expected<HandleTarget, LaunchError>` (called on the supervisor thread after launch; failure → kill, `Io`).
  - `SupervisedHelper::handleTarget()`, `IHostHandle::handleTarget()`.
- Probe: `--receive-grant` reads an `Instantiate`, claims, maps, writes `probe` at 0, and reports `granted=ok|refused`.

- [ ] **Step 1: Write the transport tests** (vtextension_test, POSIX).
  - `"Channel.handleTravelsWithItsBytes"`: same `st_ino`, `FD_CLOEXEC`.
  - `"Channel.parentInstallsNoDescriptorFromTheHelper"`: 100 frames with attached pipes; the parent's open-fd count is unchanged.
  - `"PduStream.instantiateCarriesItsDescriptor"`: the second claim → `Malformed`.
  - `"PduStream.secondUnclaimedDescriptorIsMalformed"`.
  - `"PduStream.lanesNeverInstallADescriptor"`.
  - `"PduStream.claimWithoutADescriptorIsMalformed"`.
- [ ] **Step 2: Write the codec tests.**
  - Round-trip samples gain `Instantiate`+grant, `Grow`, `Grown`.
  - `"PduCodec.protocolVersionIsThree"`.
  - `"PduCodec.grantAtItsLargestFitsItsCap"` (`UINT64_MAX` ×3).
  - `"PduCodec.memoryObjectBytes"`: (0) → 64 KiB, (32 MiB + 1) → 32 MiB + 64 KiB.
  - The existing direction/channel and mutation cases cover the new rows.
- [ ] **Step 3: Write the grant tests** (process_test, wine).
  - `"HandleGrant.reachesAChild"`: `granted=ok`, and the test's mapping reads `probe` within 5 s.
  - `"HandleGrant.forgedValueIsRefused"`: Windows sends the probe's own pipe value; POSIX attaches no descriptor → `granted=refused`, exit 0.
  - `"Supervisor.launchCarriesAHandleTarget"`: a failing target → `Io`, and the fake was killed.
- [ ] **Step 4: Build** `vtextension_test vtextension_process_test extension_host_probe`; expect failure.
- [ ] **Step 5: Implement the channel and stream:**
  - `sendmsg` with one `SCM_RIGHTS` cmsg;
  - `recvmsg` with `CMSG_SPACE(2 fds)`;
  - `MSG_CTRUNC` or a foreign cmsg → close all, `Io`;
  - Windows: not compiled.
- [ ] **Step 6: Implement grants and targets** (D69). On Windows, launcher targets are `DuplicateHandle(child, PROCESS_DUP_HANDLE)`.
- [ ] **Step 7: Implement the wire rows.** HostServer ignores `memory` for now; the helper still says Copy.
- [ ] **Step 8: Run** `vtextension_test "Channel.*" "PduStream.*" "PduCodec.*"` and `vtextension_process_test "HandleGrant.*" "Supervisor.*"` → PASS; ctest → PASS.
- [ ] **Step 9: Commit** (two):
  - `feat(vtextension): hand an extension host a descriptor with the request that needs it`
  - `feat(vtextension): protocol for memory Contour shares with an extension host`

### Task 4.4: Helper side — mapping, memory creator, growth through the parent
**Files:**
- Create:
  - `transport/GuestMemoryMapping{.hpp,_posix,_win32,_test}` (test in process_test).
  - `host/MemoryProvision{.hpp,.cpp}`.
  - `wasmtime/WasmtimeMemoryCreator{.hpp,.cpp}`.
- Modify: `wasmtime/{WasmtimeEngine.hpp,.cpp,HostedEngine.cpp,WasmtimeEngine_test.cpp}`, `testing/{HostedEngine.hpp,ProcessModules.hpp}`, `host/{HostServer.cpp,HostServer_test.cpp}`, CMake.

**Produces:**
- `MappingError { WrongSize, Io, Unsupported }`.
- `GuestMemoryMapping::map(object, objectBytes, initialBytes, guardBytes)`, `base()`, `accessibleBytes()`, `objectBytes()`, `open(bytes)` (never shrinks).
- `canMapGuestMemory()`.
- `host::MemoryProvision{OwnedHandle object; u64 objectBytes, initialBytes; function<GrowStatus(u64)> grow}`, `ProvisionScope`, `takeProvision()`.
- `wasmtime::MemorySharing { Private, Provided }`, `SignalsGuardBytes = 32 MiB`, `WasmtimeConfig::memory`.
- Test hook `testing::lastMemoryRequest() -> optional<MemoryRequest{minimum, reserved, guard}>`.
- `makeHostedEngine(MemorySharing = Private)`.
- Test modules:
  - `GrowModule`: one memory, min 1 / max 16 pages; exports `grow(pages) -> old`, `store(address, value)`.
  - `StartGrowModule`: its start function grows memory and stores the result in an exported global.

- [ ] **Step 1: Write the mapping tests.**
  - `"GuestMemoryMapping.opensOnlyTheInitialBytes"`: Linux `/proc/self/maps` shows `rw-s` / `---s` / `---p`; Windows `VirtualQuery`; other systems SKIP.
  - `"GuestMemoryMapping.openedBytesAreTheParentsBytes"`.
  - `"GuestMemoryMapping.refusesAnObjectTooSmall"`.
  - `"GuestMemoryMapping.neverShrinks"`.
  - `"GuestMemoryMapping.placeholdersWork"` (Windows; SKIP under wine if false, recorded).
- [ ] **Step 2: Write the engine tests** (Provided, Software).
  - `"WasmtimeEngine.providedMemoryBacksTheInstance"`: `store(16, 42)` is seen by the parent's mapping.
  - `"WasmtimeEngine.growthAsksAndOpens"`:
    - Granted → `grow(1) == 1`, and a store at 64 KiB + 8 is seen by the parent;
    - Refused → `grow(1) == 0xFFFFFFFF`, and `memory(65536, 1)` is `OutOfBounds`.
  - `"WasmtimeEngine.instantiateWithoutProvisionFails"`.
  - `"WasmtimeEngine.provisionMismatchFails"`.
  - `"WasmtimeEngine.reservationAndGuardAreExplicit"`: `{64 KiB, objectBytes, 0}` under Software; the 32 MiB guard under Signals runs only when not instrumented.
  - `"WasmtimeEngine.privateMemoryIsUnchanged"`.
- [ ] **Step 3: Write the server tests** (scripted parent).
  - `"HostServer.sharedInstantiateMapsTheGrant"`.
  - `"HostServer.growGoesThroughTheParent"`: Granted → `argv{1}`; Refused → `0xFFFFFFFF`.
  - `"HostServer.growOutsideALaneIsRefused"` (`StartGrowModule`: no Grow on any lane; the global is −1).
  - `"HostServer.wrongGrantIsAViolation"`, a table of six: bad `objectBytes`, `initial > object`, unaligned initial, no descriptor / zero handle, half-size object, POSIX `handle != 0`.
  - `"HostServer.anythingButGrownAnswersAGrowIsAViolation"`.
  - `"HostServer.copyEngineRefusesAGrant"`.
- [ ] **Step 4: Build** `vtextension_process_test vtextension_wasmtime_test vtextension_remote_test`; expect failure.
- [ ] **Step 5: Implement the POSIX mapping**: reserve `PROT_NONE` (`MAP_NORESERVE`), size check, `MAP_SHARED|MAP_FIXED`, `mprotect` the initial bytes.
- [ ] **Step 6: Implement the Windows mapping** with placeholders (D74); `open` checks the pages are committed.
- [ ] **Step 7: Implement the provision, the creator** (D72/D73/D75) **and the engine config.** `HelperEngine` stays Private until 4.5.
- [ ] **Step 8: Implement the server side.**
  - Copy mode: the grant must be empty and nothing attached.
  - Shared mode: check, claim, size-check, scope around instantiate.
  - `growOnCurrentLane` per D76.
- [ ] **Step 9: Run** `process_test "GuestMemoryMapping.*"`, `wasmtime_test "WasmtimeEngine.*" "[wasm-contract]"` and `remote_test "HostServer.*"` → PASS; ctest → PASS.
- [ ] **Step 10: Commit** `feat(vtextension): the extension host runs guests in memory Contour gives it`.

### Task 4.5: Parent side — an object per instance, the acknowledged size, answered growth
**Files:**
- Create: `remote/SharedGuestMemory{.hpp,.cpp,_test}` (remote_test).
- Modify:
  - `remote/{RemoteBackend.cpp,HelperIncarnation,HostRefusals,RemoteBackend_test,CopyPathMemory_test,testing/RemoteDefaultBackend,Hostility_test}`.
  - `wasmtime/{HelperEngine,HostedEngine}.cpp`, `testing/BackendContract.hpp`.
  - `contour-extension-host/testing/{HostileModes.hpp,HostileHost.cpp}`.

**Produces:**
- `GrowVerdict { Violation, Refused, Granted }`.
- `SharedGuestMemory::create(cap, initial) -> expected<…, ExtensionError>` (`OutOfMemory` / `RuntimeUnavailable`), `object()`, `closeObject()`, `grant(wire)`, `view(addr, size)`, `grow(bytes)`, `acknowledgedBytes()`.
- `HelperIncarnation`: `memoryMode()`, `handleTarget()`, `request(pdu, step, grant)`.
- `HostRefusal::MemoryMode` is removed.
- The hostile relay now forwards attached descriptors, and gains these rows:

| Row | Acts when | Action |
|---|---|---|
| `grow-past-cap` | first `Grow` | bytes → objectBytes + 64 KiB |
| `grow-shrinks` | first `Grow` | bytes → 0 |
| `grow-unaligned` | first `Grow` | bytes + 1 |
| `grow-wrong-instance` | first `Grow` | id + 1 |
| `descriptor-up` (POSIX) | first `CallResult` | attach `/dev/null` |
| `scribble-memory` | every grant | map it; a thread flips bytes until EOF |
| `copy-memory` | start | inner engine Private (Hello says Copy) |

- [ ] **Step 1: Write the memory tests.**
  - `"SharedGuestMemory.boundsAreContoursSize"`.
  - `"SharedGuestMemory.growthIsChecked"`:
    - Violation: 64 KiB (not larger), 64 KiB + 1 (unaligned), 2 MiB (past the cap), 128 KiB a second time;
    - Granted: the first 128 KiB.
  - `"SharedGuestMemory.viewsSurviveGrowth"`.
  - `"SharedGuestMemory.commitFollowsAcknowledgement"` (Windows).
- [ ] **Step 2: Write the backend tests** (scripted helper).
  - `"RemoteBackend.helloMaySayEitherMode"`.
  - `"RemoteBackend.sharedMemoryNeedsNoMemoryPdus"`.
  - `"RemoteBackend.growIsAnsweredAndBoundsFollow"`.
  - `"RemoteBackend.badGrowKillsTheHost"` (table of four → `HostCrashed`, `terminate()` once).
  - `"RemoteBackend.growWhileInterruptedIsRefused"`.
  - `"RemoteBackend.initialSizeComesFromTheModule"` (2 pages → 128 KiB; over the cap → `OutOfMemory`, nothing sent).
  - `"RemoteBackend.unexportedMemoryIsOutOfBounds"`.
  - `"RemoteBackend.instancesHoldNoDescriptor"` (1000 cycles; fd count unchanged).
  - `"RemoteBackend.growDuringLoweringIsSeen"` (256 KiB string through `cabi_realloc`).
  - The copy cases pass over `makeHostedEngine(Private)`.
- [ ] **Step 3: Write the contract case** `grownMemoryIsVisible`: `grow(1) == 1`; `store(65540, 0xC0DE)`; `memory(65540, 4)` reads it. It runs on every composition.
- [ ] **Step 4: Write the hostility rows.**
  - The grow rows: `call grow` → `HostCrashed` in < 2 s, one death, `uncleanExits == 0`, lazy recovery (`launches == 2`).
  - `descriptor-up`: `add == 5`, the parent's fd count is unchanged, no death.
  - `"hostile memory through a real host"` (`scribble-memory`): 200 statusline renders within 10 s, each ok or an `ExtensionError`, `uncleanExits == 0`.
- [ ] **Step 5: Build** the remote, test and wasmtime test binaries, `contour-extension-host`, the hostile helper and `vtextension_helper_test`; expect failure.
- [ ] **Step 6: Implement `SharedGuestMemory`**: create, map RW, seal, commit the initial bytes; an atomic acknowledged size.
- [ ] **Step 7: Implement the backend.**
  - `RemoteModule` keeps the initial size and whether a memory is exported.
  - Shared-mode instantiate: create → grant → request with the grant → `closeObject`.
  - `RemoteInstance` holds the variant; the call loop answers `Grow` per D76.
  - The handshake no longer refuses a mode.
- [ ] **Step 8: Switch Shared on**: `HelperEngine` (`canMapGuestMemory()`), `HostedEngine` defaults to Provided, plus the relay forwarding and rows.
- [ ] **Step 9: Run** `remote_test` (whole), `helper_test "~[.measure]"` (whole: contract 42, `[bundled]`, Review Focus, teardown, hostility) and `vtextension_test "[wasm-contract]"` → PASS; ctest → PASS.
- [ ] **Step 10: Commit** (two):
  - `feat(vtextension): share guest memory with the extension host instead of copying it`
  - `test(vtextension): hostile growth, descriptors and memory from an extension host`

### Task 4.6: Single-fetch decoding
**Files:**
- Modify: `vtextension/{CanonicalAbi.hpp,Instance.cpp,ControlImports.cpp,CanonicalAbi_test.cpp}`, every `IGuestMemory` double, `process/testing/LaunchProbe.cpp` (`--mutate`).
- Create: `remote/HostileMemory_test.cpp` (helper_test).

**Produces:**
- `IGuestMemory::read(Address, span<byte> into) -> Result<void>`; `view` is renamed `writable`; `allocate` and `chargeLift` are unchanged.
- `detail::readInteger<U>(memory, address)`, `detail::readBytes<Container>(memory, list, elementSize)`.
- `GuestMemory::read` = one `memcpy` from `IWasmInstance::memory()`.

- [ ] **Step 1: Write the tests.**
  - `"GuestReader.fetchesEachByteOnce"`: a TEMPLATE over every type the host lifts (string, byte list, ConfigMap, LogLevel, each control request).
    - A counting memory stores a sample, then loads it.
    - During loads, `writable()` returns a decoy buffer.
    - Assertions: `load == sample`, and each guest byte is read at most once.
  - `"GuestReader.mutationAfterReadChangesNothing"`: `read()` corrupts its source after copying; the string stays the first copy and the list keeps its first length.
  - `"hostile memory does not break decoding"`:
    - Setup: a 1 MiB object laid out with samples; the probe, launched with `--mutate`, inherits it and flips bytes (`mt19937{0x5eed}`); the test runs 100 000 decodes.
    - Each decode either fails (`Trapped`/`OutOfMemory`) or yields valid UTF-8, in-range enums and lengths ≤ the budget.
    - The probe reports `mutations > 0` and exits 0 at EOF.
- [ ] **Step 2: Build** `vtextension_test vtextension_helper_test extension_host_probe`; expect failure.
- [ ] **Step 3: Interface, Instance and doubles.**
- [ ] **Step 4: Move every decode in `CanonicalAbi.hpp` onto `read`.**
- [ ] **Step 5: Add the probe's `--mutate`.**
- [ ] **Step 6: Run** `vtextension_test "GuestReader.*" "CanonicalAbi.*" "ControlImports.*"` and `helper_test "hostile memory does not break decoding"` → PASS; ctest → PASS.
- [ ] **Step 7: Commit** `fix(vtextension): decode what an extension passes from one copy of its memory`.

### Task 4.7: Gate
- [ ] **Step 1: ASan/LSan.** Build all five test binaries and the three executables; `ctest --preset=clang-asan -R 'vtextension|wasm_runtime_containment'` → all pass. Contract: 42 cases per composition; `uncleanExits == 0`.
- [ ] **Step 2: TSan** (`out/clang-tsan`, the same targets) → zero reports, no suppressions.
- [ ] **Step 3: Measurement** (`out/clang-release`, the two needed targets, three runs each).
  - `Process.measureRender` (steady/uncached) through: the Shared helper, `copy-memory`, the in-process Shared host, and in-process Wasmtime.
  - `Process.measureRoundTrip`, `Process.measureSpawn`, new `Process.measureInstantiate`.
  - Helper and parent VmRSS + Pss (`smaps_rollup`) and VmData after 16 plasma instances.
  - Report against phase 3: steady 18.1–18.6 µs, round trip 3.3 µs, spawn 1.2 ms, RSS 10.8 MiB idle.
  - D83: the steady median must be below 18.1 µs, or stop and report.
  - Record everything in "Phase-4 facts".
- [ ] **Step 4: Windows cross-build + wine** (`linux-to-windows-clangcl-debug`, Embedded debug format, extensions on, bundled and GUI off). Build `vtextension_process_test extension_host_probe contour-extension-host vtextension_helper_test`; five wine runs.
  - **Wine can show:** clean compiles with `-Werror`; `SEC_RESERVE` create/commit; `DuplicateHandle` into a child; `HandleGrant.*`; and the Copy path end to end.
  - **Wine cannot prove:** placeholder mappings (if `canMapGuestMemory()` is false the helper says Copy, recorded); an access violation on uncommitted pages becoming a trap; job accounting of section commits.
  - Record what ran; fix any real break, each in its own commit.
- [ ] **Step 5: FreeBSD/macOS.** Code review against a checklist:
  - XNU `ftruncate` works once;
  - macOS has no `MSG_CMSG_CLOEXEC` (`fcntl` instead);
  - `CMSG` alignment;
  - shm `MAP_FIXED` over an anonymous reservation;
  - the Rosetta bound;
  - FreeBSD `memfd` seals and its parent-only role (WAMR helper in 5.3).
  
  CI results only if the owner allows a push; otherwise record them as outstanding.
- [ ] **Step 6: Coverage** of the SharedObject, HandleGrant, GuestMemoryMapping, MemoryProvision, WasmtimeMemoryCreator, SharedGuestMemory, RemoteBackend and HostServer sources; name the kinds of uncovered lines.
- [ ] **Step 7: Phase review** (`/code-review` of `4ec0d13f6..HEAD`); address every finding.
- [ ] **Step 8: Phase summary:** performance (Step 3), risk (D69, D74, D75, D76, D81), coverage, and what wine and CI did and did not prove.

### Phase-4 facts
*(Filled in by 4.4 and 4.7.)*

From 4.4 (`c2bd2249e`):
- **Wasmtime 48.0.10 memory creator, measured** (GrowModule, min 1 / max 16 pages, cap 4 MiB): `new_memory` gets `minimum = 65536` (initial bytes), `maximum = 1048576` (the module's declared max in bytes, not the cap), `reserved_size_in_bytes = 4194304` (= `memory_reservation` = `memoryObjectBytes(cap)`), `guard_size_in_bytes = 0` under Software and `33554432` under Signals. `grow_memory(env, newSize)` gets the new **total** byte length (131072 for `memory.grow 1` from 1 page). An error from `grow_memory` makes `memory.grow` return -1 (no trap) with the store limiter the backend installs. `get_memory` returns base, size (= current bytes) and capacity (= object bytes).
- `IHostedEngine::memoryGuardBytes()` is new: the guard the host maps after each object (Wasmtime Provided: 32 MiB Signals / 0 Software; Copy engines 0). The **server** maps the grant (`GuestMemoryMapping::map(handle, objectBytes, initialBytes, memoryGuardBytes())`) before instantiating, adopts the handle and closes it; `MemoryProvision` is `{GuestMemoryMapping memory; GrowRequest grow}` (not `{OwnedHandle, sizes, grow}`).
- Server grant rules (Shared): `objectBytes == memoryObjectBytes(CreateRuntime.memoryBytes)`, `initialBytes <= objectBytes`, `initialBytes % 64 KiB == 0`, claim succeeds, map succeeds; else ProtocolViolation. Copy: `memory == MemoryGrant{}` and nothing attached. Any non-Instantiate control PDU with an attached descriptor is a violation.
- The creator requires `minimum == initialBytes`, `reserved == objectBytes`, `guard <= mapping guard`; a module whose memory differs fails with `Instantiated{InvalidComponent}` (not a violation). The **parent must send `initialBytes` = the module's initial memory exactly** (D72).
- Growth: `Grow{instanceId, newTotalBytes}` then exactly one `Grown`; on the lane of the running call, or on **control before `Instantiated`** for a start function (R29). Anything else answering it is a violation. Helper opens (POSIX `mprotect`, Windows `VirtualAlloc(MEM_COMMIT)` through its own view) only after `Granted`.
- wine 11.20 supports `VirtualAlloc2`/`MapViewOfFile3` placeholders: `canMapGuestMemory()` is **true** under wine; the shared-memory server tests pass there.
- `makeHostedEngine(proto::MemoryMode = Copy)` (neutral type, not `wasmtime::MemorySharing`, because testing/ may not name the backend). `HelperEngine` is still Private/Copy.

From 4.4 fix round 1 (`3780e3062..891522849`):
- **Signal traps on provided memory (Wasmtime 48, release tree, strace):** Wasmtime bounds an access by the **reservation** (= object, 4 MiB), not the current size. A store past the open bytes but inside the object passes the check and faults `SEGV_ACCERR` in the closed pages → handled as a trap (so the helper's `PROT_NONE` tail / Windows uncommitted pages are **the** bounds enforcement there, not a belt). A store past the object fails the explicit check and is redirected to address 0 (Spectre hardening) → `SEGV_MAPERR` at NULL → trap. The 32 MiB guard is never touched with a 4 MiB reservation; no abort over `wasm_accessible()` was seen. Capacity reported to Wasmtime stays `objectBytes`.
- Catch2 restores its own signal handlers after each test case, over Wasmtime's once-per-process ones: only **one** test case per binary may bring up a Signals engine (`WasmtimeEngine.signalTrapsOnProvidedMemory`).
- `GrowRequest` is now `GrowStatus(GuestMemoryMapping& memory, uint64_t bytes)`: it asks the parent and opens on grant. Granted-but-unopenable → `ServerExit::MemoryUnusable` → `HostProcessExit::MemoryUnusable`, exit code **76**, connection ends once the guest returns (no `CallResult`/`Instantiated` is sent). `HostServerConfig::openGuestMemory` (default `&GuestMemoryMapping::open`) is the test seam.
- `MemoryProvision` lives in its own target `vtextension_memory_provision` (links `vtextension_remote_wire`); `vtextension_wasmtime` links it privately, not `vtextension_host`.
- POSIX reservation has no `MAP_NORESERVE` (FreeBSD lacks it).

From 4.5 (`9efa54729..41aeac18e`):
- **Linux glues bytes queued before an `SCM_RIGHTS` message into the same `recvmsg`** (it stops *after* the
  descriptor-carrying skb, not before). `PduStream` attributes a descriptor to the last frame starting in its
  buffer at receipt (Linux) or the first frame starting in the chunk (BSDs, by reading); `takeAttachedHandle()`
  hands it out only once that frame was received. Any relay or reader of granted descriptors must do the same.
- **Wasmtime 48 installs its signal handlers lazily, by the first instantiation**, not at engine creation. Catch2
  (a compiled library here: `CATCH_CONFIG_NO_POSIX_SIGNALS` cannot apply) restores its own after each case;
  `WasmtimeEngine_test`'s `EngineSignalHandlers` puts Wasmtime's back per Signals case. Two Signals cases now.
- R30 lives in `HostConnection::objectBytesCap` (process connector: `caps.dataBytes`; in-process default 1 GiB;
  0 = none), leased per instance (`HelperIncarnation::leaseObjectBytes`). With the 4 MiB contract cap only 256
  instances fit: tests holding `InterruptSlotCount` instances use a one-page runtime.
- `HelperIncarnation::request(pdu, step, grant, GrowAnswer)` answers control-channel Grows under the control
  mutex; the two-argument `request` treats a Grow as a wrong reply.
- The helper's Hello says Shared iff `canMapGuestMemory()`; `makeCopyingHelperEngine()` exists for the hostile
  `copy-memory` row (the D83 copy-versus-shared measurement). `makeHostedEngine()` defaults to Shared; scripted
  HostServer/HostProcess tests ask for Copy.
- The helper sets `RLIMIT_NPROC = 0` (release): a hostile-host thread must start before `serve()`. The hostile
  setup-failure code is 79 (76 is MemoryUnusable). Copy-path hostile rows run `HostMemory::Copied`.
- Release tree: the real helper with Signals traps and Provided memory passes `vtextension_helper_test
  "~[.measure]"` whole (157 cases). Under `scribble-memory` 199/200 renders succeed in both trees.

From 4.6 (`e4ffcfe5f..3a49108ff`):
- `abi::IGuestMemory` is `read(Address, span<byte>)` (one copy; the only way lifting reads), `writable(Address,
  size)` (lowering and range proofs only), `allocate`, `chargeLift`. `detail::readInteger<U>`,
  `detail::readBytes<Container>(memory, list, elementSize)` (byte-sized containers; caller charges first),
  `detail::checkElements` (range proof, reads nothing); `loadPointerAndLength` is one 8-byte read. Any new
  decoder reads through these; `"GuestReader.fetchesEachByteOnce"` (decoy `writable()`, per-byte read counts)
  over every lifted type and each control request catches one that does not.
- `RemoteRuntime::load` reads the module's initial memory before any host is asked (unreadable bytes:
  `InvalidComponent`, no Load sent, no slot made live); `RemoteModule::instantiate` refuses an over-cap initial
  memory before `_slot->live()`. A test "module" of zero bytes is now refused up front: use a real module
  (Teardown_test pads one with a custom section).
- `extension_host_probe --mutate <helper args> --seed=N`: claims one Instantiate's grant, commits the whole
  object, flips random bytes (`mt19937{N}`) on a thread until control EOF; reports `mutating`/`end`, then
  `mutations=N`/`end`, exit 0. The seed is an argument because clang-tidy refuses a constant-seeded engine.
- The `scribble-memory` row is unchanged by single-fetch (199/200 in ASan and release): it scribbles the whole
  memory of the latest instance, so the one failure is the guest's own, not a host decode. The single-fetch
  proof is the deterministic GuestReader test; the hostile decode test (`HostileMemory_test.cpp`) did not catch
  a validate-in-place-then-copy mutation in 3 runs (the window is too narrow), it proves robustness only.

From 4.7 (`3a49108ff..c326cc652`, gate):
- **Release measurement (3 runs, medians; load 1.6–1.9 on 32 threads, quiet).** Plasma steady render: real
  helper, shared memory **8.4 µs** (8.3–9.2); same helper copying memory (`--hostile=copy-memory`, which is the
  real host with `makeCopyingHelperEngine`, no relay) **18.7 µs** (18.4–19.2) — phase 3's 18.1–18.6 was this
  copy path; in-process shared host (`Remote`) 9.0 µs; in-process Wasmtime 3.4 µs. Uncached: shared 25.5,
  copy 26.4, Remote 20.0, Wasmtime 12.9 µs (uncached stays noisy: 18.3–26.5 shared). Shared saves 10.3 µs
  (55 %) of a steady frame; helper overhead (helper − Wasmtime)/helper = 60 % steady, 49 % uncached.
- Round trip 3.3 µs (2.7–5.0; phase 3 3.3), spawn to runtime 1.28 ms (phase 3 1.2), idle helper VmRSS
  10.6 MiB (phase 3 10.8). `Process.measureInstantiate` (plasma, 200 instances, each released): shared
  18.0 µs, copy 12.5 µs — an object (memfd, ftruncate, seals, parent map) and the helper's map cost ≈ 5.5 µs per
  instance.
- After 16 plasma instances: helper VmData 46.9 MiB (46.4 loaded: the 16 objects add ~0.5 MiB of VmData, the
  `MAP_SHARED` guest memory is not counted — D81 measured), VmRSS 23.0 MiB (RssAnon 5.4, RssShmem 0.7);
  parent VmData 17.1, VmRSS 10.5 (RssShmem 0.7), Pss 4.2 MiB. **The helper's Pss cannot be read by its parent**:
  it is non-dumpable (hardening), so `/proc/<pid>/smaps_rollup` is closed; `status` stays readable.
- `GuestMemoryReads.hpp` (`detail::readOnce`, `detail::guestRange`) is how `Instance::GuestMemory` reads and
  maps runtime failures; `HostileMemory_test`'s memory uses the same two. `GuestReader.flatLiftsFetchEachByteOnce`
  covers flat lifting (seen failing, 23 assertions, with `readBytes` reading through `writable()`).
- The probe's flips come from raw `mt19937` output (offset `% size`, mask `% 0xFF + 1`): a seed names the same
  flips on every standard library; the interleaving stays the scheduler's.
- **MSVC debug CRT asserted in `isspace`** (`ExtensionHost.unloadableNoticeIsDrawn` under wine: "⚠" bytes as
  negative `char`). Fixed in vtbackend (`isAsciiWhitespace`, Line/Grid trimming); pre-existing, not phase-4 code.
- wine 11.20 (`out/clangcl-debug`): process_test 98 (97 + 1 skipped cross-view), remote_test 262 (260 + 2
  skipped), helper_test `~[.measure]` 128 (126 + 2 skipped), `hostile memory does not break decoding` (probe
  `--mutate` under wine) + `Hostility.copyingHostServes` pass, HandleGrant/SharedObject/GuestMemoryMapping 18
  (17 + 1 skipped). TSan (`out/clang-tsan`): the five suites, zero reports, no suppressions.
- Coverage (`out/clang-coverage`, five suites + helpers, llvm-cov gcov): the ten phase-4 sources 92.9 % of 1206
  lines. Uncovered kinds: RAII move-assignment, syscall-failure branches (memfd/mmap/mprotect/fstat/seal), the
  creator's never-called grow guards, host-crash-mid-request and grant/create failure paths in RemoteBackend,
  HostServer's malformed-parent-PDU violations and Grow send failures.
- By reading only (no macOS SDK here): `CMSG_SPACE` is used as a `std::array` bound in `Channel_posix.cpp`;
  if an SDK's `__DARWIN_ALIGN32` casts through a pointer, that is not a constant expression — first macOS CI run
  decides.
- **R38 (fix wave, `2126634ae`):** `makeChannelPair()` sets `SO_PASSRIGHTS = 0` (value 83) on the parent end
  (Linux ≥ 6.16): a helper's `sendWithHandle` up is `ChannelError::Refused` (EPERM), nothing sent;
  `ChannelPair::upward` says `Refused`/`Discarded`; the hostile `descriptor-up` relay forwards bare on Refused.
  Tests that need a discarding parent end build one with `socketpair` + `Channel::adopt`.
- macOS shm names (`b52650093`): `SharedObjectName.hpp`, random 48-bit suffix, up to 8 tries on EEXIST.

## Review Focus addendum (phase 4)
6. **A helper mutates memory while Contour decodes.** No decoder may read a byte twice. Pinned by `"GuestReader.fetchesEachByteOnce"` and `"hostile memory does not break decoding"`; review rule: no decoder reads through `writable()`.
7. **Contour touching memory it never granted.** A helper's growth past the cap, shrink or wrong instance; reads past the acknowledged size; Windows uncommitted pages. Pinned by `"SharedGuestMemory.growthIsChecked"`, `"RemoteBackend.badGrowKillsTheHost"` and the grow hostility rows.
8. **Descriptor leaks either way.** Contour must install no descriptor a helper sends, and hold none per instance. Pinned by `"Channel.parentInstallsNoDescriptorFromTheHelper"`, `descriptor-up` and `"RemoteBackend.instancesHoldNoDescriptor"`.
9. **Growth on unusual paths:** during nested `cabi_realloc` lowering, while interrupted, or in a start function. Pinned by `"RemoteBackend.growDuringLoweringIsSeen"`, `"RemoteBackend.growWhileInterruptedIsRefused"`, `"HostServer.growOutsideALaneIsRefused"` and `grownMemoryIsVisible`.
10. **Unproven platforms.** Rosetta/XNU rounding, Windows placeholders and macOS ancillary handling are verified by reading and CI only. D81 also leaves guest memory outside RLIMIT_DATA.

## Task order
4.1 → 4.2 → 4.3 → 4.4 → 4.5 → 4.6 → 4.7 (4.6 depends only on 4.2 and may run beside 4.3–4.5).

## Phase-4 decisions

Proposed for a controller ruling. Each names the choice and, after the dash, why.

- D62. Seven tasks: 4.1 carried minors → 4.2 shared objects → 4.3 handle passing + wire v3 → 4.4 helper side → 4.5 parent side (Shared switched on) → 4.6 single-fetch decoding → 4.7 gate. — Growth spans both processes, and each side tests alone against a scripted peer. The minors touch files 4.3 edits. 4.6 needs only 4.2.
- D63. One switch, `CONTOUR_BUILD_INSTRUMENTATION` (0 none / 1 coverage / 2 sanitizers). It is a PUBLIC definition on `vtextension_remote_wire`, read only by `wasm/BuildInstrumentation.hpp` (`enum class BuildInstrumentation : uint8_t { None, Coverage, Sanitizers }`, `ThisBuildInstrumentation`). It replaces `CONTOUR_SANITIZED_BUILD`, `CONTOUR_INSTRUMENTED_BUILD` and `CONTOUR_HELPER_SOFTWARE_TRAPS`; caps, helper stderr and trap mode become constexpr reads. — Three macros with three conditions become one value.
- D64. M2: `LinuxProcessLauncher(PidfdWaits)`; `makeSystemProcessLauncher()` probes once and injects; `makeLinuxProcessLauncher(PidfdWaits)` goes in a Linux-only `process/Process_linux.hpp`. — DI without widening the cross-platform API.
- D65. M3: one `launchErrors()` table in `ProcessLauncher.hpp`, rows `{error, text, refusal}`. `launchErrorText`, the connector's refusal mapping and `refusalLine()` read it; the launch log lines become `"The extension host cannot be started: {text}. Extensions do not run."`. — Each failure is phrased once.
- D66. `hardenProcess()` returns `std::expected<void, HardeningStep>` over per-OS rows `{step, text, apply}` (`CoreDumps`, `Dumpable`, `Tracing`, `FaultReports`). Every row runs, and the first failure is returned. `beforeThreads` takes that type. The helper tells `hardening failed: <text>` once, and Hello is still `Failed`. — The log names the step, and hardening stays complete.
- D67. One creation module, `transport/SharedObject{.hpp,_posix,_win32}`, for the control page and guest memory (as D46 foresaw). `fitsSharedObject(object, expected)` accepts sizes from `expected` up to `roundUp(expected, MaxPageBytes = 16 KiB)`, whatever the reader's own page size. — Fixes M1: an x86_64 helper under Rosetta sees a 4 KiB page while the arm64 parent's object was rounded to 16 KiB.
- D68. One object per instance, always, even for modules without memory.
  - Size: `proto::memoryObjectBytes(cap) = max(roundUp(cap, 64 KiB), 64 KiB)`.
  - Linux/FreeBSD ≥ 13: memfd sealed `SHRINK|GROW`, then `SEAL` once mapped. GROW goes beyond the spec's `SHRINK|SEAL`; it is strictly stronger.
  - macOS: `/contour-eh-<pid>-<serial>`, `O_EXCL`, unlinked at once, `ftruncate` once.
  - Windows: a `SEC_RESERVE` section.
  - The parent maps the whole object RW once (it never moves) and closes its handle once the `Instantiate` carrying it is sent. — No descriptor per instance (RLIMIT_NOFILE with many panes).
- D69. Handle passing rides the **control channel with `Instantiate`** (already there per D9/D11).
  - POSIX: `SCM_RIGHTS` on the frame's first byte. Only the helper's control stream (Control, arriving ToHelper) uses `recvmsg` (`MSG_CMSG_CLOEXEC`; macOS `fcntl`) and holds at most one unclaimed descriptor. A second one, `MSG_CTRUNC` or a non-`SCM_RIGHTS` cmsg → `Malformed`.
  - Windows: the parent `DuplicateHandle`s the section (`FILE_MAP_READ|FILE_MAP_WRITE`) into the helper. It uses a `PROCESS_DUP_HANDLE` duplicate that the launcher hands out (`IProcessHandle::handleTarget()` → `SupervisedHelper` → `IHostHandle::handleTarget()`). The value travels in `Instantiate.memory.handle`, which must be 0 on POSIX.
  - **Lost:** a failed send kills the incarnation, and the handle dies with the helper.
  - **Forged:** the helper refuses what it cannot map as an object of `objectBytes` with write access, as a protocol violation. A bogus Windows value is released, not closed.
- D70. Nothing flows up. Parent reads stay plain `recv()`, so the kernel discards descriptors a helper attaches; on Windows the parent never reads a handle value. — Pinned by a test and a hostile row.
- D71. Wire v3:
  - `Instantiate.memory = MemoryGrant{u64 handle, objectBytes, initialBytes}`; `objectBytes == 0` means a copy-mode host. Row cap 32 → 64.
  - `Grow{u32 instanceId, u64 bytes}`: Lane, ToParent, cap 16.
  - `Grown{GrowStatus}`: Lane, ToHelper, cap 8; `GrowStatus { Refused, Granted }`. Kinds 16 and 17.
  - The `Memory*` PDUs stay for Copy; `DecodeError` is unchanged.
- D72. The initial size comes from the parent's own parse (`binary::largestInitialMemoryPages(_bytes)`). Above the cap → `OutOfMemory` before anything is sent. The helper's creator requires Wasmtime's `minimum == initialBytes` and `reserved == objectBytes`.
- D73. Wasmtime config in Provided mode:
  - `memory_reservation = objectBytes`.
  - `memory_guard_size` = 32 MiB under Signals, 0 under Software.
  - `reservation_for_growth = 0`, `may_move = false`, `init_cow = false`.
  - `wasmtime_config_host_memory_creator_set`.
  - `WasmtimeConfig` gains `MemorySharing { Private, Provided }`.
  - The helper mapping (`transport/GuestMemoryMapping`, std-only so it runs under wine) reserves object + guard inaccessible, maps the object `MAP_SHARED|MAP_FIXED` at the base and opens only `[0, initial)`.
- D74. Windows maps through placeholders (`VirtualAlloc2` / `MapViewOfFile3` / `UnmapViewOfFile2`), resolved from kernelbase by `GetProcAddress`, plus a 64 KiB probe (`canMapGuestMemory()`). If unavailable, the helper's Hello says `Copy` and the copy path serves. — Stated in the handshake, equally safe.
- D75. **Deviation in wording:** the provision reaches the engine-wide creator through an engine-neutral thread-local (`host::MemoryProvision`, RAII `ProvisionScope`). HostServer's **control thread** sets it, not "the executor" (spec §5.4), because instantiation runs on control (D11). The creator takes it once; none, or a second memory → `InvalidComponent`.
- D76. Growth.
  - Helper: `grow_memory(n)` sends `Grow` on `CurrentLane` and reads exactly one PDU, which must be `Grown`. Granted → `open(n)`; Refused → `memory.grow` returns −1.
  - **Outside a lane call (a start function) growth is refused**, as host calls are there (D11). This differs from in-process engines.
  - Parent, in every call loop: a violation unless the id is the innermost frame's, `n % 64 KiB == 0` and `acknowledged < n ≤ min(cap, objectBytes)`.
  - Parent answers: interrupted → Refused; Windows commit failure → Refused; otherwise commit, store `n` (release) and answer Granted.
- D77. `memory()` is bounded by the parent's acknowledged size (acquire), never by anything the helper reports. A module that exports no memory → `OutOfBounds`. Views stay valid across growth.
- D78. The mode follows each incarnation's Hello. Both modes are accepted: `HostRefusal::MemoryMode` is removed, and `RemoteInstance` holds `variant<CopyPathMemory, SharedGuestMemory>`.
- D79. Single fetch: `abi::IGuestMemory::read(Address, span<byte> into)` (one memcpy) becomes the only way decoders read guest bytes. `view()` is renamed `writable()` (lowering and range proofs only). — Enforced by `"GuestReader.fetchesEachByteOnce"`, whose `writable()` hands decoders a decoy buffer.
- D80. Hostile memory is tested two ways, with no new executable: `extension_host_probe --mutate` for the abi-level case, and the hostile relay's `scribble-memory` row end to end.
- D81. **Residual risk, needs a ruling.** Guest memory leaves D38's RLIMIT_DATA (Linux does not count `MAP_SHARED`). A compromised helper can dirty whole objects of all live instances (≤ 1024 × 32 MiB), keep released ones mapped until it dies, and on Windows commit section pages through its own view. Proposed: accept and document. The alternative is a per-incarnation byte budget.
- D82. Compositions: remote_test's in-process host runs Shared, plus a Copy composition (`makeHostedEngine(Private)`); wasmtime_test stays Private; the real helper runs Shared. The contract gains `grownMemoryIsVisible` (42 cases).
- D83. Gate: the phase closes only if the real helper's steady plasma median is below 18.1 µs (phase 3's best). Overhead is reported as (helper − in-process Wasmtime) / helper, against the copy path in the same build via the hostile row `copy-memory`.
- D84. Phase 5 inputs:
  - seccomp: `recvmsg` on control, `mmap(MAP_SHARED|MAP_FIXED)` and `mprotect` of received descriptors, `close`.
  - Seatbelt: map received shm (`ipc-posix-shm-*` by the `/contour-eh-` prefix).
  - AppContainer: map a duplicated-in section.
  - Landlock is unaffected.

Builds: `flock -o /tmp/contour-extension-host.build.lock nice -n 19 ionice -c 3 cmake --build --preset clang-asan -j 4 --target <t>`. Tests start with `unset $(env | grep -o '^CONTOUR_[A-Z_]*')`. Binaries are under `out/clang-asan/src/vtextension/` (`vtextension_test`), `.../wasm/remote/` (process, remote and helper tests) and `.../wasm/wasmtime/`. Before each commit: `ctest --preset=clang-asan -R vtextension`, clang-format, editorconfig-checker. After each task: `/simplify`.

Rulings on D62–D84 (R28–R31), applied by the controller:
- R28. D62–D84 are accepted, except as changed below.
- R29 (changes D76). Growth during instantiation, for example a start function calling `memory.grow`, is allowed and travels on the **control channel**. While the parent waits for `Instantiated`, it answers a `Grow` there with `Grown`, under the same checks. `Grow`/`Grown` gain a Control row in each direction they need. Why: memory growth is not a host call, a C or Rust runtime commonly grows memory in its start function, and refusing it would make out-of-process behave differently from in-process. Cost if wrong: one more channel row and a test (`"HostServer.growDuringInstantiateGoesThroughTheParent"`, which replaces `growOutsideALaneIsRefused`).
- R30 (changes D81). The parent caps the sum of `objectBytes` across one incarnation's live instances at `ResourceCaps::dataBytes` (1 GiB by default). An `Instantiate` that would exceed it fails with `OutOfMemory` before anything is created. This bounds the memory a compromised helper can dirty. What still remains is documented: released objects can stay mapped until the helper dies, and on Windows the helper can commit pages through its own view (the job memory limit counts those). Test: `"RemoteBackend.liveObjectsStayWithinTheDataCap"`.
- R31 (changes D83). The gate passes when the real helper's steady median is at or below phase 3's range (≤ 18.6 µs) and the copy-versus-shared delta is reported. It stops only on a regression. Why: a steady render moves few guest bytes, so "strictly faster than 18.1 µs" would gate on noise.

- R38 (phase review; revises R36, extends D84). Parent ends set `SO_PASSRIGHTS = 0` where available (Linux ≥ 6.16): an upward `SCM_RIGHTS` fails in the helper with `EPERM`, so the parent never runs the last close of a helper's file (a lingering TCP socket could otherwise block a parent thread indefinitely). Phase 5 must add: helper seccomp denies `sendmsg`/`sendmmsg` carrying rights and `socket(AF_INET/AF_INET6)`; Seatbelt denies network and grants `ipc-posix-shm-read-data`/`write-data` only (no create); shm names retry on `EEXIST` with a random suffix.

---

## Phase 5 — Sandboxes, and WAMR in the helper

Paths are relative to `src/vtextension/wasm/` unless they start with `src/`, `cmake/`, `scripts/`, `support/` or
`.github/`. Builds: `flock -o /tmp/contour-extension-host.build.lock nice -n 19 ionice -c 3 cmake --build --preset
clang-asan -j 4 --target <t>`. Test shells start with `unset $(env | grep -o '^CONTOUR_[A-Z_]*')`. Binaries are
under `out/clang-asan/src/vtextension/` (`vtextension_test`), `.../wasm/remote/` (process, remote and helper
tests) and `.../wasm/wasmtime/`. Before each commit: `ctest --preset=clang-asan -R vtextension`, clang-format
(`/usr/bin/clang-format`), editorconfig-checker. After each task: `/simplify`. Release, TSan and coverage trees
are used only where a step names them, one at a time.

**Where each platform is proven.** Linux x86_64: here (kernel 7.2, Landlock ABI with `RESTRICT_SELF_TSYNC`),
in all four trees. Linux arm64 (`membarrier`, the aarch64 syscall table): CI only. Windows: clang-cl
cross-build with `-Werror`, plus wine 11.20, which shows the plumbing but proves no containment (Task 5.7
says which). macOS and FreeBSD: reading only here; CI decides, and only if the owner allows a push.

### Task 5.1: The sandbox seam and the lockdown order (no OS mechanism yet)
**Files:**
- Create: `host/Sandbox{.hpp,.cpp,_none.cpp}` (`_none.cpp` is compiled where no platform file exists yet), `host/Sandbox_test.cpp` (remote_test).
- Modify:
  - `remote/proto/{Pdu.hpp,PduTable.hpp,Codec.cpp,Codec_test.cpp}`.
  - `host/{HostBootstrap.hpp,HostBootstrap_posix.cpp,HostBootstrap_win32.cpp,HostProcess.hpp,HostProcess.cpp,HostProcess_test.cpp,HostServer.hpp,HostServer.cpp,HostServer_test.cpp,IHostedEngine.hpp,CMakeLists.txt}`.
  - `wasmtime/{WasmtimeEngine.hpp,WasmtimeEngine.cpp,HostedEngine.cpp,WasmtimeEngine_test.cpp}`, `testing/HostedEngine.hpp`.
  - `remote/{HelperIncarnation.cpp,HostRefusals.hpp,HostRefusals.cpp,HostRefusals_test.cpp}`, `process/{ProcessHostConnector.hpp,.cpp}`, `process/testing/FakeProcessLauncher`, the supervisor/connector tests.
  - `src/contour-extension-host/testing/HostileHost.cpp`.

**Produces:**
- Proto:
  - `ProtocolVersion = 4`.
  - `LockdownStep : uint8_t { None, CoreDumps, Dumpable, Tracing, FaultReports, NoNewPrivileges, FileSystem, SystemCalls, ThreadSync, ResourceCaps, Capabilities, DescriptorRights, Profile, Container, Mitigations }`. The hardening steps fold in here (D87).
  - `Hello` gains `LockdownStep failedStep` and `uint32_t failureCode` (errno or HRESULT bit pattern).
- `lockdownSteps()` rows `{step, text}` in `remote_wire`; both sides phrase a step from it.
- `ConfinementFailure{LockdownStep step; int code}` replaces `HardeningFailure`. `hardenProcess()` returns `expected<void, ConfinementFailure>`.
- `host::ISandbox`:
  - `restrictBeforeThreads() -> expected<void, ConfinementFailure>` (what threads inherit);
  - `lockDown() -> expected<void, ConfinementFailure>` (after every thread started and was prepared).
- `SandboxDescriptors{NativeHandle control}`.
- `makeSystemSandbox(SandboxDescriptors) -> unique_ptr<ISandbox>`. `NullSandbox` reports `Unsandboxed` and is what `_none.cpp` returns.
- `IHostedEngine::prepareThread() -> expected<void, ExtensionError>`.
- `HostConfinement` gains `sandbox` (`function<unique_ptr<ISandbox>(SandboxDescriptors)>`). `confine` becomes caps then `lockDown`.
- `ProcessHostConnector` remembers a `LockdownFailed` refusal (D90).

- [ ] **Step 1: Write the order tests** (remote_test, scripted parent, recording fakes).
  - `"HostProcess.locksDownInTheSandboxOrder"`. The recorded sequence is exactly: bind, harden, adopt, engine, restrict, listener started, 4 executors started, 5 × prepareThread (4 lanes + control), caps, lockDown, Hello sent. `restrict` saw 0 threads started.
  - `"HostProcess.everyConfinementStepRunsPastAFailure"`: restrict fails `FileSystem` and lockDown fails `SystemCalls`. Both run. Hello is `{Failed, FileSystem, code}`, and one `sandbox failed: <text>: <system text>` line is told.
  - `"HostProcess.hardeningFailureIsStatedAsItsStep"`: a fake hardening row fails `Dumpable` → Hello `{Failed, Dumpable, code}`.
  - `"HostProcess.nullSandboxSaysUnsandboxed"`: Hello `{Unsandboxed, None, 0}`, which is today's behaviour.
- [ ] **Step 2: Write the server and engine tests.**
  - `"HostServer.everyThreadIsPreparedBeforeConfinement"`: a counting engine. At `confine`, prepareThread calls == `MaxLaneChannels + 1`, each on a distinct thread id, and the control thread is among them.
  - `"HostServer.unpreparedThreadEndsTheHost"`: lane 2's prepareThread fails → `ServerExit::ExecutorUnavailable`, confine never runs, nothing is sent.
  - `"WasmtimeEngine.prepareThreadRunsTheGuestPath"`: Software and Signals (counts as one of the two Signals cases, through `EngineSignalHandlers`). Returns ok, is idempotent per thread, and loads its built-in module once per engine.
- [ ] **Step 3: Write the wire, refusal and connector tests.**
  - `"PduCodec.protocolVersionIsFour"`. The Hello round-trip samples gain every `LockdownStep`. Over-cap and mutation cases cover the row.
  - `"LockdownSteps.oneRowPerStep"`: the texts name no engine.
  - `"HostRefusals.lockdownLineNamesTheStep"`: `refusalLine(LockdownFailed, step)` contains the step's text.
  - `"ProcessHostConnector.lockdownFailureIsNotRelaunched"` (FakeProcessLauncher): the first helper says `Failed` → `HostUnavailable`, and the line is logged once. Three later connects → `HostUnavailable` with `launches == 1`.
- [ ] **Step 4: Build** `vtextension_remote_test vtextension_process_test vtextension_wasmtime_test`; expect failure.
- [ ] **Step 5: Implement the wire row and the step table** (D89). Rename `HardeningStep` → `LockdownStep` throughout, and `HardeningFailure` → `ConfinementFailure`.
- [ ] **Step 6: Implement the seam and the order** (D86, D87).
  - `runExtensionHost`: bind → harden → adopt → engine → `sandbox(descriptors)->restrictBeforeThreads()` → `HostProcess` (listener, then executors) → prepare → caps → `lockDown()` → Hello.
  - Every step runs, and the first failure is the one stated.
  - The hostile host passes a `NullSandbox` factory (D97).
- [ ] **Step 7: Implement priming** (D88).
  - Executors call `prepareThread()` first and count down a latch. `HostServer::run()` prepares its own (control) thread, then waits on the latch, then runs `confine`.
  - Wasmtime: compile a built-in `(module (func (export "p")))` once per engine, then instantiate and call it per thread.
  - The copying engines and the test doubles: no-op.
- [ ] **Step 8: Implement the sticky refusal** (D90) and the refusal line.
- [ ] **Step 9: Run** `remote_test "HostProcess.*" "HostServer.*" "PduCodec.*" "LockdownSteps.*" "HostRefusals.*" "Sandbox.*"`, `process_test "ProcessHostConnector.*" "Supervisor.*"` and `wasmtime_test "WasmtimeEngine.*"` → PASS. Then `helper_test "~[.measure]"` whole → PASS (Unsandboxed; hostility unchanged); ctest → PASS.
- [ ] **Step 10: Commit** (three):
  - `refactor(vtextension): one table names every step of the extension host's confinement`
  - `feat(vtextension): the extension host locks down in a fixed order, its threads prepared first`
  - `feat(vtextension): say which confinement step of an extension host failed, and start it only once`

### Task 5.2: The Linux sandbox — seccomp, no_new_privs and Landlock
**Files:**
- Create:
  - `host/Sandbox_linux.cpp`.
  - `host/linux/{SeccompFilter.hpp,SeccompFilter.cpp,SystemCallRows.hpp,SystemCallRows.cpp,Landlock.hpp,Landlock.cpp}`.
  - `host/linux/testing/BpfEvaluator.hpp` (a test-only classic-BPF interpreter for `seccomp_data`).
  - `host/linux/{SeccompFilter_test,Landlock_test}.cpp` (process_test, Linux).
  - `process/SandboxProbe_test.cpp` (process_test, Linux): real lockdown in a child.
- Modify:
  - `process/testing/LaunchProbe.cpp`: gains `--sandbox-op=<op>`, `--outer-filter=<kind>` and `--sandboxed` (with `--receive-grant`).
  - `transport/Channel_posix.cpp`: `refuseDescriptors` gets the errno table of D95, with a test in `Channel_test.cpp`.
  - `remote/testing/ProcessDefaultBackend.cpp` and `process/testing/HelperComposition.cpp`: `RequireLockedDown` where `ThisPlatformSandbox != None`.
  - `host/CMakeLists.txt` and `remote/CMakeLists.txt`: the probe links `vtextension_host`.

**Produces:**
- `SystemCallAction { Kill, Refuse, Allow }`. `Refuse` carries an errno.
- `ArgumentCheck{uint8_t index; ArgumentTest test; uint64_t mask; uint64_t value}`, where `ArgumentTest { Equals, AllClear, AllSet, EqualsControl, EqualsOwnProcess }`.
- `SystemCallRow{long number; std::span<ArgumentCheck const> checks; SystemCallAction action; int error; std::string_view reason}`.
- `releaseRows()`, `sanitizerRows()`, `coverageRows()`, and `buildRows()` (release + the set `ThisBuildInstrumentation` names).
- `assembleFilter(span<SystemCallRow const> rows, SandboxDescriptors, int ownProcess, AuditArch) -> expected<vector<sock_filter>, ConfinementFailure>`.
- `installFilter(span<sock_filter const>) -> expected<void, ConfinementFailure>`: `SECCOMP_SET_MODE_FILTER` with `SECCOMP_FILTER_FLAG_TSYNC`. A positive return (a thread id) → `ThreadSync`.
- `LandlockAbiRow{int abi; uint64_t fsRights; uint64_t netRights; uint64_t scopes; uint32_t restrictFlags}` and `landlockAbis()`. The constants are ours, because CI headers are older.
- `LandlockOutcome { Unavailable, Restricted }`.
- `restrictFileSystem(span<PathBeneathRule const> allowed, LandlockSystem const&) -> expected<LandlockOutcome, ConfinementFailure>`. `LandlockSystem` is three injectable function pointers (DI for the syscalls).
- `ThisPlatformSandbox : enum class PlatformSandbox : uint8_t { None, Linux, FreeBSD, MacOS, Windows }`.

Probe ops table (`--sandbox-op`). The op runs on an `ExecutorThread` started before lockdown, after a go signal:

| Op | Expect |
|---|---|
| `open-file` (`/etc/hostname`) | refused `EACCES` |
| `open-directory` (`/`) | refused `EACCES` |
| `socket-inet`, `socket-unix` | refused `EPERM` |
| `send-descriptor` (sendmsg + `SCM_RIGHTS` on control) | killed `SIGSYS` |
| `fork`, `spawn-thread`, `execve /bin/true`, `ptrace-traceme` | killed `SIGSYS` |
| `signal-parent` (`kill(getppid(), 0)`) | killed `SIGSYS` |
| `map-writable-executable` | killed `SIGSYS` |
| `map-shared-unfixed` | killed `SIGSYS` |
| `protect-writable-executable` | killed `SIGSYS` |
| `permitted` | `ok`. Runs channel send/recv, `recvmsg` of a grant, `mmap(MAP_SHARED\|MAP_FIXED)` over a reservation, `mprotect` RW, `munmap`, `close`, `getrandom`, `clock_gettime`, a condvar wait (futex), `sigaltstack`, and `membarrier` on aarch64 |

- [ ] **Step 1: Derive the allow-list by measurement** (D93).
  - Run `strace -f -qq -o <scratchpad>/<tree>.%p` over the unsandboxed helper while `vtextension_helper_test "~[.measure]"` runs in `out/clang-asan` and `out/clang-release`. Then repeat for `out/clang-tsan` and `out/clang-coverage` (one tree at a time).
  - Keep only the calls after each helper's `prctl(PR_SET_NO_NEW_PRIVS)` (Task 5.1 placed that marker).
  - Tabulate syscall × tree. The scripts stay in the scratchpad and are not committed.
  - Every syscall becomes a row with a reason, or is named in the report as refused-and-handled.
- [ ] **Step 2: Write the filter tests** (`BpfEvaluator`, no installation).
  - `"SeccompFilter.decidesEachRowAsItsTableSays"`, a table of `(arch, nr, args) → action`:
    - sendto/recvfrom/futex → Allow;
    - recvmsg on control → Allow, recvmsg on a lane → Kill;
    - sendmsg, sendmmsg → Kill (R38);
    - socket(AF_INET), socket(AF_UNIX) → Refuse EPERM;
    - openat → Refuse EACCES (release) and Allow (coverage set);
    - mmap `PROT_WRITE|PROT_EXEC` → Kill; mmap `MAP_SHARED` without `MAP_FIXED` → Kill; mmap `MAP_SHARED|MAP_FIXED` RW → Allow;
    - mprotect W+X → Kill;
    - clone, clone3, fork, vfork, execve, execveat, ptrace → Kill;
    - tgkill(own) → Allow, tgkill(other) → Kill;
    - a foreign arch → Kill; `__X32_SYSCALL_BIT` → Kill; nr 4095 → Kill.
  - `"SeccompFilter.noRowSetAllowsTheForbidden"`: no row in any of the three sets allows execve*, ptrace, process_vm_*, socket, connect, bind, sendmsg, sendmmsg, clone*, fork, vfork, unshare, setns, mount, bpf or perf_event_open.
  - `"SeccompFilter.everyRowSaysWhy"`: the reason is non-empty, with no duplicate number per set.
  - `"SeccompFilter.assemblesForBothArchitectures"`: x86_64 and aarch64 row numbers (aarch64 has no `open`, `fork` or `poll`). The program stays ≤ `BPF_MAXINSNS`.
- [ ] **Step 3: Write the Landlock tests** (fake `LandlockSystem`).
  - `"Landlock.abiTableIsMonotonic"`.
  - `"Landlock.usesTheBestAbiTheKernelHas"`: ABI 3 → no net/scopes, no TSYNC; ABI ≥ the TSYNC row → `RESTRICT_SELF_TSYNC`.
  - `"Landlock.unavailableIsNotAFailure"`: `ENOSYS`, `EOPNOTSUPP` → `Unavailable`.
  - `"Landlock.otherErrorsAreFailures"`: `EPERM` from restrict_self (no NNP) → `FileSystem`.
- [ ] **Step 4: Write the real lockdown tests** (process_test, Linux; the probe in a child).
  - `"Sandbox.forbiddenOperationsFailFromAnExecutor"`: over the ops table, each row's expectation (refused errno reported on control, or `ExitStatus{Signaled, SIGSYS}`).
  - `"Sandbox.permittedOperationsWorkFromAnExecutor"`.
  - `"Sandbox.landlockCoversEveryThread"`: the probe reads `/proc/self/status` `Seccomp:\t2` and `NoNewPrivs:\t1` for each `/proc/self/task/*` (opened before lockdown). `open-file` is refused from a thread started before restrict too, when the ABI has TSYNC.
  - `"Sandbox.locksDownWithoutLandlock"` (`--outer-filter=no-landlock`: an outer ALLOW filter returning `ENOSYS` for `landlock_*`) → `LockedDown`, and `open-file` is still refused (the seccomp row).
  - `"Sandbox.locksDownUnderAFlatpakLikeFilter"` (`--outer-filter=flatpak`: ALLOW default; `clone3` → `ENOSYS`; `unshare`, `setns`, `mount`, `personality`, `ptrace` → `EPERM`) → `LockedDown`, and the permitted op is `ok`.
  - `"Sandbox.threadSyncFailureFailsLockdown"`: a thread installs its own filter first → `ThreadSync`.
  - `"Channel.refusingDescriptorsMayBeUnavailable"` (outer filter: setsockopt `SO_PASSRIGHTS` → `EPERM`): `makeChannelPair()` succeeds; `upward == Discarded`.
- [ ] **Step 5: Write the helper case.** `"Helper.locksDown"` (helper_test, Linux): Hello `{LockedDown, None, 0}`. The binary's composition now requires `LockedDown`, so the whole helper_test runs sandboxed, including `everyAllowedWasiFunctionWorks` and `[bundled]`.
- [ ] **Step 6: Build** `vtextension_process_test extension_host_probe vtextension_remote_test vtextension_helper_test contour-extension-host`; expect failure.
- [ ] **Step 7: Implement the filter** (D91, D92).
  - Hand-assembled cBPF: arch check, the x32 bit, then the rows in frequency order, default `SECCOMP_RET_KILL_PROCESS`.
  - Argument checks use 32-bit halves (`seccomp_data.args` low/high words).
- [ ] **Step 8: Implement Landlock and the sandbox** (D94, D96).
  - `restrictBeforeThreads`: `PR_SET_NO_NEW_PRIVS`, then Landlock. Every handled FS right of the ABI, with no rules except the instrumented builds' (coverage object root write, sanitizer `/proc` read). Also net TCP bind/connect and the abstract-unix + signal scopes where the ABI has them.
  - `lockDown`: `buildRows()` → assemble → install with TSYNC. Unavailable Landlock tells once: `the file system restriction is unavailable on this system; system calls are restricted`.
- [ ] **Step 9: Implement the instrumented rows** (D96).
  - Sanitizer helper environment: `detect_leaks=0` appended to `ASAN_OPTIONS`/`LSAN_OPTIONS` (a row in the launcher's sanitizer environment table).
  - Coverage: the helper opens `CONTOUR_COVERAGE_OBJECT_ROOT` (a compile definition, the build tree) before Landlock and adds a `PATH_BENEATH` write rule.
  - `refuseDescriptors`: `EPERM`/`EACCES` → `Discarded` (D95).
- [ ] **Step 10: Run.**
  - `process_test "SeccompFilter.*" "Landlock.*" "Sandbox.*" "Channel.*"` → PASS.
  - `helper_test "~[.measure]"` whole → PASS, with `uncleanExits == 0` (a `SIGSYS` kill would show there).
  - Then the release tree: `cmake --build --preset clang-release -j 4 --target vtextension_helper_test contour-extension-host extension_host_probe vtextension_process_test`, and the same two runs. This is Signals mode, which is the product's row set.
  - ctest → PASS.
- [ ] **Step 11: Commit** (three):
  - `feat(vtextension): the extension host restricts its system calls on Linux`
  - `feat(vtextension): the extension host gives up file system access on Linux before it starts a thread`
  - `fix(vtextension): an outer sandbox refusing descriptor control no longer stops every extension host`

### Task 5.3: `WamrEngine : IHostedEngine`, and a helper that runs it
Order change: this comes before the default switch, because a Linux build without Wasmtime (the Fedora RPM) must still have a helper when the default flips (D85).

**Files:**
- Create:
  - `wamr/{WamrEngine.hpp,WamrEngine.cpp,HelperEngine.cpp,WamrEngine_test.cpp}`. The test goes in `vtextension_test`, which already links WAMR.
  - `remote/testing/WamrHelperDefaultBackend.cpp` and the target `vtextension_wamr_helper_test` (D99).
- Modify:
  - `wamr/{WamrBackend.hpp,WamrBackend.cpp,WamrRuntime.hpp,CMakeLists.txt}`: the null device is injected.
  - `src/contour-extension-host/CMakeLists.txt`: the engine follows `CONTOUR_WASM_RUNTIME`; test-only `contour-extension-host-wamr`.
  - `remote/CMakeLists.txt`.
  - `cmake/ContourThirdParties.cmake`: only if the spike adopts a hook.

**Produces:**
- `wamr::NullDevice` (an owned descriptor, or `OpenPerRuntime`).
- `wamr::WamrConfig{NullDevice nullDevice}` and `makeWamrBackend(WamrConfig = {})`. In-process behaviour is unchanged.
- `wamr::makeWamrEngine() -> unique_ptr<host::IHostedEngine>`: `memoryMode() == Copy` (or `Shared` per the spike), `memoryGuardBytes() == 0`, and `prepareThread()` = WAMR's thread environment.
- The target `vtextension_wamr_helper_engine` defines `makeHelperEngine()` and `makeCopyingHelperEngine()` for WAMR.

- [ ] **Step 1: Spike** (D98): can WAMR 2.4.5 place a linear memory at a base we give and grow it in place?
  - Read `core/iwasm/common/wasm_memory.c` and the runtime's init options in the CPM checkout (`out/clang-asan/_deps/wamr-src`).
  - Rule: Shared only if such a hook exists **and** the configure probe can detect it in a system WAMR. Otherwise Copy.
  - Record the finding as a Phase-5 fact. Expected: Copy.
- [ ] **Step 2: Write the engine tests.**
  - `"WamrEngine.copiesMemory"`: `memoryMode() == Copy`, `memoryGuardBytes() == 0`.
  - `"WamrEngine.runtimesShareOneNullDevice"`: three runtimes and the engine's open-descriptor count stay unchanged. No `open()` happens after construction (counted through an injected opener).
  - `"WamrEngine.prepareThreadIsIdempotent"`.
  - The contract runs over a `RemoteBackend` in-process `HostServer` with `makeWamrEngine()` (a `[wasm-contract]` composition in remote_test, beside the Wasmtime one).
- [ ] **Step 3: Write the WAMR helper suite.** `vtextension_wamr_helper_test` is the contract, `[bundled]` and the Review Focus process cases against `contour-extension-host-wamr`, with `RequireLockedDown` on Linux. `"Helper.wamrHelperSaysCopy"`: Hello `{Copy, LockedDown}`.
- [ ] **Step 4: Build** `vtextension_test vtextension_remote_test contour-extension-host-wamr vtextension_wamr_helper_test`; expect failure.
- [ ] **Step 5: Implement** `NullDevice` injection, `WamrEngine` and `HelperEngine.cpp`. The engine opens the null device at construction, which is before lockdown (D98).
- [ ] **Step 6: Wire the helper's engine** (D99): `contour-extension-host` links `vtextension_${CONTOUR_WASM_RUNTIME}_helper_engine`. With Wasmtime present and the runtime `wasmtime`, `contour-extension-host-wamr` is also built (never installed). The status line `contour-extension-host is not built: …` goes.
- [ ] **Step 7: Run** `vtextension_test "WamrEngine.*" "[wasm-contract]"`, `remote_test "[wasm-contract]"` (both engines) and `vtextension_wamr_helper_test` whole → PASS; then the WAMR helper suite in the release tree; ctest → PASS.
- [ ] **Step 8: Commit** (two):
  - `feat(vtextension): the extension host can run its modules interpreted`
  - `build: build the extension host with whichever engine the build selected`

### Task 5.4: The default composition becomes out-of-process on Linux
**Files:**
- Create: `remote/process/{HostComposition.hpp,HostComposition.cpp,HostComposition_test.cpp}` (process_test), `remote/DefaultBackend.cpp` (target `vtextension_out_of_process`).
- Modify:
  - `CMakeLists.txt` (top level): `CONTOUR_EXTENSION_HOST`, `_CONTOUR_SANDBOXED_PLATFORMS`, `CONTOUR_WASM_RUNTIME` defaulting, summary lines.
  - `src/vtextension/CMakeLists.txt`: `vtextension_runtime`; `vtextension_test` links `vtextension_wamr` by name.
  - `src/contour-extension-host/CMakeLists.txt`: `contour_place_extension_host_beside()`.
  - `src/contour/CMakeLists.txt`, `src/vtbackend/CMakeLists.txt` (bench-headless).
  - `process/ProcessLauncher.hpp` and `Process_*.cpp`: the sanitizer environment row from 5.2 applies to production compositions too.
  - `scripts/check-wasm-runtime-containment.py`: only if the new target trips a rule; the new rules are Task 6.2's.

**Produces:**
- `process::HostComposition` (supervisor over `makeSystemProcessLauncher()`, kind, `ProcessHostConnector`).
- `process::sharedHostComposition(std::filesystem::path helper) -> std::shared_ptr<HostComposition>` (weak-cached, D101).
- `makeDefaultBackend(BackendConfig)` for out-of-process. With a null connector it locates the helper with `locateExtensionHost(vtpty::currentExecutablePath())`. If not found → every load `HostUnavailable`, with the `HelperNotFound` line logged once.
- CMake `contour_place_extension_host_beside(<target>)`.

- [ ] **Step 1: Write the tests.**
  - `"HostComposition.backendsShareOneSupervisor"`: two backends, one supervisor. Dropping both stops it, and a third backend gets a new one.
  - `"HostComposition.missingHelperRefusesEveryLoad"`: `HostUnavailable`, no launch, one log line.
  - `"HostComposition.teardownGraceFollowsTheBuild"`.
- [ ] **Step 2: Write the configure checks** (a script in the scratchpad, recorded in the report). Fresh configures under `<scratchpad>/cfg-*`:
  - `-DCONTOUR_EXTENSION_HOST=in-process -DCONTOUR_WASM_RUNTIME=wasmtime` → FATAL_ERROR naming both.
  - The default on Linux → `out-of-process` with `wasmtime` (CPM).
  - `-DCONTOUR_USE_CPM=OFF` with no Wasmtime → `out-of-process`, `wamr`.
  - An unknown value → FATAL_ERROR.
- [ ] **Step 3: Implement CMake** (D100).
- [ ] **Step 4: Implement the composition and the default backend** (D101, D102).
  - `ContourGuiApp`, `ContourApp` and `bench-headless` stay unchanged, as they call `makeDefaultBackend(BackendConfig{})`.
  - The build tree gets the helper copied beside `contour` and `bench-headless`. On macOS, the bundle's `Helpers/` copy stays as it is.
- [ ] **Step 5: Prove the GUI and daemon link no engine.**
  - `nm -C out/clang-asan/src/contour/contour | grep -cE 'wasm_runtime_|wasmtime_'` → 0.
  - The same for `bench-headless`.
  - `contour-extension-host` has them.
- [ ] **Step 6: Run the suites on the new default.**
  - ctest `-R 'vtextension|wasm_runtime_containment'` → PASS.
  - `bench-headless` extension rows run through a helper: the supervisor launch count > 0 in its output, or `strace -f -e trace=execve` shows the helper.
  - The `e2e`-labelled app runs pass (`ctest --preset=clang-asan -L e2e`).
- [ ] **Step 7: Run a manual check from the build tree** (offscreen, PID-recorded, killed by PID): `QT_QPA_PLATFORM=offscreen` contour with the bundled status-line extension renders. `pgrep -P <contour pid>` shows `contour-extension-host`, whose `/proc/<pid>/status` says `Seccomp: 2`, `NoNewPrivs: 1`.
- [ ] **Step 8: Commit** (two):
  - `feat(vtextension): run extensions in a sandboxed extension host process by default on Linux`
  - `build: select where extensions run and which engine the extension host uses`

### Task 5.5: FreeBSD sandbox
**Files:**
- Create: `host/Sandbox_freebsd.cpp`.
- Modify: `host/HostBootstrap_posix.cpp` (FreeBSD caps gain `RLIMIT_AS`; D104), `process/SandboxProbe_test.cpp` (a FreeBSD expectation column), `CMakeLists.txt` (`FreeBSD` appended to `_CONTOUR_SANDBOXED_PLATFORMS`; D110).

**Produces:**
- `DescriptorRole { Control, Lane, Page, Wake, StandardError, EngineDevice }` and its rights table (`cap_rights_t` sets per role, ioctls none, fcntls `F_GETFL`).
- `lockDown()` limits every descriptor in `SandboxDescriptors` (which gains roles) and then calls `cap_enter()`.
- `restrictBeforeThreads()`: `procctl(PROC_NO_NEW_PRIVS_CTL)` where defined, otherwise nothing.

- [ ] **Step 1: Write the tests** (they run on FreeBSD CI only). The probe ops table gains FreeBSD expectations:
  - open → `ECAPMODE`;
  - socket → `ECAPMODE`;
  - `pdfork` → `EAGAIN` (`RLIMIT_NPROC`);
  - `send-descriptor` → `ENOTCAPABLE` or success. Capsicum cannot deny `SCM_RIGHTS` on a right-holding socket; the result is recorded, and D70/R38 cover the parent;
  - an `ioctl` on control → `ENOTCAPABLE`.
  - `"Sandbox.rightsTableCoversEveryRole"` (compiles everywhere, a pure table test).
- [ ] **Step 2: Implement.** Rights per role:
  - control: `READ|WRITE|RECV|SEND|EVENT|SHUTDOWN|FSTAT`;
  - lanes: the same minus `FSTAT`;
  - page: `MMAP_R|FSTAT`;
  - wake: `READ|EVENT`;
  - stderr and the WAMR null device: `WRITE|FSTAT`.
- [ ] **Step 3: Size `RLIMIT_AS`** = `dataBytes` (WAMR Copy reserves nothing beyond it); CI's `procstat -v` measurement goes in the report.
- [ ] **Step 4: Verify by reading** against a checklist:
  - `cap_rights_init` variadic use;
  - `<sys/capsicum.h>` and `libcasper` not needed;
  - `cap_enter` after every thread;
  - WAMR's interpreter needs no new descriptor (the null device was opened before);
  - `kern.arandom` and `getrandom` work in capability mode;
  - `clock_gettime` is allowed;
  - every `#ifdef __FreeBSD__` path compiles in the mind's eye against FreeBSD 14 headers.
- [ ] **Step 5: CI** only if the owner allows a push: the FreeBSD leg runs `vtextension_process_test` and `vtextension_helper_test` (WAMR). Otherwise mark it outstanding.
- [ ] **Step 6: Commit** `feat(vtextension): the extension host enters capability mode on FreeBSD`.

### Task 5.6: macOS sandbox and the helper's entitlement
**Files:**
- Create: `host/Sandbox_macos.cpp`, `host/macos/SandboxProfile.hpp`, `support/macOS/contour-extension-host.entitlements`.
- Modify: `scripts/macos-bundle.py` (per-helper entitlements), `.github/workflows/build.yml` (the macOS ad-hoc hardened-sign step and the helper suite run), `CMakeLists.txt` (`Darwin` appended; D110).

**Produces:**
- `SandboxProfile` (a constexpr profile text) and its parameter rows `{name, value}`. `SHM_PREFIX = "/contour-eh-"` comes from `SharedObjectName.hpp`.
- Hand-declared `extern "C" int sandbox_init_with_parameters(const char*, uint64_t, const char* const[], char**)` and `void sandbox_free_error(char*)`.
- `lockDown()` applies the profile. An error → `Profile`, with `errorbuf` told.

Profile: `(version 1) (deny default)`, plus:
- `(allow ipc-posix-shm-read-data ipc-posix-shm-write-data (ipc-posix-name-prefix (param "SHM_PREFIX")))`;
- `(allow signal (target self))`;
- `(allow sysctl-read (sysctl-name …))`, the names taken from CI's first `sandbox` deny log;
- no `network*`, no `mach-lookup`, no `file-read*`, no `process-fork` or `process-exec`.

- [ ] **Step 1: Write the tests** (macOS CI only).
  - The probe ops table gains macOS expectations (open → `EPERM`, socket → `EPERM`, fork → `EPERM`, `mach-lookup` of `com.apple.system.logger` → denied).
  - `"Sandbox.profileHasNoNetworkOrMachLookup"` (compiles everywhere; a text table test on the profile string).
- [ ] **Step 2: Implement the profile and the declaration.** Wasmtime's mach ports are already off (`wasmtime_config_macos_use_mach_ports_set(false)`); the fact is restated in the file comment.
- [ ] **Step 3: Implement the entitlement.**
  - `contour-extension-host.entitlements` holds `com.apple.security.cs.allow-unsigned-executable-memory` only.
  - `macos-bundle.py` signs `Contents/Helpers/contour-extension-host` with `--options=runtime --entitlements <helper file>` before the outer bundle. The outer bundle's entitlements are unchanged.
  - A unit test in the script's own test file asserts the helper path gets the helper file and nothing else does (or an equivalent check run under `python3 -I`).
- [ ] **Step 4: Add the CI case.**
  - The macOS leg runs `codesign --force --sign - --options=runtime --entitlements support/macOS/contour-extension-host.entitlements <helper>`, then `vtextension_helper_test "~[.measure]"` with `RequireLockedDown`.
  - A second run signs without the entitlement and expects the loads to fail, which proves the entitlement is load-bearing.
- [ ] **Step 5: Verify by reading** against a checklist:
  - the declaration matches Chromium's;
  - `-Werror` sees no deprecated symbol;
  - shm grants are read/write-data only, never create (R38);
  - `MAP_SHARED|MAP_FIXED` of a received shm descriptor needs no `file-map` rule;
  - Phase 4's open macOS items: `CMSG_SPACE` as an array bound, BSD descriptor attribution.
- [ ] **Step 6: CI** only if the owner allows a push; otherwise mark it outstanding.
- [ ] **Step 7: Commit** (two):
  - `feat(vtextension): the extension host runs in a deny-by-default sandbox on macOS`
  - `build: sign the macOS extension host with the one entitlement its compiler needs`

### Task 5.7: Windows sandbox — AppContainer, child-process policy, mitigations
**Files:**
- Create: `process/AppContainer_win32{.hpp,.cpp}`, `host/Sandbox_win32.cpp`, `process/AppContainer_test.cpp` (process_test).
- Modify: `process/{ProcessLauncher.hpp,Process_win32.cpp,ProcessHostConnector.cpp}`, `process/testing/HelperComposition.cpp`, `remote/testing/ProcessDefaultBackend.cpp` (wine detection, D107), `CMakeLists.txt` (`Windows` appended; D110).

**Produces:**
- `ProcessIsolation : uint8_t { None, AppContainer }` and `LaunchSpec::isolation`. Production: `AppContainer`.
- `AppContainerApi`: a table of the userenv and advapi functions, for DI.
- `ensureAppContainer(AppContainerApi const&) -> expected<OwnedSid, LaunchError>`, run once per supervisor on its thread. It uses profile `Contour.ExtensionHost` with no capabilities; `ALREADY_EXISTS` → derive.
- `ensurePackagesCanExecute(directory, AppContainerApi const&) -> expected<void, LaunchError>`: adds `ALL APPLICATION PACKAGES` (S-1-15-2-1) RX only if no such ACE exists.
- Launch attributes: `SECURITY_CAPABILITIES` and `CHILD_PROCESS_POLICY = RESTRICTED`, beside the handle and job lists.
- Helper `lockDown()`:
  - `TokenIsAppContainer` is false → `Unsandboxed`;
  - otherwise it applies `SetProcessMitigationPolicy` rows (`ExtensionPointDisable`, `ImageLoad` no-remote/no-low-label, `StrictHandleCheck`) and checks job membership (`IsProcessInJob`);
  - any failure → `Mitigations`/`Container`;
  - all pass → `LockedDown`.
  - Dynamic-code policy is deliberately absent.

- [ ] **Step 1: Write the tests.**
  - `"AppContainer.profileIsEnsuredOnce"` (fake API): created once; `ALREADY_EXISTS` → derived; other error → `LaunchError::Unsupported`.
  - `"AppContainer.packageGrantIsIdempotent"` (real ACL on a temp directory): two calls give one ACE.
  - `"AppContainer.launchCarriesTheIsolation"` (RecordingLauncher).
  - `"Sandbox.windowsHelperRunsContained"`: real Windows only. In the AppContainer the probe cannot open a user-profile file, `socket` + `connect` to 127.0.0.1 fails, and `CreateProcessW` fails. Under wine it SKIPs with the reason.
  - `"Helper.windowsHelperIsLockedDown"`: real Windows; under wine Hello is `Unsandboxed` and recorded.
- [ ] **Step 2: Cross-build** with `linux-to-windows-clangcl-debug` (Embedded debug format, extensions on, bundled and GUI off) — `vtextension_process_test extension_host_probe contour-extension-host vtextension_helper_test`; expect failure.
- [ ] **Step 3: Implement the API table and the ensure functions** (D106).
- [ ] **Step 4: Implement the launch attributes and the helper's verification** (D106).
- [ ] **Step 5: Implement wine detection** in the test compositions (D107). `wine_get_version` in ntdll → `ProcessIsolation::None` + `AcceptUnsandboxed`.
- [ ] **Step 6: Run the wine runs** (five of process_test, three of helper_test `~[.measure]`).
  - **Wine can show:** clean `-Werror` compiles; the attribute list builds (whether `CreateProcessW` accepts `SECURITY_CAPABILITIES` under wine is recorded either way); the ACL add and the idempotence check; `TokenIsAppContainer` false → `Unsandboxed` → `AcceptUnsandboxed` path end to end; the fake-API profile logic.
  - **Wine cannot prove:** AppContainer isolation of files, network or registry; mitigation policies taking effect; the child-process policy; that the ACL grant is enough for a real AppContainer to load the helper and its CRT.
- [ ] **Step 7: CI** (Windows leg) only if the owner allows a push; otherwise mark it outstanding.
- [ ] **Step 8: Commit** (two):
  - `feat(vtextension): the extension host runs in an AppContainer on Windows`
  - `feat(vtextension): grant app containers read access to the extension host once`

### Task 5.8: Gate
- [ ] **Step 1: ASan/LSan.** Build every vtextension test binary, the three executables and `contour-extension-host-wamr`; `ctest --preset=clang-asan -R 'vtextension|wasm_runtime_containment'` → all pass. Record:
  - contract cases per composition (now including the WAMR in-process host and the WAMR helper);
  - `uncleanExits == 0`;
  - every helper Hello `LockedDown`.
- [ ] **Step 2: TSan** (`out/clang-tsan`, the same targets) → zero reports, no suppressions. The TSan helper locks down (its background thread is covered by Landlock TSYNC and seccomp TSYNC).
- [ ] **Step 3: Release** (`out/clang-release`): the helper suites whole, Signals mode, `RequireLockedDown`, then the syscall audit.
  - Re-run Task 5.2 Step 1's strace over the sandboxed helper.
  - No syscall is refused or killed outside the expected rows.
  - Kernel audit (`journalctl -k --since` for `type=1326`) shows no `SECCOMP` kill.
- [ ] **Step 4: Coverage** (`out/clang-coverage`).
  - `.gcda` files from the sandboxed helper appear, which proves the coverage rows.
  - Lines of `Sandbox*.cpp`, `SeccompFilter.cpp`, `SystemCallRows.cpp`, `Landlock.cpp`, `HostComposition.cpp`, `WamrEngine.cpp`, `AppContainer_win32.cpp` (wine has no gcov: excluded), `HostProcess.cpp` and `HostServer.cpp`.
  - Name the kinds of uncovered lines.
- [ ] **Step 5: Measurement** (release, three runs, medians).
  - Plasma steady/uncached through the sandboxed Wasmtime helper (against phase 4's 8.4 µs).
  - The WAMR helper against in-process WAMR.
  - Round trip; spawn to `RuntimeCreated` (including priming and lockdown).
  - Idle helper VmRSS.
  - Thresholds per D108; record everything in "Phase-5 facts".
- [ ] **Step 6: Windows cross-build + wine**, as Task 5.7 Step 6, on the gate head.
- [ ] **Step 7: The CI checklist** (only with the owner's push), listed as outstanding otherwise:
  - Linux arm64: the `membarrier` row and the aarch64 table, with the contract sandboxed;
  - FreeBSD: Capsicum and the WAMR helper; phase 4's descriptor attribution;
  - macOS: the profile, the signed helper with and without the entitlement; phase 4's `CMSG_SPACE`;
  - Windows: AppContainer, placeholders, cross-view commits.
- [ ] **Step 8: Phase review** (`/code-review` of `009b13c5e..HEAD`); address every finding.
- [ ] **Step 9: Phase summary.**
  - Performance (Step 5).
  - Risk (D90, D92, D95, D98, D109, D110).
  - Coverage.
  - What wine, CI and reading did and did not prove, per platform.

### Phase-5 facts
*(Filled in by 5.2 Step 1, 5.3 Step 1 and 5.8.)*

From 5.1:
- **Wire:** `ProtocolVersion = 4`. `proto::LockdownStep : uint8_t` (Pdu.hpp) `{None, CoreDumps, Dumpable, Tracing,
  FaultReports, NoNewPrivileges, FileSystem, SystemCalls, ThreadSync, ResourceCaps, Capabilities, DescriptorRights,
  Profile, Container, Mitigations}`; `Hello{version, memoryMode, lockdown, failedStep, failureCode}` (`failureCode`
  = errno or the HRESULT's bit pattern, `static_cast<uint32_t>`). Hello's row cap stays 16 (largest body 13). A new
  step is an enumerator, a case in `Codec.cpp`'s `isEnumerator(LockdownStep)` switch and a row in
  `proto/LockdownSteps.cpp` (at its index; `lockdownSteps()`/`lockdownStepText()` in `vtextension_remote_wire`).
  `LockdownSteps.oneRowPerStep` ties table and codec (one past the last row is BadEnum).
- **Seam:** `host/Sandbox.hpp`: `ConfinementFailure{LockdownStep step; int code}` (`==` defaulted),
  `confinementFailureText()` = `"<step text>: <systemErrorText(code)>"`, `SandboxDescriptors{NativeHandle control}`
  (the adopted control end's value), `ISandbox{restrictBeforeThreads(), lockDown(), lockdownState()}` -- the
  third says what Hello states when both succeeded (`NullSandbox`: `Unsandboxed`; a real one: `LockedDown`).
  `makeSystemSandbox()` is defined per system; `host/CMakeLists.txt`'s `_sandbox_source` is `Sandbox_none.cpp` for
  every system today -- a platform task sets it to its own file.
- **Bootstrap:** `HardeningRow{LockdownStep step; apply}` (no text: the table phrases it); `hardenProcess()` and
  `capResources()` return `expected<void, ConfinementFailure>` (caps: `ResourceCaps` + errno, every cap applied
  past a refusal; `Skip` is success).
- **Order** (`runExtensionHost`): bind → `beforeThreads` (harden) → adopt → engine → `confinement.sandbox(
  {control})` → `restrictBeforeThreads()` → `HostProcess::run`: listener thread, then the server: 4 executors,
  each `prepareThread()` then latch count-down; control `prepareThread()`; latch wait → `confine` = caps then
  `lockDown()` → Hello. First failure of hardened/restricted/capped/lockedDown is stated. Tell lines: `hardening
  failed: …`, `sandbox failed: …` (only the sandbox's first failure), `resource caps failed: …`.
- `HostConfinement` is now `{bindToParent, beforeThreads, sandbox, startThread, confine, tell}`; `startThread`
  (an `ExecutorStarter`, now declared in `ExecutorThread.hpp`) starts the listener (`InterruptListenerStackBytes`)
  and every executor (`ExecutorStackBytes`): tests tell them apart by stack size. `InterruptListener::start` takes
  the starter. `HostServerConfig::confine` returns `expected<LockdownState, ConfinementFailure>`.
- **Priming:** `IHostedEngine::prepareThread()`. A lane whose prepare fails returns without serving; the server
  ends `ExecutorUnavailable` (exit 75), confines nothing, sends nothing. Wasmtime: each `WasmtimeEngine` facade
  compiles `(module (func (export "p")))` once (its own `wasm_engine_t`: the facade's trap mode, `Private`
  memory, 64 KiB stack, epoch deadline 1), then per thread a store + `wasmtime_instance_new` + call; a
  `thread_local` holds the serial of the last facade that primed the thread (idempotent per thread and engine).
  `wasmtime::testing::primingCompilations()` counts compiles process-wide. The copying engine primes too (same
  class). `testing/PreparationHookEngine.hpp` wraps an engine with a hook run before each `prepareThread()`.
- The ASan tree skips the Signals section of `WasmtimeEngine.prepareThreadRunsTheGuestPath` (it asserts the
  thread gains a `sigaltstack` it lacked): run it in a release tree once, as 5.2 measures there anyway.
- **Refusal:** `IHostConnector::refused(HostRefusal)` (pure virtual; `HelperIncarnation::connect` calls it beside
  `HostRefusalLog::refused(refusal, failedStep)`). `ProcessHostConnector` keeps `LockdownHistory::Failed` on a
  `LockdownFailed` refusal; later `connect()` → `HostUnavailable`, no launch, no line (the backend's log said it).
  `refusalLine(LockdownFailed, step)` = "The extension host could not confine itself, and was refused: <step
  text>. Extensions do not run." The parent does not phrase `failureCode` (helper-side `systemErrorText`).
- `FakeProcessLauncher(LaunchHook)`: the hook runs at launch with the spec, whose inherited handles are open
  until launch returns -- a test plays the host by duplicating `spec.inherited[0]` and sending on it.
  `ProcessHostConnector.lockdownFailureIsNotRelaunched` lives in `vtextension_remote_test`
  (`process/LockdownRefusal_test.cpp`, added only where `_process_source`), since `vtextension_process_test` does
  not link `HelperIncarnation`. `PduCodec.*`/`LockdownSteps.*` live in `vtextension_test`.
- The hostile host passes `systemConfinement()` with `sandbox` = a `NullSandbox` factory.

From 5.2:
- **Item 0.1 (release tree):** `WasmtimeEngine.prepareThreadRunsTheGuestPath` passes with its Signals section
  (18 assertions: a fresh thread lacks a `sigaltstack` before priming and has one after). The strace of the
  unsandboxed release helper agrees: `rt_sigaction` appears only before lockdown (3 per helper: SEGV/ILL/FPE at
  engine priming), never after. No `rt_sigaction` row; `sigaltstack` after lockdown is thread exit only.
- **Measured allow-list (D93):** strace `-ff` of `vtextension_helper_test "~[.measure]"` in release, ASan, TSan
  and coverage (182/181/181/182 helpers), calls after a temporary `PR_GET_NO_NEW_PRIVS` marker at `lockDown()`.
  Release: shutdown, sendto, recvfrom, munmap, close, brk, recvmsg (fd = control only), madvise, sigaltstack,
  rt_sigprocmask, exit, mmap (never PROT_EXEC; shared only with MAP_FIXED), mprotect (RX/RW/NONE, never W+X),
  fstat, ioctl (`TCGETS2` on fd 2, every CreateRuntime: Wasmtime's isatty -> refused ENOTTY), write,
  exit_group, futex, mremap, rt_sigreturn (Signals traps, SIGILL), poll, restart_syscall, and 2x
  openat+read of `/proc/sys/vm/overcommit_memory` (glibc shrinking a thread arena; refused EACCES, handled).
  ASan adds getpid, gettid, sched_yield (+ LSan's clone/wait4/ptrace/prctl at exit, gone with
  `detect_leaks=0`). TSan adds clock_gettime (12855), gettimeofday, nanosleep. Coverage adds openat/read/lseek/
  fcntl(F_SETLKW)/getpid and `mmap(MAP_SHARED)` unfixed of counter files (2.9 M writes). ftruncate added to the
  coverage rows from GCDAProfiling's source (a counter file new to the tree), not seen.
- **Filter:** `host/linux/{SystemCallRows,SeccompFilter,Landlock}`; numbers per arch are our table
  (`systemCallNumbers()`, one row per `SystemCall`). One row per number per set; a row whose number matches
  decides (checks pass -> action, else its local `ret KILL`), so jumps stay < 256. `ArgumentTest` gained
  `NotEquals` (needed for "shared => fixed" and "not W+X" without two rows per number). mmap forbids
  `PROT_EXEC` outright (stricter than D92: nothing maps executable after lockdown). Int arguments compare the low
  half only. `rowsFor(BuildInstrumentation, arch)` merges; `buildRows(arch)` = `rowsFor(ThisBuildInstrumentation)`.
  aarch64 rows include `membarrier` (only there). `seccompReturnOf()` maps an action to its return value.
- **Landlock:** kernel here reports ABI 10; TSYNC is ABI 8 (kernel docs: "Thread synchronization (ABI < 8)").
  Our table covers 1..10 (RESOLVE_UNIX at 9, UDP at 10 handled too). `LandlockSystem` has four pointers (close
  added, so a fake never closes a real descriptor). Return convention: result or `-errno`.
- **Instrumented builds:** sanitizer rows allow openat (Landlock grants `/proc` read only); coverage rows allow
  openat/fcntl/lseek/ftruncate and shared unfixed mmap (Landlock grants the build tree, `CONTOUR_COVERAGE_OBJECT_ROOT`
  = `CMAKE_BINARY_DIR`, defined only when `CONTOUR_COVERAGE`). So **without Landlock, an instrumented helper can open
  files**; release cannot. Helper environment (`LaunchedHelper.hpp` `sanitizerEnvironment()`): `detect_leaks=0`
  appended to ASAN/LSAN_OPTIONS in sanitizer builds, whether set or not.
- `ThisPlatformSandbox` (Sandbox.hpp, `CONTOUR_PLATFORM_SANDBOX` public on vtextension_host); CMake switch
  `VTEXTENSION_LINUX_SANDBOX` (src/vtextension/CMakeLists.txt: Linux on x86_64/aarch64). Compositions:
  `HelperLockdown` (HelperComposition.hpp) = RequireLockedDown where a sandbox exists.
- **Probe:** `--receive-grant [--sandboxed] [--outer-filter=no-landlock|flatpak|passrights|thread]
  [--sandbox-op=…]` (`process/testing/SandboxProbe.cpp`); lockdown report on lane 0, op report on lane 1
  (one lane raced: `readReport` swallowed the second report in the coverage tree). The parent reads `/proc/<pid>/task/*/status`
  (Landlock forbids the probe opening it after restrict). A killed op: the second report never comes, exit
  `Signalled SIGSYS`.
- `Sandbox.systemWithoutOneRunsUnsandboxed` (remote_test) SKIPs where a sandbox exists: it locked down the
  test binary itself (SIGSYS later). Never call `makeSystemSandbox()`'s steps in a test process.
- **Coverage tree:** its `.gcda` files were corrupt (helpers killed mid-write); the profile runtime then
  segfaults at exit in every process. `find out/clang-coverage -name '*.gcda' -delete` before a coverage run.

## Review Focus addendum (phase 5)
11. **A syscall taken only on a rare path kills a helper in the field.** Examples: the first trap on a thread, growth, a large compile, an aarch64 icache flush, a sanitizer report. Pinned by:
    - the measured allow-list (5.2 Step 1, re-audited at the gate);
    - the release-tree helper suites under `RequireLockedDown`;
    - `"HostServer.everyThreadIsPreparedBeforeConfinement"`;
    - `uncleanExits == 0`, which counts a `SIGSYS`.

    Review rule: every row says why, and no row allows a call from `"SeccompFilter.noRowSetAllowsTheForbidden"`.
12. **A thread outside the sandbox.** A thread started before no_new_privs/Landlock, or a TSYNC that did not reach one. Pinned by `"HostProcess.locksDownInTheSandboxOrder"`, `"Sandbox.landlockCoversEveryThread"` and `"Sandbox.threadSyncFailureFailsLockdown"`.
13. **Systems without Landlock, or inside an outer filter (Flatpak, old kernels).** The helper must lock down by seccomp alone or say `Failed` with its step, never run silently unsandboxed. The parent must still launch. Pinned by `"Sandbox.locksDownWithoutLandlock"`, `"Sandbox.locksDownUnderAFlatpakLikeFilter"` and `"Channel.refusingDescriptorsMayBeUnavailable"`.
14. **A lockdown failure loop.** One failing system must not spawn a helper per load. Pinned by `"ProcessHostConnector.lockdownFailureIsNotRelaunched"`.
15. **The GUI growing an engine back.** Pinned by the `nm` check (5.4 Step 5) until Task 6.2's lint rule replaces it.
16. **Platforms flipped on evidence we do not have.** macOS, FreeBSD and Windows sandboxes are proven by CI only (D110).

## Task order
5.1 → 5.2 → 5.3 → 5.4 → 5.5, 5.6, 5.7 → 5.8.
- 5.3 depends only on 5.1 and may run beside 5.2 (disjoint directories; both touch `remote/CMakeLists.txt`, so they merge in order).
- 5.5, 5.6 and 5.7 are independent of each other. 5.7 goes first of the three because wine gives it local feedback.
- 5.4 needs 5.2 (a sandbox to require) and 5.3 (a helper without Wasmtime).

## Phase-5 decisions

Proposed for a controller ruling. Each names the choice and, after the dash, why.

- D85. Eight tasks: 5.1 seam and order → 5.2 Linux → 5.3 WAMR helper → 5.4 Linux default → 5.5 FreeBSD, 5.6 macOS, 5.7 Windows → 5.8 gate. The outline's 5.1 splits into the seam (5.1) and Linux (5.2), and WAMR moves before the default switch. — The order, priming and refusal path are platform-free and testable with fakes. A Linux build without Wasmtime must have a helper the moment the default flips.
- D86. `host::ISandbox` has two steps: `restrictBeforeThreads()` (what threads inherit: Linux no_new_privs + Landlock; FreeBSD `PROC_NO_NEW_PRIVS`) and `lockDown()` (after every thread is prepared: seccomp TSYNC, `cap_enter`, Seatbelt, the Windows verification).
  - Made by `makeSystemSandbox(SandboxDescriptors)`.
  - `NullSandbox` → `Unsandboxed`.
  - Every step runs, and the first failure is stated, as D66 does for hardening.

  — Spec §5.6's order has two points, and only one of them is per thread.
- D87. One step vocabulary: `HardeningStep` folds into `proto::LockdownStep`, and `HardeningFailure` becomes `ConfinementFailure`. `runExtensionHost` order: bind → harden → adopt → engine → restrict → listener + executors → prepare → caps → lockDown → Hello. Caps run before seccomp. — One table phrases every confinement failure. Caps first means `setrlimit` needs no row.
- D88. Priming is `IHostedEngine::prepareThread()`, on every executor and on the control thread (which runs start functions, R29), behind a latch before `confine`. A failure → `ExecutorUnavailable`.
  - Wasmtime instantiates and calls a built-in one-function module. That installs its lazily-installed handlers and the thread's sigaltstack, so `rt_sigaction` needs no row.
  - WAMR initializes its thread environment.
  - The listener thread runs no guest, so it is not primed.

  — Phase 4 measured lazy handler installation; priming makes the post-lockdown syscall set deterministic.
- D89. Hello states the failed step and code (`failedStep`, `failureCode`): `ProtocolVersion = 4`. The parent's `LockdownFailed` line names the step from the shared table. — Users on unusual kernels and in Flatpak get a reason. Release helpers' stderr reaches nobody.
- D90. **Deviation, needs a ruling.** A `LockdownFailed` refusal is sticky in its `ProcessHostConnector`: later connects fail `HostUnavailable` without launching, and the breaker counts faults as usual. It is not "open the breaker at once" (spec §5.3; D52 assigned it here). — A lockdown failure is a property of the system and build, not of an extension. This needs no new error across the seam. Cost: it stays refused until the backend is recreated (restart), even if the cause was transient (ENOMEM).
- D91. Seccomp is hand-assembled classic BPF, with no libseccomp.
  - Arch check (x86_64, aarch64; anything else and the x32 bit → kill).
  - Rows from three tables (release, sanitizer, coverage) plus `SandboxDescriptors`, in frequency order.
  - Default `SECCOMP_RET_KILL_PROCESS`.
  - Tested through a test-only BPF evaluator.

  — No new dependency, and argument checks become unit-testable without killing a process.
- D92. Argument policy:
  - recvmsg only on the control descriptor;
  - sendmsg/sendmmsg denied entirely (the helper sends with `send`; R38);
  - socket/socketpair → `EPERM`; open/openat/openat2 → `EACCES` (soft refusal for library probes; Landlock is the second layer);
  - connect/bind/accept/listen killed;
  - mmap never `W|X`; `MAP_SHARED` only with `MAP_FIXED` and without `PROT_EXEC`; mprotect never `W|X` (W^X enforced);
  - clone*/fork/vfork/exec*/ptrace/process_vm_* killed;
  - kill/tgkill only on the own tgid;
  - read/write/poll are **not** descriptor-filtered.

  — After lockdown the helper can acquire no descriptor except those the parent grants, so a descriptor filter adds rows without adding a boundary.
- D93. The allow-list is measured, not guessed: strace of the unsandboxed helper across the helper suites in all four trees, from the no_new_privs marker on. It is re-audited at the gate against kernel seccomp audit records. — The engine's rare paths (traps, growth, compile) are what a guessed list misses.
- D94. Landlock:
  - uses the best ABI from our own constant table;
  - handles every FS right of that ABI with no rules (deny all);
  - denies TCP bind/connect (ABI ≥ 4) and scopes abstract-unix and signals (ABI ≥ 6);
  - uses `LANDLOCK_RESTRICT_SELF_TSYNC` where the ABI has it.
  - Unavailable (`ENOSYS`/`EOPNOTSUPP`) is **not** a failure: seccomp alone confines, and it is told once. Other errors → `FileSystem`.

  — Fedora, Ubuntu and Debian kernels differ, and Flatpak or an LSM list may lack it. Failing every extension there would push users to in-process builds.
- D95. Flatpak:
  - nested seccomp under bwrap's filter is fine (no_new_privs is already set);
  - Landlock follows D94;
  - the parent's `SO_PASSRIGHTS` treats `EPERM`/`EACCES` like `ENOPROTOOPT`/`EINVAL` (Discarded, not `Io`), because the helper's own filter now denies upward descriptors;
  - an outer filter is emulated by the probe.
  - A real in-Flatpak run is outstanding for Task 6.3: a host-built test binary does not run in the Platform runtime, and only Platform (no Sdk) is installed here.

  — This closes the 4.7 phase-input minor. The emulation covers the two mechanisms that differ.
- D96. Instrumented builds:
  - sanitizer helpers get `detect_leaks=0` (LSan's stop-the-world needs ptrace) and a Landlock read rule on `/proc`, so reports are unsymbolized (module+offset);
  - coverage helpers open the build tree's object root before Landlock and get a write rule plus the coverage seccomp rows;
  - D57's leak half moves to the in-process `HostServer` tests, as spec §5.6 says.

  — Every widening is compile-time and lives in its own row set.
- D97. The hostile host stays unsandboxed (`NullSandbox`, `AcceptUnsandboxed`). — Its relay forwards descriptors with sendmsg, and its rows test Contour's defences, not the sandbox's.
- D98. **Deviation, flag.** WAMR in the helper keeps WAMR's own WASI (`LIBC_WASI`), not our shim. One null device is opened when the engine is made (before lockdown) and shared by every runtime. The memory mode is Copy unless the 5.3 spike finds an in-place linear-memory hook that a system WAMR also exposes. — WAMR creates its WASI context whenever a module imports WASI, whatever resolves the imports, so a descriptor is needed either way. Contract equality across both engines is the proof that behaviour matches.
- D99. The helper links the engine `CONTOUR_WASM_RUNTIME` names. Where Wasmtime is also present, a test-only `contour-extension-host-wamr` and `vtextension_wamr_helper_test` are built. — The FreeBSD/RPM helper is then proven sandboxed on this Linux box, not only in CI.
- D100. CMake:
  - `CONTOUR_EXTENSION_HOST` takes `out-of-process` or `in-process`. The default is `out-of-process` iff `CMAKE_SYSTEM_NAME` is in `_CONTOUR_SANDBOXED_PLATFORMS`; Linux is added in 5.4, and the others by their tasks.
  - `CONTOUR_WASM_RUNTIME` now names the engine: default `wasmtime` if one resolved and out-of-process, else `wamr`. `in-process` + `wasmtime` → FATAL_ERROR.
  - Only programs link `vtextension_runtime`; test binaries name their composition (`vtextension_test` links `vtextension_wamr`).

  — One table row per platform flip, and tests do not silently start spawning helpers.
- D101. `process::HostComposition` (system launcher, supervisor, kind, connector) is shared by every default backend in a process through a weak-cached `sharedHostComposition()`. The supervisor ends with the last backend, not at static destruction. Teardown grace follows `ThisBuildInstrumentation`. — One supervisor per hosting process (spec §5.3), with no `jthread` joined during exit.
- D102. In the build tree, a POST_BUILD copy places the helper beside `contour` and `bench-headless`, and `locateExtensionHost`'s "beside" candidate finds it. No build-tree path is compiled into a binary. — Developer runs work, and installed binaries never consult a path outside their install.
- D103. R37 stands: the lease is the object size. — Seccomp cannot tell regions apart, so a compromised helper may `mprotect` its own `PROT_NONE` tail; leasing the initial size would undercount what it can dirty.
- D104. FreeBSD:
  - Capsicum rights per descriptor role (a table), no ioctls, fcntl `GETFL` only, `cap_enter` in `lockDown`;
  - `RLIMIT_AS = dataBytes` (closes D38's open FreeBSD cap; WAMR Copy reserves nothing extra);
  - `PROC_NO_NEW_PRIVS_CTL` where defined.
  - Proven by CI only. Capsicum cannot refuse `SCM_RIGHTS` on a socket with `SEND`; D70/R38 cover the parent.
- D105. macOS:
  - a constexpr deny-default profile with parameters, shm read/write-data by prefix only (R38), no network, mach-lookup, file reads, fork or exec;
  - hand-declared `sandbox_init_with_parameters`;
  - `allow-unsigned-executable-memory` on the helper only, through `support/macOS/contour-extension-host.entitlements`, with the helper signed before the bundle;
  - a CI case with and without the entitlement.
  - No parent-death binding remains (documented, spec §5.3).
- D106. Windows: the **parent** confines at launch, and the helper verifies.
  - Launch: an AppContainer profile `Contour.ExtensionHost` with no capabilities, plus `CHILD_PROCESS_POLICY` restricted, on top of the existing job.
  - The profile and an idempotent `ALL APPLICATION PACKAGES` RX ACE on the helper's directory are ensured once per supervisor, on its thread, through an injectable API table.
  - Helper: not an AppContainer token → `Unsandboxed`. Otherwise it applies the mitigation rows and checks the job; a failure → `Failed`; all pass → `LockedDown`.
  - Dynamic-code policy stays off.

  — An AppContainer can only be entered at creation. Distinguishing "not contained" from "failed" keeps wine and test runs meaningful.
- D107. Under wine (`ntdll!wine_get_version`), test compositions use `ProcessIsolation::None` and `AcceptUnsandboxed`, and AppContainer cases SKIP with a reason. — Wine stubs AppContainer, so a Failed-everywhere run would hide every other Windows regression.
- D108. Gate thresholds:
  - steady render through the sandboxed helper: stop and report if the median is above phase 4's range (8.3–9.2 µs) beyond noise (> 9.5 µs) — filter order is the lever;
  - spawn to `RuntimeCreated`: stop above 2× phase 4 (2.6 ms) — priming is the lever;
  - the WAMR helper: reported against in-process WAMR, never gated (spec §10.2: "as isolated, but not faster").
- D109. **Residual risks, for the owner.**
  - (a) `newfstatat`/`statx` with `AT_EMPTY_PATH` must be allowed for `fstat`, and a path given with it still resolves: file metadata (not contents) stays observable. Landlock does not cover stat.
  - (b) Where Landlock is absent, files are guarded by seccomp alone.
  - (c) macOS has no parent-death binding.
  - (d) FreeBSD threads are not bounded by `RLIMIT_NPROC` (it counts processes).

  Proposed: accept and document in Task 6.4.
- D110. **Needs the owner.** macOS, FreeBSD and Windows join `_CONTOUR_SANDBOXED_PLATFORMS` in their tasks, but the gate passes only if their CI legs ran green, which needs a push. Without it, their rows are left out at the gate (those platforms stay in-process), and the code lands dormant. — Flipping a platform's default on reading alone risks every extension failing there.

Rulings on D85–D110 (R39–R43), applied by the controller:
- R39. D85–D110 are accepted, except as changed below.
- R40 (D90). Accepted: a `LockdownFailed` refusal is sticky per `ProcessHostConnector` and clears only when the backend is recreated. Why: a lockdown failure is a property of the system and build; relaunching per extension would spawn a doomed helper per pane. Cost if wrong: after a transient failure (ENOMEM) extensions stay off until restart; the Hello's named step (D89) makes that diagnosable.
- R41 (D98). Accepted, with the 5.3 spike time-boxed to one hour of investigation; if no in-place hook that a system WAMR also exposes is found, Copy is final for the WAMR helper in this branch. Cost: the WAMR helper pays the copy path (phase 4 measured 18.7 µs vs 8.4 µs steady), which spec §10.2 already allows ("as isolated, but not faster").
- R42 (D109). Residual risks (a)–(d) are accepted provisionally and documented in 6.4; they are surfaced to the owner, who may overrule.
- R43 (D110). Accepted. Without the owner's permission to push, macOS, FreeBSD and Windows stay in-process at the gate and their sandbox code lands dormant (compiled where CI would compile it, tested here as far as wine/fakes allow). Only Linux flips by default.

---

## Phase 6 — Cache, packaging, documentation, gates

### Task 6.0: Expand this phase to step level (waits on the Phase 5 gate)

### Task 6.1: Compile cache
The parent owns it. Key: module SHA-256 + Wasmtime version + engine config + CPU features. Writes are
atomic, files user-only, LRU-capped at 256 MiB. Artifacts are deserialized inside the sandbox.

### Task 6.2: Lint rules
`check_wasm_runtime_containment` gains two rules: `wasm/wasmtime/` and `wasm/host/` are engine
directories, and the GUI and daemon targets link no engine.

### Task 6.3: Packaging
| Package | Engine |
|---|---|
| DEB, AppImage, DMG, MSI/zip | Wasmtime |
| Flatpak manifest | Wasmtime prebuilt, pinned as a source (fallback: `flatpak-cargo-generator`) |
| RPMs `contour`, `contour-daemon` | WAMR |
| FreeBSD | WAMR |

Every package installs the helper. Packaging changes are separate commits.

### Task 6.4: Documentation
`docs/extensions.md`: "The extension host process", its sandbox, and performance, without naming the
engine. `docs/internals/extension-host.md`: rationale and protocol. A `metainfo.xml` release note.

### Task 6.5: Measurement gates (spec §9)
1. ≥ 5× on orchestrator `update(procs)`, measured on a throwaway local rebase of
   `feature/agent-orchestrator`.
2. Whole-callback IPC overhead ≤ 10 %.
3. No status-line throughput regression, and idle wake-ups unchanged.
4. RSS at 1 and 10 panes, explained.
5. Cold compile and warm load per bundled extension.
6. The stripped helper's size, per platform.

The same throwaway rebase moves `DaemonExtensionRuntime`'s module load off the daemon owner thread (spec
§5.9 item 6). That change lands with the orchestrator branch, not here.

The numbers go into the PR description.

### Task 6.6: Final `/code-review`, PR, CI green
