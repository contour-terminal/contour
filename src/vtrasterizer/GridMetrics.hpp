// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <vtbackend/core/Primitives.hpp>

#include <crispy/Point.hpp>
#include <crispy/Size.hpp>

#include <cmath>
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
    constexpr crispy::Point map(vtbackend::LineOffset line,
                                vtbackend::ColumnOffset column,
                                int yPixelOffset = 0) const noexcept
    {
        return mapTopLeft(line, column, yPixelOffset);
    }

    constexpr crispy::Point map(vtbackend::CellLocation pos, int yPixelOffset = 0) const noexcept
    {
        return map(pos.line, pos.column, yPixelOffset);
    }

    /// Maps a screen position that may lie between cells to target surface coordinates, as a cell's top left
    /// is mapped: where a cursor moving between two cells is drawn.
    ///
    /// @param pos           screen position, in cells
    /// @param yPixelOffset  sub-cell Y pixel offset for smooth scrolling (default: 0)
    ///
    /// @return 2D point in drawing system coordinates, to the nearest pixel.
    [[nodiscard]] crispy::Point map(vtbackend::FractionalCellLocation pos,
                                    int yPixelOffset = 0) const noexcept
    {
        auto const x =
            pageMargin.left + static_cast<int>(std::lround(pos.column * cellSize.width.as<float>()));
        auto const y = pageMargin.top + static_cast<int>(std::lround(pos.line * cellSize.height.as<float>()))
                       + yPixelOffset;
        return { .x = x, .y = y };
    }

    constexpr crispy::Point mapTopLeft(vtbackend::CellLocation pos, int yPixelOffset = 0) const noexcept
    {
        return mapTopLeft(pos.line, pos.column, yPixelOffset);
    }

    constexpr crispy::Point mapTopLeft(vtbackend::LineOffset line,
                                       vtbackend::ColumnOffset column,
                                       int yPixelOffset = 0) const noexcept
    {
        auto const x = pageMargin.left + (*column * cellSize.width.as<int>());
        auto const y = pageMargin.top + (*line * cellSize.height.as<int>()) + yPixelOffset;

        return { .x = x, .y = y };
    }

    constexpr crispy::Point mapBottomLeft(vtbackend::CellLocation pos, int yPixelOffset = 0) const noexcept
    {
        return mapBottomLeft(pos.line, pos.column, yPixelOffset);
    }
    constexpr crispy::Point mapBottomLeft(vtbackend::LineOffset line,
                                          vtbackend::ColumnOffset column,
                                          int yPixelOffset = 0) const noexcept
    {
        return mapTopLeft(line + 1, column, yPixelOffset);
    }
};

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
