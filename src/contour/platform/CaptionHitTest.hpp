// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <contour/config/WindowControlStyle.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <optional>
#include <ranges>

// The caption's non-client decisions, Qt-free: where WM_NCHITTEST answers HTMAXBUTTON (Snap Layouts), and
// what each non-client mouse message does to the maximise button the OS now drives. The Win32 adapter
// converts the message and asks these; everything it decides is a table row or a rectangle test here.
namespace contour::platform
{

/// A point in the window's logical (device-independent) pixels.
struct LogicalPoint
{
    double x = 0; ///< From the window's leading edge.
    double y = 0; ///< From the window's top edge.

    [[nodiscard]] bool operator==(LogicalPoint const&) const = default;
};

/// A rectangle in the window's logical pixels.
struct LogicalRect
{
    double x = 0;      ///< Leading edge.
    double y = 0;      ///< Top edge.
    double width = 0;  ///< Extent to the right.
    double height = 0; ///< Extent downward.

    /// @param p The point to test.
    /// @return Whether @p p lies inside, half-open: the leading and top edges are in, the far ones out.
    [[nodiscard]] constexpr bool contains(LogicalPoint p) const noexcept
    {
        return p.x >= x && p.x < x + width && p.y >= y && p.y < y + height;
    }

    [[nodiscard]] bool operator==(LogicalRect const&) const = default;
};

/// A point in the window's client area, in physical (device) pixels, as a Win32 message carries it.
struct PhysicalPoint
{
    int x = 0; ///< From the client area's leading edge.
    int y = 0; ///< From the client area's top edge.
};

/// Where the window stands, as far as its caption cares.
enum class WindowPlacement : uint8_t
{
    Windowed = 0, //!< A normal window: its top edge is a resize strip.
    Maximized,    //!< Fills the work area: no resize strip, the buttons reach the screen's edges.
    FullScreen,   //!< Fills the screen: as maximised.
};

/// The caption buttons' scene rectangles, by @c config::WindowControlKind; empty where a button is not shown.
struct CaptionLayout
{
    std::array<std::optional<LogicalRect>, config::WindowControlCount> buttons {}; ///< By WindowControlKind.
    double windowWidth = 0;  ///< The window's logical width.
    double resizeBorder = 0; ///< ResizeBorder.qml's thickness (UiStyleProvider::resizeBorderThickness).

    [[nodiscard]] bool operator==(CaptionLayout const&) const = default;
};

/// What WM_NCHITTEST answers.
enum class CaptionHit : uint8_t
{
    PassThrough = 0, //!< Not ours to decide: the default handling (HTCLIENT, a resize edge, ...).
    MaximizeButton,  //!< HTMAXBUTTON, which is what lets Windows 11 offer Snap Layouts on hover.
};

/// Number of @c CaptionHit enumerators, derived from the last one.
inline constexpr size_t CaptionHitCount = static_cast<size_t>(CaptionHit::MaximizeButton) + 1;

/// How the maximise button looks while the OS drives it.
enum class CaptionButtonVisual : uint8_t
{
    Rest = 0, //!< Neither hovered nor pressed.
    Hover,    //!< The pointer is over it.
    Pressed,  //!< The left button went down on it and has not come up.
};

/// Number of @c CaptionButtonVisual enumerators, derived from the last one.
inline constexpr size_t CaptionButtonVisualCount = static_cast<size_t>(CaptionButtonVisual::Pressed) + 1;

/// The non-client mouse messages the filter handles.
enum class NcEvent : uint8_t
{
    MouseMove = 0,   //!< WM_NCMOUSEMOVE.
    MouseLeave,      //!< WM_NCMOUSELEAVE.
    LeftDown,        //!< WM_NCLBUTTONDOWN.
    LeftDoubleClick, //!< WM_NCLBUTTONDBLCLK.
    LeftUp,          //!< WM_NCLBUTTONUP.
};

/// Number of @c NcEvent enumerators, derived from the last one.
inline constexpr size_t NcEventCount = static_cast<size_t>(NcEvent::LeftUp) + 1;

/// What the adapter does beyond updating the button's visual.
enum class NcEffect : uint8_t
{
    None = 0,       //!< Nothing.
    TrackLeave,     //!< Ask for WM_NCMOUSELEAVE (TrackMouseEvent, non-client leave tracking).
    ToggleMaximize, //!< Maximise the window, or restore it when it is maximised.
};

/// What becomes of the message after the filter.
enum class NcDisposition : uint8_t
{
    DefaultHandling = 0, //!< Pass it on to the default window procedure.
    Swallow,             //!< Handled here; the default procedure never sees it.
};

/// What one non-client message leads to.
struct NcTransition
{
    CaptionButtonVisual visual = CaptionButtonVisual::Rest;     ///< The button's visual afterwards.
    NcEffect effect = NcEffect::None;                           ///< What the adapter does.
    NcDisposition disposition = NcDisposition::DefaultHandling; ///< What becomes of the message.

    [[nodiscard]] bool operator==(NcTransition const&) const = default;
};

/// One row: an event, whether it lands on the maximise button, the visual before; and what follows.
/// An empty optional column matches any value.
struct NcTransitionRow
{
    NcEvent event = NcEvent::MouseMove;           ///< The message.
    std::optional<CaptionHit> hit {};             ///< Where it lands, or anywhere.
    std::optional<CaptionButtonVisual> before {}; ///< The visual before it, or any.
    NcTransition after {};                        ///< What follows.
};

/// The maximise button's state machine, first match wins (onNcEvent). A press is swallowed so the caption's
/// own maximise never runs on top of ours, and a release toggles only after a press on the button itself:
/// pressing, dragging off and releasing does nothing, as with any button.
inline constexpr auto CaptionTransitions = std::array {
    // Entering the button: light it, and ask for the leave message that will unlight it.
    NcTransitionRow { .event = NcEvent::MouseMove,
                      .hit = CaptionHit::MaximizeButton,
                      .before = CaptionButtonVisual::Rest,
                      .after = { .visual = CaptionButtonVisual::Hover,
                                 .effect = NcEffect::TrackLeave,
                                 .disposition = NcDisposition::DefaultHandling } },
    // Moving over it: a hover stays a hover, a press stays a press.
    NcTransitionRow { .event = NcEvent::MouseMove,
                      .hit = CaptionHit::MaximizeButton,
                      .before = CaptionButtonVisual::Hover,
                      .after = { .visual = CaptionButtonVisual::Hover,
                                 .effect = NcEffect::None,
                                 .disposition = NcDisposition::DefaultHandling } },
    NcTransitionRow { .event = NcEvent::MouseMove,
                      .hit = CaptionHit::MaximizeButton,
                      .before = CaptionButtonVisual::Pressed,
                      .after = { .visual = CaptionButtonVisual::Pressed,
                                 .effect = NcEffect::None,
                                 .disposition = NcDisposition::DefaultHandling } },
    // Moving off it, or leaving the non-client area: back to rest, which also cancels a press.
    NcTransitionRow { .event = NcEvent::MouseMove,
                      .hit = CaptionHit::PassThrough,
                      .before = std::nullopt,
                      .after = { .visual = CaptionButtonVisual::Rest,
                                 .effect = NcEffect::None,
                                 .disposition = NcDisposition::DefaultHandling } },
    NcTransitionRow { .event = NcEvent::MouseLeave,
                      .hit = std::nullopt,
                      .before = std::nullopt,
                      .after = { .visual = CaptionButtonVisual::Rest,
                                 .effect = NcEffect::None,
                                 .disposition = NcDisposition::DefaultHandling } },
    // A press, and a double click (which is a press to the button, not the caption's maximise). Each asks for
    // the leave as well: a pen or touch tap lights the button with no move before it (TrackMouseEvent is
    // idempotent).
    NcTransitionRow { .event = NcEvent::LeftDown,
                      .hit = CaptionHit::MaximizeButton,
                      .before = std::nullopt,
                      .after = { .visual = CaptionButtonVisual::Pressed,
                                 .effect = NcEffect::TrackLeave,
                                 .disposition = NcDisposition::Swallow } },
    NcTransitionRow { .event = NcEvent::LeftDoubleClick,
                      .hit = CaptionHit::MaximizeButton,
                      .before = std::nullopt,
                      .after = { .visual = CaptionButtonVisual::Pressed,
                                 .effect = NcEffect::TrackLeave,
                                 .disposition = NcDisposition::Swallow } },
    // A release on the button toggles once, and only after a press on it -- whose row asked for the leave.
    NcTransitionRow { .event = NcEvent::LeftUp,
                      .hit = CaptionHit::MaximizeButton,
                      .before = CaptionButtonVisual::Pressed,
                      .after = { .visual = CaptionButtonVisual::Hover,
                                 .effect = NcEffect::ToggleMaximize,
                                 .disposition = NcDisposition::Swallow } },
    NcTransitionRow { .event = NcEvent::LeftUp,
                      .hit = CaptionHit::MaximizeButton,
                      .before = std::nullopt,
                      .after = { .visual = CaptionButtonVisual::Hover,
                                 .effect = NcEffect::TrackLeave,
                                 .disposition = NcDisposition::Swallow } },
    NcTransitionRow { .event = NcEvent::LeftUp,
                      .hit = CaptionHit::PassThrough,
                      .before = std::nullopt,
                      .after = { .visual = CaptionButtonVisual::Rest,
                                 .effect = NcEffect::None,
                                 .disposition = NcDisposition::DefaultHandling } },
};

/// @param client           A point in the client area, in physical pixels.
/// @param devicePixelRatio The window's current device-pixel ratio.
/// @return The same point in logical pixels.
[[nodiscard]] constexpr LogicalPoint toLogical(PhysicalPoint client, double devicePixelRatio) noexcept
{
    return LogicalPoint { .x = client.x / devicePixelRatio, .y = client.y / devicePixelRatio };
}

/// What WM_NCHITTEST answers at @p point.
///
/// Windowed, the top @c resizeBorder pixels stay the resize edge, as over Windows' own caption buttons.
/// Maximised or full screen there is no resize strip, and a button at the window's top or trailing edge
/// also takes the pixels beyond it, so the screen's edge pixel hits (Fitts).
///
/// @param point     The point, in logical pixels.
/// @param layout    The caption buttons' rectangles.
/// @param placement Where the window stands.
/// @return @c MaximizeButton over the maximise button, else @c PassThrough.
[[nodiscard]] constexpr CaptionHit captionHitAt(LogicalPoint point,
                                                CaptionLayout const& layout,
                                                WindowPlacement placement) noexcept
{
    auto const& slot = layout.buttons[static_cast<size_t>(config::WindowControlKind::Maximize)];
    if (!slot)
        return CaptionHit::PassThrough;
    if (placement == WindowPlacement::Windowed)
        return point.y >= layout.resizeBorder && slot->contains(point) ? CaptionHit::MaximizeButton
                                                                       : CaptionHit::PassThrough;

    constexpr auto Unbounded = std::numeric_limits<double>::infinity();
    auto const right = slot->x + slot->width;
    auto const top = slot->y == 0 ? -Unbounded : slot->y;
    auto const end = right == layout.windowWidth ? Unbounded : right;
    auto const inside =
        point.x >= slot->x && point.x < end && point.y >= top && point.y < slot->y + slot->height;
    return inside ? CaptionHit::MaximizeButton : CaptionHit::PassThrough;
}

/// @param event  The message.
/// @param hit    Where it lands.
/// @param before The button's visual before it.
/// @return The first @c CaptionTransitions row matching, or nullptr when none does.
[[nodiscard]] constexpr NcTransitionRow const* findNcTransition(NcEvent event,
                                                                CaptionHit hit,
                                                                CaptionButtonVisual before) noexcept
{
    auto const matches = [&](NcTransitionRow const& row) {
        return row.event == event && (!row.hit || *row.hit == hit) && (!row.before || *row.before == before);
    };
    auto const found = std::ranges::find_if(CaptionTransitions, matches);
    return found == CaptionTransitions.end() ? nullptr : &*found;
}

/// What a non-client message does to the maximise button.
/// @param event  The message.
/// @param hit    Where it lands (captionHitAt).
/// @param before The button's visual before it.
/// @return The first matching row's transition; with none, the visual unchanged and the default handling.
[[nodiscard]] constexpr NcTransition onNcEvent(NcEvent event,
                                               CaptionHit hit,
                                               CaptionButtonVisual before) noexcept
{
    if (auto const* const row = findNcTransition(event, hit, before))
        return row->after;
    return NcTransition { .visual = before,
                          .effect = NcEffect::None,
                          .disposition = NcDisposition::DefaultHandling };
}

namespace detail
{
    /// Calls @p visit with every (event, hit, before) the transition table can be asked about.
    template <typename Visit>
    constexpr void forEachNcKey(Visit visit)
    {
        for (auto const event: std::views::iota(size_t { 0 }, NcEventCount))
            for (auto const hit: std::views::iota(size_t { 0 }, CaptionHitCount))
                for (auto const before: std::views::iota(size_t { 0 }, CaptionButtonVisualCount))
                    visit(static_cast<NcEvent>(event),
                          static_cast<CaptionHit>(hit),
                          static_cast<CaptionButtonVisual>(before));
    }

    /// @return Whether every row is the first match for some key: no row is shadowed by the ones above it.
    [[nodiscard]] constexpr bool captionTransitionsAreEachReachable() noexcept
    {
        auto reached = std::array<bool, CaptionTransitions.size()> {};
        forEachNcKey([&](NcEvent event, CaptionHit hit, CaptionButtonVisual before) {
            if (auto const* const row = findNcTransition(event, hit, before); row != nullptr)
                reached[static_cast<size_t>(row - CaptionTransitions.data())] = true;
        });
        return std::ranges::all_of(reached, std::identity {});
    }
} // namespace detail

static_assert(detail::captionTransitionsAreEachReachable(),
              "every CaptionTransitions row must be the first match for some message");

} // namespace contour::platform
