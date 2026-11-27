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

