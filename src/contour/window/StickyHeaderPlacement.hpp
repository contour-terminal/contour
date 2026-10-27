// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <contour/window/PaneHeaderPolicy.hpp>

#include <vtbackend/core/StickyHeaderKind.hpp>

#include <cstdint>

namespace contour::window
{

/// Where a pane's sticky command header lives (spec §3.12, R-l).
enum class StickyHeaderHost : uint8_t
{
    Grid = 0,   ///< Over the page's top row, as a band the renderer draws: no chrome to put it in.
    PaneHeader, ///< In the pane's own header, in place of its name.
    TitleBand,  ///< In the window's title band: the only pane, with no header of its own.
};

/// Whether the window shows a title band.
enum class TitleBandPresence : uint8_t
{
    Absent = 0, ///< No band: a native title bar, full screen with the band hidden, or none at all.
    Shown,
};

/// What the host of a pane's sticky header is decided from.
struct StickyHeaderHostInputs
{
    PaneHeaderContent header = PaneHeaderContent::Hidden; ///< What the pane's header shows, if any.
    int visiblePanes = 1;                                 ///< Panes on screen in the pane's tab.
    TitleBandPresence band = TitleBandPresence::Absent;   ///< Whether the window shows a title band.
};

/// Decides where a pane's sticky header lives: its own header when it shows one, else the title band when
/// it is the only pane and a band is shown, else over its top row (Minimal splits, no title band).
/// @param inputs The pane's header, its tab's visible panes, and the window's band.
/// @return The host.
[[nodiscard]] constexpr StickyHeaderHost stickyHeaderHostFor(StickyHeaderHostInputs const& inputs) noexcept
{
    if (inputs.header != PaneHeaderContent::Hidden)
        return StickyHeaderHost::PaneHeader;
    if (inputs.visiblePanes == 1 && inputs.band == TitleBandPresence::Shown)
        return StickyHeaderHost::TitleBand;
    return StickyHeaderHost::Grid;
}

/// How the terminal presents the header for @p host: over the grid, or as data for the chrome.
/// @param host Where the header lives.
/// @return InGrid for the grid, Chrome for either chrome host.
[[nodiscard]] constexpr vtbackend::StickyHeaderPresentation presentationOf(StickyHeaderHost host) noexcept
{
    switch (host)
    {
        case StickyHeaderHost::Grid: return vtbackend::StickyHeaderPresentation::InGrid;
        case StickyHeaderHost::PaneHeader:
        case StickyHeaderHost::TitleBand: return vtbackend::StickyHeaderPresentation::Chrome;
    }
    return vtbackend::StickyHeaderPresentation::InGrid;
}

} // namespace contour::window
