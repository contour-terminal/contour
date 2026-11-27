# Task 2.2 report: `IHostedEngine` and `HostServer`

**Status:** DONE. Four commits, nothing pushed.

| Commit | Subject |
|---|---|
| `e77776254` | fix(vtextension): refuse a PDU over its byte cap at the sender |
| `b9ac67ab6` | test(vtextension): a pane's extensions load under their host id |
| `baa7df1ba` | refactor(vtextension): the Wasmtime backend becomes an engine a host process drives |
| `b0160e5b7` | feat(vtextension): serve extension modules over channels from a host |

## Item 0 (carry-ins), commits e77776254 and b9ac67ab6

- **a.** `PduStream::send` encodes the PDU, then checks the body (the frame length less the kind's byte, read back with
  `proto::Reader`) against `PduTable[kind - 1].sizeCap(_limits)`. If it is over, it returns `StreamError::OverCap`
  and sends nothing. The docs on `send` and `StreamError::OverCap` are updated. New test
  `PduStream.overByteCapIsNotSent`: under limits `{64, 64}`, a 256-byte `Load` gets `OverCap`, and a `Release`
  sent after it still arrives.
- **b.** The `inFlight` doc in `Channel_win32.cpp` now says it also counts every `shutdown()`, from before its
  state change until after its cancel.
- **c.** `_codec_sources` in `wasm/remote/CMakeLists.txt` is now used by both `vtextension_remote_wire` and
  `vtextension_remote_fuzz`. The comment is updated.
- **d.** New test `ExtensionHost.loadsUnderTheHostId`: it runs `prepareExtensions` against a `FakeWasmBackend`
  and checks `loads() == 1` and `lastIdentity() == ModuleIdentity{"minimal@test"}`.
- **e.** `ExtensionConsent_test.cpp` and `ExtensionError_test.cpp` are now in alphabetical order in
  `src/vtextension/CMakeLists.txt`.

## Step 1: rename, commit baa7df1ba

- `git mv` renamed `WasmtimeBackend.{hpp,cpp}` to `WasmtimeEngine.{hpp,cpp}`, and `WasmtimeBackend_test.cpp` to
  `WasmtimeEngine_test.cpp`.
- `WasmtimeEngine` now derives from `host::IHostedEngine`, and `memoryMode()` returns `MemoryMode::Copy`.
- `makeWasmtimeBackend` is now `makeWasmtimeEngine`, returning `unique_ptr<host::IHostedEngine>`.
- Updated to match: `WasmtimeRuntime.hpp`'s include, the CMake source lists, the test traits, and the test-case
  names (`WasmtimeBackend.*` → `WasmtimeEngine.*`).
- New test `WasmtimeEngine.copiesMemoryToTheParent`.
- The containment script's path is updated, in its self-test sample.
- New file `src/vtextension/wasm/host/IHostedEngine.hpp`, header-only. It names only `WasmRuntime.hpp` and
  `proto/Pdu.hpp`.

## Steps 2–6: server, commit b0160e5b7

### Files

- **`wasm/remote/HostConnector.hpp`** (interfaces only): `MaxLaneChannels = 4`, `IHostHandle`, `HostConnection`
  and `IHostConnector`, as briefed and with Doxygen docs.
- **`wasm/host/HostServer.{hpp,cpp}`**: `HostServerConfig`, `ServerExit` and `HostServer`, which uses a pimpl
  (`Impl`) and is non-copyable and non-movable.
- **`wasm/host/InProcessHost.{hpp,cpp}`**: `EngineFactory`, `InProcessHost : remote::IHostHandle` (members:
  registry, state, engine, `HostServer`, `jthread`, in that order) and `InProcessHostConnector`.
- **`wasm/host/CMakeLists.txt`**: `vtextension_host` (static). It links `vtextension_remote_wire` PUBLIC and
  `core::base` PRIVATE (for `core::views::enumerate`), and no engine.
- **`wasm/testing/HostedEngine.hpp`** and **`wasm/wasmtime/HostedEngine.cpp`**: a neutral test probe,
  `makeHostedEngine()`. It wraps `makeWasmtimeEngine` over one static kind state with `TrapMode::Software`. See
  Deviation 1.
- **`wasm/remote/CMakeLists.txt`**: `vtextension_remote_test`, built `if(VTEXTENSION_TESTING AND TARGET
  vtextension_wasmtime)`. It links exactly the briefed list and is registered with `add_test`.
- **`src/vtextension/CMakeLists.txt`**: the backend directories (`wasm/${CONTOUR_WASM_RUNTIME}`, then
  `wasm/wasmtime`) now come before `wasm/remote` and the new `wasm/host`.

### Design

- **Maps.** Runtime, module and instance ids map to `shared_ptr` slots under one mutex, which is never held across
  I/O.
  - `RuntimeSlot` keeps its own copy of the `WireHostFunction` strings.
  - Each `ModuleSlot` holds its `RuntimeSlot`, and each `InstanceSlot` holds its `ModuleSlot`. The engine object
    is declared after its parent slot, so it is destroyed first, and the strings outlive everything created from
    the runtime.
  - Per-kind sets of every id ever created live on the control thread only. An id that was never created, or is
    reused, is `ProtocolViolation`.
- **Control thread (`run()`).**
  1. It sends `Hello{ProtocolVersion, engine.memoryMode(), config.lockdown}`.
  2. It starts one `jthread` per lane.
  3. It serves `CreateRuntime`, `Load`, `Instantiate` and `Release` through overloaded `serve()` functions, with
     a template catch-all that returns a violation.
  - `Load` and `Instantiate` first `prepare()` the control thread's guard. If that fails, the reply carries the
    engine's error.
  - `Instantiate`:
    - `callableExports` holds the ordinals of the function exports that `instance->function(name)` resolves and
      whose parameter and result counts are each ≤ `MaxCallValues` (see Deviation 4).
    - The instance is put in the map *before* `Instantiated` is sent, because the parent may call it on a lane
      the moment the reply arrives.
    - If the server has already finished when the instance goes in, the instance is interrupted at once.
- **Over-cap replies (binding note).** `sendOrRefuse(stream, pdu, refusal)`:
  - If `send` returns `OverCap` (element or byte cap), it sends `refusal` instead: `Loaded{error =
    InvalidComponent}`, `Instantiated{error = InvalidComponent}`, or `MemoryData{OutOfBounds}`.
  - The refused module or instance is dropped, and its id stays used.
  - Any other send failure ends the connection.
- **Lane executor.**
  - It loops on `receive()` and dispatches `Call`, `MemoryRead` and `MemoryWrite`. Anything else, including a
    `HostResult` with no host call waiting for it, is a violation.
  - An `InstanceHold` takes the instance with a CAS on `atomic<uint32_t> lane` (lane index + 1). The hold is
    re-entrant on the same lane, so calls back in work, and refused on another lane, which is a violation. It is
    released *before* `CallResult` is sent.
  - A `Call` (re-)enters the engine's guard when `!isCurrentThreadPrepared()`. The guard lives in the `Lane` for
    the thread's lifetime and is reset on that thread when the loop ends.
  - `thread_local Lane* CurrentLane` is set for the call's duration, and the outer value is restored afterwards.
  - The argument count must equal the export's parameter count. `argv` is sized to `max(params, results)` and
    trimmed to the result count afterwards.
- **`LaneHostContext::callHost`** (one context serves every instance):
  - With no current lane it returns `RuntimeUnavailable`. That is what fails a start function's host call (D11).
  - Otherwise `awaitHost` sends `CallHost` and then serves the lane — memory accesses and nested `Call`s, by
    recursion — until `HostResult` arrives.
  - Any stream failure or violation along the way records `lane.ended` and fails the host call. That unwinds the
    guest, and the lane's reason then ends the connection.
- **MemoryWrite out of bounds.** As `Pdu.hpp` documents, it "fails the call it belongs to": a lane flag fails
  either the next `Call` (`CallResult{Trap}`, without entering the guest) or the `HostResult` being waited for
  (the guest's call then fails `HostFailure`).
- **Teardown.**
  - `finish(reason)` records the first reason, interrupts every live instance under the mutex, then shuts every
    stream down. `stop()` is `finish(Stopped)`.
  - `finish` is idempotent, thread-safe and noexcept. Every way out of `run()` and of an executor goes through it.
  - `run()` then clears (joins) the executors, moves the three maps out under the lock, and destroys instances,
    modules and runtimes outside it, while the control guard is still held. It resets the guard last.
  - `Release{Instance}` also interrupts the instance it releases (Deviation 5).
- **`ServerExit` mapping.** `Closed` and `Io` → `PeerClosed`; `Malformed` and `OverCap` → `ProtocolViolation`.
  The first reason recorded wins.
- **InProcessHost** (D17):
  - The constructor starts the `jthread` running `run()`, which sets the state to `Ended` when it returns, and
    registers the host with the connector's shared `InProcessHostRegistry`.
  - `terminate()` sets the state to `Ended` and calls `server.stop()`. `interrupt()` calls `server.interrupt()`.
  - The destructor unregisters, terminates and joins.
  - Connector: `connects()` counts successful `connect()` calls. `liveHosts()` counts registered hosts still
    `Serving`. `terminateAll()` terminates those and returns how many. A null engine or a channel failure is
    `HostUnavailable`.

## Tests (TDD)

- **RED.**
  - Item a: `vtextension_test "PduStream.*"` gave `test cases: 6 | 5 passed | 1 failed`, `assertions: 26 | 24
    passed | 2 failed`.
  - Item d, mutation check: with `ExtensionHost.cpp` passing `ModuleIdentity {}`, the new case failed with
    `assertions: 4 | 3 passed | 1 failed`. I restored the file afterwards.
  - Step 3: `vtextension_remote_test` compiled, then failed to link, with undefined references to
    `HostServer::run/stop/interrupt` and `InProcessHostConnector::*`.
- **Behavioural mutation checks on the server.** I restored the file after each.
  1. `sendOrRefuse` was made to fail on `OverCap`. `HostServer.describingPastTheCapFailsTheLoad` then failed: the
     connection tore down, with `FAILED` at the fixture's receive.
  2. The interrupt loop was removed from `finish()`. `HostServer.stopInterruptsARunningGuest` then hung until
     `timeout 20` killed it (rc 124); `run()` never returned.
- **GREEN.** `out/clang-asan/src/vtextension/wasm/remote/vtextension_remote_test` → **All tests passed (429
  assertions in 18 test cases)**. It also passed 20 of 20 runs with `--order rand`.
  - The brief's 9 cases, by name: `HostServer.helloComesFirst`, `.loadDescribes`, `.callRoundTrip`,
    `.callHostNests`, `.memoryOpsOnTheLane`, `.malformedFrameEndsTheConnection`, `.stopInterruptsARunningGuest`,
    `.releaseDropsTheInstance`, `.hostCallDuringInstantiateFails`.
  - Extra server cases:
    - `.hostFailureFailsTheCall`.
    - `.peerCloseEndsTheServer`.
    - `.interruptStopsOneInstance`, which also covers an ignored unknown id and the other instance running on.
    - `.reusedIdIsAViolation`.
    - `.describingPastTheCapFailsTheLoad`, the binding-note test: limits `{4096, 1 MiB}` cap a Loaded at 16 KiB.
      A ~1.2 KB module with 20 imports of 1000 i32 parameters each describes as ~20 KB, gets
      `Loaded{1, InvalidComponent}`, and the stream then loads and instantiates another module.
  - `memoryOpsOnTheLane` has 4 sections: idle, parked in a host call (including a read-back of the guest's store
    after `HostResult`), out-of-bounds read, and out-of-bounds write failing the next call only.
  - `callHostNests` sends a nested `Call` to a second instance's `add(2, 3)` while parked in `pair(7, 9)`'s
    `CallHost{1, {7, 9}}`. It receives `CallResult{{5}}` first, then the outer `CallResult{}`.
  - `releaseDropsTheInstance` fences the unanswered `Release` with an answered `CreateRuntime` before the lane's
    `Call`, because control and lanes are separate threads.
  - `InProcessHost.*` has 4 cases: `connectStartsAServingHost`, `droppingTheHandleStopsTheHost`,
    `handleInterrupts` and `noEngineIsHostUnavailable`.
- **Full suites.**
  - `ctest --preset=clang-asan -R vtextension` → 100% tests passed, 0 tests failed out of 3:
    - `vtextension_test` (12025 assertions in 354 test cases, run directly),
    - `vtextension_wasmtime_test` (1005 assertions in 52 test cases),
    - `vtextension_remote_test`.
  - `ctest --preset=clang-asan -L lint` → 100% tests passed, 0 tests failed out of 33. This includes the
    containment check; `--self-test` → 31 cases OK.
  - `vtextension_wasmtime_test` (the `[wasm-contract]` suite included) passed after the rename, before its commit.
- **ASan/LSan.** `ASAN_OPTIONS=detect_leaks=1 vtextension_remote_test` → all passed, with no leak report and exit
  0.
- **TSan.** I built only `vtextension_remote_test` in `out/clang-tsan` (`cmake --build out/clang-tsan -j 4`, niced,
  under the build lock). Five runs with `--order rand` each passed all 18 cases with exit 0 and no `WARNING:
  ThreadSanitizer`.

## Static checks

- **clang-tidy, by hand:** `clang-tidy -p out/clang-asan --quiet src/vtextension/wasm/host/HostServer.cpp
  src/vtextension/wasm/host/InProcessHost.cpp src/vtextension/wasm/host/HostServer_test.cpp
  src/vtextension/wasm/wasmtime/HostedEngine.cpp` → no findings. The first run had 6 findings, all fixed:
  - the static member's case → `CurrentLane` (`ClassMemberCase: CamelCase`),
  - a `Visit&&` that was never forwarded,
  - two use-after-move reports from Catch2's double expansion of `REQUIRE(send(std::move(x)))` (hoisted into
    `sendControl()`),
  - one int multiplication widening.
- Also clean: `clang-tidy -p out/clang-asan --quiet src/vtextension/wasm/remote/transport/PduStream.cpp
  .../PduStream_test.cpp src/vtextension/ExtensionHost_test.cpp`, and
  `src/vtextension/wasm/wasmtime/WasmtimeEngine.cpp .../WasmtimeEngine_test.cpp`.
- **Build:** `-Werror` clean.
- `/usr/bin/clang-format --dry-run -Werror` and `editorconfig-checker` pass on every changed file.
- **Windows:** `clang-cl --target=x86_64-pc-windows-msvc /winsysroot /opt/msvc /std:c++latest /EHsc /Zs /W4` gave 0
  diagnostics on `HostServer.cpp`, `InProcessHost.cpp` and `PduStream.cpp`. Nothing ran on Windows.

## Deviations from the brief, and why

1. **The tests do not call `makeWasmtimeEngine` directly.** The containment lint's rule 3 lets no file outside
   `wasm/wasmtime/` include a header from it, tests included. `HostServer_test.cpp` lives in `wasm/host/`, so it
   uses a neutral probe instead: `wasm/testing/HostedEngine.hpp`, defined in `wasm/wasmtime/HostedEngine.cpp`.
   This is the pattern `PreparedGeneration` already follows. The probe's engine is exactly
   `makeWasmtimeEngine(kind, {TrapMode::Software})`.
2. **`InProcessHost` is a public class in `InProcessHost.hpp`, beside the connector**, and
   `InProcessHostRegistry` is forward-declared there. The connector's registry has to name the host, and keeping
   the host in an anonymous namespace of the .cpp would give a named-namespace type an internal-linkage member.
3. **Extra cases, beyond the brief's names:** listed under Tests.
4. **Exports that take or return more than `MaxCallValues` i32s are not offered as callable.** Their `CallResult`
   could never be sent, so leaving them out keeps the `CallResult` send from ever hitting `OverCap`.
5. **`Release{Instance}` interrupts the instance it drops.** Without this, a parent that releases an instance
   while a lane still runs a call on it would leave a guest spinning outside the map, beyond `stop()`'s reach, and
   `run()` could never join that executor.
6. **Out-of-bounds `MemoryWrite`** follows the semantics `Pdu.hpp` documents (it fails the call it belongs to; see
   Design), not a protocol violation.
7. **The over-cap test uses small limits** (16 KiB Loaded cap, 20 wide imports). The binding note's ~70 MB
   example would test the same code path at 70 MB of allocation, under ASan.

## Concerns, and notes for later tasks

- **`CreateRuntime` carries no `tableElements`.** The helper creates every runtime with `RuntimeLimits {}`'s
  `MaxTableElements`, so a parent's lower table cap (the contract uses 64) is lost across the wire. 2.3 should add
  the field to the PDU (the protocol is unreleased), or explicitly accept the gap.
- **A start function that never returns cannot be stopped.** It runs inside `instantiate()` on the control thread,
  before any `IWasmInstance` exists to interrupt, so `stop()` cannot end `run()`, and `~InProcessHost` would join
  forever. The process backstop (phase 3, kill) is the real answer. In-process, it is a hang.
- **`liveHosts()` counts registered hosts still serving.** A host stops counting at `terminate()`, or when its
  `run()` returns. 2.3 may want different semantics; the definition is documented at the declaration.
- **The fixture's blocking receives have no deadline.** A server regression that never answers hangs the test
  until ctest's timeout. The stop cases are bounded with `future::wait_for(1 s)`.
- **Windows:** syntax-checked only.

## Performance, risk, coverage

- **Performance:** no existing path changed, apart from one table lookup and a varint read per `PduStream::send`.
  The server is new and nothing in production links it yet.
- **Risk:** low for existing code (a rename, plus one extra check in `send`). The new server is moderately complex
  (threads, re-entrancy, teardown), and it is covered by 18 cases under ASan/LSan and TSan.
- **Coverage:** every control and lane PDU kind is served in a test, as are every `ServerExit` value and both
  refusal paths for over-cap replies (Loaded is tested; Instantiated takes the same code path, untested). I did
  not run a coverage build.

---

## Fix round 1 (review: Needs fixes)

**Commit:** `4cc49cc4b fix(vtextension): bound a lane's nested calls by the stack its thread has`

### Changes

1. **(Important) Nesting depth and executor stack.**
   - **(a) Depth cap.** `MaxCallDepth = 8`, public in `HostServer.hpp`.
     - `Lane::calls` is a stack of `InstanceSlot*` with one entry per guest call on the lane. It replaces the old
       per-lane write flag and also gives `awaitHost` the parked call's instance.
     - A `Call` when `calls.size() >= MaxCallDepth` is `ProtocolViolation`.
   - **(b) Executor stack size.** New `host/ExecutorThread.{hpp,cpp}`: a move-only joining thread whose stack
     size is chosen.
     - POSIX: `pthread_attr_setstacksize(max(size, PTHREAD_STACK_MIN))` and `pthread_create`.
     - Windows: `CreateThread(dwStackSize, STACK_SIZE_PARAM_IS_A_RESERVATION)`.
     - If the thread cannot start, it throws `std::system_error`, as `std::thread` does.
     - Executors run with `ExecutorStackBytes = MaxCallDepth × (MaxRuntimeStackBytes + HostFrameBytes)`, which is
       8 × (1 MiB + 256 KiB) = 10 MiB. This is reserved address space; pages are committed only as deep as the
       calls actually go.
   - **(c) Stack refusal.** `CreateRuntime` with `stackBytes > MaxRuntimeStackBytes` (1 MiB) gets
     `RuntimeCreated{RuntimeUnavailable}` before the engine sees it.
     - A `static_assert` holds `MaxRuntimeStackBytes >= ExtensionLimits{}.stackBytes` (128 KiB).
2. **(Important) Busy-instance test.** New `HostServer.busyInstanceIsRefusedOnAnotherLane`.
   - Lane 0 is parked in `pair`'s `CallHost`; then lane 1 sends a `Call` (section 1) or a `MemoryRead` (section
     2) for the same instance.
   - Expected: `ProtocolViolation`, and lane 1 reads `Closed`.
3. **(Minor) Exception-safe teardown.** `run()` now holds two `core::Finally` guards around the executor vector.
   - Destruction order, on every way out (exceptions included): `finish(ProtocolViolation)`, which shuts every
     stream; then the executors are joined; then `release()`.
   - `release()` drops every instance, module and runtime, then `_controlGuard`, on the control thread.
   - On the normal path, the guards are run explicitly in that same order.
4. **(Minor) Instantiated refusal interrupts.** The refusal path interrupts the instance under the lock before it
   erases it.
   - New test `HostServer.callablesPastTheCapFailTheInstantiation`, with `maxModuleBytes = 3`:
     - The 31-byte `(module (func (export "f")))` passes both the Load and Loaded caps, but its 4-byte
       Instantiated body does not.
     - Expected reply: `Instantiated{1, InvalidComponent}`.
     - A later `Call` of instance 1 is then `ProtocolViolation`.
5. **(Minor) Write failure per instance.** `WriteState write` now lives on `InstanceSlot` and is touched only by
   the lane holding the instance.
   - A failed write fails that instance's next `Call` (`Trap`), or the `HostResult` its parked call is waiting
     for (`lane.calls.back()`).
   - New section "a write out of bounds fails its own instance's call, not a nested one's". Instance 1 is parked
     in `ping-then-store`, and a bad write goes to instance 1. Then:
     - a nested `add` on instance 2 → `{5}`;
     - `HostResult` → instance 1's call fails `HostFailure`.
6. **(Minor) New tests:**
   - `HostServer.stopBeforeRun`: `stop()`, then `run()` → `Stopped`, and the parent's control end reads
     `Closed`.
   - `HostServer.stopEndsAParkedCall`: `Stopped` within 1 s, and lane 0 reads `Closed`.
   - Section "an unknown instance is a violation": `MemoryRead` of id 42 → `ProtocolViolation`.
7. **(Minor) Wasmtime test-support target.**
   - `wasm/wasmtime/CMakeLists.txt` defines `vtextension_wasmtime_hosted_engine`, which builds `HostedEngine.cpp`
     and links `vtextension_wasmtime`, under `VTEXTENSION_TESTING`.
   - `vtextension_remote_test` links that target, compiles only its own sources, and is built `if(... AND TARGET
     vtextension_wasmtime_hosted_engine)`.

Also new: **`HostServer.nestedCallsFitTheExecutorStack`**, which exercises item 1(b) itself.
- A runtime with `stackBytes = MaxRuntimeStackBytes` loads a module whose `down(n)` recurses n deep, then calls
  `pair`.
- A binary search on lane 1 finds the deepest n that still reaches the host call (32 764 here).
- Then 8 instances are nested on lane 0, each with n − 64: together they nearly fill 8 whole stacks.
- All 8 park in `CallHost`, and all 8 unwind cleanly.

### Evidence

- **Mutation checks.** I restored the file after each.
  - `InstanceHold::take` granting a second lane → `busyInstanceIsRefusedOnAnotherLane`: `assertions: 52 | 48
    passed | 4 failed`.
  - Depth cap raised to 800 → `callDepthIsBounded`: `assertions: 124 | 123 passed | 1 failed`.
  - Executors started on 8 MiB, the default thread stack, instead of `ExecutorStackBytes` →
    `nestedCallsFitTheExecutorStack`: `SIGSEGV`, reported as `AddressSanitizer: SEGV` on the executor thread.
    With the fix it passes.
- **GREEN (ASan/LSan).** `ASAN_OPTIONS=detect_leaks=1 vtextension_remote_test --order rand`, 10 runs → 10 × All
  tests passed (898 assertions in 25 test cases).
- **TSan.** `vtextension_remote_test`, rebuilt alone in `out/clang-tsan` (-j 4, niced, under the build lock).
  Five `--order rand` runs → 5 × All tests passed (898 assertions in 25 test cases), with no `WARNING:
  ThreadSanitizer`.
- `ctest --preset=clang-asan -R vtextension` → 100% tests passed, 0 tests failed out of 3.
- `ctest --preset=clang-asan -L lint` → 100% tests passed, 0 tests failed out of 33.
- **clang-tidy:** `clang-tidy -p out/clang-asan --quiet src/vtextension/wasm/host/HostServer.cpp
  src/vtextension/wasm/host/ExecutorThread.cpp src/vtextension/wasm/host/InProcessHost.cpp
  src/vtextension/wasm/host/HostServer_test.cpp src/vtextension/wasm/wasmtime/HostedEngine.cpp` → no findings.
- `-Werror` is clean. `clang-format --dry-run -Werror` and `editorconfig-checker` pass.
- **Windows:** `clang-cl ... /Zs /W4` gives 0 diagnostics on `HostServer.cpp` and `ExecutorThread.cpp`, the
  latter covering its `CreateThread` path. Nothing ran on Windows.

### Notes

- `HostFrameBytes` (256 KiB a level) is a generous estimate, not a measurement. The deep-stack test passes with
  the server's own frames under ASan; that is the build where they are largest.
- `ServerExit` gained no value for a failure to start a thread. As with `std::thread`, it throws, and the new
  scope guards make that unwind safe. The exception then leaves `run()`; for `InProcessHost`, that escapes the
  thread and terminates.
