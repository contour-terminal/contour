// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <vtbackend/PromptRegion.h>
#include <vtbackend/primitives.h>

#include <optional>

namespace contour::display
{

/// What the operating system is told about the caret.
struct CaretState
{
    /// Whether there is a caret to report at all: DECTCEM on AND the cursor on screen.
    ///
    /// Deliberately NOT the render buffer's notion of visible, which folds in the blink phase — see
    /// CaretReportGate.
    bool visible = false;

    /// Where it is, viewport-relative.
    vtbackend::CellLocation position {};

    /// The shell prompt the caret is standing in, when it is standing in one.
    std::optional<vtbackend::LivePromptSpan> prompt {};

    [[nodiscard]] bool operator==(CaretState const&) const noexcept = default;
};

/// Decides whether a caret state is worth telling an assistive client about.
///
/// The terminal's own cursorPositionChanged() fires on the BLINK phase, because the render buffer simply
/// has no cursor while the cursor is blinked off — so a perfectly stationary cursor announces itself twice
/// a second. A screen reader told the caret moved twice a second is a screen reader that reads the same
/// line aloud twice a second, and a magnifier that never settles.
///
/// This gate is fed the BLINK-FREE state and reports only real change: a move, an invisible-to-visible
/// transition, or a change of prompt region.
///
/// Note it records even when it declines, which is what makes the invisible-to-visible case work: the
/// caret going away sets `visible = false`, so coming back at the very same position is a change and is
/// reported — as it must be, since the client stopped tracking when it went.
class CaretReportGate
{
  public:
    /// @param current The caret state as it is now.
    /// @return true when an assistive client should be told.
    [[nodiscard]] bool shouldReport(CaretState const& current) noexcept
    {
        auto const changed = current.visible && current != _last;
        _last = current;
        return changed;
    }

    /// Forgets what was last reported, so the next visible state is reported afresh.
    ///
    /// Call whenever something other than the caret invalidated the client's picture: focus moving to
    /// another pane, a buffer switch, a new session. Without it a pane that regains focus at the position
    /// it lost focus at would stay silent, and the client would keep pointing at the pane it left.
    void reset() noexcept { _last = {}; }

  private:
    CaretState _last {};
};

} // namespace contour::display
