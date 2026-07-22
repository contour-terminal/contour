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

#include <vtpty/ChannelPty.h>
#include <vtpty/Pty.h>

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>

namespace contour
{

class MuxLoopThread;

/// A ChannelPty that runs an on-destroy callback when the owning terminal
/// destroys it — so each controller drops the pty's binding without its own
/// self-unbinding subclass. The callback (typically a controller's unbind,
/// keyed by the pane/session id) fires exactly once, from the destructor,
/// serialized against the controller by that unbind's own lock.
class SelfUnbindingChannelPty final: public vtpty::ChannelPty
{
  public:
    /// @param size The initial page size.
    /// @param onDestroy Invoked once from the destructor (e.g. controller.unbind(id)).
    SelfUnbindingChannelPty(vtpty::PageSize size, std::function<void()> onDestroy):
        vtpty::ChannelPty(size), _onDestroy(std::move(onDestroy))
    {
    }

    ~SelfUnbindingChannelPty() override
    {
        if (_onDestroy)
            _onDestroy();
    }

    SelfUnbindingChannelPty(SelfUnbindingChannelPty const&) = delete;
    SelfUnbindingChannelPty& operator=(SelfUnbindingChannelPty const&) = delete;
    SelfUnbindingChannelPty(SelfUnbindingChannelPty&&) = delete;
    SelfUnbindingChannelPty& operator=(SelfUnbindingChannelPty&&) = delete;

  private:
    std::function<void()> _onDestroy;
};

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

/// The reactor teardown both controllers share: flips @p stopped exactly once
/// (guarded by @p mutex), posts @p detach onto the reactor so a live client
/// detaches cleanly, then requests stop and joins the reactor thread. A task
/// still parked in connect has no client to detach; the stop request cancels the
/// loop so join() cannot block forever.
/// @param mutex Guards @p stopped (locked internally).
/// @param stopped The controller's one-shot stop flag.
/// @param reactor The controller's reactor thread.
/// @param detach Posted onto the reactor to detach the live client/gateway.
/// @return True when this call performed the teardown; false when it was already
///         stopped — so the caller closes its own bindings only on the first stop.
[[nodiscard]] bool stopMuxReactor(std::mutex& mutex,
                                  bool& stopped,
                                  MuxLoopThread& reactor,
                                  std::function<void()> detach);

} // namespace contour
