/**
 * This file is part of the "libterminal" project
 *   Copyright (c) 2019 Christian Parpart <christian@parpart.family>
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

#include <terminal/Color.h>

#include <functional>
#include <string>
#include <string_view>
#include <variant>
#include <iostream>

namespace terminal {

using cursor_pos_t = unsigned int;

/// Screen coordinates between 1..n including.
struct Coordinate {
    cursor_pos_t row = 1;
    cursor_pos_t column = 1;
};

// Prints Coordinate as human readable text to given stream (used for debugging & unit testing).
inline std::ostream& operator<<(std::ostream& _os, Coordinate const& _coord)
{
    return _os << "{" << _coord.row << ", " << _coord.column << "}";
}

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
    AutomaticNewLine,
    // }}}

    // {{{ DEC modes
    UseApplicationCursorKeys,
    DesignateCharsetUSASCII,
    Columns132,
    SmoothScroll,
    ReverseVideo,
    /**
     * DECOM - Origin Mode.
     *
     * This control function sets the origin for the cursor.
     * DECOM determines if the cursor position is restricted to inside the page margins.
     * When you power up or reset the terminal, you reset origin mode.
     *
     * Default: Origin is at the upper-left of the screen, independent of margins.
     *
     * When DECOM is set, the home cursor position is at the upper-left corner of the screen, within the margins.
     * The starting point for line numbers depends on the current top margin setting.
     * The cursor cannot move outside of the margins.
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
    UseAlternateScreen,
    BracketedPaste,
    // }}}
};

constexpr bool isAnsiMode(Mode m) noexcept
{
    switch (m)
    {
        case Mode::KeyboardAction:
        case Mode::Insert:
        case Mode::SendReceive:
        case Mode::AutomaticNewLine:
            return true;
        case Mode::UseApplicationCursorKeys:
        case Mode::DesignateCharsetUSASCII:
        case Mode::Columns132:
        case Mode::SmoothScroll:
        case Mode::ReverseVideo:
        case Mode::Origin:
        case Mode::AutoWrap:
        case Mode::ShowToolbar:
        case Mode::BlinkingCursor:
        case Mode::PrinterExtend:
        case Mode::VisibleCursor:
        case Mode::ShowScrollbar:
        case Mode::UseAlternateScreen:
        case Mode::LeftRightMargin:
        case Mode::BracketedPaste:
            return false;
    }
    return false; // Should never be reached.
}

constexpr std::string_view to_code(Mode m)
{
    switch (m)
    {
        case Mode::KeyboardAction: return "2";
        case Mode::Insert: return "4";
        case Mode::SendReceive: return "12";
        case Mode::AutomaticNewLine: return "20";

        // DEC set-mode
        case Mode::UseApplicationCursorKeys: return "?1";
        case Mode::DesignateCharsetUSASCII: return "?2";
        case Mode::Columns132: return "?3";
        case Mode::SmoothScroll: return "?4";
        case Mode::ReverseVideo: return "?5";
        case Mode::Origin: return "?6";
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

/// RIS - Reset to Initial State
///
/// See: https://vt100.net/docs/vt510-rm/RIS.html
struct FullReset {};

/// DECSTR - Soft Terminal Reset
///
/// See: https://vt100.net/docs/vt510-rm/DECSTR.html
struct SoftTerminalReset {};

/// CNL - Cursor Next Line.
///
/// Move the cursor to the next line.
///
/// The active position is moved to the first character of the n-th following line.
struct CursorNextLine {
    /// This is the active position to the first character of the n-th following line.
    cursor_pos_t n;
};

/// CPL - Cursor Previous Line.
///
/// The active position is moved to the first character of the n-th preceding line.
///
/// NB: This is respecting margins (regardless of DECOM).
struct CursorPreviousLine {
    /// This is the number of active position moved to the first character of the n-th preceding line.
    cursor_pos_t n;
};

struct DeviceStatusReport {};

/// ECH—Erase Character
///
/// This control function erases one or more characters, from the cursor position to the right.
/// ECH clears character attributes from erased character positions.
/// ECH works inside or outside the scrolling margins.
struct EraseCharacters {
    /// This is the number of characters to erase. A Pn value of 0 or 1 erases one character.
    cursor_pos_t n;
};

/// CPR - Cursor Position Report.
///
/// The host asks the terminal for a cursor position report.
/// @see CursorPositionReport.
struct ReportCursorPosition {};

/// DECXCPR - Extended Cursor Position.
///
/// The host asks the terminal for the current cursor position, including the current page number.
struct ReportExtendedCursorPosition {};

struct SendDeviceAttributes {};
struct SendTerminalId {};

struct ClearToEndOfScreen {};
struct ClearToBeginOfScreen {};
struct ClearScreen {};

struct ClearScrollbackBuffer {};

/// SU - Pan Down.
///
/// This control function moves the user window down a specified number of lines in page memory.
struct ScrollUp {
    /// This is the number of lines to move the user window down in page memory.
    /// @p n new lines appear at the bottom of the display.
    /// @p n old lines disappear at the top of the display.
    /// You cannot pan past the bottom margin of the current page.
    cursor_pos_t n;
};

/// SD - Pan Up.
///
/// This control function moves the user window up a specified number of lines in page memory.
struct ScrollDown {
    /// This is the number of lines to move the user window up in page memory.
    /// @p n new lines appear at the top of the display.
    /// @p n old lines disappear at the bottom of the display.
    /// You cannot pan past the top margin of the current page.
    cursor_pos_t n;
};

/// EL - Erase in Line (from cursor position to the end).
///
/// This control function erases characters on the line that has the cursor.
/// EL clears all character attributes from erased character positions.
/// EL works inside or outside the scrolling margins.
struct ClearToEndOfLine {};

/// EL - Erase in Line (from cursor position to beginning).
///
/// This control function erases characters on the line that has the cursor.
/// EL clears all character attributes from erased character positions.
/// EL works inside or outside the scrolling margins.
struct ClearToBeginOfLine {};

/// EL - Erase in Line (full line).
///
/// This control function erases characters on the line that has the cursor.
/// EL clears all character attributes from erased character positions.
/// EL works inside or outside the scrolling margins.
struct ClearLine {};

/// ICH - Insert Character
///
/// This control function inserts one or more space (SP) characters starting at the cursor position.
///
/// The ICH sequence inserts Pn blank characters with the normal character attribute.
/// The cursor remains at the beginning of the blank characters.
/// Text between the cursor and right margin moves to the right.
/// Characters scrolled past the right margin are lost. ICH has no effect outside the scrolling margins.
struct InsertCharacters {
    /// This is is the number of characters to insert.
    cursor_pos_t n;
};

/// DECIC - Insert Column
///
/// This control function inserts one or more columns into the scrolling region,
/// starting with the column that has the cursor.
///
/// As columns are inserted, the columns between the cursor and the right margin move to the right.
/// DECIC inserts blank columns with no visual character attributes.
/// DECIC has no effect outside the scrolling margins.
struct InsertColumns {
    /// This is the number of columns to insert. Default: Pn = 1.
    cursor_pos_t n;
};

/// DECDC - Delete Column
///
/// This control function deletes one or more columns in the scrolling region,
/// starting with the column that has the cursor.
///
/// As columns are deleted, the remaining columns between the cursor and the right margin move to the left.
/// The terminal adds blank columns with no visual character attributes at the right margin.
/// DECDC has no effect outside the scrolling margins.
struct DeleteColumns {
    cursor_pos_t n;
};

/// IL - Insert Line
///
/// This control function inserts one or more blank lines, starting at the cursor.
///
/// As lines are inserted, lines below the cursor and in the scrolling region move down.
/// Lines scrolled off the page are lost. IL has no effect outside the page margins.
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
    /// This is the number of lines to delete.
    cursor_pos_t n;
};

/// DCH - Delete Character.
///
/// This control function deletes one or more characters from the cursor position to the right.
///
/// As characters are deleted, the remaining characters between the cursor and right margin move to the left.
/// Character attributes move with the characters.
/// The terminal adds blank spaces with no visual character attributes at the right margin.
/// DCH has no effect outside the scrolling margins.
struct DeleteCharacters {
    /// This is the number of characters to delete.
    ///
    /// If this value is greater than the number of characters between the cursor and the right margin,
    /// then DCH only deletes the remaining characters.
    cursor_pos_t n;
};

// HPA - Horizontal Position Absolute.
struct HorizontalPositionAbsolute {
    cursor_pos_t n;
};

// HPR - Horizontal Position Relative.
struct HorizontalPositionRelative {
    cursor_pos_t n;
};

/// CUU - Cursor Up.
/// Moves the cursor up a specified number of lines in the same column.
/// The cursor stops at the top margin.
/// If the cursor is already above the top margin, then the cursor stops at the top line.
struct MoveCursorUp {
    /// This is the number of lines to move the cursor up.
    cursor_pos_t n;
};

/// CUD - Cursor Down.
///
/// This control function moves the cursor down a specified number of lines in the same column.
/// The cursor stops at the bottom margin.
/// If the cursor is already below the bottom margin, then the cursor stops at the bottom line.
struct MoveCursorDown {
    /// This is the number of lines to move the cursor down.
    cursor_pos_t n;
};

/// CUF - Cursor Forward.
///
/// This control function moves the cursor to the right by a specified number of columns.
/// The cursor stops at the right border of the page.
struct MoveCursorForward {
    /// This is the number of columns to move the cursor to the right.
    cursor_pos_t n;
};

/// CUB - Cursor Backward.
///
/// This control function moves the cursor to the left by a specified number of columns.
/// The cursor stops at the left border of the page.
struct MoveCursorBackward {
    /// This is the number of columns to move the cursor to the left.
    cursor_pos_t n;
};

/// CHA - Cursor Horizontal Absolute.
///
/// Move the active position to the n-th character of the active line.
///
/// The active position is moved to the n-th character position of the active line.
struct MoveCursorToColumn {
    /// This is the number of active positions to the n-th character of the active line.
    cursor_pos_t column;
};

/// Moves the cursor to the left margin on the current line.
struct MoveCursorToBeginOfLine {};

/// CUP - Cursor Position.
///
/// This control function moves the cursor to the specified line and column.
/// The starting point for lines and columns depends on the setting of origin mode (DECOM).
/// CUP applies only to the current page.
struct MoveCursorTo {
    /// This is the number of the line to move to. If the value is 0 or 1, then the cursor moves to line 1.
    cursor_pos_t row;

    /// This is the number of the column to move to. If the value is 0 or 1, then the cursor moves to column 1.
    cursor_pos_t column;
};

struct MoveCursorToNextTab {};

/// VPA - Vertical Line Position Absolute
///
/// VPA causes the active position to be moved to the corresponding horizontal position.
///
/// The default value is 1.
///
/// Move cursor to line Pn. VPA causes the active position to be moved to the corresponding horizontal
/// position at vertical position Pn.
/// If an attempt is made to move the active position below the last
/// line, then the active position stops on the last line.
struct MoveCursorToLine {
    cursor_pos_t row;
};

/// DECSC - Save Cursor
struct SaveCursor {};

/// DECRS - Restore Cursor
struct RestoreCursor {};

struct SetForegroundColor { Color color; };
struct SetBackgroundColor { Color color; };
struct SetGraphicsRendition { GraphicsRendition rendition; };

struct AppendChar { char32_t ch; };

struct SetMode { Mode mode; bool enable; };

/// DECRQM - Request Mode
///
/// The host sends this control function to find out if a particular mode is set or reset. The terminal responds with a report mode function (DECRPM—Report Mode - Terminal To Host).
struct RequestMode {
    Mode mode;
};

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
    cursor_pos_t top;

    /// The line number for the bottom margin.
    /// Default: current number of lines per screen
    cursor_pos_t bottom;
};

/// DECSLRM - Set Left and Right Margins.
///
/// This control function sets the left and right margins to define the scrolling region.
/// DECSLRM only works when vertical split screen mode (DECLRMM) is set.
///
/// The value of the left margin (Pl) must be less than the right margin (Pr).
///
/// Notes:
/// * The maximum size of the scrolling region is the page size, based on the setting of set columns per page (DECSCPP).
/// * The minimum size of the scrolling region is two columns.
/// * The terminal only recognizes this control function if vertical split screen mode (DECLRMM) is set.
/// * DECSLRM moves the cursor to column 1, line 1 of the page.
/// * If the left and right margins are set to columns other than 1 and 80 (or 132), then the terminal cannot scroll smoothly.
/// * Available in: VT Level 4 mode only
/// * Default: Margins are at the left and right page borders.
struct SetLeftRightMargin {
    cursor_pos_t left;
    cursor_pos_t right;
};

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

/// DECBI - Back Index.
///
/// This control function moves the cursor backward one column.
/// If the cursor is at the left margin, then all screen data within the margin moves one column to the right.
/// The column that shifted past the right margin is lost.
///
/// DECBI adds a new column at the left margin with no visual attributes.
/// DECBI is not affected by the margins.
/// If the cursor is at the left border of the page when the terminal receives DECBI, then the terminal ignores DECBI.
struct BackIndex {};

/// DECFI - Forward Index
///
/// This control function moves the cursor forward one column.
/// If the cursor is at the right margin, then all screen data within the margins moves one column to the left.
/// The column shifted past the left margin is lost.
///
/// DECFI adds a new column at the right margin, with no visual attributes.
/// DECFI is not affected by the margins.
/// If the cursor is at the right border of the page when the terminal receives DECFI,
/// then the terminal ignores DECFI.
struct ForwardIndex {};

/// DECALN - Screen Alignment Pattern.
///
/// This control function fills the complete screen area with a test pattern used for adjusting screen alignment.
/// Normally, only manufacturing and service personnel would use DECALN.
///
/// DECALN sets the margins to the extremes of the page, and moves the cursor to the home position.
struct ScreenAlignmentPattern {};

/// Changes the Window's title to given title.
struct ChangeWindowTitle { std::string title; };

/// Resizes window to given dimension.
///
/// `CSI 4 ; height ; width t` and `CSI 8 ; height ; width t`
///
/// A height/width value of 0 means "current value" unless both are 0,
/// that means full screen dimensions are to be used.
struct ResizeWindow {
    enum class Unit { Characters, Pixels };

    unsigned int width;
    unsigned int height;
    Unit unit;
};

struct SaveWindowTitle {};
struct RestoreWindowTitle {};

using Command = std::variant<
    AppendChar,

    AlternateKeypadMode,
    BackIndex,
    Backspace,
    Bell,
    ChangeWindowTitle,
    ClearLine,
    ClearScreen,
    ClearScrollbackBuffer,
    ClearToBeginOfLine,
    ClearToBeginOfScreen,
    ClearToEndOfLine,
    ClearToEndOfScreen,
    CursorNextLine,
    CursorPreviousLine,
    DeleteCharacters,
    DeleteColumns,
    DeleteLines,
    DesignateCharset,
    DeviceStatusReport,
    EraseCharacters,
    ForwardIndex,
    FullReset,
    HorizontalPositionAbsolute,
    HorizontalPositionRelative,
    Index,
    InsertCharacters,
    InsertColumns,
    InsertLines,
    Linefeed,
    MoveCursorBackward,
    MoveCursorDown,
    MoveCursorForward,
    MoveCursorTo,
    MoveCursorToBeginOfLine,
    MoveCursorToColumn,
    MoveCursorToLine,
    MoveCursorToNextTab,
    MoveCursorUp,
    ReportCursorPosition,
    ReportExtendedCursorPosition,
    RequestMode,
    ResizeWindow,
    RestoreCursor,
    RestoreWindowTitle,
    ReverseIndex,
    SaveCursor,
    SaveWindowTitle,
    ScreenAlignmentPattern,
    ScrollDown,
    ScrollUp,
    SendDeviceAttributes,
    SendMouseEvents,
    SendTerminalId,
    SetBackgroundColor,
    SetForegroundColor,
    SetGraphicsRendition,
    SetLeftRightMargin,
    SetMode,
    SetTopBottomMargin,
    SingleShiftSelect,
    SoftTerminalReset
>;

std::string to_string(Command const& cmd);

std::vector<std::string> to_mnemonic(std::vector<Command> const& _commands, bool _withParameters, bool _withComment);

}  // namespace terminal
