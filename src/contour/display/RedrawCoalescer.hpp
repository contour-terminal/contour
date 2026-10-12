// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <atomic>
#include <cstdint>

namespace contour::display
{

/// Whether a redraw is queued on the GUI thread.
enum class RedrawState : std::uint8_t
{
    Idle,   ///< None is queued: the next request must post one.
    Queued, ///< One is queued and has not started yet: it will draw whatever is requested meanwhile.
};

/// What a redraw request asks of its caller.
enum class RedrawPost : std::uint8_t
{
    NotNeeded, ///< A queued redraw that has not started yet covers this request.
    Needed,    ///< Nothing is queued: post a redraw, and disarm when it starts.
};

/// Collapses redraw requests into one queued redraw at a time.
///
/// The parser thread asks for a redraw after every batch it parses, many times per frame under a
/// flood, and each request used to post its own queued call to the GUI thread. A frame draws the state
/// as it is when it renders, so one queued redraw covers every request made before it starts.
///
/// Thread-safe: arm() is called from the parser thread and the GUI thread alike, disarm() from the GUI
/// thread.
class RedrawCoalescer
{
  public:
    /// Records a redraw request.
    /// @return Needed when nothing was queued -- the caller posts the redraw -- and NotNeeded when a
    ///         queued redraw has yet to start and will cover this request.
    [[nodiscard]] RedrawPost arm() noexcept
    {
        return _state.exchange(RedrawState::Queued, std::memory_order_acq_rel) == RedrawState::Idle
                   ? RedrawPost::Needed
                   : RedrawPost::NotNeeded;
    }

    /// Marks the queued redraw as started, or forgets one that will never run.
    ///
    /// A redraw calls this before it reads anything, so a request made while it runs queues a new one
    /// instead of being swallowed by one that has already looked.
    void disarm() noexcept { _state.store(RedrawState::Idle, std::memory_order_release); }

    /// @return Whether a redraw is queued.
    [[nodiscard]] RedrawState state() const noexcept { return _state.load(std::memory_order_acquire); }

  private:
    std::atomic<RedrawState> _state { RedrawState::Idle };
};

} // namespace contour::display
