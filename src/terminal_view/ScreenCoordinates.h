#pragma once

#include <terminal/Screen.h>
#include <terminal/Color.h>

#include <ostream>
#include <utility>

#include <QtCore/QPoint>

namespace terminal::view {

struct ScreenCoordinates {
    Size screenSize;
    int cellWidth;
    int cellHeight;
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
            static_cast<int>(leftMargin + (_pos.column - 1) * cellWidth),
#if defined(LIBTERMINAL_VIEW_NATURAL_COORDS) && LIBTERMINAL_VIEW_NATURAL_COORDS
            static_cast<int>(bottomMargin + (screenSize.height - _pos.row) * cellHeight)
#else
            static_cast<int>((_pos.row - 1) * cellHeight)
#endif
        };
    }
};

inline std::ostream& operator<<(std::ostream& os, terminal::view::ScreenCoordinates const& _coords)
{
    os << "screen: " << _coords.screenSize.width<< 'x' << _coords.screenSize.height
       << ", cell:" << _coords.cellWidth << 'x' << _coords.cellHeight
       << ", base: " << _coords.textBaseline
       << ", margin: " << _coords.leftMargin << 'x' << _coords.bottomMargin;
    return os;
}

} // end namespace
