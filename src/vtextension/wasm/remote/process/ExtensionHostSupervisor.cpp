// SPDX-License-Identifier: Apache-2.0
#include <vtextension/wasm/remote/process/ExtensionHostSupervisor.hpp>

#include <core/Assert.hpp>
#include <core/Overloaded.hpp>
#include <core/log/LogStore.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <functional>
#include <future>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <ranges>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>
#include <variant>
#include <vector>

#include <vtextension/wasm/remote/HostConnector.hpp>
#include <vtextension/wasm/remote/process/HostTimers.hpp>

namespace vtextension::wasm::remote::process
{

namespace
{
    /// The serial of a let-go helper's teardown timer; no watch is handed it
    /// (SupervisorMailbox::nextSerial()).
    constexpr auto TeardownSerial = uint32_t { 0 };

    /// How often a supervisor that can no longer wait on its set looks for ended helpers. The one wait that
    /// is not the injected clock's: it is a broken system's fallback, never a timeout anyone relies on.
    constexpr auto DegradedPollPeriod = std::chrono::milliseconds { 100 };

    /// How a process ends when the supervisor kills it: SIGKILL (9) on POSIX, KilledExitCode on Windows.
    constexpr auto KillStatuses = std::array {
        ExitStatus { .kind = ExitKind::Signalled, .value = 9 },
        ExitStatus { .kind = ExitKind::Exited, .value = static_cast<int>(KilledExitCode) },
    };

    /// The status of a helper that ended as it should: by exiting once its channels closed.
    constexpr auto CleanExit = ExitStatus { .kind = ExitKind::Exited, .value = 0 };

    /// How a log line names a step.
    constexpr auto StepNames = std::array<std::string_view, 3> {
        "start", // HostStep::Handshake
        "load a module",
        "instantiate a module",
    };
    static_assert(std::to_underlying(HostStep::Handshake) == 0 && std::to_underlying(HostStep::Load) == 1
                  && std::to_underlying(HostStep::Instantiate) == 2);

    /// A helper launched: what the launching caller gets back.
    struct LaunchedProcess
    {
        uint64_t key = 0;
        ProcessId processId = 0;
    };

    using LaunchResult = std::expected<LaunchedProcess, LaunchError>;

    struct LaunchCommand
    {
        LaunchSpec spec;
        std::promise<LaunchResult> launched;
    };

    struct WatchCommand
    {
        TimerKey timer;
        HostStep step;
        core::SteadyTimePoint start;
        std::chrono::milliseconds budget;
    };

    struct DisarmCommand
    {
        TimerKey timer;
    };

    struct AttachCommand
    {
        uint64_t helper;
        HostAttachment attachment;
    };

    struct KillCommand
    {
        uint64_t helper;
    };

    struct DropCommand
    {
        uint64_t helper;
    };

    struct SyncCommand
    {
        std::promise<void> done;
    };

    struct StopCommand
    {
    };
} // namespace

struct SupervisorMailbox::Command
{
    std::variant<LaunchCommand,
                 WatchCommand,
                 DisarmCommand,
                 AttachCommand,
                 KillCommand,
                 DropCommand,
                 SyncCommand,
                 StopCommand>
        value;
};

// {{{ SupervisedHelper

SupervisedHelper::SupervisedHelper(std::shared_ptr<SupervisorMailbox> mailbox,
                                   uint64_t key,
                                   ProcessId processId) noexcept:
    _mailbox { std::move(mailbox) }, _key { key }, _processId { processId }
{
}

SupervisedHelper::SupervisedHelper(SupervisedHelper&& other) noexcept:
    _mailbox { std::move(other._mailbox) },
    _key { std::exchange(other._key, 0) },
    _processId { std::exchange(other._processId, 0) }
{
}

SupervisedHelper& SupervisedHelper::operator=(SupervisedHelper&& other) noexcept
{
    if (this != &other)
    {
        release();
        _mailbox = std::move(other._mailbox);
        _key = std::exchange(other._key, 0);
        _processId = std::exchange(other._processId, 0);
    }
    return *this;
}

SupervisedHelper::~SupervisedHelper()
{
    release();
}

void SupervisedHelper::release() noexcept
{
    if (_key != 0)
        _mailbox->post(SupervisorMailbox::Command { DropCommand { .helper = std::exchange(_key, 0) } });
}

void SupervisedHelper::kill() noexcept
{
    if (_key != 0)
        _mailbox->post(SupervisorMailbox::Command { KillCommand { .helper = _key } });
}

StepWatch SupervisedHelper::watch(HostStep step, std::chrono::milliseconds budget)
{
    if (_key == 0)
        return StepWatch {};
    // Timed from the caller's instant, not from when the supervisor gets to it.
    auto const start = _mailbox->now();
    if (!start)
        return StepWatch {};
    auto const timer = TimerKey { .helper = _key, .serial = _mailbox->nextSerial() };
    if (!_mailbox->post(SupervisorMailbox::Command {
            WatchCommand { .timer = timer, .step = step, .start = *start, .budget = budget } }))
        return StepWatch {};
    return StepWatch { [mailbox = _mailbox, timer] {
        mailbox->post(SupervisorMailbox::Command { DisarmCommand { .timer = timer } });
    } };
}

void SupervisedHelper::attach(HostAttachment attachment)
{
    if (_key != 0)
        _mailbox->post(SupervisorMailbox::Command {
            AttachCommand { .helper = _key, .attachment = std::move(attachment) } });
}

void SupervisedHelper::nudge() noexcept
{
    if (_mailbox)
        _mailbox->wake();
}

// }}}
// {{{ SupervisorMailbox

SupervisorMailbox::SupervisorMailbox(Passkey /*passkey*/,
                                     std::unique_ptr<IExitWaitSet> waitSet,
                                     core::IClock& clock):
    _waitSet { std::move(waitSet) }, _clock { &clock }
{
}

SupervisorMailbox::~SupervisorMailbox() = default;

std::expected<SupervisedHelper, LaunchError> SupervisorMailbox::launch(LaunchSpec spec)
{
    // The supervisor would wait for itself.
    Require(std::this_thread::get_id() != _supervisorThread);
    auto launched = std::promise<LaunchResult> {};
    auto result = launched.get_future();
    if (!post(Command { LaunchCommand { .spec = std::move(spec), .launched = std::move(launched) } }))
        return std::unexpected(LaunchError::Stopped);
    auto const outcome = [&]() -> LaunchResult {
        try
        {
            return result.get();
        }
        catch (std::future_error const&)
        {
            // The supervisor stopped before it got to it.
            return std::unexpected(LaunchError::Stopped);
        }
    }();
    return outcome.transform([&](LaunchedProcess const& process) {
        return SupervisedHelper { shared_from_this(), process.key, process.processId };
    });
}

bool SupervisorMailbox::post(Command command) noexcept
{
    {
        auto const lock = std::scoped_lock { _mutex };
        if (_state == MailboxState::Closed)
            return false;
        _commands.push_back(std::move(command));
    }
    _posted.notify_one();
    wake();
    return true;
}

std::vector<SupervisorMailbox::Command> SupervisorMailbox::take() noexcept
{
    auto const lock = std::scoped_lock { _mutex };
    return std::exchange(_commands, {});
}

void SupervisorMailbox::waitForCommands(std::chrono::milliseconds timeout)
{
    auto lock = std::unique_lock { _mutex };
    std::ignore = _posted.wait_for(lock, timeout, [this] { return !_commands.empty(); });
}

void SupervisorMailbox::close(Command stop) noexcept
{
    {
        auto const lock = std::scoped_lock { _mutex };
        _commands.push_back(std::move(stop));
        _state = MailboxState::Closed;
    }
    _posted.notify_one();
    wake();
}

std::optional<core::SteadyTimePoint> SupervisorMailbox::now() const noexcept
{
    auto const lock = std::scoped_lock { _mutex };
    if (_state == MailboxState::Closed)
        return std::nullopt;
    return _clock->now();
}

uint32_t SupervisorMailbox::nextSerial() noexcept
{
    auto serial = _nextSerial.fetch_add(1);
    if (serial == TeardownSerial) // after 2^32 watches
        serial = _nextSerial.fetch_add(1);
    return serial;
}

// }}}
// {{{ SupervisorLoop

/// The supervisor's thread: takes the mailbox's commands, reaps what ended, expires what is due, and waits on
/// the set until any of that changes. Everything but the published counters is its thread's alone.
class SupervisorLoop
{
  public:
    SupervisorLoop(SupervisorConfig config, SupervisorMailbox& mailbox) noexcept:
        _launcher { std::move(config.launcher) },
        _clock { *config.clock },
        _teardownGrace { config.teardownGrace },
        _mailbox { mailbox },
        _waitSet { *mailbox._waitSet }
    {
    }

    /// Runs until the Stop command, then stops every helper (stop()).
    void run();

    /// @return The counters, as of the latest batch processed.
    [[nodiscard]] SupervisorCounters counters() const
    {
        auto const lock = std::scoped_lock { _publishedMutex };
        return _published;
    }

    /// @return How many helpers were not reaped, as of the latest batch processed.
    [[nodiscard]] std::size_t liveProcesses() const
    {
        auto const lock = std::scoped_lock { _publishedMutex };
        return _publishedLive;
    }

  private:
    /// Whether onDeath ran.
    enum class DeathNotice : uint8_t
    {
        Alive, ///< Not dead: it runs and was not killed for a timeout.
        Due,   ///< Dead, with no onDeath attached yet: attach() runs it at once.
        Given, ///< onDeath ran.
    };

    /// Whether a holder still holds the helper.
    enum class Holding : uint8_t
    {
        Held,    ///< A SupervisedHelper holds it.
        Dropped, ///< Let go: forgotten once reaped.
    };

    /// Whether the supervisor killed the helper, which tells a kill's status from a crash.
    enum class KillState : uint8_t
    {
        None, ///< Never killed by the supervisor.
        Sent, ///< Killed by the supervisor.
    };

    /// Whether an end was noticed while running, or while stopping.
    enum class Observation : uint8_t
    {
        Running,
        Stopping,
    };

    /// Whether the loop goes on.
    enum class LoopState : uint8_t
    {
        Stopping, ///< The Stop command came.
        Running,  ///< It waits for more.
    };

    /// Whether the wait set still works.
    enum class WaitState : uint8_t
    {
        Broken,  ///< A wait failed: the loop polls, and launches nothing more.
        Working, ///< It waits on the set.
    };

    /// One helper.
    struct Supervised
    {
        std::unique_ptr<IProcessHandle> process; ///< Null once reaped.
        std::function<void()> onDeath;           ///< The attachment's; null until attached, and once run.
        DeathNotice notice = DeathNotice::Alive;
        Holding holding = Holding::Held;
        KillState killed = KillState::None;
        std::map<uint32_t, HostStep> steps; ///< The steps its armed watches watch, by serial.
    };

    void launch(LaunchCommand& command);
    void watch(WatchCommand const& command);
    void disarm(DisarmCommand const& command);
    void attach(AttachCommand& command);
    void kill(KillCommand const& command);
    void drop(DropCommand const& command);

    /// Reaps every helper that ended. @param observation Whether the loop still runs.
    void reapEnded(Observation observation);

    /// Kills each helper whose timer is due.
    void expireTimers();

    /// Blocks until the set reports an end, a command wakes it, or the next timer is due.
    void waitForWork();

    /// Kills every helper and reaps what ends within the budget, after `teardownGrace` for them to exit.
    void stop();

    /// Reaps until no helper is left, or @p deadline (by the injected clock) passed.
    void reapUntil(core::SteadyTimePoint deadline);

    /// Sends @p helper a kill for @p reason, unless it was reaped.
    void killProcess(Supervised& helper, KillReason reason) noexcept;

    /// Declares @p helper dead: runs its onDeath now, or once attached.
    static void announceDeath(Supervised& helper);

    /// Makes counters() and liveProcesses() tell what is true now.
    void publish();

    /// @return Whether @p status is an end the supervisor did not cause, by a helper it killed or not.
    [[nodiscard]] static bool isUnclean(ExitStatus status, KillState killed) noexcept
    {
        auto const isOwnKill = killed == KillState::Sent && std::ranges::contains(KillStatuses, status);
        return status != CleanExit && !isOwnKill;
    }

    /// @return How many helpers were not reaped.
    [[nodiscard]] std::size_t liveCount() const noexcept
    {
        return static_cast<std::size_t>(std::ranges::count_if(
            _helpers, [](auto const& entry) { return entry.second.process != nullptr; }));
    }

    std::shared_ptr<IProcessLauncher> _launcher;
    core::IClock& _clock;
    std::chrono::milliseconds _teardownGrace;
    SupervisorMailbox& _mailbox;
    IExitWaitSet& _waitSet;
    HostTimers _timers;
    std::map<uint64_t, Supervised> _helpers;
    uint64_t _nextKey = 1;
    WaitState _wait = WaitState::Working;
    SupervisorCounters _counters;

    mutable std::mutex _publishedMutex;
    SupervisorCounters _published;  ///< Guarded by _publishedMutex.
    std::size_t _publishedLive = 0; ///< Guarded by _publishedMutex.
};

void SupervisorLoop::run()
{
    auto state = LoopState::Running;
    while (state == LoopState::Running)
    {
        _clock.refresh();
        auto syncs = std::vector<std::promise<void>> {};
        for (auto& command: _mailbox.take())
            std::visit(core::Overloaded {
                           [this](LaunchCommand& launched) { launch(launched); },
                           [this](WatchCommand const& watched) { watch(watched); },
                           [this](DisarmCommand const& disarmed) { disarm(disarmed); },
                           [this](AttachCommand& attached) { attach(attached); },
                           [this](KillCommand const& killed) { kill(killed); },
                           [this](DropCommand const& dropped) { drop(dropped); },
                           [&](SyncCommand& synced) { syncs.push_back(std::move(synced.done)); },
                           [&](StopCommand const& /*stopped*/) { state = LoopState::Stopping; },
                       },
                       command.value);
        // What ended or fell due by now is part of this batch: a sync() returns after it.
        reapEnded(Observation::Running);
        expireTimers();
        reapEnded(Observation::Running);
        publish();
        for (auto& sync: syncs)
            sync.set_value();
        if (state == LoopState::Running)
            waitForWork();
    }
    stop();
    publish();
}

void SupervisorLoop::launch(LaunchCommand& command)
{
    if (_wait == WaitState::Broken)
    {
        command.launched.set_value(std::unexpected(LaunchError::Stopped));
        return;
    }
    auto process = _launcher->launch(command.spec);
    if (!process)
    {
        command.launched.set_value(std::unexpected(process.error()));
        return;
    }
    auto const key = _nextKey++;
    if (auto const watched = _waitSet.watch(key, **process); !watched)
    {
        // A helper nobody can wait for does not run: killed, and reaped when the loop next looks.
        (*process)->kill();
        _helpers.emplace(key,
                         Supervised { .process = std::move(*process),
                                      .onDeath = nullptr,
                                      .notice = DeathNotice::Given,
                                      .holding = Holding::Dropped,
                                      .killed = KillState::Sent,
                                      .steps = {} });
        command.launched.set_value(std::unexpected(watched.error()));
        return;
    }
    ++_counters.launches;
    auto const processId = (*process)->id();
    _helpers.emplace(key,
                     Supervised { .process = std::move(*process),
                                  .onDeath = nullptr,
                                  .notice = DeathNotice::Alive,
                                  .holding = Holding::Held,
                                  .killed = KillState::None,
                                  .steps = {} });
    command.launched.set_value(LaunchedProcess { .key = key, .processId = processId });
}

void SupervisorLoop::watch(WatchCommand const& command)
{
    auto const helper = _helpers.find(command.timer.helper);
    // A helper that ended, or was let go, takes no step any more.
    if (helper == _helpers.end() || !helper->second.process || helper->second.holding == Holding::Dropped)
        return;
    _timers.arm(command.timer, command.start, command.budget);
    helper->second.steps.insert_or_assign(command.timer.serial, command.step);
}

void SupervisorLoop::disarm(DisarmCommand const& command)
{
    _timers.disarm(command.timer);
    if (auto const helper = _helpers.find(command.timer.helper); helper != _helpers.end())
        helper->second.steps.erase(command.timer.serial);
}

void SupervisorLoop::attach(AttachCommand& command)
{
    auto const helper = _helpers.find(command.helper);
    if (helper == _helpers.end() || !command.attachment.onDeath)
        return;
    auto& supervised = helper->second;
    if (supervised.notice == DeathNotice::Alive)
    {
        supervised.onDeath = std::move(command.attachment.onDeath);
        return;
    }
    supervised.notice = DeathNotice::Given;
    command.attachment.onDeath();
}

void SupervisorLoop::kill(KillCommand const& command)
{
    if (auto const helper = _helpers.find(command.helper); helper != _helpers.end())
        killProcess(helper->second, KillReason::Requested);
}

void SupervisorLoop::drop(DropCommand const& command)
{
    auto const helper = _helpers.find(command.helper);
    if (helper == _helpers.end())
        return;
    auto& supervised = helper->second;
    supervised.holding = Holding::Dropped;
    supervised.onDeath = nullptr;
    supervised.steps.clear();
    _timers.disarmHelper(command.helper);
    if (!supervised.process)
    {
        _helpers.erase(helper);
        return;
    }
    if (supervised.killed == KillState::Sent)
        return; // dying already: reaped, and forgotten, when it is gone
    if (_teardownGrace > std::chrono::milliseconds::zero())
    {
        // Its channels close as its holder goes: it may exit on its own, writing what it has to, first.
        _timers.arm(
            TimerKey { .helper = command.helper, .serial = TeardownSerial }, _clock.now(), _teardownGrace);
        return;
    }
    killProcess(supervised, KillReason::Dropped);
}

void SupervisorLoop::reapEnded(Observation observation)
{
    auto ended = std::vector<std::pair<uint64_t, ExitStatus>> {};
    for (auto const& [key, helper]: _helpers)
        if (helper.process)
            if (auto const status = helper.process->tryReap())
                ended.emplace_back(key, *status);
    for (auto const& [key, status]: ended)
    {
        auto& helper = _helpers.at(key);
        _waitSet.unwatch(key);
        helper.process.reset();
        helper.steps.clear();
        _timers.disarmHelper(key);
        ++_counters.reaped;
        if (observation == Observation::Running)
            ++_counters.deathsObserved;
        if (isUnclean(status, helper.killed))
        {
            ++_counters.uncleanExits;
            errorLog()("An extension host ended unexpectedly ({} {}).",
                       status.kind == ExitKind::Exited ? "exit code" : "signal",
                       status.value);
        }
        announceDeath(helper);
        if (helper.holding == Holding::Dropped)
            _helpers.erase(key);
    }
}

void SupervisorLoop::expireTimers()
{
    for (auto const& timer: _timers.expire(_clock.now()))
    {
        auto const helper = _helpers.find(timer.helper);
        if (helper == _helpers.end())
            continue;
        auto& supervised = helper->second;
        if (timer.serial == TeardownSerial)
        {
            killProcess(supervised, KillReason::Dropped);
            continue;
        }
        if (auto const step = supervised.steps.find(timer.serial); step != supervised.steps.end())
        {
            errorLog()("An extension host took too long to {}; it was stopped.",
                       StepNames.at(std::to_underlying(step->second)));
            supervised.steps.erase(step);
        }
        killProcess(supervised, KillReason::StepTimeout);
        announceDeath(supervised);
    }
}

void SupervisorLoop::waitForWork()
{
    if (_wait == WaitState::Broken)
    {
        _mailbox.waitForCommands(DegradedPollPeriod);
        return;
    }
    if (_waitSet.wait(_timers.nextDeadline()))
        return;
    // Waiting again would fail again at once. Without the set no end is noticed in time, and no timeout fires
    // in time: every helper is stopped, none is started any more, and the loop polls for the ends and for its
    // commands until it is told to stop.
    _wait = WaitState::Broken;
    errorLog()("Contour can no longer wait for its extension hosts; it stops them all and starts no more.");
    for (auto& [key, helper]: _helpers)
    {
        killProcess(helper, KillReason::Shutdown);
        announceDeath(helper);
    }
}

void SupervisorLoop::stop()
{
    if (_teardownGrace > std::chrono::milliseconds::zero())
        reapUntil(_clock.now() + _teardownGrace);
    for (auto& [key, helper]: _helpers)
        killProcess(helper, KillReason::Shutdown);
    reapUntil(_clock.now() + ShutdownReapBudget);
    for (auto& [key, helper]: _helpers)
    {
        if (helper.process)
            _waitSet.unwatch(key);
        announceDeath(helper);
    }
    // A handle that the system binds its process's life to ends it as it goes (Windows, FreeBSD).
    _helpers.clear();
}

void SupervisorLoop::reapUntil(core::SteadyTimePoint deadline)
{
    while (true)
    {
        _clock.refresh();
        reapEnded(Observation::Stopping);
        if (liveCount() == 0 || _clock.now() >= deadline)
            return;
        if (_wait == WaitState::Broken)
            _mailbox.waitForCommands(DegradedPollPeriod);
        else if (!_waitSet.wait(deadline))
            _wait = WaitState::Broken;
    }
}

void SupervisorLoop::killProcess(Supervised& helper, KillReason reason) noexcept
{
    if (!helper.process)
        return;
    helper.process->kill();
    helper.killed = KillState::Sent;
    ++_counters.kills.at(std::to_underlying(reason));
}

void SupervisorLoop::announceDeath(Supervised& helper)
{
    if (helper.notice != DeathNotice::Alive)
        return;
    if (!helper.onDeath)
    {
        helper.notice = DeathNotice::Due;
        return;
    }
    helper.notice = DeathNotice::Given;
    std::exchange(helper.onDeath, nullptr)();
}

void SupervisorLoop::publish()
{
    auto const live = liveCount();
    auto const lock = std::scoped_lock { _publishedMutex };
    _published = _counters;
    _publishedLive = live;
}

// }}}
// {{{ ExtensionHostSupervisor

std::expected<std::unique_ptr<ExtensionHostSupervisor>, SupervisorError> ExtensionHostSupervisor::start(
    SupervisorConfig config)
{
    Require(config.launcher != nullptr && config.clock != nullptr);
    auto waitSet = config.launcher->makeWaitSet(*config.clock);
    if (!waitSet)
        return std::unexpected(SupervisorError::WaitSetUnavailable);
    auto mailbox = std::make_shared<SupervisorMailbox>(
        SupervisorMailbox::Passkey {}, std::move(*waitSet), *config.clock);
    auto loop = std::make_unique<SupervisorLoop>(std::move(config), *mailbox);
    try
    {
        return std::make_unique<ExtensionHostSupervisor>(Passkey {}, std::move(mailbox), std::move(loop));
    }
    catch (std::system_error const&)
    {
        return std::unexpected(SupervisorError::ThreadUnavailable);
    }
}

ExtensionHostSupervisor::ExtensionHostSupervisor(Passkey /*passkey*/,
                                                 std::shared_ptr<SupervisorMailbox> mailbox,
                                                 std::unique_ptr<SupervisorLoop> loop):
    _mailbox { std::move(mailbox) },
    _loop { std::move(loop) },
    _thread { [loop = _loop.get()] { loop->run(); } },
    _threadId { _thread.get_id() }
{
    _mailbox->_supervisorThread = _threadId;
}

ExtensionHostSupervisor::~ExtensionHostSupervisor()
{
    _mailbox->close(SupervisorMailbox::Command { StopCommand {} });
    _thread.join();
}

void ExtensionHostSupervisor::sync()
{
    auto done = std::promise<void> {};
    auto processed = done.get_future();
    if (_mailbox->post(SupervisorMailbox::Command { SyncCommand { .done = std::move(done) } }))
        processed.wait();
}

SupervisorCounters ExtensionHostSupervisor::counters() const
{
    return _loop->counters();
}

std::size_t ExtensionHostSupervisor::liveProcesses() const
{
    return _loop->liveProcesses();
}

// }}}

} // namespace vtextension::wasm::remote::process
