/**
 * This file is part of the "contour" project
 *   Copyright (c) 2019-2020 Christian Parpart <christian@parpart.family>
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#pragma once

#include <terminal/Screen.h>
#include <terminal/Color.h>

#include <ostream>
#include <utility>

#include <fmt/format.h>

#include <QtCore/QPoint>

namespace terminal::view {

/// GridMetrics contains any valuable metrics required to calculate positions on the grid.
struct GridMetrics
{
    Size pageSize;      // page size in column- and line count
    Size cellSize;      // grid cell size in pixels

    int baseline;           // glyph's baseline position relative to cell bottom.
    int ascender;           // glyph ascender relative to baseline
    int descender;          // glyph descender relative to baseline

    struct {
        int position = 1;   // center underline position relative to cell bottom
        int thickness = 1;  // underline thickness
    } underline{};

    struct {
        int top = 0;
        int left = 0;
        int bottom = 0;
        int right = 0;
    } cellMargin{};         // TODO: use me

    struct {
        int left = 0;
        int bottom = 0;
    } pageMargin{};

    /// Maps screen coordinates to target surface coordinates.
    ///
    /// @param col screen coordinate's column (between 1 and number of screen columns)
    /// @param row screen coordinate's line (between 1 and number of screen lines)
    ///
    /// @return 2D point into drawing coordinate system
    constexpr QPoint map(int col, int row) const noexcept
    {
        return map(Coordinate{row, col});
    }

    constexpr QPoint map(Coordinate const& _pos) const noexcept
    {
        auto const x = pageMargin.left + (_pos.column - 1) * cellSize.width;
        auto const y = pageMargin.bottom + (pageSize.height - _pos.row) * cellSize.height;

        return QPoint{x, y};
    }
};

} // end namespace

namespace fmt
{
    template <>
    struct formatter<terminal::view::GridMetrics> {
        template <typename ParseContext>
        constexpr auto parse(ParseContext& ctx) { return ctx.begin(); }
        template <typename FormatContext>
        auto format(terminal::view::GridMetrics const& v, FormatContext& ctx)
        {
            return format_to(
                ctx.out(),
                "(pageSize={}, cellSize={}, baseline={}, underline={}@{}, margin=(left={}, bottom={}))",
                v.pageSize,
                v.cellSize,
                v.baseline,
                v.underline.position,
                v.underline.thickness,
                v.pageMargin.left,
                v.pageMargin.bottom
            );
        }
    };

}
