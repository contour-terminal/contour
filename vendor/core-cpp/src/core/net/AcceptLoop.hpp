// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// `acceptLoop` -- accepts from a listener until it is closed, answering every failed accept through
/// @c AcceptErrorPolicy, and hands each accepted socket to the caller's flow: awaited before the next
/// accept, or started in a flow of its own. One loop, so `serve` (`HttpServer.hpp`) and `core::http`'s
/// server cannot drift apart in how they treat an `EMFILE` or a dead listener.

#include <core/async/Task.hpp>
#include <core/net/AcceptPolicy.hpp>
#include <core/net/EventLoop.hpp>
#include <core/net/IListener.hpp>
#include <core/net/ISocket.hpp>
#include <core/net/NetExport.hpp>

#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace core::net
{

/// Where an accept loop reports what @c AcceptErrorPolicy decided (`AcceptPolicy.hpp`).
struct AcceptLoopReporting
{
    std::string surface = "http"; ///< What the loop serves, as its reports name it.
    /// Told each report: a failed accept the loop went on past (rate-limited), the loop becoming
    /// degraded on a long run of unclassified failures, recovering from one, and giving up on a dead
    /// listener. @c AcceptLoopHealth::record is written to be handed these. **When empty, each
    /// report's line goes to @c reportDiagnostic instead**, so a loop that degrades or gives up is
    /// never silent by default -- the shape of the incident `AcceptPolicy.hpp` opens with.
    std::function<void(AcceptLoopEvent const&)> onEvent;
};

/// Hands one report to where @p reporting says, or to @c reportDiagnostic when it names nowhere.
/// @param reporting Where reports go.
/// @param event The report.
CORE_NET_API void reportAcceptLoopEvent(AcceptLoopReporting const& reporting, AcceptLoopEvent const& event);

/// What an accept loop does with the flow that serves a connection.
enum class ConnectionFlow : std::uint8_t
{
    Awaited, ///< Awaited before the next accept: one connection at a time, on the loop's own flow.
    Spawned, ///< Started with @c EventLoop::spawn: each connection in a flow of its own.
};

/// What an accept loop calls with each socket it accepts: the flow that serves it. A socket it does
/// not want, it drops (which closes it) and returns a flow that does nothing.
using OnAccepted = std::function<async::Task<void>(std::unique_ptr<ISocket>)>;

/// Accepts from @p listener until it is closed, handing each socket to @p onAccepted.
///
/// **A failed accept is answered by @c AcceptErrorPolicy, and only a closed or dead listener ends
/// the loop.** A connection that failed before it was handed over is accepted past; exhaustion
/// backs off on @p loop's timer and accepts again; a failure nothing classifies is backed off on for
/// as long as it lasts, and a long run of them is reported as the loop being degraded. A listener
/// found dead (@c NetErrorCode::BadHandle) is CLOSED before the loop reports giving up, so its port
/// refuses rather than queues. A loop that stops while degraded says it stopped.
///
/// **When it returns.** At its next accept after the listener is closed, at once when the close lands
/// during a backoff, when the listener is dead, and when the flow is cancelled -- parked in an accept
/// or in a backoff, it returns rather than throwing. The close token is taken while the listener is
/// alive and asked before every call into it, so its owner may close and destroy it at any time on
/// @p loop's thread.
/// @param loop The loop @p listener runs on, never null: its timer waits out a backoff, its clock
///        paces the reports, and a @c ConnectionFlow::Spawned flow runs on it (not owned).
/// @param listener The bound listener (not owned); alive when the flow is first resumed.
/// @param reporting Where the loop's reports go.
/// @param flow Whether each connection's flow is awaited or spawned.
/// @param onAccepted The flow for each accepted socket.
/// @return A task that completes when the loop ends.
[[nodiscard]] CORE_NET_API async::Task<void> acceptLoop(EventLoop* loop,
                                                        IListener* listener,
                                                        AcceptLoopReporting reporting,
                                                        ConnectionFlow flow,
                                                        OnAccepted onAccepted);

} // namespace core::net
