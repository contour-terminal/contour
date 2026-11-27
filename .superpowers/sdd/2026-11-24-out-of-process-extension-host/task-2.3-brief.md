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

