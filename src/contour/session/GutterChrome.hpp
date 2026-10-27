// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <contour/config/ChromeTokenGroups.hpp>
#include <contour/config/UiStyle.hpp>

#include <vtbackend/core/Primitives.hpp>
#include <vtbackend/screen/GutterSettings.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>

namespace contour::session
{

/// How a session's chrome row draws the gutter's block column, in points: the form, and the rail's metrics.
///
/// Fixed per session: the chrome row is resolved once, before the first session exists (`ui_style` takes
/// effect on the next start), so whatever reads it -- the page fit on the render thread among them -- reads
/// one value for the session's life.
struct GutterChrome
{
    vtbackend::BlockColumnForm form =
        vtbackend::BlockColumnForm::Glyphs; ///< Glyph cells, or the rail's strip.
    int stripPoints = 0;                    ///< The strip's width (@c space.blockGutter); 0 under Glyphs.
    int barPoints = 0;                      ///< The rail bar's width (@c size.rail).
    int barInsetPoints = 0;                 ///< The bar's left edge within the strip (@c space.railInset).
    int barRadiusPoints = 0;                ///< The bar's end radius (@c radius.rail).
    int markPoints = 0;                     ///< The rail mark's edge (@c size.railMark).
    int barEndPoints = 0;      ///< What the bar keeps clear of a block's ends (@c space.railEnd).
    int foldChipHitPoints = 0; ///< The fold chip's least hit width (@c size.foldChipHit).

    bool operator==(GutterChrome const&) const = default;
};

/// The gutter chrome of chrome row @p style, from its tokens.
/// @param style The chrome row.
/// @return Its block column's form and the rail's metrics in points.
[[nodiscard]] inline GutterChrome gutterChromeFor(config::ChromeStyle style) noexcept
{
    auto const& row = config::ChromeTokenRows[static_cast<std::size_t>(style)];
    return GutterChrome { .form = row.layout.blockColumn,
                          .stripPoints = row.space.blockGutter,
                          .barPoints = row.size.rail,
                          .barInsetPoints = row.space.railInset,
                          .barRadiusPoints = row.radii.rail,
                          .markPoints = row.size.railMark,
                          .barEndPoints = row.space.railEnd,
                          .foldChipHitPoints = row.size.foldChipHit };
}

/// @p points of the chrome at @p scale, in whole device pixels: rounded (R-b), so the strip the page fit
/// reserves and the strip the renderer draws are the one width.
/// @param points A chrome extent in points.
/// @param scale The device-pixel ratio.
/// @return The extent in device pixels.
[[nodiscard]] inline int chromeDevicePixels(int points, double scale) noexcept
{
    return static_cast<int>(std::lround(points * scale));
}

/// The fewest columns a click on a fold chip spans: @p chrome's @c size.foldChipHit in cells @p cellWidthPx
/// device pixels wide at @p scale, rounded up. At least one, the chip's own cells.
/// @param chrome The session's gutter chrome.
/// @param scale The device-pixel ratio.
/// @param cellWidthPx The cell width in device pixels.
/// @return The columns.
[[nodiscard]] inline vtbackend::ColumnCount foldChipHitColumns(GutterChrome const& chrome,
                                                               double scale,
                                                               int cellWidthPx) noexcept
{
    if (cellWidthPx <= 0)
        return vtbackend::ColumnCount(1);
    auto const columns = static_cast<int>(std::ceil(chrome.foldChipHitPoints * scale / cellWidthPx));
    return vtbackend::ColumnCount(std::max(1, columns));
}

} // namespace contour::session
