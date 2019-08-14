#pragma once

#include <terminal/Color.h>

#include <functional>
#include <string>
#include <string_view>
#include <variant>

namespace terminal {

using cursor_pos_t = size_t;

/// Screen coordinates between 1..n including.
struct Coordinate {
    size_t row;
    size_t column;
};

constexpr inline bool operator==(Coordinate const& a, Coordinate const& b) noexcept
{
    return a.row == b.row && a.column == b.column;
}

constexpr inline bool operator!=(Coordinate const& a, Coordinate const& b) noexcept
{
    return !(a == b);
}

enum class GraphicsRendition {
    Reset = 0,              //!< Reset any rendition (style as well as foreground / background coloring).

    Bold = 1,               //!< Bold glyph width
    Faint = 2,              //!< Decreased intensity
    Italic = 3,             //!< Italic glyph
    Underline = 4,          //!< Underlined glyph
    Blinking = 5,           //!< Blinking glyph
    Inverse = 7,            //!< Swaps foreground with background color.
    Hidden = 8,             //!< Glyph hidden (somewhat like space character).
    CrossedOut = 9,         //!< Crossed out glyph space.
    DoublyUnderlined = 21,  //!< Underlined with two lines.

    Normal = 22,            //!< Neither Bold nor Faint.
    NoItalic = 23,          //!< Reverses Italic.
    NoUnderline = 24,       //!< Reverses Underline.
    NoBlinking = 25,        //!< Reverses Blinking.
    NoInverse = 27,         //!< Reverses Inverse.
    NoHidden = 28,          //!< Reverses Hidden (Visible).
    NoCrossedOut = 29,      //!< Reverses CrossedOut.
};

std::string to_string(GraphicsRendition s);

enum class Mode {
    // {{{ normal modes
    KeyboardAction,
    Insert,
    SendReceive,
    AutomaticLinefeed,
    // }}}

    // {{{ DEC modes
    UseApplicationCursorKeys,
    DesignateCharsetUSASCII,
    Columns132,
    SmoothScroll,
    ReverseVideo,
    CursorRestrictedToMargin, 

    /**
     * If enabled, when the cursor is at the right margin and a character is send, it will be 
     * wrapped onto the next line, poentially scrolling the screen.
     */
    AutoWrap,

    PrinterExtend,
    LeftRightMargin,

    ShowToolbar,
    BlinkingCursor,
    VisibleCursor,
    ShowScrollbar,
    UseAlternateScreen,
    BracketedPaste,
    // }}}
};

constexpr std::string_view to_code(Mode m)
{
    switch (m)
    {
        case Mode::KeyboardAction: return "2";
        case Mode::Insert: return "4";
        case Mode::SendReceive: return "12";
        case Mode::AutomaticLinefeed: return "20";

        // DEC set-mode
        case Mode::UseApplicationCursorKeys: return "?1";
        case Mode::DesignateCharsetUSASCII: return "?2";
        case Mode::Columns132: return "?3";
        case Mode::SmoothScroll: return "?4";
        case Mode::ReverseVideo: return "?5";
        case Mode::CursorRestrictedToMargin: return "?6"; 
        case Mode::AutoWrap: return "?7";
        case Mode::ShowToolbar: return "?10";
        case Mode::BlinkingCursor: return "?12";
        case Mode::PrinterExtend: return "?19";
        case Mode::VisibleCursor: return "?25";
        case Mode::ShowScrollbar: return "?30";
        case Mode::UseAlternateScreen: return "?47";
        case Mode::LeftRightMargin: return "?69";
        case Mode::BracketedPaste: return "?2004";
    }
    return "0";
}

std::string to_string(Mode m);

enum class CharsetTable {
    G0 = 0,
    G1 = 1,
    G2 = 2,
    G3 = 3
};

std::string to_string(CharsetTable i);

enum class Charset {
    Special, // Special Character and Line Drawing Set

    UK,
    USASCII,
    German,

    // ... TODO
};

std::string to_string(Charset charset);

struct Bell {};

/// LF - Causes a line feed or a new line operation, depending on the setting of line feed/new line mode.
struct Linefeed {};

struct Backspace {};
struct FullReset {};

struct DeviceStatusReport {};
struct ReportCursorPosition {};
struct SendDeviceAttributes {};
struct SendTerminalId {};

struct ClearToEndOfScreen {};
struct ClearToBeginOfScreen {};
struct ClearScreen {};

struct ClearScrollbackBuffer {};
struct ScrollUp { cursor_pos_t n; };
struct ScrollDown { cursor_pos_t n; };

struct ClearToEndOfLine {};
struct ClearToBeginOfLine {};
struct ClearLine {};

struct InsertLines { cursor_pos_t n; };

/// DL - Delete Line
///
/// This control function deletes one or more lines in the scrolling region,
/// starting with the line that has the cursor.
///
/// As lines are deleted, lines below the cursor and in the scrolling region move up.
/// The terminal adds blank lines with no visual character attributes at the bottom of the scrolling region.
/// If Pn is greater than the number of lines remaining on the page, DL deletes only the remaining lines.
///
/// DL has no effect outside the scrolling margins.
struct DeleteLines {
    cursor_pos_t n;
};

struct DeleteCharacters { cursor_pos_t n; };

struct MoveCursorUp { cursor_pos_t n; };
struct MoveCursorDown { cursor_pos_t n; };
struct MoveCursorForward { cursor_pos_t n; };
struct MoveCursorBackward { cursor_pos_t n; };
struct MoveCursorToColumn { cursor_pos_t column; };

/// Moves the cursor to the left margin on the current line.
struct MoveCursorToBeginOfLine {};

struct MoveCursorTo { cursor_pos_t row; cursor_pos_t column; };
struct MoveCursorToNextTab {};

struct HideCursor {};
struct ShowCursor {};
struct SaveCursor {};
struct RestoreCursor {};

struct SetForegroundColor { Color color; };
struct SetBackgroundColor { Color color; };
struct SetGraphicsRendition { GraphicsRendition rendition; };

struct AppendChar { wchar_t ch; };

struct SetMode { Mode mode; bool enable; };

/// DECSTBM - Set Top and Bottom Margins
///
/// This control function sets the top and bottom margins for the current page.
/// You cannot perform scrolling outside the margins.
///
/// Default: Margins are at the page limits.
///
/// The value of the top margin (Pt) must be less than the bottom margin (Pb).
/// The maximum size of the scrolling region is the page size.
///
/// DECSTBM moves the cursor to column 1, line 1 of the page.
struct SetTopBottomMargin {
    /// The line number for the top margin.
    /// Default: 1
    size_t top;

    /// The line number for the bottom margin.
    /// Default: current number of lines per screen
    size_t bottom;
};

struct SetLeftRightMargin { size_t left; size_t right; };

enum class MouseProtocol {
    X10 = 9,
    VT200 = 1000,
    VT200_Highlight = 1001,
    ButtonEvent = 1002,
    AnyEvent = 1003,
    FocusEvent = 1004,
    Extended = 1005,
    SGR = 1006,
    URXVT = 1015,

    AlternateScroll = 1007,
};

std::string to_string(MouseProtocol protocol);
unsigned to_code(MouseProtocol protocol);

struct SendMouseEvents { MouseProtocol protocol; bool enable; };

struct AlternateKeypadMode { bool enable; };

struct DesignateCharset { CharsetTable table; Charset charset; };

//! Selects given CharsetTable for the very next character only.
struct SingleShiftSelect { CharsetTable table; };

/// IND - Index
///
/// Moves the cursor down one line in the same column.
/// If the cursor is at the bottom margin, then the screen performs a scroll-up.
struct Index {};

/// RI - Reverse Index
///
/// Moves the cursor up, but also scrolling the screen if already at top
struct ReverseIndex {};

// DECBI - Back Index
struct BackIndex {};

// DECFI - Forward Index
struct ForwardIndex {};

// OSC commands:
struct ChangeWindowTitle { std::string title; };
struct ChangeIconName { std::string name; };

using Command = std::variant<
    Bell,
    Linefeed,
    Backspace,
    FullReset,

    DeviceStatusReport,
    ReportCursorPosition,
    SendDeviceAttributes,
    SendTerminalId,

    ClearToEndOfScreen,
    ClearToBeginOfScreen,
    ClearScreen,

    ClearScrollbackBuffer,
    ScrollUp,
    ScrollDown,

    ClearToEndOfLine,
    ClearToBeginOfLine,
    ClearLine,

    InsertLines,
    DeleteLines,
    DeleteCharacters,

    MoveCursorUp,
    MoveCursorDown,
    MoveCursorForward,
    MoveCursorBackward,
    MoveCursorToColumn,
    MoveCursorToBeginOfLine,
    MoveCursorTo,
    MoveCursorToNextTab,
    HideCursor,
    ShowCursor,
    SaveCursor,
    RestoreCursor,

    Index,
    ReverseIndex,
    BackIndex,
    ForwardIndex,

    SetForegroundColor,
    SetBackgroundColor,
    SetGraphicsRendition,

    SetMode,

    SendMouseEvents,

    AlternateKeypadMode,
    DesignateCharset,
    SingleShiftSelect,
    SetTopBottomMargin,
    SetLeftRightMargin,

    // OSC
    ChangeWindowTitle,
    ChangeIconName,

    // Ground
    AppendChar
>;

std::string to_string(Command const& cmd);

}  // namespace terminal
