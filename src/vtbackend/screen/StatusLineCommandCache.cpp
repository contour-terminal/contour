// SPDX-License-Identifier: Apache-2.0
#include <vtbackend/screen/StatusLineCommandCache.hpp>

#include <vtbackend/Logging.hpp>

#include <core/async/DetachedTask.hpp>
#include <core/async/ResumeOn.hpp>
#include <core/async/StopToken.hpp>

#include <algorithm>
#include <cstdint>
#include <map>
#include <mutex>
#include <optional>
#include <ranges>
#include <utility>

namespace vtbackend
{

namespace
{
    /// Where a command is in its refresh cycle.
    enum class RunState : uint8_t
    {
        NeverRun, ///< No run has been started yet.
        Idle,     ///< The last run finished; the next one is due after the interval.
        Running,  ///< A run is in flight.
    };

    /// Whether the current status-line definition still asks for a command.
    enum class Demand : uint8_t
    {
        Forgotten, ///< The definition changed; the entry schedules no wakeups and notifies nobody.
        Requested, ///< Asked for by the latest output() call since the last forget.
    };
} // namespace

struct StatusLineCommandCache::State
{
    /// Identifies a status-line command: its text and how often it runs.
    struct Key
    {
        std::string command;
        std::chrono::seconds interval;
        auto operator<=>(Key const&) const = default;
    };

    /// The cached result of one command and its scheduling bookkeeping.
    struct Entry
    {
        std::string text;
        Clock::time_point lastStarted;
        Clock::time_point lastRequested;
        Demand demand = Demand::Forgotten;
        RunState runState = RunState::NeverRun;
    };

    State(vtpty::CommandCapture& capture,
          core::async::IExecutor& executor,
          std::function<void(StatusLineCommandOutput)> callback):
        capture { capture }, executor { executor }, onRunCompleted { std::move(callback) }
    {
    }

    /// Runs one command on the executor and files its outcome. A static member with by-value parameters:
    /// the coroutine frame outlives the call that started it.
    static core::async::DetachedTask runJob(std::shared_ptr<State> state,
                                            Key key,
                                            std::chrono::milliseconds timeout)
    {
        co_await core::async::ResumeOn { state->executor };
        auto result = state->capture.run(key.command, timeout, state->stop.get_token());
        state->complete(key, timeout, std::move(result));
    }

    /// Stores a finished run's outcome and tells the owner, unless the run was cancelled or the entry is
    /// no longer requested.
    /// @param key     The command that was run.
    /// @param timeout The bound the run was given, for logging.
    /// @param result  What the run produced.
    void complete(Key const& key,
                  std::chrono::milliseconds timeout,
                  std::expected<std::string, vtpty::CommandCaptureError> result)
    {
        using Kind = vtpty::CommandCaptureError::Kind;
        auto lock = std::unique_lock { mutex };
        auto const it = entries.find(key);
        if (it == entries.end())
            return;
        auto& entry = it->second;
        entry.runState = RunState::Idle;
        auto const error =
            result ? std::optional<vtpty::CommandCaptureError> {} : std::optional { result.error() };
        if (error && error->kind == Kind::Cancelled)
            return;

        auto output = StatusLineCommandOutput::Unchanged;
        if (auto newText =
                error ? spawnFailureText(*error) : std::optional<std::string> { *std::move(result) };
            newText && *newText != entry.text)
        {
            entry.text = *std::move(newText);
            output = StatusLineCommandOutput::Changed;
        }
        // Called under the lock on purpose: the destructor clears the callback under the same lock, so
        // it waits for a notification in flight. A forgotten entry is not on display: nobody waits for it.
        if (entry.demand == Demand::Requested && onRunCompleted)
            onRunCompleted(output);

        if (error && error->kind == Kind::TimedOut)
        {
            lock.unlock(); // Never log under the cache mutex.
            statusLineLog()("Command timed out after {}: {}", timeout, key.command);
        }
    }

    /// @return The message to display for a failed spawn; nullopt for any other error (the text is kept).
    static std::optional<std::string> spawnFailureText(vtpty::CommandCaptureError const& error)
    {
        if (error.kind == vtpty::CommandCaptureError::Kind::SpawnFailed)
            return error.cause.message();
        return std::nullopt;
    }

    std::mutex mutex;
    vtpty::CommandCapture& capture;
    core::async::IExecutor& executor;
    std::function<void(StatusLineCommandOutput)> onRunCompleted;
    core::async::StopSource stop;
    std::map<Key, Entry> entries;
};

StatusLineCommandCache::StatusLineCommandCache(StatusLineCommandServices services,
                                               std::function<void(StatusLineCommandOutput)> onRunCompleted):
    _state { std::make_shared<State>(services.capture, services.executor, std::move(onRunCompleted)) }
{
}

StatusLineCommandCache::~StatusLineCommandCache()
{
    {
        auto const lock = std::scoped_lock { _state->mutex };
        _state->onRunCompleted = nullptr;
    }
    _state->stop.request_stop();
}

std::string StatusLineCommandCache::output(std::string const& command,
                                           std::chrono::seconds interval,
                                           Clock::time_point now)
{
    auto key = State::Key { .command = command, .interval = interval };
    auto text = std::string {};
    auto shouldStart = false;
    {
        auto const lock = std::scoped_lock { _state->mutex };
        auto& entries = _state->entries;
        std::erase_if(entries, [&](auto const& item) {
            auto const& [itemKey, entry] = item;
            return itemKey != key && entry.runState != RunState::Running
                   && now - entry.lastRequested > 2 * itemKey.interval;
        });

        auto& entry = entries[key];
        entry.lastRequested = now;
        entry.demand = Demand::Requested;
        shouldStart = entry.runState == RunState::NeverRun
                      || (entry.runState == RunState::Idle && now - entry.lastStarted >= interval);
        if (shouldStart)
        {
            entry.runState = RunState::Running;
            entry.lastStarted = now;
        }
        text = entry.text;
    }

    if (shouldStart)
        (void) State::runJob(
            _state, std::move(key), std::min<std::chrono::milliseconds>(MaxCommandRuntime, interval));
    return text;
}

void StatusLineCommandCache::forgetRequests()
{
    auto const lock = std::scoped_lock { _state->mutex };
    for (auto& entry: _state->entries | std::views::values)
        entry.demand = Demand::Forgotten;
}

std::optional<std::chrono::milliseconds> StatusLineCommandCache::nextDue(Clock::time_point now) const
{
    auto const lock = std::scoped_lock { _state->mutex };
    auto result = std::optional<std::chrono::milliseconds> {};
    for (auto const& [key, entry]: _state->entries)
    {
        if (entry.demand == Demand::Forgotten)
            continue;
        auto wait = std::chrono::milliseconds::zero();
        if (entry.runState != RunState::NeverRun)
        {
            wait = std::chrono::ceil<std::chrono::milliseconds>(entry.lastStarted + key.interval - now);
            // A run in flight that is already overdue reports its completion to the owner, which re-arms; do
            // not spin.
            if (entry.runState == RunState::Running && wait <= std::chrono::milliseconds::zero())
                continue;
            wait = std::max(std::chrono::milliseconds::zero(), wait);
        }
        result = result ? std::min(*result, wait) : wait;
    }
    return result;
}

} // namespace vtbackend
