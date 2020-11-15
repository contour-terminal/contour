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

    constexpr QPoint map(Coordinate const& _pos) const noexcept {
        return QPoint{
            static_cast<int>(leftMargin + (_pos.column - 1) * cellSize.width),
#if defined(LIBTERMINAL_VIEW_NATURAL_COORDS) && LIBTERMINAL_VIEW_NATURAL_COORDS
            static_cast<int>(bottomMargin + (screenSize.height - _pos.row) * cellSize.height)
#else
            static_cast<int>((_pos.row - 1) * cellSize.height)
#endif
        };
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
