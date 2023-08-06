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
    struct column_offset {};
    struct column_position {};

    // line types
    struct line_offset {};
    struct scroll_offset {};

    // misc.
    struct tab_stop_count {};

    // generic length
    struct length {};

    // range
    struct from {};
    struct to {};

    // margin
    struct top {};
    struct left {};
    struct bottom {};
    struct right {};
    // clang-format on
} // namespace detail::tags
// }}}

// {{{ Column types

/// ColumnPosition represents the absolute column on the visibile screen area
/// (usually the main page unless scrolled upwards).
///
/// A column position starts at 1.
using column_position = crispy::boxed<int, detail::tags::column_position>;

using column_offset = crispy::boxed<int, detail::tags::column_offset>;

// }}}
// {{{ Line types

// clang-format off
/// Special structure for inifinite history of Grid
struct infinite {};
// clang-format on
/// MaxHistoryLineCount represents type that are used to store number
/// of lines that can be stored in history
using max_history_line_count = std::variant<LineCount, infinite>;
/// Represents the line offset relative to main-page top.
///
/// *  0  is top-most line on main page
/// *  -1 is the bottom most line in scrollback
using line_offset = crispy::boxed<int, detail::tags::line_offset>;

/// Represents the number of lines the viewport has been scrolled up into
/// the scrollback lines history.
///
/// A value of 0 means that it is not scrolled at all (bottom), and
/// a value equal to the number of scrollback lines means it is scrolled
/// to the top.
using scroll_offset = crispy::boxed<int, detail::tags::scroll_offset>;

constexpr int operator*(LineCount a, ColumnCount b) noexcept
{
    return a.as<int>() * b.as<int>();
}
constexpr int operator*(ColumnCount a, LineCount b) noexcept
{
    return a.as<int>() * b.as<int>();
}
// }}}

struct pixel_coordinate
{
    // clang-format off
    struct x { int value; };
    struct y { int value; };
    // clang-format on

    x x {};
    y y {};
};

struct [[nodiscard]] cell_location
{
    line_offset line {};
    column_offset column {};

    constexpr cell_location& operator+=(cell_location a) noexcept
    {
        line += a.line;
        column += a.column;
        return *this;
    }

    constexpr cell_location& operator+=(column_offset x) noexcept
    {
        column += x;
        return *this;
    }
    constexpr cell_location& operator+=(line_offset y) noexcept
    {
        line += y;
        return *this;
    }
};

inline std::ostream& operator<<(std::ostream& os, cell_location coord)
{
    return os << fmt::format("({}, {})", coord.line, coord.column);
}

constexpr bool operator==(cell_location a, cell_location b) noexcept
{
    return a.line == b.line && a.column == b.column;
}
constexpr bool operator!=(cell_location a, cell_location b) noexcept
{
    return !(a == b);
}

constexpr bool operator<(cell_location a, cell_location b) noexcept
{
    if (a.line < b.line)
        return true;

    if (a.line == b.line && a.column < b.column)
        return true;

    return false;
}

constexpr bool operator<=(cell_location a, cell_location b) noexcept
{
    return a < b || a == b;
}

constexpr bool operator>=(cell_location a, cell_location b) noexcept
{
    return !(a < b);
}

constexpr bool operator>(cell_location a, cell_location b) noexcept
{
    return !(a == b || a < b);
}

inline cell_location operator+(cell_location a, cell_location b) noexcept
{
    return { a.line + b.line, a.column + b.column };
}

constexpr cell_location operator+(cell_location c, line_offset y) noexcept
{
    return cell_location { c.line + y, c.column };
}

constexpr cell_location operator-(cell_location c, line_offset y) noexcept
{
    return cell_location { c.line - y, c.column };
}

constexpr cell_location operator+(cell_location c, column_offset x) noexcept
{
    return cell_location { c.line, c.column + x };
}

constexpr cell_location operator-(cell_location c, column_offset x) noexcept
{
    return cell_location { c.line, c.column - x };
}
// Constructs a top-left and bottom-right coordinate-pair from given input.
constexpr std::pair<cell_location, cell_location> orderedPoints(cell_location a, cell_location b) noexcept
{
    auto const topLeft = cell_location { std::min(a.line, b.line), std::min(a.column, b.column) };
    auto const bottomRight = cell_location { std::max(a.line, b.line), std::max(a.column, b.column) };
    return std::pair { topLeft, bottomRight };
}

/// Tests whether given CellLocation is within the right hand side's PageSize.
constexpr bool operator<(cell_location location, PageSize pageSize) noexcept
{
    return location.line < boxed_cast<line_offset>(pageSize.lines)
           && location.column < boxed_cast<column_offset>(pageSize.columns);
}

struct cell_location_range
{
    cell_location first;
    cell_location second;

    [[nodiscard]] bool contains(cell_location location) const noexcept
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

struct column_range
{
    line_offset line;
    column_offset fromColumn;
    column_offset toColumn;

    [[nodiscard]] constexpr ColumnCount length() const noexcept
    {
        return boxed_cast<ColumnCount>(toColumn - fromColumn + 1);
    }

    [[nodiscard]] constexpr bool contains(cell_location location) const noexcept
    {
        return line == location.line && fromColumn <= location.column && location.column <= toColumn;
    }
};

// }}}
// {{{ Range

/// Represents the first value of a range.
using from = crispy::boxed<int, detail::tags::from>;

/// Represents the last value of a range (inclusive).
using to = crispy::boxed<int, detail::tags::to>;

// Range (e.g. a range of lines from X to Y).
struct range
{
    from from;
    to to;

    // So you can do: for (auto const v: Range{3, 5}) { ... }
    struct value_tag
    {
    };
    using iterator = crispy::boxed<int, value_tag>;
    [[nodiscard]] iterator begin() const { return iterator { from.value }; }
    [[nodiscard]] auto end() const { return iterator { to.value + 1 }; }
    // iterator end() const { return crispy::boxed_cast<iterator>(to) + iterator{1}; }
};

// }}}
// {{{ Rect & Margin

// Rectangular operations
//
using top = crispy::boxed<int, detail::tags::top>;
using left = crispy::boxed<int, detail::tags::left>;
using bottom = crispy::boxed<int, detail::tags::bottom>;
using right = crispy::boxed<int, detail::tags::right>;

// Rectangular screen operations
//
struct rect
{
    top top;
    left left;
    bottom bottom;
    right right;

    [[nodiscard]] rect clampTo(PageSize size) const noexcept
    {
        return rect { top,
                      left,
                      std::min(bottom, bottom::cast_from(size.lines)),
                      std::min(right, right::cast_from(size.columns)) };
    }
};

// Screen's page margin
//
struct page_margin
{
    top top;
    left left;
    bottom bottom;
    right right;
};

constexpr range horizontal(page_margin m) noexcept
{
    return range { from { *m.top }, to { *m.bottom } };
}
constexpr range vertical(page_margin m) noexcept
{
    return range { from { *m.left }, to { *m.right } };
}

// }}}
// {{{ Length

// Lengths and Ranges
using length = crispy::boxed<int, detail::tags::length>;

// }}}
// {{{ Coordinate types

// (0, 0) is home position
struct screen_position
{
    line_offset line;
    column_offset column;
};

// }}}
// {{{ GridSize

struct grid_size
{
    LineCount lines;
    ColumnCount columns;

    struct offset
    {
        line_offset line;
        column_offset column;
    };

    /// This iterator can be used to iterate through each and every point between (0, 0) and (width, height).
    struct iterator // NOLINT(readability-identifier-naming)
    {
      public:
        constexpr iterator(ColumnCount width, int next) noexcept:
            _width { width }, _next { next }, _offset { makeOffset(next) }
        {
        }

        constexpr auto operator*() const noexcept { return _offset; }

        constexpr iterator& operator++() noexcept
        {
            _offset = makeOffset(++_next);
            return *this;
        }

        constexpr iterator& operator++(int) noexcept
        {
            ++*this;
            return *this;
        }

        constexpr bool operator==(iterator const& other) const noexcept { return _next == other._next; }
        constexpr bool operator!=(iterator const& other) const noexcept { return _next != other._next; }

      private:
        ColumnCount _width;
        int _next;
        offset _offset;

        constexpr offset makeOffset(int offsetValue) noexcept
        {
            return offset { line_offset(offsetValue / *_width), column_offset(offsetValue % *_width) };
        }
    };

    [[nodiscard]] constexpr iterator begin() const noexcept { return iterator { columns, 0 }; }
    [[nodiscard]] constexpr iterator end() const noexcept { return iterator { columns, *columns * *lines }; }
};

constexpr cell_location operator+(cell_location a, grid_size::offset b) noexcept
{
    return cell_location { a.line + b.line, a.column + b.column };
}

constexpr grid_size::iterator begin(grid_size const& s) noexcept
{
    return s.begin();
}
constexpr grid_size::iterator end(grid_size const& s) noexcept
{
    return s.end();
}
// }}}
// {{{ misc

using tab_stop_count = crispy::boxed<int, detail::tags::tab_stop_count>;

// }}}
// {{{ convenience methods

constexpr length make_length(range range) noexcept
{
    // assert(range.to.value >= range.from.value);
    return length::cast_from(*range.to - *range.from) + length { 1 };
}

// }}}
// {{{ ImageSize types

using width = crispy::Width;
using height = crispy::Height;
using image_size = crispy::ImageSize;

// }}}
// {{{ Mixed boxed types operator overloads
constexpr LineCount operator+(LineCount a, line_offset b) noexcept
{
    return a + b.value;
}
constexpr LineCount operator-(LineCount a, line_offset b) noexcept
{
    return a - b.value;
}
constexpr line_offset& operator+=(line_offset& a, LineCount b) noexcept
{
    a.value += b.value;
    return a;
}
constexpr line_offset& operator-=(line_offset& a, LineCount b) noexcept
{
    a.value -= b.value;
    return a;
}

constexpr ColumnCount operator+(ColumnCount a, column_offset b) noexcept
{
    return a + b.value;
}
constexpr ColumnCount operator-(ColumnCount a, column_offset b) noexcept
{
    return a - b.value;
}
constexpr column_offset& operator+=(column_offset& a, ColumnCount b) noexcept
{
    a.value += b.value;
    return a;
}
constexpr column_offset& operator-=(column_offset& a, ColumnCount b) noexcept
{
    a.value -= b.value;
    return a;
}
// }}}

enum class highlight_search_matches
{
    No,
    Yes
};

enum class screen_type
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

enum class cursor_display
{
    Steady,
    Blink
};

enum class cursor_shape
{
    Block,
    Rectangle,
    Underscore,
    Bar,
};

cursor_shape makeCursorShape(std::string const& name);

enum class control_transmission_mode
{
    S7C1T, // 7-bit controls
    S8C1T, // 8-bit controls
};

enum class graphics_rendition
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

enum class status_display_type
{
    None,
    Indicator,
    HostWritable,
};

// Mandates the position to show the statusline at.
enum class status_display_position
{
    // The status line is classically shown at the bottom of the render target.
    Bottom,

    // The status line is shown at the top of the render target
    Top,
};

// Selects whether the terminal sends data to the main display or the status line.
enum class active_status_display
{
    // Selects the main display. The terminal sends data to the main display only.
    Main,

    // Selects the host-writable status line. The terminal sends data to the status line only.
    StatusLine,

    IndicatorStatusLine,
};

enum class ansi_mode
{
    KeyboardAction = 2,    // KAM
    Insert = 4,            // IRM
    SendReceive = 12,      // SRM
    AutomaticNewLine = 20, // LNM
};

enum class dec_mode
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

    // If enabled, UI text selection will be reported to the application for the regions
    // intersecting with the main page area.
    ReportGridCellSelection = 2030,

    // If enabled (default, as per spec), then the cursor is left next to the graphic,
    // that is, the text cursor is placed at the position of the sixel cursor.
    // If disabled otherwise, the cursor is placed below the image, as if CR LF was sent,
    // which is how xterm behaves by default (sadly).
    SixelCursorNextToGraphic = 8452,
    // }}}
};

/// OSC color-setting related commands that can be grouped into one
enum class dynamic_color_name
{
    DefaultForegroundColor,
    DefaultBackgroundColor,
    TextCursorColor,
    MouseForegroundColor,
    MouseBackgroundColor,
    HighlightForegroundColor,
    HighlightBackgroundColor,
};

enum class vi_mode
{
    /// Vi-like normal-mode.
    Normal, // <Escape>, <C-[>

    /// Vi-like insert/terminal mode.
    Insert, // i

    /// Vi-like visual select mode.
    Visual, // v

    /// Vi-like visual line-select mode.
    VisualLine, // V

    /// Vi-like visual block-select mode.
    VisualBlock, // <C-V>
};

std::string to_string(graphics_rendition s);

constexpr unsigned toAnsiModeNum(ansi_mode m)
{
    switch (m)
    {
        case ansi_mode::KeyboardAction: return 2;
        case ansi_mode::Insert: return 4;
        case ansi_mode::SendReceive: return 12;
        case ansi_mode::AutomaticNewLine: return 20;
    }
    return static_cast<unsigned>(m);
}

constexpr bool isValidAnsiMode(unsigned int mode) noexcept
{
    switch (static_cast<ansi_mode>(mode))
    {
        case ansi_mode::KeyboardAction:
        case ansi_mode::Insert:
        case ansi_mode::SendReceive:
        case ansi_mode::AutomaticNewLine: return true;
    }
    return false;
}

std::string to_string(ansi_mode mode);
std::string to_string(dec_mode mode);

constexpr unsigned toDECModeNum(dec_mode m)
{
    switch (m)
    {
        case dec_mode::UseApplicationCursorKeys: return 1;
        case dec_mode::DesignateCharsetUSASCII: return 2;
        case dec_mode::Columns132: return 3;
        case dec_mode::SmoothScroll: return 4;
        case dec_mode::ReverseVideo: return 5;
        case dec_mode::Origin: return 6;
        case dec_mode::AutoWrap: return 7;
        case dec_mode::MouseProtocolX10: return 9;
        case dec_mode::ShowToolbar: return 10;
        case dec_mode::BlinkingCursor: return 12;
        case dec_mode::PrinterExtend: return 19;
        case dec_mode::VisibleCursor: return 25;
        case dec_mode::ShowScrollbar: return 30;
        case dec_mode::AllowColumns80to132: return 40;
        case dec_mode::DebugLogging: return 46;
        case dec_mode::UseAlternateScreen: return 47;
        case dec_mode::LeftRightMargin: return 69;
        case dec_mode::MouseProtocolNormalTracking: return 1000;
        case dec_mode::MouseProtocolHighlightTracking: return 1001;
        case dec_mode::MouseProtocolButtonTracking: return 1002;
        case dec_mode::MouseProtocolAnyEventTracking: return 1003;
        case dec_mode::SaveCursor: return 1048;
        case dec_mode::ExtendedAltScreen: return 1049;
        case dec_mode::BracketedPaste: return 2004;
        case dec_mode::FocusTracking: return 1004;
        case dec_mode::NoSixelScrolling: return 80;
        case dec_mode::UsePrivateColorRegisters: return 1070;
        case dec_mode::MouseExtended: return 1005;
        case dec_mode::MouseSGR: return 1006;
        case dec_mode::MouseURXVT: return 1015;
        case dec_mode::MouseSGRPixels: return 1016;
        case dec_mode::MouseAlternateScroll: return 1007;
        case dec_mode::MousePassiveTracking: return 2029;
        case dec_mode::ReportGridCellSelection: return 2030;
        case dec_mode::BatchedRendering: return 2026;
        case dec_mode::Unicode: return 2027;
        case dec_mode::TextReflow: return 2028;
        case dec_mode::SixelCursorNextToGraphic: return 8452;
    }
    return static_cast<unsigned>(m);
}

constexpr bool isValidDECMode(unsigned int mode) noexcept
{
    switch (static_cast<dec_mode>(mode))
    {
        case dec_mode::UseApplicationCursorKeys:
        case dec_mode::DesignateCharsetUSASCII:
        case dec_mode::Columns132:
        case dec_mode::SmoothScroll:
        case dec_mode::ReverseVideo:
        case dec_mode::MouseProtocolX10:
        case dec_mode::MouseProtocolNormalTracking:
        case dec_mode::MouseProtocolHighlightTracking:
        case dec_mode::MouseProtocolButtonTracking:
        case dec_mode::MouseProtocolAnyEventTracking:
        case dec_mode::SaveCursor:
        case dec_mode::ExtendedAltScreen:
        case dec_mode::Origin:
        case dec_mode::AutoWrap:
        case dec_mode::PrinterExtend:
        case dec_mode::LeftRightMargin:
        case dec_mode::ShowToolbar:
        case dec_mode::BlinkingCursor:
        case dec_mode::VisibleCursor:
        case dec_mode::ShowScrollbar:
        case dec_mode::AllowColumns80to132:
        case dec_mode::DebugLogging:
        case dec_mode::UseAlternateScreen:
        case dec_mode::BracketedPaste:
        case dec_mode::FocusTracking:
        case dec_mode::NoSixelScrolling:
        case dec_mode::UsePrivateColorRegisters:
        case dec_mode::MouseExtended:
        case dec_mode::MouseSGR:
        case dec_mode::MouseURXVT:
        case dec_mode::MouseSGRPixels:
        case dec_mode::MouseAlternateScroll:
        case dec_mode::MousePassiveTracking:
        case dec_mode::ReportGridCellSelection:
        case dec_mode::BatchedRendering:
        case dec_mode::Unicode:
        case dec_mode::TextReflow:
        case dec_mode::SixelCursorNextToGraphic:
            //.
            return true;
    }
    return false;
}

constexpr dynamic_color_name getChangeDynamicColorCommand(unsigned value)
{
    switch (value)
    {
        case 10: return dynamic_color_name::DefaultForegroundColor;
        case 11: return dynamic_color_name::DefaultBackgroundColor;
        case 12: return dynamic_color_name::TextCursorColor;
        case 13: return dynamic_color_name::MouseForegroundColor;
        case 14: return dynamic_color_name::MouseBackgroundColor;
        case 19: return dynamic_color_name::HighlightForegroundColor;
        case 17: return dynamic_color_name::HighlightBackgroundColor;
        default: return dynamic_color_name::DefaultForegroundColor;
    }
}

constexpr unsigned setDynamicColorCommand(dynamic_color_name name)
{
    switch (name)
    {
        case dynamic_color_name::DefaultForegroundColor: return 10;
        case dynamic_color_name::DefaultBackgroundColor: return 11;
        case dynamic_color_name::TextCursorColor: return 12;
        case dynamic_color_name::MouseForegroundColor: return 13;
        case dynamic_color_name::MouseBackgroundColor: return 14;
        case dynamic_color_name::HighlightForegroundColor: return 19;
        case dynamic_color_name::HighlightBackgroundColor: return 17;
        default: return 0;
    }
}

struct search_result
{
    column_offset column;          // column at the start of match
    size_t partialMatchLength = 0; // length of partial match that happens at either end
};

} // namespace terminal

namespace std
{
template <>
struct numeric_limits<terminal::cursor_shape>
{
    constexpr static terminal::cursor_shape min() noexcept { return terminal::cursor_shape::Block; }
    constexpr static terminal::cursor_shape max() noexcept { return terminal::cursor_shape::Bar; }
    constexpr static size_t count() noexcept { return 4; }
};
} // namespace std

// {{{ fmt formatter
template <>
struct fmt::formatter<terminal::cursor_shape>: formatter<std::string_view>
{
    auto format(terminal::cursor_shape value, format_context& ctx) -> format_context::iterator
    {
        string_view name;
        switch (value)
        {
            case terminal::cursor_shape::Bar: name = "Bar"; break;
            case terminal::cursor_shape::Block: name = "Block"; break;
            case terminal::cursor_shape::Rectangle: name = "Rectangle"; break;
            case terminal::cursor_shape::Underscore: name = "Underscore"; break;
        }
        return formatter<string_view>::format(name, ctx);
    }
};

template <>
struct fmt::formatter<terminal::cell_location>: formatter<std::string>
{
    auto format(terminal::cell_location coord, format_context& ctx) -> format_context::iterator
    {
        return formatter<std::string>::format(fmt::format("({}, {})", coord.line, coord.column), ctx);
    }
};

template <>
struct fmt::formatter<terminal::PageSize>: formatter<std::string>
{
    auto format(terminal::PageSize value, format_context& ctx) -> format_context::iterator
    {
        return formatter<std::string>::format(fmt::format("{}x{}", value.columns, value.lines), ctx);
    }
};

template <>
struct fmt::formatter<terminal::grid_size>: formatter<std::string>
{
    auto format(terminal::grid_size value, format_context& ctx) -> format_context::iterator
    {
        return formatter<std::string>::format(fmt::format("{}x{}", value.columns, value.lines), ctx);
    }
};

template <>
struct fmt::formatter<terminal::screen_type>: formatter<std::string_view>
{
    auto format(const terminal::screen_type value, format_context& ctx) -> format_context::iterator
    {
        string_view name;
        switch (value)
        {
            case terminal::screen_type::Primary: name = "Primary"; break;
            case terminal::screen_type::Alternate: name = "Alternate"; break;
        }
        return formatter<string_view>::format(name, ctx);
    }
};

template <>
struct fmt::formatter<terminal::pixel_coordinate>: formatter<std::string>
{
    auto format(const terminal::pixel_coordinate coord, format_context& ctx) -> format_context::iterator
    {
        return formatter<std::string>::format(fmt::format("{}:{}", coord.x.value, coord.y.value), ctx);
    }
};

template <>
struct fmt::formatter<terminal::vi_mode>: formatter<std::string_view>
{
    auto format(terminal::vi_mode mode, format_context& ctx) -> format_context::iterator
    {
        using terminal::vi_mode;
        string_view name;
        switch (mode)
        {
            case vi_mode::Normal: name = "Normal"; break;
            case vi_mode::Insert: name = "Insert"; break;
            case vi_mode::Visual: name = "Visual"; break;
            case vi_mode::VisualLine: name = "VisualLine"; break;
            case vi_mode::VisualBlock: name = "VisualBlock"; break;
        }
        return formatter<string_view>::format(name, ctx);
    }
};

// }}}
