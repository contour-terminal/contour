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
