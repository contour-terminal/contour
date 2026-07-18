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
                // Each discrete notch is its own gesture: there is no phase to bound one, and a wheel
                // tilt is already the user's unit of intent.
                _navigationSpent = false;
                // A discrete tilt reports one axis at a time; anything carrying both is a diagonal
                // trackpad event from a driver that omits phases, which we decline rather than guess at.
                return delta.x != 0 && delta.y == 0;
            case vtbackend::ScrollPhase::Begin:
                _verticalLatched = false;
                _navigationSpent = false;
                break;
            case vtbackend::ScrollPhase::End:
                _verticalLatched = false;
                _navigationSpent = false;
                return false;
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

    /// Observes a gesture boundary, without judging any delta.
    ///
    /// Must be called for EVERY wheel event, including the ones a caller then handles by another route.
    /// A purely horizontal swipe carries no vertical motion, so the smooth-scroll path treats its Begin
    /// and End as zero-delta phase events and consumes them — and if those never reached this object, the
    /// gesture would never end: the first swipe would claim the navigation step below and no later swipe
    /// could ever claim another.
    ///
    /// @param phase The gesture phase the windowing system reported.
    void notePhase(vtbackend::ScrollPhase phase) noexcept
    {
        switch (phase)
        {
            case vtbackend::ScrollPhase::Begin:
            case vtbackend::ScrollPhase::End:
            case vtbackend::ScrollPhase::NoPhase: reset(); break;
            case vtbackend::ScrollPhase::Update:
            case vtbackend::ScrollPhase::Momentum: break;
        }
    }

    /// Claims the one discrete NAVIGATION step this gesture is allowed, if it is still unclaimed.
    ///
    /// Scrolling is continuous; switching tabs is not, and the two must not share a step size. The scroll
    /// accumulator quantizes horizontal motion by CELL WIDTH — right for scrolling a wide line, and wildly
    /// wrong for a discrete action, where a single ~150px flick becomes nearly twenty column steps and
    /// therefore nearly twenty tab switches. A wheel notch is no better: it is three steps.
    ///
    /// So a navigation step is latched to **one per gesture** — one per swipe on a trackpad, one per notch
    /// on a wheel — which is what a browser does with the very same gesture.
    ///
    /// @return true exactly once per gesture; false for every further event of it.
    [[nodiscard]] bool consumeNavigationStep() noexcept
    {
        if (_navigationSpent)
            return false;
        _navigationSpent = true;
        return true;
    }

    /// Forgets the current gesture, so the next event is judged afresh.
    void reset() noexcept
    {
        _verticalLatched = false;
        _navigationSpent = false;
    }

  private:
    bool _verticalLatched = false;
    bool _navigationSpent = false;
};

} // namespace contour
