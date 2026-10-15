// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <vtbackend/core/Primitives.hpp>
#include <vtbackend/core/SteadyClockRef.hpp>
#include <vtbackend/input/InputGenerator.hpp>

#include <core/platform/Clock.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>

namespace vtbackend
{

/// Counts multi-clicks: how many presses of one button landed on one cell, each within an interval of
/// the press before it.
///
/// The count picks between a drag, a word, an extended word and a whole line (1..4), and it is what a
/// `clicks:` mouse binding matches on. The clock is injected so a test sets the gap between presses
/// instead of sleeping it.
class ClickCounter
{
  public:
    /// The longest gap between two presses that still continues a sequence.
    static constexpr std::chrono::milliseconds DefaultInterval { 1000 };

    /// The count after which a sequence starts over at 1 (a fifth click is a single click again).
    static constexpr uint8_t MaxClickCount = 4;

    /// The buttons whose presses are counted, each in a sequence of its own. Any other button (a wheel
    /// step, a release) is always a single click.
    static constexpr auto CountedButtons =
        std::array { MouseButton::Left, MouseButton::Middle, MouseButton::Right };

    /// Whether presses of @p button are counted, so that a `clicks:` binding of it can ever fire.
    [[nodiscard]] static constexpr bool countsClicks(MouseButton button) noexcept
    {
        return std::ranges::contains(CountedButtons, button);
    }

    /// @param clock Where "now" comes from. Borrowed: it must outlive the counter, and a temporary is
    ///              refused.
    /// @param interval The longest gap between presses that continues a sequence (inclusive).
    ClickCounter(SteadyClockRef clock, std::chrono::milliseconds interval) noexcept;

    /// Records a press and returns its place in its sequence.
    /// @param position The cell the press landed on.
    /// @param button The button. Each of CountedButtons keeps its own sequence; any other button is
    ///               always a single click.
    /// @return 1 for a new sequence, up to MaxClickCount, then 1 again.
    [[nodiscard]] uint8_t press(CellLocation position, MouseButton button) noexcept;

    /// Ends every sequence, so the next press of any button counts 1 (a pointer move, a Shift+click).
    void reset() noexcept;

  private:
    /// One button's sequence so far.
    struct Sequence
    {
        CellLocation position {};                     ///< The cell its presses landed on.
        core::platform::SteadyTimePoint lastPress {}; ///< When its latest press was.
        uint8_t count = 0;                            ///< Its latest press's count; 0 = no sequence.
    };

    /// The slot of @p button in _sequences, or nullopt for a button whose presses are not counted.
    [[nodiscard]] static std::optional<size_t> slotOf(MouseButton button) noexcept;

    SteadyClockRef _clock;               ///< Borrowed; when each press happened.
    std::chrono::milliseconds _interval; ///< The longest gap that continues a sequence (inclusive).
    std::array<Sequence, CountedButtons.size()>
        _sequences {}; ///< One per CountedButtons entry, in its order.
};

} // namespace vtbackend
