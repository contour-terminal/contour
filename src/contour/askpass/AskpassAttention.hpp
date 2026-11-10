// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// Whether the window that would show an ssh prompt is on screen. A reconnect that would prompt waits on
/// this, from the reactor's thread, while the windows live on the GUI's: the GUI thread publishes a snapshot,
/// the reactor reads it.

#include <contour/askpass/AskpassPromptRouter.hpp>

#include <memory>
#include <mutex>
#include <vector>

namespace contour::askpass
{

/// What the GUI thread publishes about one window that has a prompt sink.
struct SinkExposure
{
    AskpassRouteId route = AnyWindow; ///< The window's route.
    /// Whether the window is on screen for the user.
    enum class State : std::uint8_t
    {
        OnScreen = 0,
        Hidden, ///< Hidden, minimised, or parked off screen.
    } state = State::OnScreen;
};

/// A snapshot of which sinks are on screen, readable from any thread.
class AskpassAttention
{
  public:
    /// Replaces the snapshot. GUI thread.
    /// @param sinks Every sink, oldest first, as the router has them.
    void publish(std::vector<SinkExposure> sinks);

    /// @param route The route of an attach: the window that started it.
    /// @return Whether the sink that would show its prompts is on screen. True when there is no sink at all:
    ///         the prompt then gets a window of its own.
    [[nodiscard]] bool isAttended(AskpassRouteId route) const;

  private:
    mutable std::mutex _mutex;
    std::vector<SinkExposure> _sinks;
};

} // namespace contour::askpass
