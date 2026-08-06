// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// `LastSessionWatcher` — ends an auto-spawned daemon together with its last session.

#include <functional>
#include <utility>

#include <net/EventLoop.hpp>
#include <vthost/SessionHost.hpp>
#include <vtworkspace/SessionModel.hpp>

namespace vthost
{

/// Asks the daemon to shut down once the LAST hosted session is gone — the whole lifecycle rule of
/// a daemon nobody started by hand (see DaemonLifecycle::ExitWhenEmpty, and the "Daemon lifetime"
/// section of docs/internals/vthost.md for how it fits the shutdown paths).
///
/// Constructing one arms it: it subscribes itself to @p host and unsubscribes on destruction, so
/// there is no second setup call and no unsubscribed state to reason about.
///
/// EDGE-triggered, deliberately: the daemon binds its sockets with ZERO sessions and only gains one
/// when a client asks (attaching to a daemon with no tabs spawns the first session, see
/// NativeSession), so a level check would end it before anyone could attach.
///
/// The decision is then DEFERRED one pump, for three reasons — none an optimization:
///
/// - `sessionClosed` fires from inside SessionHost's `_streamSubscribers` loop, which the shutdown
///   would mutate as connection flows unwind and their subscriptions unregister.
/// - Buffered input is dispatched without touching the reactor, so a `ClosePane` followed by a
///   `CreateTab` — or tmux's `kill-pane` then `new-window` — arriving in ONE read is fully
///   dispatched before the loop regains control. Deciding synchronously would kill a daemon that is
///   about to be handed a new session.
/// - It gives each connection's write queue one pump to flush the layout change the close itself
///   produced, before requestStop cancels the drains.
///
/// The posted callback therefore RE-READS the live session count rather than trusting the edge that
/// armed it.
///
/// Lifetime: the posted callback captures `this`, which is sound because posted callbacks run only
/// from `EventLoop::runPostedCallbacks` on the pump, and `~EventLoop` drains the ready queue without
/// running pending posts — so one still queued when `blockOn` returns is never invoked.
class LastSessionWatcher final: public SessionStreamEvents
{
  public:
    /// @param host The host whose closes are observed; its session count is re-read live.
    /// @param loop The loop the deferred decision is posted onto.
    /// @param requestShutdown Invoked on the loop thread once no session remains.
    LastSessionWatcher(SessionHost& host, net::EventLoop& loop, std::function<void()> requestShutdown):
        _host(host),
        _loop(loop),
        _requestShutdown(std::move(requestShutdown)),
        _subscription(host, *this, &SessionHost::subscribeStream, &SessionHost::unsubscribeStream)
    {
    }

    void sessionClosed(vtworkspace::SessionId /*session*/) override
    {
        // At most one close can be the last one, so this arms at most once per zero-transition —
        // no latch needed.
        if (_host.sessionCount() != 0)
            return;

        _loop.post([this] {
            if (_host.sessionCount() != 0)
                return; // a session arrived while we were queued; stay alive
            _requestShutdown();
        });
    }

  private:
    SessionHost& _host;
    net::EventLoop& _loop;
    std::function<void()> _requestShutdown;
    /// Last member: registers `*this` only once every member above it is initialized. Spelled out
    /// rather than via makeScopedStreamSubscription, which every other call site uses: that factory
    /// returns by value and ScopedSubscription is non-movable, so it cannot initialize a member.
    ScopedStreamSubscription _subscription;
};

} // namespace vthost
