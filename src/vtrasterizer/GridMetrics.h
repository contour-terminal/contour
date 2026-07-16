// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <vtbackend/primitives.h>

#include <crispy/point.h>
#include <crispy/size.h>

#include <format>

namespace vtrasterizer
{

/**
 * Uniform cell margin for the grid view.
 *
 * Values are usually 0 or positive but MAY also be negative.
 */
struct CellMargin
{
    int top = 0;
    int left = 0;
    int bottom = 0;
    int right = 0;
};

/**
 * margin for the render view, so that the text isn't glued to the edge of the window/view.
 */
struct PageMargin
{
    int left;
    int top;
    int bottom;
};

/// GridMetrics contains any valuable metrics required to calculate positions on the grid.
struct GridMetrics
{
    vtbackend::PageSize pageSize;  // page size in column- and line count
    vtbackend::ImageSize cellSize; // grid cell size in pixels

    int baseline = 0; // glyph's baseline position relative to cell bottom.

    struct
    {
        int position = 1;  // center underline position relative to cell bottom
        int thickness = 1; // underline thickness
    } underline {};

    CellMargin cellMargin {}; // TODO: implement respecting cell margins.
    PageMargin pageMargin {};

    /// Maps screen coordinates to target surface coordinates.
    ///
    /// @param line          screen coordinate's line (between 0 and number of screen lines minus 1)
    /// @param column        screen coordinate's column (between 0 and number of screen columns minus 1)
    /// @param yPixelOffset  sub-cell Y pixel offset for smooth scrolling (default: 0)
    ///
    /// @return 2D point into the grid cell's top left in drawing system coordinates.
    constexpr crispy::point map(vtbackend::LineOffset line,
                                vtbackend::ColumnOffset column,
                                int yPixelOffset = 0) const noexcept
    {
        return mapTopLeft(line, column, yPixelOffset);
    }

    constexpr crispy::point map(vtbackend::CellLocation pos, int yPixelOffset = 0) const noexcept
    {
        return map(pos.line, pos.column, yPixelOffset);
    }

    constexpr crispy::point mapTopLeft(vtbackend::CellLocation pos, int yPixelOffset = 0) const noexcept
    {
        return mapTopLeft(pos.line, pos.column, yPixelOffset);
    }

    constexpr crispy::point mapTopLeft(vtbackend::LineOffset line,
                                       vtbackend::ColumnOffset column,
                                       int yPixelOffset = 0) const noexcept
    {
        auto const x = pageMargin.left + (*column * cellSize.width.as<int>());
        auto const y = pageMargin.top + (*line * cellSize.height.as<int>()) + yPixelOffset;

        return { .x = x, .y = y };
    }

    constexpr crispy::point mapBottomLeft(vtbackend::CellLocation pos, int yPixelOffset = 0) const noexcept
    {
        return mapBottomLeft(pos.line, pos.column, yPixelOffset);
    }
    constexpr crispy::point mapBottomLeft(vtbackend::LineOffset line,
                                          vtbackend::ColumnOffset column,
                                          int yPixelOffset = 0) const noexcept
    {
        return mapTopLeft(line + 1, column, yPixelOffset);
    }
};

/// Rounds a device-pixel cell extent up to a whole number of logical pixels.
///
/// A cell that is not a multiple of the content scale has no whole size in logical pixels -- and
/// logical pixels are what the terminal reports to applications. A 19px cell at scale 2 is 9.5
/// logical: an application can only ask for 9, and the half pixel it drops on every column
/// accumulates across the row. Measured on a 150-column window that is 150 device pixels, ~5% of
/// the width, of image that simply does not get drawn. The vertical axis of the same window was
/// exact only because its 44px cell happened to divide by 2.
///
/// Rounds UP so a glyph's own advance always still fits the cell it is drawn in.
///
/// @note At a fractional scale the quantum is coarse: whole logical pixels at 1.75 (= 7/4) means
///       the cell must be a multiple of 7 device pixels, so 19 becomes 21 rather than 20. That is
///       inherent -- there is no finer cell that has a whole size in both units.
/// @param deviceExtent The font's own extent in device pixels.
/// @param scale        Device pixels per logical pixel.
/// @return The extent rounded up to the next whole logical pixel, never smaller than the input.
[[nodiscard]] constexpr int snapToWholeLogicalPixel(int deviceExtent, double scale) noexcept
{
    if (!(scale > 1.0) || deviceExtent <= 0)
        return deviceExtent; // an unscaled display already measures in whole logical pixels

    // The smallest device extent with a whole logical size is a multiple of the scale's numerator.
    // Rather than factorize the scale, walk up to the next extent that divides cleanly -- the search
    // is bounded by the denominator, which is tiny for every scale a display actually reports.
    auto constexpr Slack = 1e-6;
    auto constexpr MaxSteps = 64; // 64 device px covers any real cell; beyond it, leave the cell be
    for (auto step = 0; step < MaxSteps; ++step)
    {
        auto const candidate = deviceExtent + step;
        auto const logical = static_cast<double>(candidate) / scale;
        auto const rounded = static_cast<double>(static_cast<int>(logical + 0.5));
        if (logical > rounded - Slack && logical < rounded + Slack)
            return candidate;
    }
    return deviceExtent;
}

} // namespace vtrasterizer

template <>
struct std::formatter<vtrasterizer::GridMetrics>: formatter<std::string>
{
    auto format(vtrasterizer::GridMetrics const& v, auto& ctx) const
    {
        return formatter<std::string>::format(
            std::format(
                "(pageSize={}, cellSize={}, baseline={}, underline={}@{}, margin=(left={}, bottom={}))",
                v.pageSize,
                v.cellSize,
                v.baseline,
                v.underline.position,
                v.underline.thickness,
                v.pageMargin.left,
                v.pageMargin.bottom),
            ctx);
    }
};
