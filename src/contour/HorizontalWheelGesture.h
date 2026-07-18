// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <vtbackend/Terminal.h> // vtbackend::ScrollPhase

#include <crispy/point.h>

#include <cstdlib>

namespace contour
{

/// Decides whether the horizontal component of a wheel event is an intentional sideways gesture, or
/// incidental drift from a vertical one.
///
/// A two-finger trackpad scroll is never perfectly vertical: it drifts a few pixels sideways per event,
/// and over the hundreds of events a long scroll produces that drift accumulates past a cell width. Left
/// unfiltered it turns into a WheelLeft/WheelRight press, and — since those now switch tabs — scrolling
/// through a build log would randomly change tab.
///
/// So the gesture LATCHES: once a phased gesture has shown one vertically dominant event, its horizontal
/// component is ignored for the remainder of that gesture, however far it later drifts. Discrete input
/// (a mouse wheel tilt or ball) carries no gesture boundary to latch across and is judged per event
/// instead.
///
/// Kept free of Qt and of TerminalSession so the decision is unit-testable on its own; see
/// HorizontalWheelGesture_test.cpp.
class HorizontalWheelGesture
{
  public:
    /// Whether the horizontal component of this event should be acted upon.
    ///
    /// @param pixelDelta Pixel-precise delta (trackpads), or {0,0}.
    /// @param angleDelta Angle delta (wheels), or {0,0}.
    /// @param phase      The gesture phase the windowing system reported.
    /// @return true when the horizontal component is an intentional horizontal gesture.
    [[nodiscard]] bool acceptsHorizontal(crispy::point pixelDelta,
                                         crispy::point angleDelta,
                                         vtbackend::ScrollPhase phase) noexcept
    {
        // Judge on whichever delta actually carries the motion, preferring the pixel-precise one — that
        // is the trackpad, and the trackpad is the whole reason this class exists.
        auto const delta = pixelDelta ? pixelDelta : angleDelta;
        auto const horizontal = std::abs(delta.x);
        auto const vertical = std::abs(delta.y);

        switch (phase)
        {
            case vtbackend::ScrollPhase::NoPhase:
                // A discrete tilt reports one axis at a time; anything carrying both is a diagonal
                // trackpad event from a driver that omits phases, which we decline rather than guess at.
                return delta.x != 0 && delta.y == 0;
            case vtbackend::ScrollPhase::Begin: _verticalLatched = false; break;
            case vtbackend::ScrollPhase::End: _verticalLatched = false; return false;
            case vtbackend::ScrollPhase::Update: break;
            case vtbackend::ScrollPhase::Momentum:
                // Momentum is the tail of a gesture already judged: it inherits the latch and never sets
                // one, so a flick that started vertical stays vertical all the way through its glide.
                return !_verticalLatched && horizontal >= vertical && delta.x != 0;
        }

        if (vertical > horizontal)
            _verticalLatched = true;

        return !_verticalLatched && horizontal >= vertical && delta.x != 0;
    }

    /// Forgets the current gesture, so the next event is judged afresh.
    void reset() noexcept { _verticalLatched = false; }

  private:
    bool _verticalLatched = false;
};

} // namespace contour
