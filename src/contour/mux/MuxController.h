// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// Shared scaffolding for the GUI's mux controllers — the native-attach engine
/// (`AttachController`) and the tmux -CC mirror (`TmuxController`). Both run a
/// Qt-free client on a `MuxLoopThread` reactor and expose the same connection
/// handshake to the GUI thread. This header single-sources the parts that must
/// stay identical between them: the connection phase, the blocking
/// connect-and-wait, and the fallback pty handed out when a session must be born
/// with nothing pending.

#include <vtbackend/primitives.h>

#include <vtpty/Pty.h>

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>

namespace contour
{

/// A mux controller's connection lifecycle. `connectAndWait` blocks until the
/// phase leaves Connecting; the reactor's serve-loop epilogue settles it to
/// Failed or Closed.
enum class MuxConnectPhase : uint8_t
{
    Connecting, ///< The reactor started; no session/pane discovered yet.
    Ready,      ///< The first unit of remote structure arrived (handshake done).
    Failed,     ///< The connection could not be established.
    Closed,     ///< The connection ended after having been Ready.
};

/// The result of `awaitMuxConnect`: exactly one of ready/timedOut holds, or
/// neither (the phase reached Failed/Closed without a timeout).
struct MuxConnectOutcome
{
    bool ready = false;    ///< The phase reached Ready.
    bool timedOut = false; ///< The wait elapsed while still Connecting.
    std::string failure;   ///< The recorded failure reason (empty if none).
};

/// Blocks the calling (GUI) thread until @p phase leaves Connecting or @p timeout
/// elapses — the condition-variable handshake both controllers share. The caller
/// owns @p mutex / @p cv / @p phase / @p failure (its own members) and maps the
/// outcome to its own messages: on `timedOut` it must run its `stop()`.
/// @param mutex Guards @p phase and @p failure (locked internally).
/// @param cv Notified by the reactor on every phase transition.
/// @param phase The controller's connection phase.
/// @param failure The controller's failure reason (copied out when not Ready).
/// @param timeout How long to wait before giving up.
/// @return The outcome; see @c MuxConnectOutcome.
[[nodiscard]] MuxConnectOutcome awaitMuxConnect(std::mutex& mutex,
                                                std::condition_variable& cv,
                                                MuxConnectPhase const& phase,
                                                std::string const& failure,
                                                std::chrono::milliseconds timeout);

/// The dead-end pty a controller hands out when a session must be created but
/// none is pending (the creation guards should have prevented this). Sized to
/// @p pageSize or a sane default. Shared so both controllers behave identically.
/// @param pageSize The requested size, if any.
/// @return An unbound `ChannelPty` the session can drive and close cleanly.
[[nodiscard]] std::unique_ptr<vtpty::Pty> makeUnboundFallbackPty(std::optional<vtbackend::PageSize> pageSize);

} // namespace contour
