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

