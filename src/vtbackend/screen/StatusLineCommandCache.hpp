// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <vtpty/CommandCapture.hpp>

#include <core/async/IExecutor.hpp>

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>

namespace vtbackend
{

/// What a terminal needs to run status-line commands: who runs them, and where.
/// `capture.run` is called concurrently from executor threads. Both must outlive every terminal given them,
/// and the executor must be destroyed (joined) before the capture.
struct StatusLineCommandServices
{
    vtpty::CommandCapture& capture;   ///< Spawns the commands.
    core::async::IExecutor& executor; ///< Where the blocking runs happen.
};

/// Whether a finished status-line command run changed what the status line displays.
enum class StatusLineCommandOutput : uint8_t
{
    Unchanged, ///< The displayed text is the same; the owner only needs to re-evaluate its next wakeup.
    Changed,   ///< The displayed text changed; the owner must repaint the status line.
};

/// The longest a status-line command may run before it is killed.
inline constexpr auto MaxCommandRuntime = std::chrono::milliseconds { 5000 };

/// Status-line command results, refreshed asynchronously so rendering never waits on a process.
class StatusLineCommandCache
{
  public:
    using Clock = std::chrono::steady_clock;

    /// @param services        Who runs commands, and where.
    /// @param onRunCompleted Called on an executor thread, while the cache's own mutex is held, whenever a
    ///                       run of a still-requested command finishes, except when it was cancelled; it
    ///                       says whether the displayed text changed. Must not take Terminal's state mutex
    ///                       (lock order is state mutex, then cache mutex); must be cheap; must not call
    ///                       output(), nextDue() or forgetRequests() (the mutex is not recursive); must
    ///                       not throw (a detached job would terminate); and must not touch anything
    ///                       destroyed before the cache.
    StatusLineCommandCache(StatusLineCommandServices services,
                           std::function<void(StatusLineCommandOutput)> onRunCompleted);
    ~StatusLineCommandCache();
    StatusLineCommandCache(StatusLineCommandCache const&) = delete;
    StatusLineCommandCache& operator=(StatusLineCommandCache const&) = delete;
    StatusLineCommandCache(StatusLineCommandCache&&) = delete;
    StatusLineCommandCache& operator=(StatusLineCommandCache&&) = delete;

    /// The last result of @p command, never blocking; starts a run when one is due and none is in flight.
    /// @param command  Shell command line.
    /// @param interval How often the command re-runs; must be greater than zero.
    /// @param now      The terminal's current time.
    /// @return The command's last displayed text; empty before its first result.
    [[nodiscard]] std::string output(std::string const& command,
                                     std::chrono::seconds interval,
                                     Clock::time_point now);

    /// The status-line definition changed: every entry stops scheduling wakeups until it is requested again.
    /// Results and runs in flight are kept, so a command still in the new definition neither loses its text
    /// nor runs twice.
    void forgetRequests();

    /// @param now The terminal's current time.
    /// @return How long until the earliest requested command is due again, or nullopt when none qualifies.
    ///         A run in flight counts until its interval is overdue, after which its completion
    ///         notifies the owner instead.
    [[nodiscard]] std::optional<std::chrono::milliseconds> nextDue(Clock::time_point now) const;

  private:
    struct State;
    std::shared_ptr<State> _state;
};

} // namespace vtbackend
