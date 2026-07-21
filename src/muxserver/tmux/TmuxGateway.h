// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// `TmuxGateway` — the client side of a tmux control-mode connection
/// (`tmux -CC`, or this project's own daemon speaking the same protocol).
///
/// The gateway owns the transport and the protocol state machine — recovery
/// mode until the opening guard, FIFO command/response correlation, and
/// notification fan-out to a consumer interface (wezterm's gateway states +
/// iTerm2's correlation details). It deliberately knows nothing about panes'
/// contents: consumers feed %output bytes into their own terminals.

#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include <coro/Task.hpp>
#include <muxserver/tmux/ControlModeParser.h>
#include <net/EventLoop.h>
#include <net/ISocket.h>
#include <net/WriteQueue.h>

namespace muxserver::tmux
{

/// One command's completion: whether the guard closed with %end (not %error)
/// and the body lines collected in between.
using CommandCallback = std::function<void(bool ok, std::vector<std::string> const& body)>;

/// The consumer half: notifications become calls on this interface.
/// Defaulted no-ops so consumers override only what they render.
class GatewayEvents
{
  public:
    virtual ~GatewayEvents() = default;

    virtual void outputReceived(uint64_t pane, std::string_view bytes) { (void) pane, (void) bytes; }
    virtual void layoutChanged(uint64_t window, std::string_view layout) { (void) window, (void) layout; }
    virtual void windowAdded(uint64_t window) { (void) window; }
    virtual void windowClosed(uint64_t window) { (void) window; }
    virtual void windowRenamed(uint64_t window, std::string_view name) { (void) window, (void) name; }
    virtual void sessionChanged(uint64_t session, std::string_view name) { (void) session, (void) name; }
    virtual void panePaused(uint64_t pane, bool paused) { (void) pane, (void) paused; }
    virtual void exited(std::string_view reason) { (void) reason; }
};

/// One connected control-mode client endpoint.
class TmuxGateway final
{
  public:
    /// @param loop The event loop everything runs on.
    /// @param connection The transport to the tmux server (owned).
    /// @param events The notification consumer (not owned; outlives this).
    TmuxGateway(net::EventLoop& loop, std::unique_ptr<net::ISocket> connection, GatewayEvents& events);

    /// The connection flow: recovery until the opening guard, then serve
    /// notifications and command responses until %exit or disconnect.
    [[nodiscard]] coro::Task<void> run();

    /// Queues @p command; @p callback fires when its guard block closes.
    /// FIFO: responses correlate to commands strictly in send order.
    void sendCommand(std::string command, CommandCallback callback = {});

    /// Sends literal text to @p pane in bounded `send-keys -l` batches
    /// (iTerm2's 1000-character cap per command).
    void sendKeys(uint64_t pane, std::string_view text);

    /// Detaches: control mode ends on an empty input line.
    void detach();

    /// @return True once the opening guard completed (notifications flow).
    [[nodiscard]] bool initialised() const noexcept { return _initialised; }

  private:
    enum class State : uint8_t
    {
        Recovery, ///< Discard everything until the opening %begin (or %exit).
        Idle,     ///< Between guard blocks: notifications dispatch.
        InGuard,  ///< Collecting a guard block's body lines.
    };

    void handleLine(std::string_view line);
    void dispatchNotification(ControlEvent const& event);

    net::EventLoop& _loop;
    std::unique_ptr<net::ISocket> _connection;
    net::WriteQueue _writer;
    GatewayEvents& _events;

    State _state = State::Recovery;
    bool _openingGuard = true; ///< The guard currently open is the implicit initial command's.
    bool _initialised = false;
    bool _detached = false;
    bool _exited = false;
    std::vector<std::string> _guardBody;
    bool _guardIsError = false;
    std::deque<CommandCallback> _pending; ///< FIFO command correlation.
};

} // namespace muxserver::tmux
