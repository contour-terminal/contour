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

