// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <vtpty/ImageSize.h>
#include <vtpty/PageSize.h>

#include <crispy/flags.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <iterator>
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

    // page
    struct PageIndex {};

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

/// Linearly interpolates between two cell locations.
/// At t=0 returns @p a, at t=1 returns @p b.
///
/// @param a  start location.
/// @param b  end location.
/// @param t  interpolation factor in [0, 1].
///
/// @return the component-wise interpolated CellLocation.
constexpr CellLocation lerpCellLocation(CellLocation a, CellLocation b, float t) noexcept
{
    return CellLocation {
        .line = a.line + LineOffset::cast_from(t * static_cast<float>(*b.line - *a.line)),
        .column = a.column + ColumnOffset::cast_from(t * static_cast<float>(*b.column - *a.column)),
    };
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

    [[nodiscard]] CellLocationRange ordered() const noexcept
    {
        auto [a, b] = orderedPoints(first, second);
        return CellLocationRange { .first = a, .second = b };
    }

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
/// A rectangular area of the page, as zero-based offsets, both corners inclusive.
struct Rect
{
    Top top;
    Left left;
    Bottom bottom;
    Right right;
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

/// Zero-based page index for DEC multi-page support.
/// Pages 0..14 map to DEC pages 1..15. Page 15 (index) is the Xterm alternate screen.
using PageIndex = boxed::boxed<int, detail::tags::PageIndex>;

/// Maximum number of pages supported (DEC pages 1-15 + Xterm alternate screen at index 15).
constexpr int MaxPageCount = 16;

/// The page index reserved for the Xterm alternate screen buffer.
constexpr PageIndex AlternateScreenPageIndex { MaxPageCount - 1 };

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

/// Returns the ScreenType for a given page index.
constexpr ScreenType screenTypeFromPage(PageIndex page) noexcept
{
    return page == PageIndex(0) ? ScreenType::Primary : ScreenType::Alternate;
}

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

/// Determines the visual style of cell blink animation (SGR 5/6).
enum class BlinkStyle : uint8_t
{
    Classic, //!< Abrupt on/off toggle at half-period intervals.
    Smooth,  //!< Continuous cosine-based pulse.
    Linger,  //!< Like Smooth but stays visible longer.
};

/// Determines the visual style of screen transitions between primary and alternate screens.
enum class ScreenTransitionStyle : uint8_t
{
    Classic, ///< Instant switch (no animation).
    Fade,    ///< Smooth crossfade over a configurable duration.
};

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

/// The ANSI modes ECMA-48 defines that this terminal recognizes but has hard-wired *off*.
///
/// DECRQM must answer 4 (permanently reset) for these, and not 0 (not recognized). The two are
/// different claims, and applications rely on the difference: 0 says "I have never heard of this mode",
/// 4 says "I know exactly what you mean, and it can never be on here". A terminal that answers 0 for a
/// mode the standard defines is disclaiming knowledge it has.
///
/// None of these has any bearing on a terminal that is not a printing VT with guarded areas and
/// selectable transfer semantics -- which is to say, on any terminal built in the last forty years.
/// They are listed so that Contour can say so precisely, rather than by silence.
///
/// Adding a mode is adding a row.
constexpr auto PermanentlyResetAnsiModes = std::array<unsigned, 12> {
    1,  // GATM -- guarded area transfer
    5,  // SRTM -- status reporting transfer
    7,  // VEM  -- vertical editing
    10, // HEM  -- horizontal editing
    11, // PUM  -- positioning unit
    13, // FEAM -- format effector action
    14, // FETM -- format effector transfer
    15, // MATM -- multiple area transfer
    16, // TTM  -- transfer termination
    17, // SATM -- selected area transfer
    18, // TSM  -- tabulation stop
    19, // EBM  -- editing boundary
};

/// @return Whether @p mode is an ANSI mode this terminal knows of but can never turn on.
constexpr bool isPermanentlyResetAnsiMode(unsigned mode) noexcept
{
    return std::ranges::find(PermanentlyResetAnsiModes, mode) != PermanentlyResetAnsiModes.end();
}

/// DEC private modes this terminal recognises but can never turn on, so DECRQM answers
/// PermanentlyReset (4) -- "I know exactly what you mean, and it can never be on here" -- rather than
/// Reset (2) or NotRecognized (0).
///
/// Adding a mode is adding a row.
constexpr auto PermanentlyResetDECModes = std::array<unsigned, 1> {
    60, // DECHCCM -- horizontal cursor coupling. Contour's page never scrolls horizontally.
};

/// @return Whether @p mode is a DEC private mode this terminal knows of but can never turn on.
constexpr bool isPermanentlyResetDECMode(unsigned mode) noexcept
{
    return std::ranges::find(PermanentlyResetDECModes, mode) != PermanentlyResetDECModes.end();
}

enum class DECMode : std::uint8_t
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

    /// DECSET 1047 -- Optional Alternate Screen Buffer (xterm).
    ///
    /// Behaves like mode 47 (@ref UseAlternateScreen) on entry -- the alternate page is NOT erased and
    /// the cursor is carried across continuously -- but on exit it erases the alternate page first
    /// (xterm's FromAlternate-with-clear). @see alternateScreenBehavior. The ordinal (69) is only this
    /// mode's index into the DEC-mode bitset; it is unrelated to the DEC mode number 1047.
    OptionalAltScreen = 69,

    /// DECNKM — Numeric Keypad Mode (VT320).
    ///
    /// When set, the numeric keypad generates application sequences (same as DECKPAM).
    /// When reset, the numeric keypad generates numeric characters (same as DECKPNM).
    ApplicationKeypad = 27,

    /// DECARM — Auto Repeat Mode (VT100).
    ///
    /// When set (default), keys auto-repeat when held down.
    /// When reset, keyboard auto-repeat is disabled.
    AutoRepeat = 28,

    /// DECBKM — Backarrow Key Mode (VT340, VT420).
    ///
    /// When set, the Backspace key sends BS (0x08).
    /// When reset (default), the Backspace key sends DEL (0x7F).
    BackarrowKey = 29,

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

    /// DECPCCM — Page Cursor Coupling Mode (VT420).
    ///
    /// When set (default), switching the cursor to a different page also updates the displayed page.
    /// When reset, the displayed page and cursor page can be independent.
    PageCursorCoupling = 30,

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
    /// Extend mouse protocol encoding (DEC mode 1005).
    MouseExtended = 31,

    /// Uses a (SGR-style?) different encoding (DEC mode 1006).
    MouseSGR = 32,

    /// URXVT invented extend mouse protocol (DEC mode 1015).
    MouseURXVT = 33,

    /// SGR-Pixels, like SGR but with pixels instead of line/column positions (DEC mode 1016).
    MouseSGRPixels = 34,

    /// Toggles scrolling in alternate screen buffer, encodes CUP/CUD instead of mouse wheel events
    /// (DEC mode 1007).
    MouseAlternateScroll = 35,
    // }}}
    // {{{ Extensions
    /// Synchronized Output (DEC mode 2026).
    ///
    /// This merely resembles the "Synchronized Output" feature from iTerm2, except that it is using
    /// a different VT sequence to be enabled. Instead of a DCS,
    /// this feature is using CSI ? 2026 h (DECSM and DECRM).
    BatchedRendering = 36,

    /// See https://github.com/contour-terminal/terminal-unicode-core (DEC mode 2027).
    Unicode = 37,

    /// Text reflow control (DEC mode 2028).
    ///
    /// If this mode is unset, text reflow is blocked on this line and any lines below.
    /// If this mode is set, the current line and any line below is allowed to reflow.
    /// Default: Enabled (if supported by terminal).
    TextReflow = 38,

    /// Passive mouse tracking (DEC mode 2029).
    ///
    /// Tell the terminal emulator that the application is only passively tracking on mouse events.
    /// This for example might be used by the terminal emulator to still allow mouse selection.
    MousePassiveTracking = 39,

    /// Grid cell selection reporting (DEC mode 2030).
    ///
    /// If enabled, UI text selection will be reported to the application for the regions
    /// intersecting with the main page area.
    ReportGridCellSelection = 40,

    /// Color palette update reporting (DEC mode 2031).
    ///
    /// If enabled, the terminal will report color palette changes to the application,
    /// if modified by the user or operating system (e.g. dark/light mode adaption).
    ReportColorPaletteUpdated = 41,

    /// DEC Private Mode 2034 — Semantic Block Reader Protocol.
    ///
    /// When enabled, the terminal tracks semantic zones from OSC 133 shell integration
    /// and the query sequence CSI > Ps ; Pn b becomes available for retrieving structured
    /// JSON blocks of semantic command data.
    SemanticBlockProtocol = 42,

    /// Sixel cursor positioning (DEC mode 8452).
    ///
    /// If enabled (default, as per spec), then the cursor is left next to the graphic,
    /// that is, the text cursor is placed at the position of the sixel cursor.
    /// If disabled otherwise, the cursor is placed below the image, as if CR LF was sent,
    /// which is how xterm behaves by default (sadly).
    SixelCursorNextToGraphic = 43,

    /// Win32 Input Mode (DEC private mode 9001).
    ///
    /// When enabled, key events are sent in the Win32 KEY_EVENT_RECORD format:
    /// CSI Vk ; Sc ; Uc ; Kd ; Cs ; Rc _
    /// This is the native input protocol for Windows ConPTY. When both Win32 Input Mode
    /// and CSI u (Kitty keyboard protocol) are active, Win32 Input Mode takes precedence
    /// as the more powerful and versatile protocol for the ConPTY environment.
    Win32InputMode = 44,
    // }}}

    // {{{ Modes the terminal remembers and reports, but which have nothing here to act on.
    //
    // DECRQM's contract is to report a mode's *state*, and SM/RM's is to change it. A terminal that
    // faithfully remembers what it was told is not lying; it would only be lying if it claimed an
    // effect. These four describe a printing VT with a national keyboard and a page memory Contour
    // does not have -- so there is nothing for them to do here, and saying "I have never heard of
    // that mode" would be the less truthful answer.

    /// DECPFF (18) -- Print Form Feed. Contour has no printer.
    PrintFormFeed = 45,

    /// DECHEBM (35) -- Hebrew/N-A Keyboard Mapping. Contour has no keyboard mapping of its own.
    HebrewKeyboardMapping = 46,

    /// DECNRCM (42) -- National Replacement Character Set.
    ///
    /// Remembered and reported, but not yet acted on: the 96-character sets and the GR register it
    /// selects between are not implemented. When they are, this stops being inert.
    NationalReplacementCharacterSet = 47,

    /// DECHCCM (60) -- Horizontal Cursor Coupling. Contour's page never scrolls horizontally.
    HorizontalCursorCoupling = 48,
    // }}}

    /// more(1) fix (41), xterm's -- a curses workaround, aka the "curses hack".
    ///
    /// When set, a horizontal tab arriving while a wrap is pending (the line was just filled to the
    /// right margin) honours the pending wrap first -- moving to the next line -- and then tabs. When
    /// reset (the default), the tab is swallowed at the right margin and the pending wrap waits for the
    /// next printable character. @see Screen::moveCursorToNextTab.
    MoreFix = 70,

    /// Reverse wraparound (45), xterm's.
    ///
    /// With it -- and DECAWM -- a backspace at the left margin moves to the right margin of the line
    /// above, but only if the text actually wrapped onto this line. It does nothing on its own: a
    /// terminal that does not wrap forward has no wrap to reverse.
    ReverseWraparound = 49,

    /// Extended reverse wraparound (1045), xterm's -- "reverse-wrap without limits".
    ///
    /// As above, but it follows *any* line, wrapped or not, and from the top of the scrolling region it
    /// comes back round at the bottom.
    ReverseWraparoundExtended = 50,

    // {{{ VT525 keyboard / national / hardware modes -- settable, but not yet acted on.
    //
    // TODO: These are the DEC private modes a VT525 defines that Contour does not yet *implement*. Each
    // is a real, distinct mode, so Contour remembers and reports its state (SM/RM toggle it, DECRQM
    // reports Set/Reset) -- but nothing here acts on the bit yet. This is deliberately a step above the
    // "PermanentlyReset" block above (modes 45..48): those can *never* mean anything here, whereas these
    // are on the roadmap to gain real behaviour. As each is implemented, move its handling out of the
    // "toggle only" default and this comment shrinks.
    //
    // The bidirectional pair is the priority: RightToLeftMode and
    // HebrewEncodingMode are the entry points for real bidirectional/Hebrew support, not mere toggles.

    /// DECRLM (34) -- Right-to-Left Mode. TODO: drive bidirectional layout; the priority of this group.
    RightToLeftMode = 51,

    /// DECHEM (36) -- Hebrew Encoding Mode. TODO: pair with DECRLM for real Hebrew/bidirectional text.
    HebrewEncodingMode = 52,

    /// DECNAKB (57) -- Greek/N-A Keyboard Mapping. TODO: keyboard layout selection.
    GreekKeyboardMapping = 53,

    /// DECVCCM (61) -- Vertical Cursor Coupling. TODO: couple the displayed page to vertical scrolling.
    VerticalCursorCoupling = 54,

    /// DECKBUM (68) -- Keyboard Usage Mode (typewriter vs. data processing keys). TODO: keyboard layer.
    KeyboardUsageMode = 55,

    /// DECXRLM (73) -- Transmit Rate Limiting. TODO: throttle host transmission (largely a no-op on a pty).
    TransmitRateLimiting = 56,

    /// DECKPM (81) -- Key Position Mode (report key position vs. character). TODO: key reporting layer.
    KeyPositionMode = 57,

    /// DECRLCM (96) -- Right-to-Left Copy. TODO: rides with RightToLeftMode's bidirectional support.
    RightToLeftCopyMode = 58,

    /// DECCRTSM (97) -- CRT Save Mode (screen blanking). TODO: display-power policy, a frontend concern.
    CRTSaveMode = 59,

    /// DECARSM (98) -- Auto Resize Mode. TODO: auto-resize the page on DECSLPP/DECSCPP.
    AutoResizeMode = 60,

    /// DECMCM (99) -- Modem Control Mode. TODO: modem signalling; largely inert on a pty.
    ModemControlMode = 61,

    /// DECAAM (100) -- Auto Answerback Mode. TODO: send the answerback message on connect.
    AutoAnswerbackMode = 62,

    /// DECCANSM (101) -- Conceal Answerback Message. TODO: hide the answerback from the display.
    ConcealAnswerbackMode = 63,

    /// DECNULM (102) -- Null Mode (how a received NUL is treated). TODO: discard vs. pass NUL.
    NullMode = 64,

    /// DECHDPXM (103) -- Half Duplex Mode. TODO: half- vs. full-duplex; inert on a pty.
    HalfDuplexMode = 65,

    /// DECESKM (104) -- Enable Secondary Keyboard Language. TODO: secondary keyboard layout.
    SecondaryKeyboardLanguageMode = 66,

    /// DECOSCNM (106) -- Overscan Mode (border colour region). TODO: overscan/border rendering.
    OverscanMode = 67,

    /// DECNCSM (95) -- No Clearing Screen on Column change. When set, DECCOLM (80<->132) does not
    /// erase page memory; reset (the default) clears the screen on a column-width change (VT100
    /// behaviour).
    NoClearScreenOnColumnChange = 68,
    // }}}

    /// Sentinel value for sizing the mode bitset. Must remain the last entry.
    DECModeCount = 71
};

/// The minimum ANSI conformance level (1..5, matching conformanceLevelOf(VTType)) at which a DEC
/// private mode is recognised. Below it, DECRQM answers "not recognised" and DECSET/DECRST ignore the
/// mode -- how a real VT gates level-specific features (DECNCSM is a VT500 / level-5 feature). Data
/// driven: a mode gains a floor by adding a case; everything else is available from VT100 (level 1).
[[nodiscard]] constexpr int minimumConformanceLevel(DECMode mode) noexcept
{
    switch (mode)
    {
        case DECMode::NoClearScreenOnColumnChange: return 5; // DECNCSM: VT510+
        case DECMode::LeftRightMargin: return 4;             // DECLRMM / DECSLRM: VT420+
        default: return 1;
    }
}

/// How one of the "alternate screen buffer" DEC private modes (47, 1047, 1049) behaves when the
/// application switches into and out of the alternate page.
///
/// xterm implements the three as points in this small space (charproc.c: ToAlternate / FromAlternate
/// and the srm_*ALTBUF* cases). Describing them as data keeps the switch itself a single code path,
/// rather than three near-duplicate handlers -- adding a fourth alternate-screen variant would be a
/// new row here, not new control flow.
struct AlternateScreenBehavior
{
    /// Whether the cursor is carried across the switch so that it appears not to move (modes 47 and
    /// 1047). xterm's cursor is a terminal-level entity -- SwitchBufs swaps only the line storage, never
    /// the cursor -- so it is continuous across the buffer swap. When false (mode 1049), each page keeps
    /// its own cursor, which serves as an implicit DECSC/DECRC: the main cursor waits untouched on the
    /// main page while the application works on the alternate one.
    bool carryCursor;

    /// Whether the alternate page is erased when entering it (mode 1049 clears; 47 and 1047 keep it).
    bool clearOnEnter;

    /// Whether the alternate page is erased when leaving it (mode 1047 clears; 47 and 1049 keep it --
    /// 1049 relies on its clear-on-enter instead).
    bool clearOnExit;
};

/// Maps an alternate-screen DEC private mode to its entry/exit behavior, or std::nullopt for any other
/// mode. @see AlternateScreenBehavior, Terminal::setAlternateScreen.
/// @param mode The DEC private mode to classify.
/// @return The behavior for modes 47/1047/1049, otherwise std::nullopt.
[[nodiscard]] constexpr std::optional<AlternateScreenBehavior> alternateScreenBehavior(DECMode mode) noexcept
{
    switch (mode)
    {
            // clang-format off
        case DECMode::UseAlternateScreen: return AlternateScreenBehavior { .carryCursor = true,  .clearOnEnter = false, .clearOnExit = false }; // 47
        case DECMode::OptionalAltScreen:  return AlternateScreenBehavior { .carryCursor = true,  .clearOnEnter = false, .clearOnExit = true  }; // 1047
        case DECMode::ExtendedAltScreen:  return AlternateScreenBehavior { .carryCursor = false, .clearOnEnter = true,  .clearOnExit = false }; // 1049
        // clang-format on
        default: return std::nullopt;
    }
}

/// The top-left corner of a window, in screen pixels.
///
/// Signed, because a window manager may place a window partly off the screen, and because a terminal
/// must report back whatever position it was actually given rather than a clamped fiction.
struct WindowPosition
{
    int x = 0;
    int y = 0;

    constexpr bool operator==(WindowPosition const&) const noexcept = default;
};

/// Where and how the terminal's window sits on the user's screen.
///
/// A terminal engine has no window, no screen and no window manager -- all of this is the frontend's to
/// know, and the frontend pushes it in as it changes. Nothing here is inferred: a frontend that has no
/// real window (a test harness, a headless session) states what it has, rather than having a fiction
/// invented on its behalf.
struct WindowState
{
    /// The window's top-left corner, in screen pixels. Reported by XTWINOPS `CSI 13 t`.
    WindowPosition position {};

    /// The size of the screen the window is on, in pixels. Reported by XTWINOPS `CSI 15 t`.
    ///
    /// An empty size means the frontend has no screen to speak of, in which case the window's own size
    /// stands in for it -- an honest answer for a headless terminal, which is exactly as large as the
    /// display it does not have. @see Terminal::screenPixelSize().
    ImageSize screenPixelSize {};

    /// Whether the window is iconified (minimized). Reported by XTWINOPS `CSI 11 t`.
    bool iconified = false;
};

/// What a maximize request asks of the window manager. @see XTWINOPS (`CSI 9 ; Ps t`).
enum class WindowMaximize : uint8_t
{
    Restore,      ///< `Ps = 0`: put the window back to the size it had before it was maximized.
    Both,         ///< `Ps = 1`: maximize along both axes.
    Vertically,   ///< `Ps = 2`
    Horizontally, ///< `Ps = 3`
};

/// @return What the selector @p ps of `CSI 9 ; Ps t` asks for, or std::nullopt if it asks for nothing.
constexpr std::optional<WindowMaximize> windowMaximizeOf(unsigned ps) noexcept
{
    switch (ps)
    {
        case 0: return WindowMaximize::Restore;
        case 1: return WindowMaximize::Both;
        case 2: return WindowMaximize::Vertically;
        case 3: return WindowMaximize::Horizontally;
        default: return std::nullopt;
    }
}

/// What a full-screen request asks of the window manager. @see XTWINOPS (`CSI 10 ; Ps t`).
enum class WindowFullScreen : uint8_t
{
    Exit,   ///< `Ps = 0`
    Enter,  ///< `Ps = 1`
    Toggle, ///< `Ps = 2`
};

/// @return What the selector @p ps of `CSI 10 ; Ps t` asks for, or std::nullopt if it asks for nothing.
constexpr std::optional<WindowFullScreen> windowFullScreenOf(unsigned ps) noexcept
{
    switch (ps)
    {
        case 0: return WindowFullScreen::Exit;
        case 1: return WindowFullScreen::Enter;
        case 2: return WindowFullScreen::Toggle;
        default: return std::nullopt;
    }
}

/// One of the two titles a terminal carries.
///
/// They are independent, and each has a save stack of its own: `OSC 1` sets the icon's title alone,
/// `CSI 22 ; 1 t` pushes the icon's title alone, and `CSI 23 ; 2 t` pops the window's alone. A single
/// shared title, or a single shared stack, cannot express that.
enum class TitleKind : uint8_t
{
    /// The icon (or tab) title. Set by `OSC 1`, reported by `CSI 20 t` as `OSC L <title> ST`.
    Icon = 1 << 0,

    /// The window title. Set by `OSC 2`, reported by `CSI 21 t` as `OSC l <title> ST`.
    Window = 1 << 1,
};

/// A set of titles a single XTPUSHTITLE, XTPOPTITLE or `OSC 0` acts on.
using TitleKinds = crispy::flags<TitleKind>;

/// One entry of the title stack: the titles a single XTPUSHTITLE saved.
///
/// Either may be absent, because `CSI 22 ; 1 t` saves the icon's title alone and `CSI 22 ; 2 t` the
/// window's alone. There is one stack, not one per title: a pop takes an entry off it whatever that
/// entry holds, so pushing both and popping only the icon leaves nothing behind for a later pop of the
/// window. A pop that needs a title the entry does not carry looks further *down* the stack for the
/// nearest entry that does, which is what makes "push the icon's, push the window's, pop both" restore
/// both. @see xterm's xtermPopTitle() and its TryHigher().
struct SavedTitles
{
    std::optional<std::string> icon;
    std::optional<std::string> window;
};

/// How deep the title stack goes, matching xterm's MAX_SAVED_TITLES.
///
/// Bounded on purpose: an unbounded stack is a memory-growth lever for any application that can write
/// to the terminal. A push onto a full stack discards the oldest entry.
constexpr auto MaxSavedTitles = size_t { 10 };

/// The titles the selector @p ps of XTPUSHTITLE / XTPOPTITLE (`CSI 22 / 23 ; Ps t`) names.
///
/// @param ps The selector: 0 both, 1 the icon's title, 2 the window's.
/// @return The titles named, or std::nullopt if @p ps names none.
constexpr std::optional<TitleKinds> titleKindsOf(unsigned ps) noexcept
{
    switch (ps)
    {
        case 0: return TitleKinds { TitleKind::Icon } | TitleKind::Window;
        case 1: return TitleKinds { TitleKind::Icon };
        case 2: return TitleKinds { TitleKind::Window };
        default: return std::nullopt;
    }
}

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

    /// Hint overlay mode (keyboard-driven link following).
    Hint,
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

/// One row of the @ref DECModeNumbers table: a @ref DECMode and the DEC private mode number it is
/// spelled as on the wire (the `Ps` in `CSI ? Ps h`).
struct DECModeNumbering
{
    DECMode mode;    ///< The internal mode enumerator.
    unsigned number; ///< Its DEC private mode number.
};

/// The bijection between a @ref DECMode and its DEC private mode number, shared by SM/RM,
/// DECSET/DECRST and DECRQM/DECRPM. Both @ref toDECModeNum and @ref fromDECModeNum read this single
/// table, so adding a mode is one new row and the two directions can never fall out of sync.
///
/// Unmapped numbers a real terminal recognises but Contour does not yet implement include 38 (enter
/// Tektronix mode, DECTEK) and 44 (turn on margin bell); they intentionally have no row.
constexpr inline auto DECModeNumbers = std::to_array<DECModeNumbering>({
    { DECMode::UseApplicationCursorKeys, 1 },
    { DECMode::DesignateCharsetUSASCII, 2 },
    { DECMode::Columns132, 3 },
    { DECMode::NoClearScreenOnColumnChange, 95 },
    { DECMode::SmoothScroll, 4 },
    { DECMode::ReverseVideo, 5 },
    { DECMode::Origin, 6 },
    { DECMode::AutoWrap, 7 },
    { DECMode::AutoRepeat, 8 },
    { DECMode::MouseProtocolX10, 9 },
    { DECMode::ShowToolbar, 10 },
    { DECMode::BlinkingCursor, 12 },
    { DECMode::PrinterExtend, 19 },
    { DECMode::VisibleCursor, 25 },
    { DECMode::ShowScrollbar, 30 },
    { DECMode::AllowColumns80to132, 40 },
    { DECMode::DebugLogging, 46 },
    { DECMode::UseAlternateScreen, 47 },
    { DECMode::OptionalAltScreen, 1047 },
    { DECMode::MoreFix, 41 },
    { DECMode::PageCursorCoupling, 64 },
    { DECMode::ApplicationKeypad, 66 },
    { DECMode::BackarrowKey, 67 },
    { DECMode::LeftRightMargin, 69 },
    { DECMode::MouseProtocolNormalTracking, 1000 },
    { DECMode::MouseProtocolHighlightTracking, 1001 },
    { DECMode::MouseProtocolButtonTracking, 1002 },
    { DECMode::MouseProtocolAnyEventTracking, 1003 },
    { DECMode::SaveCursor, 1048 },
    { DECMode::ExtendedAltScreen, 1049 },
    { DECMode::BracketedPaste, 2004 },
    { DECMode::FocusTracking, 1004 },
    { DECMode::NoSixelScrolling, 80 },
    { DECMode::UsePrivateColorRegisters, 1070 },
    { DECMode::MouseExtended, 1005 },
    { DECMode::MouseSGR, 1006 },
    { DECMode::MouseURXVT, 1015 },
    { DECMode::MouseSGRPixels, 1016 },
    { DECMode::MouseAlternateScroll, 1007 },
    { DECMode::MousePassiveTracking, 2029 },
    { DECMode::ReportGridCellSelection, 2030 },
    { DECMode::ReportColorPaletteUpdated, 2031 },
    { DECMode::SemanticBlockProtocol, 2034 },
    { DECMode::PrintFormFeed, 18 },
    { DECMode::HebrewKeyboardMapping, 35 },
    { DECMode::NationalReplacementCharacterSet, 42 },
    { DECMode::HorizontalCursorCoupling, 60 },
    { DECMode::RightToLeftMode, 34 },
    { DECMode::HebrewEncodingMode, 36 },
    { DECMode::GreekKeyboardMapping, 57 },
    { DECMode::VerticalCursorCoupling, 61 },
    { DECMode::KeyboardUsageMode, 68 },
    { DECMode::TransmitRateLimiting, 73 },
    { DECMode::KeyPositionMode, 81 },
    { DECMode::RightToLeftCopyMode, 96 },
    { DECMode::CRTSaveMode, 97 },
    { DECMode::AutoResizeMode, 98 },
    { DECMode::ModemControlMode, 99 },
    { DECMode::AutoAnswerbackMode, 100 },
    { DECMode::ConcealAnswerbackMode, 101 },
    { DECMode::NullMode, 102 },
    { DECMode::HalfDuplexMode, 103 },
    { DECMode::SecondaryKeyboardLanguageMode, 104 },
    { DECMode::OverscanMode, 106 },
    { DECMode::ReverseWraparound, 45 },
    { DECMode::ReverseWraparoundExtended, 1045 },
    { DECMode::BatchedRendering, 2026 },
    { DECMode::Unicode, 2027 },
    { DECMode::TextReflow, 2028 },
    { DECMode::SixelCursorNextToGraphic, 8452 },
    { DECMode::Win32InputMode, 9001 },
});

/// @return The DEC private mode number for @p m, or its raw ordinal when it has no assigned number.
constexpr unsigned toDECModeNum(DECMode m) noexcept
{
    // A range-based scan rather than std::ranges::find: std::array's iterator is a raw pointer on
    // libstdc++/libc++ but a wrapper class on MSVC, so binding the result to a typed local is not
    // portable across standard libraries.
    for (auto const& row: DECModeNumbers)
        if (row.mode == m)
            return row.number;
    return static_cast<unsigned>(m);
}

/// @return The @ref DECMode a DEC private mode number denotes, or std::nullopt when unrecognised.
constexpr std::optional<DECMode> fromDECModeNum(unsigned int modeNum) noexcept
{
    for (auto const& row: DECModeNumbers)
        if (row.number == modeNum)
            return row.mode;
    return std::nullopt;
}

constexpr bool isValidDECMode(unsigned int mode) noexcept
{
    return fromDECModeNum(mode).has_value();
}

/// Lowest OSC command addressing a dynamic color (xterm's OSC 10).
constexpr auto FirstDynamicColorCommand = unsigned { 10 };

/// The dynamic color each of OSC 10..19 addresses, indexed by `command - FirstDynamicColorCommand`.
///
/// The three slots Contour does not model -- xterm's Tektronix colors, OSC 15, 16 and 18 -- are
/// present but empty on purpose. One `OSC 10 ; spec ; spec ; ... ST` walks these slots one at a time,
/// so a slot omitted from the table would silently shift every later specification of such a sequence
/// onto the wrong color.
constexpr auto DynamicColorCommands = std::array<std::optional<DynamicColorName>, 10> {
    DynamicColorName::DefaultForegroundColor,   // OSC 10
    DynamicColorName::DefaultBackgroundColor,   // OSC 11
    DynamicColorName::TextCursorColor,          // OSC 12
    DynamicColorName::MouseForegroundColor,     // OSC 13
    DynamicColorName::MouseBackgroundColor,     // OSC 14
    std::nullopt,                               // OSC 15: Tektronix foreground
    std::nullopt,                               // OSC 16: Tektronix background
    DynamicColorName::HighlightBackgroundColor, // OSC 17
    std::nullopt,                               // OSC 18: Tektronix cursor
    DynamicColorName::HighlightForegroundColor, // OSC 19
};

/// Highest OSC command addressing a dynamic color (xterm's OSC 19).
constexpr auto LastDynamicColorCommand =
    FirstDynamicColorCommand + static_cast<unsigned>(DynamicColorCommands.size()) - 1;

/// @param command The OSC command number, e.g. 11 for the default background color.
/// @return The dynamic color @p command addresses, or std::nullopt if it addresses no color Contour
///         models -- including any command outside OSC 10..19.
constexpr std::optional<DynamicColorName> getChangeDynamicColorCommand(unsigned command) noexcept
{
    if (command < FirstDynamicColorCommand || command > LastDynamicColorCommand)
        return std::nullopt;

    return DynamicColorCommands[command - FirstDynamicColorCommand];
}

/// @param name The dynamic color.
/// @return The OSC command number addressing @p name, e.g. 11 for the default background color.
constexpr unsigned setDynamicColorCommand(DynamicColorName name) noexcept
{
    // The iterator is deliberately not bound to a named variable. libstdc++'s `std::array` iterator
    // *is* a raw pointer, so `auto const*` compiles there -- and clang-tidy's readability-qualified-auto
    // asks for exactly that -- while MSVC's is a class type and rejects it. Naming nothing satisfies
    // both.
    return FirstDynamicColorCommand
           + static_cast<unsigned>(
               std::ranges::distance(DynamicColorCommands.begin(),
                                     std::ranges::find(DynamicColorCommands, std::optional { name })));
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
struct std::formatter<vtbackend::BlinkStyle>: formatter<std::string_view>
{
    auto format(vtbackend::BlinkStyle value, auto& ctx) const
    {
        string_view name;
        switch (value)
        {
            case vtbackend::BlinkStyle::Classic: name = "classic"; break;
            case vtbackend::BlinkStyle::Smooth: name = "smooth"; break;
            case vtbackend::BlinkStyle::Linger: name = "linger"; break;
        }
        return formatter<string_view>::format(name, ctx);
    }
};

template <>
struct std::formatter<vtbackend::ScreenTransitionStyle>: formatter<std::string_view>
{
    auto format(vtbackend::ScreenTransitionStyle value, auto& ctx) const
    {
        string_view name;
        switch (value)
        {
            case vtbackend::ScreenTransitionStyle::Classic: name = "classic"; break;
            case vtbackend::ScreenTransitionStyle::Fade: name = "fade"; break;
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
            case ViMode::Hint: name = "Hint"; break;
        }
        return formatter<string_view>::format(name, ctx);
    }
};

// }}}
