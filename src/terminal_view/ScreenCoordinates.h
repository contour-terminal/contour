#pragma once

#include <terminal/Screen.h>
#include <terminal/Color.h>

#include <ostream>
#include <utility>

#include <fmt/format.h>

#include <QtCore/QPoint>

namespace terminal::view {

struct ScreenCoordinates {
    Size screenSize;
    Size cellSize;

    /// baseline for the pen in relative to cell bottom.
    int textBaseline;

    int leftMargin = 0;
    int bottomMargin = 0;

    /// Maps screen coordinates to target surface coordinates.
    ///
    /// @param col screen coordinate's column (between 1 and number of screen columns)
    /// @param row screen coordinate's line (between 1 and number of screen lines)
    ///
    /// @return 2D point into drawing coordinate system
    constexpr QPoint map(int col, int row) const noexcept {
        return map(Coordinate{row, col});
    }

    constexpr QPoint map(Coordinate const& _pos) const noexcept
    {
        auto const x = leftMargin + (_pos.column - 1) * cellSize.width;
        auto const y = bottomMargin + (screenSize.height - _pos.row) * cellSize.height;

        return QPoint{x, y};
    }
};

} // end namespace

namespace fmt
{
    template <>
    struct formatter<terminal::view::ScreenCoordinates> {
        template <typename ParseContext>
        constexpr auto parse(ParseContext& ctx) { return ctx.begin(); }
        template <typename FormatContext>
        auto format(terminal::view::ScreenCoordinates const& v, FormatContext& ctx)
        {
            return format_to(ctx.out(),
                            "(screenSize={}, cellSize={}, base={}, margin=(left={}, bottom={}))",
                            v.screenSize,
                            v.cellSize,
                            v.textBaseline,
                            v.leftMargin,
                            v.bottomMargin);
        }
    };

}
