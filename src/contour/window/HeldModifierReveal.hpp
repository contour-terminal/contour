// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <contour/window/PaneHeaderPolicy.hpp>

#include <chrono>
#include <cstdint>
#include <optional>

namespace contour::window
{

/// Tracks a modifier key held on its own, and reveals the pane headers once it has been held for a delay.
///
/// Time is passed in rather than read, so the decision is testable without a clock. Not thread-safe;
/// used from the GUI thread.
class HeldModifierReveal
{
  public:
    /// @param delay How long the modifier must be held alone before the headers are revealed.
    explicit constexpr HeldModifierReveal(std::chrono::milliseconds delay) noexcept: _delay { delay } {}

    /// The modifier went down. Only a lone press arms the reveal; a press while already armed (the other
    /// side's key of the same modifier) keeps the first press's time, so the hold is measured from it.
    /// @param now When it happened.
    void modifierPressed(std::chrono::steady_clock::time_point now) noexcept
    {
        if (_state != HoldState::Armed)
            _pressedAt = now;
        _state = HoldState::Armed;
    }

    /// Any other input arrived while the modifier is down; disarms until the next press.
    /// Has no effect while the modifier is up.
    void otherInput() noexcept
    {
        if (_state == HoldState::Armed)
            _state = HoldState::Disarmed;
    }

    /// The modifier went up.
    void modifierReleased() noexcept { _state = HoldState::Released; }

    /// The reveal state at @p now.
    /// @param now The time to evaluate at.
    /// @return HeaderReveal::HeldModifier once armed for at least the delay, else HeaderReveal::None.
    [[nodiscard]] HeaderReveal at(std::chrono::steady_clock::time_point now) const noexcept
    {
        if (_state == HoldState::Armed && now - _pressedAt >= _delay)
            return HeaderReveal::HeldModifier;
        return HeaderReveal::None;
    }

    /// When at() will next change, for the caller to arm its timer.
    /// @return The reveal time, or nullopt when nothing is pending.
    [[nodiscard]] std::optional<std::chrono::steady_clock::time_point> dueAt() const noexcept
    {
        if (_state == HoldState::Armed)
            return _pressedAt + _delay;
        return std::nullopt;
    }

  private:
    /// Where the modifier is in its press; the one state the reveal depends on.
    enum class HoldState : std::uint8_t
    {
        Released = 0, //!< The modifier is up.
        Armed,        //!< Pressed on its own; the reveal is pending or shown.
        Disarmed,     //!< Pressed, then other input arrived; nothing reveals until the next press.
    };

    std::chrono::milliseconds _delay;
    HoldState _state = HoldState::Released;
    std::chrono::steady_clock::time_point _pressedAt {}; ///< Meaningful only while Armed.
};

} // namespace contour::window
