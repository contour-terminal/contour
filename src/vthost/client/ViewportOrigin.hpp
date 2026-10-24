// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// `viewportOrigin` -- which part of a shared grid a client smaller than it shows.

#include <vtbackend/core/Primitives.hpp>

#include <algorithm>

namespace vthost::client
{

/// Places a client's viewport inside a grid that may be larger than it.
///
/// A daemon-hosted grid is shared by every attached client, so its size is not any one client's to
/// choose (@see vthost::ClientSizePolicy). A client whose window is SMALLER than the resolved grid
/// therefore has to show part of it, and the part it shows must contain the cursor — otherwise the
/// user is typing somewhere they cannot see. This is tmux's rule, ported from `tty_window_offset1`
/// (tmux tty.c): centre the cursor horizontally once it is past the viewport, keep it on the last
/// visible row vertically, and clamp so the viewport never runs off the grid.
///
/// A client whose window is at least as large as the grid gets the origin (0,0) — the grid is drawn
/// at its own size and the surplus is background, which is the letterbox case and needs no special
/// path here.
///
/// Pure, so the decision is testable without a window, a renderer or a daemon. Its caller is
/// ScreenMirror, which shows the grid's window at this origin and has the mirror terminal add it
/// to every mouse report (vtbackend::Terminal::setReportedCellOrigin) -- which is why it lives here,
/// below the GUI, rather than with the window geometry it was first written beside.
/// @param grid The authoritative grid, in cells.
/// @param viewport What this client can display, in cells.
/// @param cursor Where the cursor is in @p grid.
/// @return The grid cell the viewport's top-left corner shows.
[[nodiscard]] constexpr vtbackend::CellLocation viewportOrigin(vtbackend::PageSize grid,
                                                               vtbackend::PageSize viewport,
                                                               vtbackend::CellLocation cursor) noexcept
{
    // Per axis, independently: a viewport can be wide enough and too short at the same time, and
    // the axis that fits must not be offset just because the other one does not.
    auto const place = [](int gridExtent, int viewportExtent, int cursorAt, bool centre) {
        auto const slack = gridExtent - viewportExtent;
        if (slack <= 0)
            return 0; // fits: no panning, and never a negative origin
        // Before the viewport's own extent, the start of the grid is still the best view.
        if (cursorAt < viewportExtent)
            return 0;
        // Centring horizontally keeps the text either side of the cursor visible; vertically the
        // cursor sits on the LAST visible row, because what a user needs to see is what they have
        // already typed, which is above it.
        auto const wanted = centre ? cursorAt - (viewportExtent / 2) : cursorAt - viewportExtent + 1;
        return std::clamp(wanted, 0, slack);
    };

    return vtbackend::CellLocation {
        .line = vtbackend::LineOffset(place(unbox<int>(grid.lines),
                                            unbox<int>(viewport.lines),
                                            unbox<int>(cursor.line),
                                            /*centre=*/false)),
        .column = vtbackend::ColumnOffset(place(unbox<int>(grid.columns),
                                                unbox<int>(viewport.columns),
                                                unbox<int>(cursor.column),
                                                /*centre=*/true)),
    };
}

} // namespace vthost::client
