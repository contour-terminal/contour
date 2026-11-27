# Task 2.3 report: `RemoteBackend` over an in-process `HostServer` (copy-path memory)

**Status:** DONE. Five commits on top of `4cc49cc4b`, nothing pushed.

| Commit | Subject |
|---|---|
| `a4b683eae` | fix(vtextension): report a host that died before the host call it failed |
| `a74ad411e` | feat(vtextension): carry a runtime's table cap to its host |
| `b6b24f85c` | fix(vtextension): end a host that cannot start a lane's thread, instead of throwing |
| `02559f053` | test(vtextension): measure what each nested call takes of a lane's stack |
| `e71d4013f` | feat(vtextension): a backend that runs extensions in a separate host over channels |

## Item 0

- **d (`a4b683eae`).** `Instance::guestFailure` checks `CallFailure::HostCrashed` before `_hostCallError`.
  - New test `Instance.hostCrashedOutranksAFailedHostCall`: a FakeWasmBackend `render` makes a host call to
    index 999, which records `Trapped`, then returns `HostCrashed`.
  - RED: `test cases: 2 | 1 passed | 1 failed`. GREEN: `Instance.*` 332 assertions in 26 cases.
- **c (`a74ad411e`, ruling R13).** `CreateRuntime.tableElements`, a u64 that defaults to `MaxTableElements` (as
  in `RuntimeLimits`), so existing senders keep the engine's cap.
  - The codec row lists it after `stackBytes`. The 64 KiB cap is unchanged; the field adds at most 10 bytes.
  - The round-trip sample sets 64. The host-functions count case's prefix is now `{1, 2, 3, 4}`.
  - HostServer refuses `tableElements > MaxTableElements` (`RuntimeUnavailable`) before narrowing it to u32,
    then passes it into `RuntimeLimits`.
  - The HostServer fixture's `createRuntime` now sends `contract::Limits.tableElements`.
  - New test `HostServer.runtimeHoldsTheParentsTableCap`: under the contract's cap of 64,
    `OversizedTableModule` → `Instantiated{OutOfMemory}`. Caps of 1025 and 2^32 + 64 are both refused, which
    covers the truncation case.
  - RED: 3 failed assertions. GREEN after the server change.
  - RemoteBackend sends the runtime's limit.
- **a (`b6b24f85c`).** The constructor is replaced by
  `[[nodiscard]] static std::expected<ExecutorThread, std::error_code> ExecutorThread::start(stackBytes, body)`,
  on both POSIX and Win32 paths. The private constructor takes the state.
  - Injection: `HostServerConfig::startExecutor` (`ExecutorStarter`, a `std::function`), defaulting to
    `&ExecutorThread::start`. This is DI of the OS thread resource.
  - `run()` starts the executors one by one. If one fails, it calls `finish(ServerExit::ExecutorUnavailable)`
    and does not serve control. The existing `core::Finally` guards join the threads already started.
  - The control loop moved to `serveControl()`.
  - Because `HostServerConfig` now holds a `std::function`, clang-tidy flagged its by-value copies
    (`performance-unnecessary-value-param`). HostServer, InProcessHost and the test fixture now move it.
  - Tests:
    - `ExecutorThread.runsItsBody`.
    - `ExecutorThread.unavailableStackIsAnError`: a SIZE_MAX/2 stack returns an error code, and the body never
      runs.
    - `HostServer.executorThatCannotStartEndsTheServer`: the second start fails. The server returns
      `ExecutorUnavailable`, the parent reads `Closed` after Hello, and there were exactly 2 start attempts.
  - RED was the compile failure (no `start`, no `startExecutor`).
- **b (`02559f053`).** `nestedCallsFitTheExecutorStack` now runs on a `ProbedEngine`, a test-only decorator for
  the engine, runtime, module and instance. It records `__builtin_frame_address(0)` at each `call()` entry, so
  ASan's fake stack does not distort the reading.
  - `ScriptedParent` gained a constructor that takes an engine.
  - Nesting is measured twice, each time with 8 levels (7 deltas):
    1. **Near-full guest stacks:** each level must be `< MaxRuntimeStackBytes + HostFrameBytes`.
    2. **`down(0)`, one guest frame:** each level must be `< HostFrameBytes / 2`, the host's own share.
  - Measured on ASan:
    - With fake stacks on: 1 049 344 B per full level, and 2 944 B for the host's share.
    - With `detect_stack_use_after_return=0`: 1 051 968 B and 5 568 B.
    - Either way the host's share is far below the 128 KiB bound.
  - The 2.2 note that "ASan is where frames are largest" does not hold with fake stacks; frame addresses
    sidestep this.

## The task (`e71d4013f`)

### Files

- `wasm/BackendConfig.hpp`: `HostTimeouts` and `BackendConfig`, as briefed (D15). It forward-declares
  `remote::IHostConnector` and is listed in `vtextension_HEADERS`.
- `wasm/remote/RemoteBackend.{hpp,cpp}`:
  - The header declares `makeRemoteKindState`, `makeRemoteBackend` and `preparedGeneration`.
  - The .cpp holds `HelperSlot`, `RemoteRuntime`, `RemoteModule`, `RemoteInstance` and `RemoteBackend`, all in
    an anonymous namespace.
- `wasm/remote/RemoteKindState.{hpp,cpp}`:
  - **Deviation:** a `.cpp` beside the briefed header.
  - `LaneRefusal { Unprepared, Exhausted }`.
- `wasm/remote/HelperIncarnation.{hpp,cpp}`, and `wasm/remote/CopyPathMemory.{hpp,cpp}`.
- Tests: `RemoteBackend_test.cpp` joins `vtextension_remote_test`, and `CopyPathMemory_test.cpp` joins
  `vtextension_test`.
- CMake:
  - `vtextension_remote` (STATIC) links `vtextension` and `vtextension_remote_wire` (PUBLIC).
  - `vtextension_test` and `vtextension_remote_test` link it.

### Design

- **Contract traits.** These use `testing::makeHostedEngine()`, not `wasmtime::makeWasmtimeEngine`: the
  containment lint's rule 3 forbids including `wasm/wasmtime/` headers from `wasm/remote/`, as 2.2 found.
  - The probe's engine is the same: a static kind state, with `TrapMode::Software`.
  - `BackendConfig` is filled in full (`.helper = {}, .timeouts = {}`), as
    `-Wmissing-designated-field-initializers` requires.
- **`RemoteKindState`.**
  - Like Wasmtime's `KindState`: a mutex, `_liveRuntimes`, and an atomic generation bumped on each 0→1
    transition.
  - It holds `std::array<LaneState, MaxLaneChannels>`.
  - Its serial comes from a process-wide atomic counter (D29).
  - Thread stamps are a `thread_local std::vector<ThreadStamp{kind serial, generation, optional lane}>`, one
    per kind. A thread prepared by two kinds therefore never overwrites, and leaks, the other kind's lane.
  - The guard holds the kind as a `shared_ptr` (`enable_shared_from_this`).
  - The outermost guard frees the lane and erases the stamp, but only if the stamp is still for its own
    generation.
  - Renewing a stale-generation stamp frees its lane first.
  - `laneOfCurrentThread()`:
    - fails `Unprepared` unless a guard of the current generation prepares the thread;
    - otherwise takes the lowest free lane;
    - when none is free, `Exhausted` is logged through `errorLog()` (D10).
  - `preparedGeneration()` returns the newest stamp's generation, or 0.
- **`HelperIncarnation`.**
  - Members: a control `PduStream` with its mutex, `MaxLaneChannels` lane streams, `atomic<IncarnationState>
    { Dead, Alive }`, and module/instance id counters. `RuntimeId` is 1.
  - The `IHostHandle` is declared last, so it is destroyed first.
  - `connect(connector, CreateRuntime)`:
    1. The connector must hand back 4 lanes and a handle.
    2. It reads Hello and checks the version, `lockdown != Failed`, and `memoryMode == Copy` (a Shared host
       cannot be driven before phase 4).
    3. It sends `CreateRuntime` and requires `RuntimeCreated{1, ok}`.
    4. Any failure is `markDead()` (which terminates the host) followed by `HostUnavailable`.
  - `request()` holds the mutex over a send and its receive. Any send or receive failure kills the incarnation,
    except a send-side `OverCap`, where nothing was sent and the stream is intact.
  - `post()` is for Release and ignores errors.
  - `markDead()` uses an atomic exchange, so it acts once. It calls `terminate()`, then shuts down the parent's
    control and every lane, so that no parent thread stays blocked, even on a hostile host that keeps its ends
    open.
  - `interrupt(id)` goes straight to the handle (D13).
- **`HelperSlot`** (one per identity key within a `RemoteRuntime`): it holds the current incarnation and a
  respawn counter.
  - `live()` returns the current incarnation if it is alive, or else connects a new one under the slot's mutex.
  - A respawn logs `errorLog()` with the count.
  - The runtime's map holds `weak_ptr`s, so a host lives while some module of its identity lives. An empty key
    always gets a fresh slot (D18).
- **`RemoteRuntime`.** It counts itself alive in the kind from its constructor to its destructor.
  - `load()` gets the slot's live incarnation, then calls `loadInto()`:
    - it sends Load;
    - a wrong reply kind or id → `markDead`, `HostUnavailable`;
    - a reply carrying an error passes that error through;
    - **D6:** `declaredCounts(bytes)` must succeed, and the described imports and exports must not exceed the
      declared counts. Otherwise `markDead`, `HostUnavailable`;
    - a send-side `OverCap` → `InvalidComponent`.
  - The module keeps its bytes, its slot and the incarnation it was loaded in.
- **`RemoteModule::instantiate`.** It runs under the module's mutex and asks `slot->live()`.
  - If the incarnation changed (respawn), it loads the bytes again. The new description must equal the old one,
    or the result is `InvalidComponent` (D19).
  - It then sends `Instantiate` on control.
  - A wrong reply, or a `callableExports` ordinal that is not a function export with ≤ 64 parameters and results,
    → `markDead`, `HostUnavailable`.
  - Otherwise the helper's error, if any, passes through. On success the result is a `RemoteInstance` holding
    the module and the incarnation.
- **`RemoteInstance`.**
  - `function(name)` offers only ordinals named in `Instantiated`, by name, and only all-i32 signatures.
  - `call()`:
    1. Return `Interrupted` if the sticky flag is set, `HostCrashed` if the incarnation is dead.
    2. Return `Trap` for an unknown ordinal, an `argv` smaller than max(params, results), or no lane
       (unprepared, or the fifth thread).
    3. Flush the copies to `MemoryWrite`s and clear them.
    4. Send `Call` on the lane.
    5. Loop:
       - On `CallHost`: check the index and arity against the runtime's table, run `context.callHost`, flush and
         clear, then send `HostResult`.
       - On `CallResult`: a failure is mapped (`MemoryGrowthRefused` → `Trap`; a host claiming `HostCrashed` →
         `Trap`). Otherwise the result count must match exactly; the results are copied out.
       - Anything else, or any `StreamError`, → `markDead()`, `HostCrashed`.
  - `memory()`:
    - size 0 → an empty span, with no I/O;
    - a dead incarnation → `nullopt`;
    - otherwise a `CopyPathMemory` view.
  - The fetch sends `MemoryRead` on the lane. A wrong reply, or an Ok reply with the wrong byte count, kills the
    incarnation. `OutOfBounds` → `nullopt`.
  - `interrupt()` sets the flag, then calls `incarnation->interrupt(id)`.
  - The destructor posts `Release{Instance}`, and the module's destructor posts `Release{Module}`; both are
    no-ops once the incarnation is dead. The last reference to the incarnation destroys its handle (D17).
- **`CopyPathMemory`** (D12).
  - Construction takes an instance id, a cap and an injected `MemoryFetch`, so it is unit-tested against a fake
    memory.
  - `view()`:
    - size 0 → empty;
    - a range past 2^32 → `nullopt`, with no fetch;
    - the latest cached range containing the request is served from the cache;
    - a fetch that would take the cache past the cap → `nullopt`, with no fetch;
    - otherwise it fetches, overlays the current bytes of every earlier overlapping range (in fetch order), and
      snapshots the result.
  - `flush()` walks the ranges in fetch order and emits one write per maximal run of bytes that differ from the
    snapshot. `clear()` drops everything.
  - The cap is `min(runtime memoryBytes, PduLimits{}.maxMemoryBytes)`, so a `MemoryWrite` can never be over its
    cap.
- **`RemoteBackend`.**
  - `createRuntime` refuses `tableElements > MaxTableElements`, more than `MaxHostFunctions`, and a function
    with more than `MaxHostParameters`, all with `RuntimeUnavailable`. The contract's
    `tableCapAboveMaximumIsRefused` needs this, since no host is connected until a load.
  - `capabilities()` is `Unreported` (see Concerns).
  - A null connector makes every load `HostUnavailable`.

## Tests

- **RED.**
  - The tests and headers compiled against stub `.cpp` files; the link then failed on the `CopyPathMemory`
    symbols.
  - Behavioural mutation checks, with the file restored after each:
    - the D6 check disabled, together with `call()` skipping `flushTo`;
    - result: `RemoteBackend.*` gave `test cases: 14 | 11 passed | 3 failed`, the failures being
      `describedMoreThanDeclaredKillsTheHelper`, `writesFlushBeforeCall` and `overlappingViewsSeePendingWrites`.
- **GREEN (ASan/LSan):**
  - `vtextension_remote_test "[wasm-contract]"` → All tests passed (472 assertions in 40 test cases).
  - `"RemoteBackend.*"` → All tests passed (129 assertions in 14 test cases). That is the 13 briefed cases, plus
    `RemoteBackend.preparedGenerationFollowsTheOutermostGuard`.
  - `installsNoSignalHandlers` passes.
  - The whole `vtextension_remote_test`, with `ASAN_OPTIONS=detect_leaks=1` → All tests passed (1616
    assertions in 83 test cases). It also passed 3 runs with `--order rand`.
  - `vtextension_test "CopyPathMemory.*"` → All tests passed (35 assertions in 4 test cases). That is the 2
    briefed cases, plus `viewsInsideAFetchedRangeFetchNothing` and `refusesWhatItCannotHold`.
  - `vtextension_test` (full) → All tests passed (12041 assertions in 359 test cases).
  - `vtextension_wasmtime_test` → All tests passed (1005 assertions in 52 test cases).
  - `ctest --preset=clang-asan -R vtextension` → 100% tests passed, 0 tests failed out of 3.
  - `ctest --preset=clang-asan -L lint` → 100% tests passed, 0 tests failed out of 33, the containment check
    included.
- **TSan.**
  - Built only `vtextension_remote_test` in `out/clang-tsan` (`-j 4`, niced, under the build lock).
  - 5 runs with `--order rand`: each exited 0 with All tests passed (1616 assertions in 83 test cases) and 0 `WARNING:
    ThreadSanitizer`.
  - Those runs include `fifthLaneThreadFailsItsCall`, `interruptIsWaitFreeOfTheHelper`,
    `instantiateRespawnsLazily` and every contract case.

## Static checks

- **clang-tidy, by hand** (command below) → no findings.
  ```
  clang-tidy -p out/clang-asan --quiet src/vtextension/wasm/remote/RemoteBackend.cpp
  src/vtextension/wasm/remote/RemoteKindState.cpp src/vtextension/wasm/remote/HelperIncarnation.cpp
  src/vtextension/wasm/remote/CopyPathMemory.cpp src/vtextension/wasm/remote/RemoteBackend_test.cpp
  src/vtextension/wasm/remote/CopyPathMemory_test.cpp
  ```
- For item 0, the same command over `HostServer.cpp`, `ExecutorThread.cpp`, `InProcessHost.cpp`,
  `HostServer_test.cpp`, `ExecutorThread_test.cpp`, `Codec.cpp`, `Codec_test.cpp`, `Instance.cpp` and
  `Instance_test.cpp` → no findings, after the move fixes above.
- **Build:** `-Werror` clean.
- **Formatting:** `/usr/bin/clang-format --dry-run -Werror` and `editorconfig-checker` are clean on every changed
  file.
- **Windows:** not syntax-checked this round. `ExecutorThread::start`'s Win32 branch builds the `error_code`
  from `GetLastError()`.

## Deviations

1. The contract traits use `testing::makeHostedEngine()`, as required by the containment lint (see Design).
2. `RemoteKindState.cpp` is a file the brief did not list.
3. `ExecutorStarter` lives in `HostServerConfig`; the brief left the injection point open.
4. `CopyPathMemory` takes an injected `MemoryFetch`, and its `view()` keeps the brief's signature.
5. Extra tests: `preparedGenerationFollowsTheOutermostGuard`, two more `CopyPathMemory` cases, and the item 0
   cases.
6. `HelperIncarnation` sends no `Release{Runtime}`. An incarnation holds exactly one runtime and is dropped with
   its handle, so ending the host releases it.

## Concerns

- **`capabilities()` is `Unreported`,** and a host's `MemoryGrowthRefused` is mapped to `Trap`. The parent does
  not know a host's engine before Hello. Wasmtime is `Unreported` anyway, so nothing is lost today. If a
  `Reported` engine runs in a host (WAMR, in phase 5), Hello could carry the capability.
- **No deadline on the handshake, a Load, an Instantiate or a call.** A host that is alive but never answers
  blocks its caller; `HostTimeouts` is carried but unused. A host's death or `terminate()` does wake every
  blocked receive. Enforcing the deadlines is phase 3's supervisor.
- **A respawn reloads lazily, per module, under that module's mutex.** Two modules of one identity each reload
  on their own next instantiate.
- **Overlapping views.** If both ranges are written at the same byte after the later one was fetched, the later
  fetched range wins, even if the earlier view was written last. This is documented in `CopyPathMemory`, and
  phase 4's shared memory removes it.
- **Windows:** not checked.

## Performance, risk, coverage

- **Performance:** no production path changes. `Instance::guestFailure` gains one comparison. `PduStream::send`
  is unchanged, and `CreateRuntime` is 1–10 bytes larger.
- **Risk:** moderate, in new code nothing in production links yet. It is threaded (lanes, kind state, death
  from any thread), and covered by the contract suite plus 14 remote cases under ASan/LSan and TSan. Item 0
  changes `HostServer::run()`'s startup path, covered by the injected failure.
- **Coverage:** every PDU kind the parent receives on a call path has a test of its unexpected or malformed
  form: a malformed frame, an over-cap frame, an extra description, and a version or lockdown refusal. No
  coverage build was run (ASan tree only, per the run rules).

---

## Fix round 1 (review: Needs fixes)

**Commit:** `7e77b3019 fix(vtextension): refuse a respawned host's other module, and serve no stale memory copy`

### Changes

1. **(Important) A mismatched reload is no longer adopted.** In `RemoteModule::instantiate`, the reloaded
   description is now compared *before* `_loadedIn` and `_moduleId` change.
   - On a mismatch, the new incarnation is `markDead()`ed and the call returns `InvalidComponent`.
   - The next instantiate therefore respawns, reloads, compares and refuses again.
   - Test `RemoteBackend.respawnDescribingAnotherModuleIsRefused`. The scripted connector now passes the
     connection index to its script.
     - Connection 0 serves honestly, then writes garbage on its lanes, so the first call kills it.
     - Every later connection describes PeekModule without its last export.
     - Two instantiate attempts each fail `InvalidComponent`, with terminations at 2 and then 3.
     - The module's exports stay 3.
2. **(Important) No stale cache hit.** `CopyPathMemory::view` now serves from the cache only when the *latest
   range overlapping the request* contains all of it. Otherwise it fetches and overlays, as for a new range.
   - Test `CopyPathMemory.overlappedRangeIsNotServedStale`:
     - A = [0,8) is fetched, then B = [4,12) is fetched and written at 4 and 7.
     - `view(0,8)` then shows B's bytes at 4 and 7.
     - `view(4,4)` is served from B without a fetch.
3. **(Minor) An over-cap reply is the host's fault.** In `loadInto`, `OverCap` maps to `InvalidComponent` only
   while the incarnation is still alive, which is the send-side case. Otherwise it is `HostUnavailable`.
   - Test `RemoteBackend.overCapReplyIsTheHostsFault`: the Load is answered with a Loaded header declaring
     4 × maxModuleBytes + 1. The result is `HostUnavailable`, with 1 termination.
4. **(Minor, R15)** `BackendConfig::connector`'s doc now says that null leaves the choice to the composition
   root. The remote backend needs a connector and fails every load `HostUnavailable` without one; a
   composition that can run hosts in-process supplies one.
5. **(Minor) Validation paths, now tested.**
   - `RemoteBackend.wrongRepliesKillTheHelper` covers four cases, each → `HostUnavailable` with 1 termination:
     - Loaded echoing another module id;
     - Instantiated echoing another instance id;
     - a callable ordinal past the exports;
     - a callable memory export (ordinal 0).
   - `RemoteBackend.memoryDataOfTheWrongSizeKillsTheHelper`: an Ok `MemoryData` of 3 bytes for a 4-byte read.
     `memory()` → `nullopt`, terminations == 1 (a `REQUIRE`, before any call), then a call → `HostCrashed`.
   - `RemoteBackend.wrongResultCountKillsTheHelper`: a `CallResult` with 2 values for a 1-result export →
     `HostCrashed`, 1 termination, and the next call is also `HostCrashed`.
   - To write these, `HelperEnds` now holds a raw control `Channel`. It encodes PDUs with
     `proto::encodeFrame` and decodes with `proto::decodeFrame`, so a script can send any frame, an over-cap
     header included.
   - A `HelperScript` (Loaded and Instantiated frame builders, plus bytes for every lane) replaces
     `serveUpToInstantiation`.
   - `ScriptedRig` and `ScriptedInstance` remove the repeated setup.
6. **(Minor)** `RemoteRuntime::slotFor` erases expired `weak_ptr` entries on each lookup with a key (one
   `std::erase_if`). Nothing outside the runtime can see this, so it has no test.

### Evidence

- **RED:** the new tests, run against the pre-fix `RemoteBackend.cpp` and `CopyPathMemory.cpp` (stashed):
  - `CopyPathMemory.*`: `test cases: 5 | 4 passed | 1 failed`, the failure being
    `overlappedRangeIsNotServedStale`.
  - `RemoteBackend.*`: `test cases: 19 | 17 passed | 2 failed` (3 assertions), the failures being
    `overCapReplyIsTheHostsFault` and `respawnDescribingAnotherModuleIsRefused`.
  - The item-5 tests passed before the fix too, as expected: those checks existed, untested.
- **GREEN (ASan/LSan):**
  - `vtextension_test "CopyPathMemory.*"` → All tests passed (44 assertions in 5 test cases).
  - `vtextension_remote_test "RemoteBackend.*"` → All tests passed (177 assertions in 19 test cases).
  - Whole `vtextension_remote_test` with `detect_leaks=1 --order rand` → All tests passed (1664 assertions in 88
    test cases).
  - Whole `vtextension_test` → All tests passed (12050 assertions in 360 test cases).
  - `ctest --preset=clang-asan -R vtextension` → 3/3 passed; `ctest --preset=clang-asan -L lint` → 33/33
    passed.
- **TSan:** `vtextension_remote_test` rebuilt in `out/clang-tsan` (-j 4, niced, under the lock).
  5 × `--order rand` → All tests passed (1664 assertions in 88 test cases), with 0 `WARNING: ThreadSanitizer`.
- **clang-tidy:** `clang-tidy -p out/clang-asan --quiet src/vtextension/wasm/remote/RemoteBackend.cpp
  src/vtextension/wasm/remote/CopyPathMemory.cpp src/vtextension/wasm/remote/RemoteBackend_test.cpp
  src/vtextension/wasm/remote/CopyPathMemory_test.cpp` → no findings. One finding on the first run was fixed:
  a by-value vector in a test lambda, now moved into its capture.
- `/usr/bin/clang-format --dry-run -Werror` and `editorconfig-checker` are clean.

### Note

- Killing a host that describes the reloaded bytes differently also fails the other live modules of the same
  identity in that incarnation, which then respawn. This is the intended reading of "an honest host is
  deterministic".
