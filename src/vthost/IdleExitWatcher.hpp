// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// `IdleExitWatcher` — ends an on-demand daemon once it has had nothing to serve for a grace period.

#include <core/net/DeadlineTimer.hpp>
#include <core/net/EventLoop.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <utility>

#include <vthost/ClientCensus.hpp>
#include <vthost/SessionHost.hpp>
#include <vtworkspace/SessionModel.hpp>

namespace vthost
{

/// Whether a daemon has anything to serve.
enum class DaemonActivity : std::uint8_t
{
    Idle = 0, ///< No session is hosted and no client is attached.
    Busy,     ///< At least one session or one client exists.
};

/// @param sessions How many sessions the daemon hosts.
/// @param clients How many clients are attached.
/// @return Idle iff both are zero.
[[nodiscard]] constexpr DaemonActivity daemonActivity(std::size_t sessions, std::size_t clients) noexcept
{
    return sessions == 0 && clients == 0 ? DaemonActivity::Idle : DaemonActivity::Busy;
}

/// Asks the daemon to shut down once it has been Idle for a whole grace period (see
/// DaemonLifecycle::ExitWhenIdle).
///
/// LEVEL-triggered with a grace, unlike LastSessionWatcher's edge: an on-demand daemon starts with no
/// session and no client, so it is Idle from the first instant and must give the client that spawned it
/// time to attach. Evaluated at construction, on every client-count change and one pump after every
/// session close (posted, for the reasons given at LastSessionWatcher). Idle arms a deadline unless one is
/// armed; Busy disarms it.
///
/// The deadline callback RE-READS both counts before asking for the shutdown: a client may have been
/// admitted, or a session created, in the turn the deadline passed but before the loop ran the timer —
/// and a session creation fires no event this watcher hears.
///
/// It subscribes to @p host's stream only to hear `sessionClosed` and never applies a client size, so it
/// is not a client itself. Constructing it arms it; destroying it unsubscribes and disarms.
///
/// Lifetime: the posted callback and the timer capture `this`. As with LastSessionWatcher, the watcher
/// must outlive the owner's LoopDrain, which runs the posts still queued when `blockOn` returns and may
/// fire the deadline. The shutdown either then asks for is harmless, for the reason LastSessionWatcher
/// gives. The timer is disarmed by the destructor, while the loop still exists.
class IdleExitWatcher final: public SessionStreamEvents, public CensusEvents
{
  public:
    /// @param host The host whose session count is re-read live and whose closes are observed.
    /// @param census The client count, re-read live and observed.
    /// @param loop The loop whose clock and thread gate the deadline.
    /// @param grace How long the daemon stays Idle before it asks to end.
    /// @param requestShutdown Invoked on the loop thread when the grace elapsed and the daemon is still Idle.
    IdleExitWatcher(SessionHost& host,
                    ClientCensus& census,
                    core::net::EventLoop& loop,
                    std::chrono::seconds grace,
                    std::function<void()> requestShutdown):
        _host(host),
        _census(census),
        _loop(loop),
        _grace(grace),
        _requestShutdown(std::move(requestShutdown)),
        _subscription(host, *this, &SessionHost::subscribeStream, &SessionHost::unsubscribeStream)
    {
        _census.subscribe(this);
        evaluate();
    }

    IdleExitWatcher(IdleExitWatcher const&) = delete;
    IdleExitWatcher& operator=(IdleExitWatcher const&) = delete;
    IdleExitWatcher(IdleExitWatcher&&) = delete;
    IdleExitWatcher& operator=(IdleExitWatcher&&) = delete;

    ~IdleExitWatcher() override { _census.unsubscribe(this); }

    /// The daemon's own observer, which shows no screen: it keeps no indicator row repainted.
    [[nodiscard]] bool mirrorsScreens() const noexcept override { return false; }

    /// @return Whether a deadline is pending, i.e. the daemon is counting down its grace.
    [[nodiscard]] bool isArmed() const noexcept { return _timer && !_timer->settled(); }

    void clientCountChanged(std::size_t /*count*/) override { evaluate(); }

    void sessionClosed(vtworkspace::SessionId /*session*/) override
    {
        // Deferred one pump: this fires inside SessionHost's subscriber loop, and a ClosePane followed
        // by a CreateTab in one read must be fully dispatched before the decision is taken.
        _loop.post([this] { evaluate(); });
    }

  private:
    [[nodiscard]] DaemonActivity activity() const noexcept
    {
        return daemonActivity(_host.sessionCount(), _census.count());
    }

    void evaluate()
    {
        if (activity() == DaemonActivity::Busy)
        {
            if (_timer)
                _timer->disarm();
        }
        else if (!isArmed())
            _timer.emplace(_loop, _loop.clock().now() + _grace, &IdleExitWatcher::expired, this);
    }

    static void expired(void* self) { static_cast<IdleExitWatcher*>(self)->onExpired(); }

    void onExpired()
    {
        if (activity() == DaemonActivity::Idle)
            _requestShutdown();
    }

    SessionHost& _host;
    ClientCensus& _census;
    core::net::EventLoop& _loop;
    std::chrono::seconds _grace;
    std::function<void()> _requestShutdown;
    std::optional<core::net::DeadlineTimer> _timer;
    /// Last member, as in LastSessionWatcher: registers `*this` only once the members above are ready.
    ScopedStreamSubscription _subscription;
};

} // namespace vthost
