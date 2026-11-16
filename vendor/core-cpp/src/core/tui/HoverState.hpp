// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <core/Clock.hpp>
#include <core/Defines.hpp>

#include <chrono>
#include <functional>
#include <optional>

namespace core::tui
{

class Component;

/// @brief Information about the current hover position.
struct HoverInfo
{
    int x = 0;                   ///< Mouse X position (1-based, as from mouse events).
    int y = 0;                   ///< Mouse Y position (1-based, as from mouse events).
    Component* target = nullptr; ///< Component under cursor (from hit-test).
};

/// @brief Manages hover state tracking with delay timer.
///
/// HoverState tracks the mouse position and provides a delay-based hover
/// confirmation mechanism. It's designed to integrate with the Screen's
/// event dispatch and poll loop.
///
/// Usage:
/// 1. Call onMouseMove() when mouse moves
/// 2. Use timeoutMs() to get poll timeout
/// 3. Call tick() when poll times out (no events)
/// 4. Register callbacks for hover confirmed/leave
class HoverState
{
  public:
    /// @brief Callback type for hover events.
    using HoverCallback = std::function<void(HoverInfo const&)>;
    using LeaveCallback = std::function<void()>;

    /// @brief Default hover delay (500ms).
    static constexpr auto DefaultHoverDelay = std::chrono::milliseconds(500);

    /// @brief Constructs HoverState with the specified delay, timed by @p clock.
    /// @param delay How long the pointer must rest before a hover is confirmed.
    /// @param clock The clock the delay is measured on (not owned; must outlive this object).
    explicit HoverState(std::chrono::milliseconds delay = DefaultHoverDelay,
                        IClock const& clock CORE_LIFETIMEBOUND = defaultSteadyClock());

    /// A clock is kept, not copied: a temporary would dangle, so an rvalue clock is refused.
    HoverState(std::chrono::milliseconds delay, IClock const&& clock) = delete;

    /// @brief Called when the mouse moves.
    /// @param x Mouse X position (1-based).
    /// @param y Mouse Y position (1-based).
    /// @param target Component under cursor (from hit-test), may be nullptr.
    void onMouseMove(int x, int y, Component* target);

    /// @brief Called when the mouse leaves the tracked area.
    void onMouseLeave();

    /// @brief Called on poll timeout to check if hover is confirmed.
    /// @param now Current time point, on the clock this object was constructed with.
    void tick(SteadyTimePoint now);

    /// @brief Sets the callback for when hover is confirmed (after delay).
    void setOnHoverConfirmed(HoverCallback callback);

    /// @brief Sets the callback for when hover leaves.
    void setOnHoverLeave(LeaveCallback callback);

    /// @brief Returns the timeout in ms for poll, or -1 if not hovering.
    ///
    /// This should be used to determine the poll timeout in the event loop.
    /// When the timeout expires, call tick() to process the hover timer.
    [[nodiscard]] int timeoutMs() const;

    /// @brief Returns true if hover has been confirmed (after delay).
    [[nodiscard]] bool isHoverConfirmed() const noexcept { return _confirmed; }

    /// @brief Returns the current hover info, if any.
    [[nodiscard]] std::optional<HoverInfo> currentHover() const;

    /// @brief Returns the current mouse position, if hovering.
    [[nodiscard]] std::optional<std::pair<int, int>> mousePosition() const;

    /// @brief Sets the hover delay.
    void setDelay(std::chrono::milliseconds delay);

    /// @brief Returns the current hover delay.
    [[nodiscard]] std::chrono::milliseconds delay() const noexcept { return _delay; }

    /// @brief Resets hover state (e.g., when hiding tooltip).
    void reset();

    /// @brief Ends the hover if @p component is its target, as reset() does; keeps any other.
    /// @param component A component leaving the screen, which must not be pointed at after it goes.
    void forget(Component const& component);

    /// @brief Returns the clock the hover delay is measured on.
    [[nodiscard]] IClock const& clock() const noexcept { return *_clock; }

  private:
    std::chrono::milliseconds _delay;
    IClock const* _clock; ///< Never null; a pointer so that HoverState stays assignable.
    std::optional<HoverInfo> _hover;
    SteadyTimePoint _hoverStart;
    bool _confirmed = false;
    HoverCallback _onHoverConfirmed;
    LeaveCallback _onHoverLeave;
};

} // namespace core::tui
