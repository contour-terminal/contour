/**
 * This file is part of the "libterminal" project
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

#include <vtpty/PageSize.h>

#include <crispy/ImageSize.h>
#include <crispy/boxed.h>

#include <cassert>
#include <cstdint>
#include <limits>
#include <ostream>
#include <type_traits>
#include <variant>

// TODO
// - [ ] rename all History to Scrollback
// - [ ] make sense out of all the semantically different line primitives.

namespace terminal
{

namespace detail::tags // {{{
{
    // clang-format off
    // column types
    struct ColumnOffset {};
    struct ColumnPosition {};

    // line types
    struct LineOffset {};
    struct ScrollOffset {};

    // misc.
    struct TabStopCount {};

    // generic length
    struct Length {};

    // range
    struct From {};
    struct To {};

    // margin
    struct Top {};
    struct Left {};
    struct Bottom {};
    struct Right {};
    // clang-format on
} // namespace detail::tags
// }}}

// {{{ Column types

/// ColumnPosition represents the absolute column on the visibile screen area
/// (usually the main page unless scrolled upwards).
///
/// A column position starts at 1.
using ColumnPosition = crispy::boxed<int, detail::tags::ColumnPosition>;

using ColumnOffset = crispy::boxed<int, detail::tags::ColumnOffset>;

// }}}
// {{{ Line types

// clang-format off
/// Special structure for inifinite history of Grid
struct Infinite {};
// clang-format on
/// MaxHistoryLineCount represents type that are used to store number
/// of lines that can be stored in history
using MaxHistoryLineCount = std::variant<LineCount, Infinite>;
/// Represents the line offset relative to main-page top.
///
/// *  0  is top-most line on main page
/// *  -1 is the bottom most line in scrollback
using LineOffset = crispy::boxed<int, detail::tags::LineOffset>;

/// Represents the number of lines the viewport has been scrolled up into
/// the scrollback lines history.
///
/// A value of 0 means that it is not scrolled at all (bottom), and
/// a value equal to the number of scrollback lines means it is scrolled
/// to the top.
using ScrollOffset = crispy::boxed<int, detail::tags::ScrollOffset>;

constexpr int operator*(LineCount a, ColumnCount b) noexcept
{
    return a.as<int>() * b.as<int>();
}
constexpr int operator*(ColumnCount a, LineCount b) noexcept
{
    return a.as<int>() * b.as<int>();
}
// }}}

struct PixelCoordinate
{
    // clang-format off
    struct X { int value; };
    struct Y { int value; };
    // clang-format on

    X x {};
    Y y {};
};

struct [[nodiscard]] CellLocation
{
    LineOffset line {};
    ColumnOffset column {};

    constexpr CellLocation& operator+=(CellLocation a) noexcept
    {
        line += a.line;
        column += a.column;
        return *this;
    }

    constexpr CellLocation& operator+=(ColumnOffset x) noexcept
    {
        column += x;
        return *this;
    }
    constexpr CellLocation& operator+=(LineOffset y) noexcept
    {
        line += y;
        return *this;
    }
};

inline std::ostream& operator<<(std::ostream& os, CellLocation coord)
{
    return os << fmt::format("({}, {})", coord.line, coord.column);
}

constexpr bool operator==(CellLocation a, CellLocation b) noexcept
{
    return a.line == b.line && a.column == b.column;
}
constexpr bool operator!=(CellLocation a, CellLocation b) noexcept
{
    return !(a == b);
}

constexpr bool operator<(CellLocation a, CellLocation b) noexcept
{
    if (a.line < b.line)
        return true;

    if (a.line == b.line && a.column < b.column)
        return true;

    return false;
}

constexpr bool operator<=(CellLocation a, CellLocation b) noexcept
{
    return a < b || a == b;
}

constexpr bool operator>=(CellLocation a, CellLocation b) noexcept
{
    return !(a < b);
}

constexpr bool operator>(CellLocation a, CellLocation b) noexcept
{
    return !(a == b || a < b);
}

inline CellLocation operator+(CellLocation a, CellLocation b) noexcept
{
    return { a.line + b.line, a.column + b.column };
}

constexpr CellLocation operator+(CellLocation c, LineOffset y) noexcept
{
    return CellLocation { c.line + y, c.column };
}

constexpr CellLocation operator-(CellLocation c, LineOffset y) noexcept
{
    return CellLocation { c.line - y, c.column };
}

constexpr CellLocation operator+(CellLocation c, ColumnOffset x) noexcept
{
    return CellLocation { c.line, c.column + x };
}

constexpr CellLocation operator-(CellLocation c, ColumnOffset x) noexcept
{
    return CellLocation { c.line, c.column - x };
}
// Constructs a top-left and bottom-right coordinate-pair from given input.
constexpr std::pair<CellLocation, CellLocation> orderedPoints(CellLocation a, CellLocation b) noexcept
{
    auto const topLeft = CellLocation { std::min(a.line, b.line), std::min(a.column, b.column) };
    auto const bottomRight = CellLocation { std::max(a.line, b.line), std::max(a.column, b.column) };
    return std::pair { topLeft, bottomRight };
}

/// Tests whether given CellLocation is within the right hand side's PageSize.
constexpr bool operator<(CellLocation location, PageSize pageSize) noexcept
{
    return location.line < boxed_cast<LineOffset>(pageSize.lines)
           && location.column < boxed_cast<ColumnOffset>(pageSize.columns);
}

struct CellLocationRange
{
    CellLocation first;
    CellLocation second;

    [[nodiscard]] bool contains(CellLocation location) const noexcept
    {
        switch (abs(unbox<int>(first.line) - unbox<int>(second.line)))
        {
            case 0: // range is single line
                return location.line == first.line && first.column <= location.column
                       && location.column <= second.column;
            case 1: // range is two lines
                return (location.line == first.line && first.column <= location.column)
                       || (location.line == second.line && location.column <= second.column);
            default: // range is more than two lines
                return (location.line == first.line && first.column <= location.column)
                       || (first.line < location.line && location.line < second.line)
                       || (location.line == second.line && location.column <= second.column);
                break;
        }
        return false;
    }
};

struct ColumnRange
{
    LineOffset line;
    ColumnOffset fromColumn;
    ColumnOffset toColumn;

    [[nodiscard]] constexpr ColumnCount length() const noexcept
    {
        return boxed_cast<ColumnCount>(toColumn - fromColumn + 1);
    }

    [[nodiscard]] constexpr bool contains(CellLocation location) const noexcept
    {
        return line == location.line && fromColumn <= location.column && location.column <= toColumn;
    }
};

// }}}
// {{{ Range

/// Represents the first value of a range.
using From = crispy::boxed<int, detail::tags::From>;

/// Represents the last value of a range (inclusive).
using To = crispy::boxed<int, detail::tags::To>;

// Range (e.g. a range of lines from X to Y).
struct Range
{
    From from;
    To to;

    // So you can do: for (auto const v: Range{3, 5}) { ... }
    struct ValueTag
    {
    };
    using iterator = crispy::boxed<int, ValueTag>;
    [[nodiscard]] iterator begin() const { return iterator { from.value }; }
    [[nodiscard]] auto end() const { return iterator { to.value + 1 }; }
    // iterator end() const { return crispy::boxed_cast<iterator>(to) + iterator{1}; }
};

// }}}
// {{{ Rect & Margin

// Rectangular operations
//
using Top = crispy::boxed<int, detail::tags::Top>;
using Left = crispy::boxed<int, detail::tags::Left>;
using Bottom = crispy::boxed<int, detail::tags::Bottom>;
using Right = crispy::boxed<int, detail::tags::Right>;

// Rectangular screen operations
//
struct Rect
{
    Top top;
    Left left;
    Bottom bottom;
    Right right;

    [[nodiscard]] Rect clampTo(PageSize size) const noexcept
    {
        return Rect { top,
                      left,
                      std::min(bottom, Bottom::cast_from(size.lines)),
                      std::min(right, Right::cast_from(size.columns)) };
    }
};

// Screen's page margin
//
struct PageMargin
{
    Top top;
    Left left;
    Bottom bottom;
    Right right;
};

constexpr Range horizontal(PageMargin m) noexcept
{
    return Range { From { *m.top }, To { *m.bottom } };
}
constexpr Range vertical(PageMargin m) noexcept
{
    return Range { From { *m.left }, To { *m.right } };
}

// }}}
// {{{ Length

// Lengths and Ranges
using Length = crispy::boxed<int, detail::tags::Length>;

// }}}
// {{{ Coordinate types

// (0, 0) is home position
struct ScreenPosition
{
    LineOffset line;
    ColumnOffset column;
};

// }}}
// {{{ GridSize

struct GridSize
{
    LineCount lines;
    ColumnCount columns;

    struct Offset
    {
        LineOffset line;
        ColumnOffset column;
    };

    /// This iterator can be used to iterate through each and every point between (0, 0) and (width, height).
    struct iterator
    {
      public:
        constexpr iterator(ColumnCount _width, int _next) noexcept:
            width { _width }, next { _next }, offset { makeOffset(_next) }
        {
        }

        constexpr auto operator*() const noexcept { return offset; }

        constexpr iterator& operator++() noexcept
        {
            offset = makeOffset(++next);
            return *this;
        }

        constexpr iterator& operator++(int) noexcept
        {
            ++*this;
            return *this;
        }

        constexpr bool operator==(iterator const& other) const noexcept { return next == other.next; }
        constexpr bool operator!=(iterator const& other) const noexcept { return next != other.next; }

      private:
        ColumnCount width;
        int next;
        Offset offset;

        constexpr Offset makeOffset(int offset) noexcept
        {
            return Offset { LineOffset(offset / *width), ColumnOffset(offset % *width) };
        }
    };

    [[nodiscard]] constexpr iterator begin() const noexcept { return iterator { columns, 0 }; }
    [[nodiscard]] constexpr iterator end() const noexcept { return iterator { columns, *columns * *lines }; }

    [[nodiscard]] constexpr iterator begin() noexcept { return iterator { columns, 0 }; }
    [[nodiscard]] constexpr iterator end() noexcept { return iterator { columns, *columns * *lines }; }
};

constexpr CellLocation operator+(CellLocation a, GridSize::Offset b) noexcept
{
    return CellLocation { a.line + b.line, a.column + b.column };
}

constexpr GridSize::iterator begin(GridSize const& s) noexcept
{
    return s.begin();
}
constexpr GridSize::iterator end(GridSize const& s) noexcept
{
    return s.end();
}
// }}}
// {{{ misc

using TabStopCount = crispy::boxed<int, detail::tags::TabStopCount>;

// }}}
// {{{ convenience methods

constexpr Length length(Range range) noexcept
{
    // assert(range.to.value >= range.from.value);
    return Length::cast_from(*range.to - *range.from) + Length { 1 };
}

// }}}
// {{{ ImageSize types

using Width = crispy::Width;
using Height = crispy::Height;
using ImageSize = crispy::ImageSize;

// }}}
// {{{ Mixed boxed types operator overloads
constexpr LineCount operator+(LineCount a, LineOffset b) noexcept
{
    return a + b.value;
}
constexpr LineCount operator-(LineCount a, LineOffset b) noexcept
{
    return a - b.value;
}
constexpr LineOffset& operator+=(LineOffset& a, LineCount b) noexcept
{
    a.value += b.value;
    return a;
}
constexpr LineOffset& operator-=(LineOffset& a, LineCount b) noexcept
{
    a.value -= b.value;
    return a;
}

constexpr ColumnCount operator+(ColumnCount a, ColumnOffset b) noexcept
{
    return a + b.value;
}
constexpr ColumnCount operator-(ColumnCount a, ColumnOffset b) noexcept
{
    return a - b.value;
}
constexpr ColumnOffset& operator+=(ColumnOffset& a, ColumnCount b) noexcept
{
    a.value += b.value;
    return a;
}
constexpr ColumnOffset& operator-=(ColumnOffset& a, ColumnCount b) noexcept
{
    a.value -= b.value;
    return a;
}
// }}}

enum class HighlightSearchMatches
{
    No,
    Yes
};

enum class ScreenType
{
    Primary = 0,
    Alternate = 1
};

// TODO: Maybe make boxed.h into its own C++ github repo?
// TODO: Differenciate Line/Column types for DECOM enabled/disabled coordinates?
//
// Line, Column                 : respects DECOM if enabled (a.k.a. logical column)
// PhysicalLine, PhysicalColumn : always relative to origin (top left)
// ScrollbackLine               : line number relative to top-most line in scrollback buffer.
//
// Respectively for Coordinates:
// - Coordinate
// - PhysicalCoordinate
// - ScrollbackCoordinate

enum class CursorDisplay
{
    Steady,
    Blink
};

enum class CursorShape
{
    Block,
    Rectangle,
    Underscore,
    Bar,
};

CursorShape makeCursorShape(std::string const& _name);

enum class ControlTransmissionMode
{
    S7C1T, // 7-bit controls
    S8C1T, // 8-bit controls
};

enum class GraphicsRendition
{
    Reset = 0, //!< Reset any rendition (style as well as foreground / background coloring).

    Bold = 1,              //!< Bold glyph width
    Faint = 2,             //!< Decreased intensity
    Italic = 3,            //!< Italic glyph
    Underline = 4,         //!< Underlined glyph
    Blinking = 5,          //!< Blinking glyph
    RapidBlinking = 6,     //!< Blinking glyph
    Inverse = 7,           //!< Swaps foreground with background color.
    Hidden = 8,            //!< Glyph hidden (somewhat like space character).
    CrossedOut = 9,        //!< Crossed out glyph space.
    DoublyUnderlined = 21, //!< Underlined with two lines.

    Normal = 22,       //!< Neither Bold nor Faint.
    NoItalic = 23,     //!< Reverses Italic.
    NoUnderline = 24,  //!< Reverses Underline.
    NoBlinking = 25,   //!< Reverses Blinking.
    NoInverse = 27,    //!< Reverses Inverse.
    NoHidden = 28,     //!< Reverses Hidden (Visible).
    NoCrossedOut = 29, //!< Reverses CrossedOut.

    CurlyUnderlined = 30, //!< Curly line below the baseline.
    DottedUnderline = 31, //!< Dotted line below the baseline.
    DashedUnderline = 32, //!< Dashed line below the baseline.
    Framed = 51,          //!< Frames the glyph with lines on all sides
    Overline = 53,        //!< Overlined glyph
    NoFramed = 54,        //!< Reverses Framed
    NoOverline = 55,      //!< Reverses Overline.
};

enum class StatusDisplayType
{
    None,
    Indicator,
    HostWritable,
};

// Selects whether the terminal sends data to the main display or the status line.
enum class ActiveStatusDisplay
{
    // Selects the main display. The terminal sends data to the main display only.
    Main,

    // Selects the host-writable status line. The terminal sends data to the status line only.
    StatusLine,
};

enum class AnsiMode
{
    KeyboardAction = 2,    // KAM
    Insert = 4,            // IRM
    SendReceive = 12,      // SRM
    AutomaticNewLine = 20, // LNM
};

enum class DECMode
{
    UseApplicationCursorKeys,
    DesignateCharsetUSASCII,
    Columns132,
    SmoothScroll,
    ReverseVideo,

    MouseProtocolX10,
    MouseProtocolNormalTracking,
    MouseProtocolHighlightTracking,
    MouseProtocolButtonTracking,
    MouseProtocolAnyEventTracking,

    SaveCursor,
    ExtendedAltScreen,

    /**
     * DECOM - Origin Mode.
     *
     * This control function sets the origin for the cursor.
     * DECOM determines if the cursor position is restricted to inside the page margins.
     * When you power up or reset the terminal, you reset origin mode.
     *
     * Default: Origin is at the upper-left of the screen, independent of margins.
     *
     * When DECOM is set, the home cursor position is at the upper-left corner of the screen, within the
     * margins. The starting point for line numbers depends on the current top margin setting. The cursor
     * cannot move outside of the margins.
     *
     * When DECOM is reset, the home cursor position is at the upper-left corner of the screen.
     * The starting point for line numbers is independent of the margins.
     * The cursor can move outside of the margins.
     */
    Origin,

    /**
     * DECAWM - Autowrap Mode.
     *
     * This control function determines whether or not received characters automatically wrap
     * to the next line when the cursor reaches the right border of a page in page memory.
     *
     * If the DECAWM function is set, then graphic characters received when the cursor
     * is at the right border of the page appear at the beginning of the next line.
     *
     * Any text on the page scrolls up if the cursor is at the end of the scrolling region.
     */
    AutoWrap,

    PrinterExtend,
    LeftRightMargin,

    ShowToolbar,
    BlinkingCursor,
    VisibleCursor, // DECTCEM
    ShowScrollbar,
    AllowColumns80to132, // ?40
    DebugLogging,        // ?46,
    UseAlternateScreen,
    BracketedPaste,
    FocusTracking,            // 1004
    NoSixelScrolling,         // ?80
    UsePrivateColorRegisters, // ?1070

    // {{{ Mouse related flags
    /// extend mouse protocl encoding
    MouseExtended = 1005,

    /// Uses a (SGR-style?) different encoding.
    MouseSGR = 1006,

    // URXVT invented extend mouse protocol
    MouseURXVT = 1015,

    // SGR-Pixels, like SGR but with pixels instead of line/column positions.
    MouseSGRPixels = 1016,

    /// Toggles scrolling in alternate screen buffer, encodes CUP/CUD instead of mouse wheel events.
    MouseAlternateScroll = 1007,
    // }}}
    // {{{ Extensions
    // This merely resembles the "Synchronized Output" feature from iTerm2, except that it is using
    // a different VT sequence to be enabled. Instead of a DCS,
    // this feature is using CSI ? 2026 h (DECSM and DECRM).
    BatchedRendering = 2026,

    // See https://github.com/contour-terminal/terminal-unicode-core
    Unicode = 2027,

    // If this mode is unset, text reflow is blocked on on this line and any lines below.
    // If this mode is set, the current line and any line below is allowed to reflow.
    // Default: Enabled (if supported by terminal).
    TextReflow = 2028,

    // Tell the terminal emulator that the application is only passively tracking on mouse events.
    // This for example might be used by the terminal emulator to still allow mouse selection.
    MousePassiveTracking = 2029,

    // If enabled (default, as per spec), then the cursor is left next to the graphic,
    // that is, the text cursor is placed at the position of the sixel cursor.
    // If disabled otherwise, the cursor is placed below the image, as if CR LF was sent,
    // which is how xterm behaves by default (sadly).
    SixelCursorNextToGraphic = 8452,
    // }}}
};

/// OSC color-setting related commands that can be grouped into one
enum class DynamicColorName
{
    DefaultForegroundColor,
    DefaultBackgroundColor,
    TextCursorColor,
    MouseForegroundColor,
    MouseBackgroundColor,
    HighlightForegroundColor,
    HighlightBackgroundColor,
};

std::string to_string(GraphicsRendition s);

constexpr unsigned toAnsiModeNum(AnsiMode m)
{
    switch (m)
    {
        case AnsiMode::KeyboardAction: return 2;
        case AnsiMode::Insert: return 4;
        case AnsiMode::SendReceive: return 12;
        case AnsiMode::AutomaticNewLine: return 20;
    }
    return static_cast<unsigned>(m);
}

constexpr bool isValidAnsiMode(unsigned int _mode) noexcept
{
    switch (static_cast<AnsiMode>(_mode))
    {
        case AnsiMode::KeyboardAction:
        case AnsiMode::Insert:
        case AnsiMode::SendReceive:
        case AnsiMode::AutomaticNewLine: return true;
    }
    return false;
}

std::string to_string(AnsiMode _mode);
std::string to_string(DECMode _mode);

constexpr unsigned toDECModeNum(DECMode m)
{
    switch (m)
    {
        case DECMode::UseApplicationCursorKeys: return 1;
        case DECMode::DesignateCharsetUSASCII: return 2;
        case DECMode::Columns132: return 3;
        case DECMode::SmoothScroll: return 4;
        case DECMode::ReverseVideo: return 5;
        case DECMode::Origin: return 6;
        case DECMode::AutoWrap: return 7;
        case DECMode::MouseProtocolX10: return 9;
        case DECMode::ShowToolbar: return 10;
        case DECMode::BlinkingCursor: return 12;
        case DECMode::PrinterExtend: return 19;
        case DECMode::VisibleCursor: return 25;
        case DECMode::ShowScrollbar: return 30;
        case DECMode::AllowColumns80to132: return 40;
        case DECMode::DebugLogging: return 46;
        case DECMode::UseAlternateScreen: return 47;
        case DECMode::LeftRightMargin: return 69;
        case DECMode::MouseProtocolNormalTracking: return 1000;
        case DECMode::MouseProtocolHighlightTracking: return 1001;
        case DECMode::MouseProtocolButtonTracking: return 1002;
        case DECMode::MouseProtocolAnyEventTracking: return 1003;
        case DECMode::SaveCursor: return 1048;
        case DECMode::ExtendedAltScreen: return 1049;
        case DECMode::BracketedPaste: return 2004;
        case DECMode::FocusTracking: return 1004;
        case DECMode::NoSixelScrolling: return 80;
        case DECMode::UsePrivateColorRegisters: return 1070;
        case DECMode::MouseExtended: return 1005;
        case DECMode::MouseSGR: return 1006;
        case DECMode::MouseURXVT: return 1015;
        case DECMode::MouseSGRPixels: return 1016;
        case DECMode::MouseAlternateScroll: return 1007;
        case DECMode::MousePassiveTracking: return 2029;
        case DECMode::BatchedRendering: return 2026;
        case DECMode::Unicode: return 2027;
        case DECMode::TextReflow: return 2028;
        case DECMode::SixelCursorNextToGraphic: return 8452;
    }
    return static_cast<unsigned>(m);
}

constexpr bool isValidDECMode(unsigned int _mode) noexcept
{
    switch (static_cast<DECMode>(_mode))
    {
        case DECMode::UseApplicationCursorKeys:
        case DECMode::DesignateCharsetUSASCII:
        case DECMode::Columns132:
        case DECMode::SmoothScroll:
        case DECMode::ReverseVideo:
        case DECMode::MouseProtocolX10:
        case DECMode::MouseProtocolNormalTracking:
        case DECMode::MouseProtocolHighlightTracking:
        case DECMode::MouseProtocolButtonTracking:
        case DECMode::MouseProtocolAnyEventTracking:
        case DECMode::SaveCursor:
        case DECMode::ExtendedAltScreen:
        case DECMode::Origin:
        case DECMode::AutoWrap:
        case DECMode::PrinterExtend:
        case DECMode::LeftRightMargin:
        case DECMode::ShowToolbar:
        case DECMode::BlinkingCursor:
        case DECMode::VisibleCursor:
        case DECMode::ShowScrollbar:
        case DECMode::AllowColumns80to132:
        case DECMode::DebugLogging:
        case DECMode::UseAlternateScreen:
        case DECMode::BracketedPaste:
        case DECMode::FocusTracking:
        case DECMode::NoSixelScrolling:
        case DECMode::UsePrivateColorRegisters:
        case DECMode::MouseExtended:
        case DECMode::MouseSGR:
        case DECMode::MouseURXVT:
        case DECMode::MouseSGRPixels:
        case DECMode::MouseAlternateScroll:
        case DECMode::MousePassiveTracking:
        case DECMode::BatchedRendering:
        case DECMode::Unicode:
        case DECMode::TextReflow:
        case DECMode::SixelCursorNextToGraphic:
            //.
            return true;
    }
    return false;
}

constexpr DynamicColorName getChangeDynamicColorCommand(unsigned value)
{
    switch (value)
    {
        case 10: return DynamicColorName::DefaultForegroundColor;
        case 11: return DynamicColorName::DefaultBackgroundColor;
        case 12: return DynamicColorName::TextCursorColor;
        case 13: return DynamicColorName::MouseForegroundColor;
        case 14: return DynamicColorName::MouseBackgroundColor;
        case 19: return DynamicColorName::HighlightForegroundColor;
        case 17: return DynamicColorName::HighlightBackgroundColor;
        default: return DynamicColorName::DefaultForegroundColor;
    }
}

constexpr unsigned setDynamicColorCommand(DynamicColorName name)
{
    switch (name)
    {
        case DynamicColorName::DefaultForegroundColor: return 10;
        case DynamicColorName::DefaultBackgroundColor: return 11;
        case DynamicColorName::TextCursorColor: return 12;
        case DynamicColorName::MouseForegroundColor: return 13;
        case DynamicColorName::MouseBackgroundColor: return 14;
        case DynamicColorName::HighlightForegroundColor: return 19;
        case DynamicColorName::HighlightBackgroundColor: return 17;
        default: return 0;
    }
}

struct SearchResult
{
    ColumnOffset column;           // column at the start of match
    size_t partialMatchLength = 0; // length of partial match that happens at either end
};

} // namespace terminal

namespace std
{
template <>
struct numeric_limits<terminal::CursorShape>
{
    constexpr static terminal::CursorShape min() noexcept { return terminal::CursorShape::Block; }
    constexpr static terminal::CursorShape max() noexcept { return terminal::CursorShape::Bar; }
    constexpr static size_t count() noexcept { return 4; }
};
} // namespace std

// {{{ fmt formatter
namespace fmt
{

template <>
struct formatter<terminal::CursorShape>
{
    template <typename ParseContext>
    constexpr auto parse(ParseContext& ctx)
    {
        return ctx.begin();
    }
    template <typename FormatContext>
    auto format(terminal::CursorShape value, FormatContext& ctx)
    {
        switch (value)
        {
            case terminal::CursorShape::Bar: return fmt::format_to(ctx.out(), "Bar");
            case terminal::CursorShape::Block: return fmt::format_to(ctx.out(), "Block");
            case terminal::CursorShape::Rectangle: return fmt::format_to(ctx.out(), "Rectangle");
            case terminal::CursorShape::Underscore: return fmt::format_to(ctx.out(), "Underscore");
        }
        return fmt::format_to(ctx.out(), "{}", static_cast<unsigned>(value));
    }
};

template <>
struct formatter<terminal::CellLocation>
{
    template <typename ParseContext>
    constexpr auto parse(ParseContext& ctx)
    {
        return ctx.begin();
    }
    template <typename FormatContext>
    auto format(terminal::CellLocation coord, FormatContext& ctx)
    {
        return fmt::format_to(ctx.out(), "({}, {})", coord.line, coord.column);
    }
};

template <>
struct formatter<terminal::PageSize>
{
    template <typename ParseContext>
    constexpr auto parse(ParseContext& ctx)
    {
        return ctx.begin();
    }
    template <typename FormatContext>
    auto format(terminal::PageSize value, FormatContext& ctx)
    {
        return fmt::format_to(ctx.out(), "{}x{}", value.columns, value.lines);
    }
};

template <>
struct formatter<terminal::GridSize>
{
    template <typename ParseContext>
    constexpr auto parse(ParseContext& ctx)
    {
        return ctx.begin();
    }
    template <typename FormatContext>
    auto format(terminal::GridSize value, FormatContext& ctx)
    {
        return fmt::format_to(ctx.out(), "{}x{}", value.columns, value.lines);
    }
};

template <>
struct formatter<terminal::ScreenType>
{
    template <typename ParseContext>
    constexpr auto parse(ParseContext& ctx)
    {
        return ctx.begin();
    }

    template <typename FormatContext>
    auto format(const terminal::ScreenType value, FormatContext& ctx)
    {
        switch (value)
        {
            case terminal::ScreenType::Primary: return fmt::format_to(ctx.out(), "Primary");
            case terminal::ScreenType::Alternate: return fmt::format_to(ctx.out(), "Alternate");
        }
        return fmt::format_to(ctx.out(), "({})", static_cast<unsigned>(value));
    }
};

template <>
struct formatter<terminal::PixelCoordinate>
{
    template <typename ParseContext>
    constexpr auto parse(ParseContext& ctx)
    {
        return ctx.begin();
    }

    template <typename FormatContext>
    auto format(const terminal::PixelCoordinate coord, FormatContext& ctx)
    {
        return fmt::format_to(ctx.out(), "{}:{}", coord.x.value, coord.y.value);
    }
};

} // namespace fmt
// }}}
