# Fix wave: whole-diff review findings 1-7

Base d0cf0568b, head 3abef4e10. Builds were out/clang-asan (and out/clang-tsan for finding 6), `-j 4`, niced, behind the flock, and covered only the targets named below.

## Commits

| SHA | Subject | Finding |
|---|---|---|
| 07aa6b700 | fix(vtextension): count a host dying during a memory read as the host's | 1 |
| cea3496e3 | fix(vtextension): refuse a stack no extension host runs before starting one | 2 |
| b5707570c | fix(vtextension): treat a host's implausible load refusal as a protocol break | 3 |
| 7edcfdf0c | fix(vtextension): name a Wasmtime kind in a thread's stamp by serial | 4 |
| ad7feb73f | perf(vtextension): size an extension host stream's buffer to the frame once | 5 |
| 161737157 | test(vtextension): promise and check concurrent instantiation of one module | 6 |
| 3abef4e10 | refactor(vtextension): count a registry waiter out with core::Finally | 7 |

## Finding 1: host death during a memory read (seam change)

**Change**
- `WasmRuntime.hpp`: new `enum class MemoryFailure : uint8_t { OutOfBounds, HostCrashed }`. `IWasmInstance::memory` now returns `std::expected<std::span<std::byte>, MemoryFailure>`, documented.
- WAMR (`WamrInstance.cpp`), Wasmtime (`WasmtimeInstance.cpp`) and `FakeWasmBackend.hpp` return `OutOfBounds` wherever they used to return nullopt.
- `CopyPathMemory`: `MemoryFetch` returns `expected<vector, MemoryFailure>` and `view()` returns `expected<span, MemoryFailure>`. The private `FetchError` is gone and the fetch's own reason passes through. A fetch that answers with the wrong size is refused as `OutOfBounds`, which cannot happen in practice because `RemoteInstance::fetch` already checks the size.
- `RemoteBackend.cpp`:
  - `memory()` on a dead incarnation returns `HostCrashed`.
  - In `fetch()`, a failed send, a failed receive, a wrong PDU or a size mismatch calls `markDead()` and returns `HostCrashed`.
  - A host answering with a non-Ok status returns `OutOfBounds`.
  - A thread without a lane returns `OutOfBounds`, mirroring `call()`, which traps for such a thread.
- `Instance.cpp` `GuestMemory::view`: `OutOfBounds` maps to `Trapped` and `HostCrashed` to `HostUnavailable`. This covers render's `loadByteString` as well as lowering and lifting in update and load. A host call that hits it records `HostUnavailable` as its error, and `guestFailure` already ranks `HostCrashed` first.
- `HostServer.cpp` needed no change, because `if (auto view = ...)` works on `expected`.
- Contract helpers (`BackendContract.hpp`) and the remote and CopyPathMemory tests now assert the specific failure instead of `!has_value()`: `OutOfBounds` for out-of-range reads, `HostCrashed` after a host died.
- New `wasm/testing/DecoratedEngine.hpp` (test-only): `ForwardingInstance` plus `DecoratedEngine`, which wraps every instance a hosted engine creates. `HostServer_test.cpp`'s four-class Probed* chain was replaced by `ProbedInstance : ForwardingInstance` and `makeProbedEngine`, which removes the duplication.

**Test**
- `RegistryRemote_test.cpp` gains the case "host death during a memory read counts as the host's". Its `DyingOnRead` instance (through `DecoratedEngine`), once armed, calls `connector->terminateAll()` from inside the helper-side `memory()` that serves the parent's `MemoryRead`. So the host answers render's `CallResult` honestly and dies before it answers the read.

**RED** (seam unchanged; the test header briefly used the old optional signature):
```
out/clang-asan/src/vtextension/wasm/remote/vtextension_remote_test "host death during a memory read counts as the host's"
RegistryRemote_test.cpp:253: FAILED: CHECK( (*instance)->render(surface(10, 1)).error() == ExtensionError::HostUnavailable )  with expansion: 3 == 16
RegistryRemote_test.cpp:254: FAILED: CHECK( compiled->breaker()->faultCount() == 1 )  with expansion: 0 == 1
test cases: 1 | 1 failed; assertions: 7 | 5 passed | 2 failed
```
Here 3 is `Trapped` and 16 is `HostUnavailable`.

**GREEN**
- The case: `All tests passed (7 assertions in 1 test case)`.
- Suites after the change:
  - vtextension_test: 13257 assertions / 385 cases
  - vtextension_wasmtime_test: 1005 / 52
  - vtextension_remote_test: 4137 / 182
- Added: `CopyPathMemory.reportsAHostGoneAsSuch`, in which a `HostCrashed` fetch surfaces as `HostCrashed`.

Error messages stay user-facing-neutral: no new user-visible text, and `HostUnavailable` keeps its existing wording.

## Finding 2: wire-side limits

**Change**
- `MaxCallDepth` and `MaxRuntimeStackBytes` (with the static_assert against `ExtensionLimits::stackBytes`) moved from `host/HostServer.hpp` into `remote/proto/Pdu.hpp`.
- `HostServer.hpp` keeps `HostFrameBytes` and `ExecutorStackBytes`, which are now built from `proto::`. It no longer includes `ExtensionLimits.hpp`.
- `HostServer.cpp` and `HostServer_test.cpp` use `proto::`.
- `RemoteBackend.cpp`:
  - `static_assert(MaxLaneChannels == MaxExtensionWorkers)`, via an include of `ExtensionWorkerPool.hpp`.
  - `createRuntime` refuses `stackBytes > proto::MaxRuntimeStackBytes` with `RuntimeUnavailable`.

**Test:** `RemoteBackend.refusesAStackNoHostRuns` checks that one byte over the cap is `RuntimeUnavailable` with `connects() == 0`, and that a stack exactly at the cap loads with one host.

**RED** (test against the old RemoteBackend.cpp): `REQUIRE_FALSE( refused.has_value() )`, expansion `!true`.

**GREEN:** passes. The intermediate commit cea3496e3 was not built on its own; its tree is HEAD minus finding 3's lines, and the combined tree was built and tested.

## Finding 3: Loaded.error and Instantiated.error trusted as-is

**Change:** `RemoteBackend.cpp` adds the table `HostRefusals = { InvalidComponent, OutOfMemory, RuntimeUnavailable, ModuleTooLarge }` and `isHostRefusal()`.
- A `Loaded` or `Instantiated` carrying any other error is a protocol violation: the parent calls `markDead()` and returns `HostUnavailable`.

**Tests** (scripted connector):
- `RemoteBackend.wrongRepliesKillTheHelper` gains four rows: a Loaded with `HostUnavailable`, a Loaded with `Trapped`, an Instantiated with `HostUnavailable`, and an Instantiated with `DeadlineExceeded`. Each expects `HostUnavailable` and one termination.
- New `RemoteBackend.honestRefusalsPassThrough`: each of the four allowed errors, at both load and instantiate, passes through unchanged with zero terminations.

**RED:**
- The Loaded rows got `3 == 16` and `terminations 0 == 1`.
- The Instantiated rows got `4 == 16` and `0 == 1`.
- Together with finding 2's case: 7 assertions failed out of 61.

**GREEN:** `All tests passed (66 assertions in 3 test cases)`. Full remote suite: 4191 assertions / 184 cases.

## Finding 4: Wasmtime KindState keyed by address

**Change**
- `WasmtimeRuntime.hpp` `KindState` gains a `noexcept` constructor that takes a process-unique serial from an atomic counter, plus a `serial()` accessor.
- In `WasmtimeEngine.cpp`, `ThreadStamp.kind` and `ThreadGuard::_kind` now hold the serial. All comparisons use the serial, as `RemoteKindState` does.

**Test:** `WasmtimeEngine.kindStampsOutliveNoKind` constructs two KindStates one after the other in the same `std::optional`, which guarantees the same address (REQUIRE'd).
- The successor, at the same generation, is not prepared by the predecessor's stamp.
- Its own guard is Outermost.
- The predecessor's guard going does not clear the successor's stamp.

**RED:**
- `CHECK_FALSE( kind->isCurrentThreadPrepared() )` gave `!true`.
- After the old guard went, `CHECK( kind->isCurrentThreadPrepared() )` gave `false`.

**GREEN:** 8/8. vtextension_wasmtime_test: 1013 assertions / 53 cases.

## Finding 5: PduStream receive buffer

**Change**
- In `proto/Codec.{hpp,cpp}`, header reading and checking moved into an internal `readHeader()`. The new public `frameSizeOf(buffer, arriving, channel, limits)` reports the total frame size (header included) once the header has arrived and passed the same checks. `decodeFrame` is built on `readHeader`, with unchanged semantics and error order.
- `PduStream` holds `unique_ptr<uint8_t[]>` from `make_unique_for_overwrite`, so it is not zero-filled, together with `_capacity`.
- On `NeedMoreData` it reallocates once to `max(frameSize, _buffered + 1, 64 KiB)`, copying only the buffered bytes. There is no doubling.
- After a frame is consumed, the buffer is dropped only if it is drained and larger than `RetainedBufferBytes` (1 MiB + 64 KiB). Smaller buffers are kept between frames.
- Test hook `receiveBufferStats()` returns `{allocations, peakBytes}`.

**Tests:**
- `PduStream.largeFrameAllocatesItsSizeOnce`: a Load of `maxModuleBytes - 64` bytes, checking `peakBytes <= frameBytes + 64` and `allocations <= 2`.
- `PduStream.keepsAModerateBufferBetweenFrames`: four 512 KiB frames, checking `allocations <= 2`.

**RED** (old algorithm, with the stats hook added to it):
- The large frame took 9 allocations.
- The four moderate frames took 20.
- The old peak stayed within the bound in that run: 16 MiB doubling happened to land at or under frame + 64.

**GREEN:** allocations `2 <= 2`. Measured peak `16777163 <= 16777227`, which equals the frame size exactly. All 8 PduStream cases and all 10 codec/remote cases in vtextension_test pass.

**Deviation:** the finding says "have decodeFrame report the total frame size". It is reported by `frameSizeOf()`, a sibling that `decodeFrame` is built on, rather than by changing `decodeFrame`'s error type. About 20 call sites (codec tests, the fuzz harness, RemoteBackend_test) match on `DecodeError`, and the single header parse is shared, so nothing is duplicated.

**Note:** during a grow, the old 64 KiB buffer and the new one coexist briefly. `peakBytes` counts the single buffer.

## Finding 6: concurrent IWasmModule::instantiate

**Change**
- `WasmRuntime.hpp`: `IWasmModule::instantiate` now promises that it may run "on several prepared threads at once: concurrent instantiations of one module neither fail nor disturb one another". The backend-level Threading paragraph says the same.
- `BackendContract.hpp` adds `CONTOUR_WASM_BACKEND_CONTRACT_THREADING_CASE`, tagged `[wasm-contract][threading]`, and the case `instantiatesConcurrently`.
  - Two `std::async` threads are each prepared and released together by a `std::latch`.
  - Each instantiates one shared module 25 times and calls `add`, checking the sum.
  - It repeats with `UnknownImportModule` (an unsatisfied import, the WAMR re-link path).
  - Each outcome is compared with a single-thread reference instantiation.
- The case runs in all three compositions through `CONTOUR_WASM_BACKEND_CONTRACT`.

**WAMR:** no serialisation was needed. In the vendored WAMR, `wasm_resolve_symbols` (which writes `func_ptr_linked`) is reached only through the public `wasm_runtime_resolve_symbols`. Neither `wasm_runtime_instantiate*` nor our code calls it (grep of `_deps/wamr-src` and `src/`), and imports are linked at load. WAMR in out/clang-tsan is TSan-instrumented: vmlib objects are built with `-fsanitize=thread`.

**TSan** (out/clang-tsan, `-j 4` niced, only the three targets, no suppressions):
```
cmake --build out/clang-tsan -j 4 --target vtextension_test vtextension_wasmtime_test vtextension_remote_test   -> exit 0
<binary> "WasmContract.instantiatesConcurrently*"  (each binary run 3x; WAMR also 5x on the first version of the case)
  out/clang-tsan/src/vtextension/vtextension_test                          All tests passed (10 assertions in 1 test case) x3, 0 TSan warnings
  out/clang-tsan/src/vtextension/wasm/wasmtime/vtextension_wasmtime_test   All tests passed (10 assertions in 1 test case) x3, 0 TSan warnings
  out/clang-tsan/src/vtextension/wasm/remote/vtextension_remote_test       All tests passed (10 assertions in 1 test case) x3, 0 TSan warnings
```
The same case also passes under ASan in all three binaries. It is a contract (promise) addition and green on first run, so there is no RED.

## Finding 7

- `CompiledExtension::breaker()` is documented as a test hook, the preferred alternative. RegistryRemote_test and ExtensionRegistry_test observe the breaker through it, 11 uses.
- `DecrementOnExit` (a unique_ptr with a custom deleter) is replaced by `core::Finally { [this] { --_waiting; } }`.
- `[registry]` in vtextension_test: 942 assertions / 18 cases pass.

## Final gates

```
ctest --preset=clang-asan -R 'vtextension|check_wasm_runtime_containment'
  check_wasm_runtime_containment, _selftest, vtextension_test, vtextension_wasmtime_test, vtextension_remote_test
  100% tests passed, 0 tests failed out of 5
```
Direct runs at the end:
- vtextension_test: 13282 assertions / 388 cases
- vtextension_wasmtime_test: 1023 / 54
- vtextension_remote_test: 4201 / 185, rerun after the clang-tidy fixes

`cmake --build --preset clang-asan -j 4 --target contour vthost_test` (flock, niced) exited 0, with no warnings or errors in the log.

clang-tidy by hand:
```
nice -n 19 ionice -c 3 clang-tidy -p out/clang-asan --quiet \
  src/vtextension/ExtensionRegistry.cpp src/vtextension/Instance.cpp src/vtextension/wasm/host/HostServer.cpp \
  src/vtextension/wasm/host/HostServer_test.cpp src/vtextension/wasm/remote/CopyPathMemory.cpp \
  src/vtextension/wasm/remote/CopyPathMemory_test.cpp src/vtextension/wasm/remote/RegistryRemote_test.cpp \
  src/vtextension/wasm/remote/RemoteBackend.cpp src/vtextension/wasm/remote/RemoteBackend_test.cpp \
  src/vtextension/wasm/remote/proto/Codec.cpp src/vtextension/wasm/remote/transport/PduStream.cpp \
  src/vtextension/wasm/remote/transport/PduStream_test.cpp src/vtextension/wasm/wamr/WamrInstance.cpp \
  src/vtextension/wasm/wasmtime/WasmtimeEngine.cpp src/vtextension/wasm/wasmtime/WasmtimeEngine_test.cpp \
  src/vtextension/wasm/wasmtime/WasmtimeInstance.cpp
```
The first run found three issues, all in new test code:
- `misc-use-internal-linkage` on `loadedFailing` and `instantiatedFailing`, now moved into an anonymous namespace.
- `performance-unnecessary-value-param` on `makeDyingConnector`, which now takes a const&.

These were folded into commits 07aa6b700 and b5707570c by fixup and autosquash. The re-run on the two files is clean, and the other 14 files reported nothing.

`clang-format --dry-run --Werror` and `editorconfig-checker` are clean on every changed file.

## Concerns and risk

- **Performance:** the stream no longer zero-fills or doubles, so the receive path for large frames allocates once at exactly the frame size, and frames of a few hundred KiB reuse one buffer. Nothing else is on a hot path. No Callgrind run was made.
- **Risk:**
  - The seam change in finding 1 touches every backend, but the mapping is mechanical, and every contract case passes on WAMR, Wasmtime and Remote.
  - The `readHeader` split in finding 5 preserves the order of the codec checks. The codec tests pass, but the fuzz target was not built (it is not part of the gated targets).
- **Coverage:** every new branch has a test. These include: `HostCrashed` in CopyPathMemory and `GuestMemory::view`; the refusal whitelist both ways; the stack cap on both sides of the limit; serial stamps; buffer sizing and retention; concurrent instantiation. No coverage build was run.
