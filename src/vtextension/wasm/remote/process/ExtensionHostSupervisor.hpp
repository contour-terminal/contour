// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// The one thread per hosting process that owns every extension host process: it launches them, watches
/// for their end, reaps them, kills them, and keeps their timers.
///
/// Who wants a helper reaches the supervisor through its mailbox, which every connector and handle shares and
/// none of them owns: the last of them can never be dropped on the supervisor's own thread and have to join
/// it. A launch runs on the supervisor's thread and blocks its caller until it did; everything else is
/// posted and returns at once. Each helper is held by a SupervisedHelper, whose destruction lets it go.

#include <core/Clock.hpp>

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>
#include <vector>

#include <vtextension/wasm/remote/HostConnector.hpp>
#include <vtextension/wasm/remote/process/ProcessLauncher.hpp>

namespace vtextension::wasm::remote::process
{

/// How long a stopping supervisor waits for the helpers it killed to be reaped.
inline constexpr auto ShutdownReapBudget = std::chrono::milliseconds { 100 };

/// How a supervisor is set up.
struct SupervisorConfig
{
    std::shared_ptr<IProcessLauncher> launcher; ///< What starts and waits for the helpers.
    core::IClock* clock = nullptr;              ///< Non-null; outlives the supervisor.
    /// How long a helper that was let go may take to exit on its own once its channels closed, before it is
    /// killed: 0 kills at once. Test compositions wait, so that coverage and sanitizer reports get written.
    std::chrono::milliseconds teardownGrace { 0 };
};

/// Why a supervisor could not start.
enum class SupervisorError : uint8_t
{
    WaitSetUnavailable, ///< The launcher made no set to wait for its processes with.
    ThreadUnavailable,  ///< No thread could be started.
};

/// Why the supervisor killed a helper; indexes SupervisorCounters::kills.
enum class KillReason : uint8_t
{
    Requested,   ///< Its holder asked (SupervisedHelper::kill()).
    StepTimeout, ///< It overstayed a step's watch.
    Grace,       ///< An interrupted call of its did not end within the grace, by its own time.
    Dropped,     ///< Its holder let it go.
    Shutdown,    ///< The supervisor stopped, or can no longer wait for its helpers.
};

/// How many KillReasons there are.
inline constexpr std::size_t KillReasonCount = 5;

/// What a supervisor did so far. Test hook.
struct SupervisorCounters
{
    std::size_t launches = 0;       ///< Helpers started.
    std::size_t reaped = 0;         ///< Helpers whose end was collected.
    std::size_t deathsObserved = 0; ///< Ends noticed while the supervisor ran, rather than while it stopped.
    std::size_t uncleanExits = 0;   ///< Ends other than exit code 0, or a kill the supervisor sent.
    std::array<std::size_t, KillReasonCount> kills {}; ///< Kills sent, indexed by KillReason.
};

class SupervisorMailbox;
class ExtensionHostSupervisor;
/// The supervisor thread's state; what it does is in the source.
class SupervisorLoop;

/// The parent's grip on one supervised helper. Move-only; destroying it lets the helper go: the supervisor
/// kills it (after `teardownGrace`, if it has not exited by then) and reaps it, and this returns at once.
/// Every call is a no-op once the supervisor stopped.
class SupervisedHelper
{
  public:
    SupervisedHelper(SupervisedHelper const&) = delete;
    SupervisedHelper& operator=(SupervisedHelper const&) = delete;
    SupervisedHelper(SupervisedHelper&& other) noexcept;
    SupervisedHelper& operator=(SupervisedHelper&& other) noexcept;
    ~SupervisedHelper();

    /// @return The helper's process id.
    [[nodiscard]] ProcessId processId() const noexcept { return _processId; }

    /// @return What handles are granted to the helper through (HandleGrant.hpp), made by the supervisor once
    ///         it started the helper. Thread-safe: granting reads it, and nothing changes it but a move.
    [[nodiscard]] transport::HandleTarget const& handleTarget() const noexcept { return _target; }

    /// Has the supervisor kill the helper. Posts, and returns.
    void kill() noexcept;

    /// Lets the helper go now, as destroying this would; every later call is a no-op. Posts, and returns.
    /// Thread-safe against the other calls, as a holder shared by threads needs it to be.
    void letGo() noexcept;

    /// Arms a step's timeout, from now: should the watch still live once @p budget passed, the supervisor
    /// kills the helper and runs the attachment's onDeath.
    /// @param step The step about to begin.
    /// @param budget How long it may take.
    /// @return What disarms it when destroyed; inert once the supervisor stopped.
    [[nodiscard]] remote::StepWatch watch(remote::HostStep step, std::chrono::milliseconds budget);

    /// Tells the supervisor what to do when the helper ends, and which lanes to watch. Posts, and returns.
    /// @param attachment Its onDeath runs once, on the supervisor's thread, once the helper ended or was
    ///        killed for a timeout; at once there if that happened already. Its lanes are looked at whenever
    ///        the supervisor wakes, and every tenth of the grace while one is armed: a frame that stays armed
    ///        for the grace, the time its innermost frame serves a host call not counted, has the helper
    ///        killed (KillReason::Grace).
    void attach(remote::HostAttachment attachment);

    /// Wakes the supervisor. Wait-free.
    void nudge() noexcept;

  private:
    friend class SupervisorMailbox;

    SupervisedHelper(std::shared_ptr<SupervisorMailbox> mailbox,
                     uint64_t key,
                     ProcessId processId,
                     transport::HandleTarget target) noexcept;

    std::shared_ptr<SupervisorMailbox> _mailbox;
    std::atomic<uint64_t> _key = 0; ///< The supervisor's name for the helper; 0 once moved from or let go.
    ProcessId _processId = 0;
    transport::HandleTarget _target; ///< What handles are granted to the helper through.
};

/// The supervisor's command queue, and the wake of the set it waits on. Shared and thread-safe; outlives the
/// supervisor, after which it refuses every command.
class SupervisorMailbox: public std::enable_shared_from_this<SupervisorMailbox>
{
    /// Restricts construction to the supervisor, while std::make_shared can call it.
    struct Passkey
    {
        explicit Passkey() = default;
    };

  public:
    /// One command, of the kinds the supervisor's thread processes.
    struct Command;

    /// Made by ExtensionHostSupervisor::start().
    /// @param waitSet What the supervisor waits on; woken by every post.
    /// @param clock What watches are timed by; outlives the supervisor, not necessarily the mailbox.
    SupervisorMailbox(Passkey, std::unique_ptr<IExitWaitSet> waitSet, core::IClock& clock);
    SupervisorMailbox(SupervisorMailbox const&) = delete;
    SupervisorMailbox(SupervisorMailbox&&) = delete;
    SupervisorMailbox& operator=(SupervisorMailbox const&) = delete;
    SupervisorMailbox& operator=(SupervisorMailbox&&) = delete;
    ~SupervisorMailbox();

    /// Launches @p spec on the supervisor's thread, and blocks the caller until it did. Never called on the
    /// supervisor's thread.
    /// @return The helper; why it did not start -- `Stopped` once the supervisor is gone, or can no longer
    ///         wait for its helpers; `Io` if no handle could be granted to it
    ///         (IProcessHandle::handleTarget()), in which case it was killed.
    [[nodiscard]] std::expected<SupervisedHelper, LaunchError> launch(LaunchSpec spec);

  private:
    friend class SupervisedHelper;
    friend class ExtensionHostSupervisor;
    friend class SupervisorLoop;

    /// Whether commands are taken.
    enum class MailboxState : uint8_t
    {
        Closed, ///< The supervisor stopped: every command is dropped.
        Open,   ///< The supervisor takes them.
    };

    /// What became of a posted command.
    enum class Delivery : uint8_t
    {
        Dropped, ///< The mailbox closed: the supervisor is gone.
        Queued,  ///< The supervisor will process it.
    };

    /// Queues @p command and wakes the supervisor.
    /// @return Delivery::Queued if the supervisor will process it; Delivery::Dropped once the mailbox closed.
    [[nodiscard]] Delivery post(Command command) noexcept;

    /// @return Every command queued, in order; the queue empty.
    [[nodiscard]] std::vector<Command> take() noexcept;

    /// Blocks until a command is queued, or @p timeout passed; with no timeout, until a command is queued.
    /// For a supervisor that cannot wait on its set.
    void waitForCommands(std::optional<std::chrono::milliseconds> timeout);

    /// Queues the last command, and closes. @param stop The Stop command.
    void close(Command stop) noexcept;

    /// Refreshes the supervisor's clock first, which an idle supervisor does not: a caching clock would
    /// otherwise tell the time of its last wake-up.
    /// @return The time by the supervisor's clock; nothing once the mailbox closed, when the clock may be
    /// gone.
    [[nodiscard]] std::optional<core::SteadyTimePoint> now() const noexcept;

    /// @return A timer serial not handed out before; never 0, which a helper's teardown timer uses.
    [[nodiscard]] uint32_t nextSerial() noexcept;

    /// Wakes the supervisor. Wait-free.
    void wake() noexcept { _waitSet->wake(); }

    mutable std::mutex _mutex;
    std::condition_variable _posted;          ///< Notified on every post; waited on by waitForCommands().
    MailboxState _state = MailboxState::Open; ///< Guarded by _mutex.
    std::vector<Command> _commands;           ///< Guarded by _mutex.
    std::unique_ptr<IExitWaitSet> _waitSet;   ///< Fixed after construction; waited on by the supervisor only.
    core::IClock* _clock;                     ///< Read only under _mutex while open.
    std::atomic<uint32_t> _nextSerial { 1 };  ///< The next timer serial.
    std::thread::id _supervisorThread;        ///< Written once, before the mailbox is handed out.
};

/// The one thread that owns every extension host process of this hosting process. Its destructor kills every
/// helper, reaps those that end within ShutdownReapBudget (after `teardownGrace` for them to exit on their
/// own, when that is set), and joins: bounded however the helpers behave.
class ExtensionHostSupervisor
{
    struct Passkey
    {
        explicit Passkey() = default;
    };

  public:
    /// Starts the supervisor's thread.
    /// @param config Its launcher and clock, both non-null.
    /// @return The supervisor; or why it could not start.
    [[nodiscard]] static std::expected<std::unique_ptr<ExtensionHostSupervisor>, SupervisorError> start(
        SupervisorConfig config);

    /// Use start(). Public for std::make_unique only.
    ExtensionHostSupervisor(Passkey,
                            std::shared_ptr<SupervisorMailbox> mailbox,
                            std::unique_ptr<SupervisorLoop> loop);
    ExtensionHostSupervisor(ExtensionHostSupervisor const&) = delete;
    ExtensionHostSupervisor(ExtensionHostSupervisor&&) = delete;
    ExtensionHostSupervisor& operator=(ExtensionHostSupervisor const&) = delete;
    ExtensionHostSupervisor& operator=(ExtensionHostSupervisor&&) = delete;

    /// Kills every helper, reaps within the budget, and joins.
    ~ExtensionHostSupervisor();

    /// @return What connectors and handles reach the supervisor through.
    [[nodiscard]] std::shared_ptr<SupervisorMailbox> mailbox() const noexcept { return _mailbox; }

    /// Test hook: returns once everything posted before it was processed -- and every helper that ended by
    /// then, and every timer due by then, with it.
    void sync();

    /// Test hook.
    /// @return What it did so far.
    [[nodiscard]] SupervisorCounters counters() const;

    /// Test hook.
    /// @return The supervisor's thread.
    [[nodiscard]] std::thread::id threadId() const noexcept { return _threadId; }

    /// Test hook.
    /// @return How many helpers were launched and not yet reaped.
    [[nodiscard]] std::size_t liveProcesses() const;

  private:
    std::shared_ptr<SupervisorMailbox> _mailbox;
    std::unique_ptr<SupervisorLoop> _loop;
    std::jthread _thread;
    std::thread::id _threadId;
};

} // namespace vtextension::wasm::remote::process
