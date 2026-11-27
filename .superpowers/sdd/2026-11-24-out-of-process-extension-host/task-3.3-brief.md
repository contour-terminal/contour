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

