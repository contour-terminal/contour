## Phase-3 decisions

Proposed for a controller ruling (as R10 did for D1–D29). Each names the choice and, after the dash, why.

- D30. New directory `src/vtextension/wasm/remote/process/` (namespace `vtextension::wasm::remote::process`), target `vtextension_remote_process`: launcher, process handles, exit wait set, supervisor, `ProcessHostConnector`, `HostTimers`, `LaneWatch` (namespace `remote`), helper location. Links only `vtextension_remote_wire core::base core::platform`; `vtextension_remote` and `vtextension_host` link it — it stays std-only, so it cross-builds and runs under wine without vtbackend.
- D31. `vtextension_remote_wire` gains what both processes need: `Channel::adopt`, `Channel::trySend`, `ControlPage`, `WakeSender`/`WakeReceiver`, `HelperArguments` — the helper must not link the launcher's library to read its own arguments.
- D32. `src/contour-extension-host/` holds `main.cpp` only and is built iff `TARGET vtextension_wasmtime` (the WAMR engine in the helper is Task 5.3). The engine comes from a neutral `wasm/host/HelperEngine.hpp` (`makeHelperEngine()`), defined in `wasm/wasmtime/HelperEngine.cpp` (target `vtextension_wasmtime_helper_engine`) — the containment lint's rule 3 forbids Wasmtime headers outside `wasm/wasmtime/`, as phase 2's `HostedEngine` probe already respects.
- D33. Helper trap mode: `TrapMode::Signals` in plain builds, `TrapMode::Software` when `CONTOUR_SANITIZE` or `CONTOUR_COVERAGE` is set (compile definition on `vtextension_wasmtime_helper_engine`) — sanitizer runtimes own SIGSEGV; Task 5.2 inherits this.
- D34. `main` is a thin call to `host::runExtensionHost(arguments, engines)` (`wasm/host/HostProcess.{hpp,cpp}`): parse, adopt, bind to parent, serve through `HostProcess` (server + interrupt listener), map `HostProcessExit` to an exit code through one table (codes 0 and 70–75, never 1/23/66, which ASan/LSan/TSan use). `InProcessHost` is rebuilt on `HostProcess` — the in-process tests then drive the same interrupt path as the real helper, under TSan.
- D35. Handles reach the helper by inheritance, named in argv: `--control=H --lanes=H,H,H,H --page=H --wake=H --parent=PID --data-bytes=N`. POSIX child numbers are fixed (control 3, lanes 4–7, page 8, wake 9; `dup2` by the launcher); Windows passes the inherited handle values. `LaunchSpec::arguments` is a function of the child-side values, because on Windows they are known only after duplication. Table-driven parser: unknown, duplicate, missing or malformed → `HostProcessExit::BadArguments` — one row per option, so phase 5 adds a row, not a branch.
- D36. Launch per OS. Linux: `posix_spawn` (glibc's `CLONE_VFORK` path: no `fork` of a large, sanitized, multi-threaded parent) with `posix_spawn_file_actions_addclosefrom_np(10)`, `addchdir_np("/")`, `POSIX_SPAWN_SETSIGDEF|SETSIGMASK`, then `pidfd_open`; a kernel without pidfd (< 5.3) → `LaunchError::Unsupported` (Landlock in phase 5 needs 5.13 anyway). FreeBSD: `pdfork(PD_CLOEXEC)`, the child only `dup2`/`closefrom(10)`/`chdir`/`execve`. macOS: `posix_spawn` with `POSIX_SPAWN_CLOEXEC_DEFAULT` + `dup2` actions + `addchdir_np`, exit through kqueue `EVFILT_PROC`; the CLOEXEC window on the parent's ends is documented (spec §5.3). Windows: `CreateProcessW` with `PROC_THREAD_ATTRIBUTE_HANDLE_LIST` (inheritable duplicates, closed after the call) and `PROC_THREAD_ATTRIBUTE_JOB_LIST` (job: `KILL_ON_JOB_CLOSE`, `ACTIVE_PROCESS_LIMIT` 1, `PROCESS_MEMORY` = `ResourceCaps::dataBytes`, `DIE_ON_UNHANDLED_EXCEPTION`) — the job applies from the first instruction, no suspended window.
- D37. Parent-death binding and POSIX caps are applied by the helper itself. `bindToParent(parentPid)` runs first in `runExtensionHost`: Linux `prctl(PR_SET_PDEATHSIG, SIGKILL)` then `getppid() == parentPid` (else `ParentGone`) — the binding is to the thread that ran `posix_spawn`, the supervisor thread (D42), exactly as a pre-exec `prctl` would be; FreeBSD `procctl(PROC_PDEATHSIG_CTL)` + the same check; macOS the check only (no equivalent, documented); Windows nothing (the job). Caps (`RLIMIT_NPROC = 0` on Linux and FreeBSD, `RLIMIT_DATA` on Linux) are applied by a new `HostServerConfig::confine` hook that `HostServer::run()` calls after every lane's thread and the listener started, before `Hello` — on Linux `RLIMIT_NPROC` counts threads, so caps applied pre-exec would stop the helper from starting its own executors; [CORRECTED in the phase-3 fix wave] phase 5's lockdown does NOT plug into this hook: no_new_privs/Landlock are per-thread and go into `HostConfinement::beforeThreads` (right after `bindToParent`, before endpoints, engine or any thread; R26 lives there as `hardenProcess()`); only seccomp+TSYNC runs here. A `Failed` from `beforeThreads` makes `Hello` state `Failed`.
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
