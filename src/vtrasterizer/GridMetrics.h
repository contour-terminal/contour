// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <vtbackend/primitives.h>

#include <crispy/point.h>
#include <crispy/size.h>

#include <fmt/format.h>

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
    /// @param col screen coordinate's column (between 0 and number of screen columns minus 1)
    /// @param line screen coordinate's line (between 0 and number of screen lines minus 1)
    ///
    /// @return 2D point into the grid cell's top left in drawing system coordinates.
    constexpr crispy::point map(vtbackend::LineOffset line, vtbackend::ColumnOffset column) const noexcept
    {
        return mapTopLeft(line, column);
    }

    constexpr crispy::point map(vtbackend::CellLocation pos) const noexcept
    {
        return map(pos.line, pos.column);
    }

    constexpr crispy::point mapTopLeft(vtbackend::CellLocation pos) const noexcept
    {
        return mapTopLeft(pos.line, pos.column);
    }

    constexpr crispy::point mapTopLeft(vtbackend::LineOffset line,
                                       vtbackend::ColumnOffset column) const noexcept
    {
        auto const x = pageMargin.left + *column * cellSize.width.as<int>();
        auto const y = pageMargin.top + *line * cellSize.height.as<int>();

        return { x, y };
    }

    constexpr crispy::point mapBottomLeft(vtbackend::CellLocation pos) const noexcept
    {
        return mapBottomLeft(pos.line, pos.column);
    }
    constexpr crispy::point mapBottomLeft(vtbackend::LineOffset line,
                                          vtbackend::ColumnOffset column) const noexcept
    {
        return mapTopLeft(line + 1, column);
    }
};

} // namespace vtrasterizer

template <>
struct fmt::formatter<vtrasterizer::GridMetrics>: formatter<std::string>
{
    auto format(vtrasterizer::GridMetrics const& v,
                fmt::format_context& ctx) const -> format_context::iterator
    {
        return formatter<std::string>::format(
            fmt::format(
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
