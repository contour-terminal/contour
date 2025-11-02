// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <vtpty/ImageSize.h>
#include <vtpty/PageSize.h>

#include <cstdint>
#include <limits>
#include <optional>
#include <ostream>
#include <string>
#include <variant>

#include <boxed-cpp/boxed.hpp>

// TODO
// - [ ] rename all History to Scrollback
// - [ ] make sense out of all the semantically different line primitives.

namespace vtbackend
{

struct FontDef
{
    double size;
    std::string regular;
    std::string bold;
    std::string italic;
    std::string boldItalic;
    std::string emoji;
};

using LineCount = vtpty::LineCount;
using ColumnCount = vtpty::ColumnCount;
using PageSize = vtpty::PageSize;

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
using ColumnPosition = boxed::boxed<int, detail::tags::ColumnPosition>;

using ColumnOffset = boxed::boxed<int, detail::tags::ColumnOffset>;

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
using LineOffset = boxed::boxed<int, detail::tags::LineOffset>;

/// Represents the number of lines the viewport has been scrolled up into
/// the scrollback lines history.
///
/// A value of 0 means that it is not scrolled at all (bottom), and
/// a value equal to the number of scrollback lines means it is scrolled
/// to the top.
using ScrollOffset = boxed::boxed<int, detail::tags::ScrollOffset>;

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

// {{{ CellLocation and related types
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

    constexpr auto operator<=>(CellLocation const&) const noexcept = default;
};

inline std::ostream& operator<<(std::ostream& os, CellLocation coord)
{
    return os << std::format("({}, {})", coord.line, coord.column);
}

inline CellLocation operator+(CellLocation a, CellLocation b) noexcept
{
    return { .line = a.line + b.line, .column = a.column + b.column };
}

constexpr CellLocation operator+(CellLocation c, LineOffset y) noexcept
{
    return CellLocation { .line = c.line + y, .column = c.column };
}

constexpr CellLocation operator-(CellLocation c, LineOffset y) noexcept
{
    return CellLocation { .line = c.line - y, .column = c.column };
}

constexpr CellLocation operator+(CellLocation c, ColumnOffset x) noexcept
{
    return CellLocation { .line = c.line, .column = c.column + x };
}

constexpr CellLocation operator-(CellLocation c, ColumnOffset x) noexcept
{
    return CellLocation { .line = c.line, .column = c.column - x };
}
// Constructs a top-left and bottom-right coordinate-pair from given input.
constexpr std::pair<CellLocation, CellLocation> orderedPoints(CellLocation a, CellLocation b) noexcept
{
    auto const topLeft =
        CellLocation { .line = std::min(a.line, b.line), .column = std::min(a.column, b.column) };
    auto const bottomRight =
        CellLocation { .line = std::max(a.line, b.line), .column = std::max(a.column, b.column) };
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
        switch (abs(unbox(first.line) - unbox(second.line)))
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
using From = boxed::boxed<int, detail::tags::From>;

/// Represents the last value of a range (inclusive).
using To = boxed::boxed<int, detail::tags::To>;

// Range (e.g. a range of lines from X to Y).
struct Range
{
    From from;
    To to;

    // So you can do: for (auto const v: Range{3, 5}) { ... }
    struct ValueTag
    {
    };
    using iterator = boxed::boxed<int, ValueTag>;
    [[nodiscard]] iterator begin() const { return iterator { from.value }; }
    [[nodiscard]] auto end() const { return iterator { to.value + 1 }; }
    // iterator end() const { return boxed::boxed_cast<iterator>(to) + iterator{1}; }
};

// }}}
// {{{ Rect & Margin

// Rectangular operations
//
using Top = boxed::boxed<int, detail::tags::Top>;
using Left = boxed::boxed<int, detail::tags::Left>;
using Bottom = boxed::boxed<int, detail::tags::Bottom>;
using Right = boxed::boxed<int, detail::tags::Right>;

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
        return Rect { .top = top,
                      .left = left,
                      .bottom = std::min(bottom, Bottom::cast_from(size.lines)),
                      .right = std::min(right, Right::cast_from(size.columns)) };
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
    return Range { .from = From { *m.top }, .to = To { *m.bottom } };
}
constexpr Range vertical(PageMargin m) noexcept
{
    return Range { .from = From { *m.left }, .to = To { *m.right } };
}

// }}}
// {{{ Length

// Lengths and Ranges
using Length = boxed::boxed<int, detail::tags::Length>;

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
        Offset _offset;

        constexpr Offset makeOffset(int offset) noexcept
        {
            return Offset { .line = LineOffset(offset / unbox(_width)),
                            .column = ColumnOffset(offset % unbox(_width)) };
        }
    };

    [[nodiscard]] constexpr iterator begin() const noexcept { return iterator { columns, 0 }; }
    [[nodiscard]] constexpr iterator end() const noexcept
    {
        return iterator { columns, unbox(columns) * unbox(lines) };
    }
};

constexpr CellLocation operator+(CellLocation a, GridSize::Offset b) noexcept
{
    return CellLocation { .line = a.line + b.line, .column = a.column + b.column };
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

using TabStopCount = boxed::boxed<int, detail::tags::TabStopCount>;

// }}}
// {{{ convenience methods

constexpr Length length(Range range) noexcept
{
    // assert(range.to.value >= range.from.value);
    return Length::cast_from(*range.to - *range.from) + Length { 1 };
}

// }}}
// {{{ ImageSize types

using Width = vtpty::Width;
using Height = vtpty::Height;
using ImageSize = vtpty::ImageSize;

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

enum class HighlightSearchMatches : uint8_t
{
    No,
    Yes
};

enum class ScreenType : uint8_t
{
    Primary = 0,
    Alternate = 1,
};

// TODO: Maybe make boxed.h into its own C++ github repo?
// TODO: Differentiate Line/Column types for DECOM enabled/disabled coordinates?
//
// Line, Column                 : respects DECOM if enabled (a.k.a. logical column)
// PhysicalLine, PhysicalColumn : always relative to origin (top left)
// ScrollbackLine               : line number relative to top-most line in scrollback buffer.
//
// Respectively for Coordinates:
// - Coordinate
// - PhysicalCoordinate
// - ScrollbackCoordinate

enum class CursorDisplay : uint8_t
{
    Steady,
    Blink
};

enum class CursorShape : uint8_t
{
    Block,
    Rectangle,
    Underscore,
    Bar,
};

CursorShape makeCursorShape(std::string const& name);

enum class ControlTransmissionMode : uint8_t
{
    S7C1T, // 7-bit controls
    S8C1T, // 8-bit controls
};

enum class GraphicsRendition : uint8_t
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

enum class StatusDisplayType : uint8_t
{
    None,
    Indicator,
    HostWritable,
};

// Mandates the position to show the statusline at.
enum class StatusDisplayPosition : uint8_t
{
    // The status line is classically shown at the bottom of the render target.
    Bottom,

    // The status line is shown at the top of the render target
    Top,
};

// Selects whether the terminal sends data to the main display or the status line.
enum class ActiveStatusDisplay : uint8_t
{
    // Selects the main display. The terminal sends data to the main display only.
    Main,

    // Selects the host-writable status line. The terminal sends data to the status line only.
    StatusLine,

    IndicatorStatusLine,
};

enum class AnsiMode : uint8_t
{
    KeyboardAction = 2,    // KAM
    Insert = 4,            // IRM
    SendReceive = 12,      // SRM
    AutomaticNewLine = 20, // LNM
};

enum class DECMode : std::uint16_t
{
    UseApplicationCursorKeys = 0,
    DesignateCharsetUSASCII = 1,
    Columns132 = 2,
    SmoothScroll = 3,
    ReverseVideo = 4,

    MouseProtocolX10 = 5,
    MouseProtocolNormalTracking = 6,
    MouseProtocolHighlightTracking = 7,
    MouseProtocolButtonTracking = 8,
    MouseProtocolAnyEventTracking = 9,

    SaveCursor = 10,
    ExtendedAltScreen = 11,

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
    Origin = 12,

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
    AutoWrap = 13,

    PrinterExtend = 14,
    LeftRightMargin = 15,

    ShowToolbar = 16,
    BlinkingCursor = 17,
    VisibleCursor = 18, // DECTCEM
    ShowScrollbar = 19,
    AllowColumns80to132 = 20, // ?40
    DebugLogging = 21,        // ?46,
    UseAlternateScreen = 22,
    BracketedPaste = 23,
    FocusTracking = 24,            // 1004
    NoSixelScrolling = 25,         // ?80
    UsePrivateColorRegisters = 26, // ?1070

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

    // If enabled, the terminal will report color palette changes to the application,
    // if modified by the user or operating system (e.g. dark/light mode adaption).
    ReportColorPaletteUpdated = 2031,

    // If enabled (default, as per spec), then the cursor is left next to the graphic,
    // that is, the text cursor is placed at the position of the sixel cursor.
    // If disabled otherwise, the cursor is placed below the image, as if CR LF was sent,
    // which is how xterm behaves by default (sadly).
    SixelCursorNextToGraphic = 8452,
    // }}}
};

/// OSC color-setting related commands that can be grouped into one
enum class DynamicColorName : uint8_t
{
    DefaultForegroundColor,
    DefaultBackgroundColor,
    TextCursorColor,
    MouseForegroundColor,
    MouseBackgroundColor,
    HighlightForegroundColor,
    HighlightBackgroundColor,
};

enum class ViMode : uint8_t
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

constexpr bool isValidAnsiMode(unsigned int mode) noexcept
{
    switch (static_cast<AnsiMode>(mode))
    {
        case AnsiMode::KeyboardAction:
        case AnsiMode::Insert:
        case AnsiMode::SendReceive:
        case AnsiMode::AutomaticNewLine: return true;
    }
    return false;
}

std::string to_string(AnsiMode mode);
std::string to_string(DECMode mode);

constexpr unsigned toDECModeNum(DECMode m) noexcept
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
        case DECMode::ReportGridCellSelection: return 2030;
        case DECMode::ReportColorPaletteUpdated: return 2031;
        case DECMode::BatchedRendering: return 2026;
        case DECMode::Unicode: return 2027;
        case DECMode::TextReflow: return 2028;
        case DECMode::SixelCursorNextToGraphic: return 8452;
    }
    return static_cast<unsigned>(m);
}

constexpr std::optional<DECMode> fromDECModeNum(unsigned int modeNum) noexcept
{
    switch (modeNum)
    {
        case 1: return DECMode::UseApplicationCursorKeys;
        case 2: return DECMode::DesignateCharsetUSASCII;
        case 3: return DECMode::Columns132;
        case 4: return DECMode::SmoothScroll;
        case 5: return DECMode::ReverseVideo;
        case 6: return DECMode::Origin;
        case 7: return DECMode::AutoWrap;
        // TODO: Ps = 8  -> Auto-repeat Keys (DECARM), VT100.
        case 9: return DECMode::MouseProtocolX10;
        case 10: return DECMode::ShowToolbar;
        case 12: return DECMode::BlinkingCursor;
        case 19: return DECMode::PrinterExtend;
        case 25: return DECMode::VisibleCursor;
        case 30: return DECMode::ShowScrollbar;
        // TODO: Ps = 3 5  -> Enable font-shifting functions (rxvt).
        // IGNORE? Ps = 3 8  -> Enter Tektronix Mode (DECTEK), VT240, xterm.
        case 40: return DECMode::AllowColumns80to132;
        // IGNORE: Ps = 4 1  -> more(1) fix (see curses resource).
        // TODO: Ps = 4 2  -> Enable National Replacement Character sets (DECNRCM), VT220.
        // TODO: Ps = 4 4  -> Turn On Margin Bell, xterm.
        // TODO: Ps = 4 5  -> Reverse-wraparound Mode, xterm.
        case 46: return DECMode::DebugLogging;
        case 47: return DECMode::UseAlternateScreen;
        // TODO: Ps = 6 6  -> Application keypad (DECNKM), VT320.
        // TODO: Ps = 6 7  -> Backarrow key sends backspace (DECBKM), VT340, VT420.  This sets the
        // backarrowKey resource to "true".
        case 69: return DECMode::LeftRightMargin;
        case 80: return DECMode::NoSixelScrolling;
        case 1000: return DECMode::MouseProtocolNormalTracking;
        case 1001: return DECMode::MouseProtocolHighlightTracking;
        case 1002: return DECMode::MouseProtocolButtonTracking;
        case 1003: return DECMode::MouseProtocolAnyEventTracking;
        case 1004: return DECMode::FocusTracking;
        case 1005: return DECMode::MouseExtended;
        case 1006: return DECMode::MouseSGR;
        case 1007: return DECMode::MouseAlternateScroll;
        case 1015: return DECMode::MouseURXVT;
        case 1016: return DECMode::MouseSGRPixels;
        case 1048: return DECMode::SaveCursor;
        case 1049: return DECMode::ExtendedAltScreen;
        case 1070: return DECMode::UsePrivateColorRegisters;
        case 2004: return DECMode::BracketedPaste;
        case 2026: return DECMode::BatchedRendering;
        case 2027: return DECMode::Unicode;
        case 2028: return DECMode::TextReflow;
        case 2029: return DECMode::MousePassiveTracking;
        case 2030: return DECMode::ReportGridCellSelection;
        case 2031: return DECMode::ReportColorPaletteUpdated;
        case 8452: return DECMode::SixelCursorNextToGraphic;
        default: return std::nullopt;
    }
}

constexpr bool isValidDECMode(unsigned int mode) noexcept
{
    return fromDECModeNum(mode).has_value();
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

} // namespace vtbackend

namespace std
{
template <>
struct numeric_limits<vtbackend::CursorShape>
{
    constexpr static vtbackend::CursorShape min() noexcept { return vtbackend::CursorShape::Block; }
    constexpr static vtbackend::CursorShape max() noexcept { return vtbackend::CursorShape::Bar; }
    constexpr static size_t count() noexcept { return 4; }
};
} // namespace std

// {{{ fmt formatter
template <>
struct std::formatter<vtbackend::CursorShape>: formatter<std::string_view>
{
    auto format(vtbackend::CursorShape value, auto& ctx) const
    {
        string_view name;
        switch (value)
        {
            case vtbackend::CursorShape::Bar: name = "Bar"; break;
            case vtbackend::CursorShape::Block: name = "Block"; break;
            case vtbackend::CursorShape::Rectangle: name = "Rectangle"; break;
            case vtbackend::CursorShape::Underscore: name = "Underscore"; break;
        }
        return formatter<string_view>::format(name, ctx);
    }
};

template <>
struct std::formatter<vtbackend::CellLocation>: formatter<std::string>
{
    auto format(vtbackend::CellLocation coord, auto& ctx) const
    {
        return formatter<std::string>::format(std::format("({}, {})", coord.line, coord.column), ctx);
    }
};

template <>
struct std::formatter<vtbackend::PageSize>: formatter<std::string>
{
    auto format(vtbackend::PageSize value, auto& ctx) const
    {
        return formatter<std::string>::format(std::format("{}x{}", value.columns, value.lines), ctx);
    }
};

template <>
struct std::formatter<vtbackend::GridSize>: formatter<std::string>
{
    auto format(vtbackend::GridSize value, auto& ctx) const
    {
        return formatter<std::string>::format(std::format("{}x{}", value.columns, value.lines), ctx);
    }
};

template <>
struct std::formatter<vtbackend::ScreenType>: formatter<std::string_view>
{
    auto format(const vtbackend::ScreenType value, auto& ctx) const
    {
        string_view name;
        switch (value)
        {
            case vtbackend::ScreenType::Primary: name = "Primary"; break;
            case vtbackend::ScreenType::Alternate: name = "Alternate"; break;
        }
        return formatter<string_view>::format(name, ctx);
    }
};

template <>
struct std::formatter<vtbackend::PixelCoordinate>: formatter<std::string>
{
    auto format(const vtbackend::PixelCoordinate coord, auto& ctx) const
    {
        return formatter<std::string>::format(std::format("{}:{}", coord.x.value, coord.y.value), ctx);
    }
};

template <>
struct std::formatter<vtbackend::ViMode>: formatter<std::string_view>
{
    auto format(vtbackend::ViMode mode, auto& ctx) const
    {
        using vtbackend::ViMode;
        string_view name;
        switch (mode)
        {
            case ViMode::Normal: name = "Normal"; break;
            case ViMode::Insert: name = "Insert"; break;
            case ViMode::Visual: name = "Visual"; break;
            case ViMode::VisualLine: name = "VisualLine"; break;
            case ViMode::VisualBlock: name = "VisualBlock"; break;
        }
        return formatter<string_view>::format(name, ctx);
    }
};

// }}}
