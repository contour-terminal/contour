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

#include <terminal/Color.h>
#include <terminal/Size.h>
#include <terminal/VTType.h>
#include <terminal/Functions.h>

#include <functional>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace terminal {

using cursor_pos_t = int;

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

    CurlyUnderlined = 30,   //!< Curly line below the baseline.
    DottedUnderline = 31,   //!< Dotted line below the baseline.
    DashedUnderline = 32,   //!< Dashed line below the baseline.
    Framed = 51,            //!< Frames the glyph with lines on all sides
    Overline = 53,          //!< Overlined glyph
    NoFramed = 54,          //!< Reverses Framed
    NoOverline = 55,        //!< Reverses Overline.
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
    FocusTracking, // 1004
    // }}}
    // {{{ Mouse related flags
    /// extend mouse protocl encoding
    MouseExtended = 1005,

    /// Uses a (SGR-style?) different encoding.
    MouseSGR = 1006,

    // URXVT invented extend mouse protocol
    MouseURXVT = 1015,

    /// Toggles scrolling in alternate screen buffer, encodes CUP/CUD instead of mouse wheel events.
    MouseAlternateScroll = 1007,
    // }}}
    // {{{ Extensions
    // This merely resembles the "Synchronized Output" feature from iTerm2, except that it is using
    // a different VT sequence to be enabled. Instead of a DCS,
    // this feature is using CSI ? 2026 h (DECSM and DECRM).
    BatchedRendering = 2026,
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
        case Mode::FocusTracking:
        case Mode::MouseExtended:
        case Mode::MouseSGR:
        case Mode::MouseURXVT:
        case Mode::MouseAlternateScroll:
        case Mode::BatchedRendering:
            return false;
        case Mode::SaveCursor:
        case Mode::ExtendedAltScreen:
        case Mode::MouseProtocolAnyEventTracking:
        case Mode::MouseProtocolButtonTracking:
        case Mode::MouseProtocolHighlightTracking:
        case Mode::MouseProtocolNormalTracking:
        case Mode::MouseProtocolX10:
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
        case Mode::MouseProtocolX10: return "?9";
        case Mode::ShowToolbar: return "?10";
        case Mode::BlinkingCursor: return "?12";
        case Mode::PrinterExtend: return "?19";
        case Mode::VisibleCursor: return "?25";
        case Mode::ShowScrollbar: return "?30";
        case Mode::UseAlternateScreen: return "?47";
        case Mode::LeftRightMargin: return "?69";
        case Mode::MouseProtocolNormalTracking: return "?1000";
        case Mode::MouseProtocolHighlightTracking: return "?1001";
        case Mode::MouseProtocolButtonTracking: return "?1002";
        case Mode::MouseProtocolAnyEventTracking: return "?1003";
        case Mode::SaveCursor: return "?1048";
        case Mode::ExtendedAltScreen: return "?1049";
        case Mode::BracketedPaste: return "?2004";
        case Mode::FocusTracking: return "?1004";
        case Mode::MouseExtended: return "?1005";
        case Mode::MouseSGR: return "?1006";
        case Mode::MouseURXVT: return "?1015";
        case Mode::MouseAlternateScroll: return "?1007";
        case Mode::BatchedRendering: return "2026";
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

enum class CharsetId {
    Special, // Special Character and Line Drawing Set

    British,
    Dutch,
    Finnish,
    French,
    FrenchCanadian,
    German,
    NorwegianDanish,
    Spanish,
    Swedish,
    Swiss,
    USASCII
};

std::string to_string(CharsetId charset);

struct InvalidCommand {
    enum class Reason {
        Unknown,
        Unsupported,
        Invalid
    };
    Sequence sequence;
    Reason reason;
};

struct UnsupportedCommand {
    Sequence sequence;
};

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

/// DA1 - Primary Device Attributes
///
/// In this DA exchange, the host asks for the terminal's architectural class and basic attributes.
struct SendDeviceAttributes {};

/// DA2 - Secondary Device Attributes.
///
/// In this DA exchange, the host requests the terminal's identification code, firmware version level,
/// and hardware options.
struct SendTerminalId {};

struct ClearToEndOfScreen {};
struct ClearToBeginOfScreen {};
struct ClearScreen {};

struct ClearScrollbackBuffer {};

enum class ControlTransmissionMode {
    S7C1T, // 7-bit controls
    S8C1T, // 8-bit controls
};

/// DECSCL - Select Conformance Level
struct SelectConformanceLevel {
    VTType level;
    ControlTransmissionMode c1t;
};

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

/// HT - Horizontal Tab
///
/// Moves the cursor to the next tab stop. If there are no more tab stops, the cursor moves to the
/// right margin. HT does not cause text to auto wrap.
struct MoveCursorToNextTab {};

/// CBT - Cursor Backward Tabulation
///
/// Move the active position n tabs backward. (default: 1)
struct CursorBackwardTab {
    int count = 1;
};

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
struct SetUnderlineColor { Color color; };
struct SetGraphicsRendition { GraphicsRendition rendition; };

struct AppendChar { char32_t ch; };

struct SetMode { Mode mode; bool enable; };

struct SaveMode { std::vector<Mode> modes; };
struct RestoreMode { std::vector<Mode> modes; };

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
    std::optional<cursor_pos_t> top;

    /// The line number for the bottom margin.
    /// Default: current number of lines per screen
    std::optional<cursor_pos_t> bottom;
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
    std::optional<cursor_pos_t> left;
    std::optional<cursor_pos_t> right;
};

/// Mutualy exclusive mouse protocls.
enum class MouseProtocol {
    /// Old X10 mouse protocol
    X10 = 9,
    /// Normal tracking mode, that's X10 with mouse release events and modifiers
    NormalTracking = 1000,
    /// Highlight mouse tracking
    HighlightTracking = 1001,
    /// Button-event tracking protocol.
    ButtonTracking = 1002,
    /// Like ButtonTracking plus motion events.
    AnyEventTracking = 1003,
};

std::string to_string(MouseProtocol protocol);
unsigned to_code(MouseProtocol protocol) noexcept;

// TODO: Consider removing thix and extend SetMode instead.
struct SendMouseEvents { MouseProtocol protocol; bool enable; };

/// DECKPAM - Keypad Application Mode: ESC =
/// DECKPNM - Keypad Numeric Mode: ESC >
///
/// Enables (DECKPAM) or disables (DECKPNM) sending application keys when pressing keypad keys.
///
/// See:
/// - https://vt100.net/docs/vt510-rm/DECKPAM.html
/// - https://vt100.net/docs/vt510-rm/DECKPNM.html
struct ApplicationKeypadMode { bool enable; };

struct DesignateCharset { CharsetTable table; CharsetId charset; };

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

/// TBC - Tab Clear
///
/// This control function clears tab stops.
struct HorizontalTabClear {
    enum Which {
        /// Ps = 0 (default)
        AllTabs,

        /// Ps = 3
        UnderCursor,
    };
    Which which = AllTabs;
};

/// HTS - Horizontal Tab Set
///
/// HTS sets a horizontal tab stop at the column position indicated by the value of the active
/// column when the terminal receives an HTS.
struct HorizontalTabSet {};

/// DECALN - Screen Alignment Pattern.
///
/// This control function fills the complete screen area with a test pattern used for adjusting screen alignment.
/// Normally, only manufacturing and service personnel would use DECALN.
///
/// DECALN sets the margins to the extremes of the page, and moves the cursor to the home position.
struct ScreenAlignmentPattern {};

/// Changes the Window's icon title, that is, when the window is iconized, that given text is being
/// displayed underneath the icon.
///
/// To be fair, nobody uses twm or fvwm anymore, or do you?
struct ChangeIconTitle { std::string title; };

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

    int width;
    int height;
    Unit unit;
};

enum class CursorDisplay { Steady, Blink };

inline std::string to_string(CursorDisplay _v)
{
	switch (_v)
	{
		case CursorDisplay::Steady:
			return "Steady";
		case CursorDisplay::Blink:
			return "Blink";
	}
	return "";
}

enum class CursorShape {
    Block,
	Rectangle,
    Underscore,
    Bar,
};

CursorShape makeCursorShape(std::string const& _name);
inline std::string to_string(CursorShape _value)
{
    switch (_value)
    {
        case CursorShape::Block: return "Block";
        case CursorShape::Rectangle: return "Rectangle";
        case CursorShape::Underscore: return "Underscore";
        case CursorShape::Bar: return "Bar";
    }
    return "";
}

/// DECSCUSR - Set Cursor Style
///
/// Select the style of the cursor on the screen.
struct SetCursorStyle {
	CursorDisplay display;
    CursorShape shape;
};

struct SaveWindowTitle {};
struct RestoreWindowTitle {};

/// SETMARK - Sets a marker at the current cursor line position that can be jumped to later.
struct SetMark {};

/// OSC 8 - Sets or resets the hyperlink for text this OSC.
struct Hyperlink {
    std::string id;
    std::string uri;
};

/// OSC 777 - notify
struct Notify {
    std::string title;
    std::string content;
};

/// OSC 888
struct DumpState {};

// OSC 52 ; c ; Base64EncodedData ST
struct CopyToClipboard { std::string data; };

// {{{ config commands
/// OSC color-setting related commands that can be grouped into one
enum class DynamicColorName {
    DefaultForegroundColor,
    DefaultBackgroundColor,
    TextCursorColor,
    MouseForegroundColor,
    MouseBackgroundColor,
    HighlightForegroundColor,
    HighlightBackgroundColor,
};

constexpr DynamicColorName getChangeDynamicColorCommand(int value)
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
        default:
            return DynamicColorName::DefaultForegroundColor;
    }
}

constexpr DynamicColorName getResetDynamicColorCommand(int value)
{
    switch (value)
    {
        case 110: return DynamicColorName::DefaultForegroundColor;
        case 111: return DynamicColorName::DefaultBackgroundColor;
        case 112: return DynamicColorName::TextCursorColor;
        case 113: return DynamicColorName::MouseForegroundColor;
        case 114: return DynamicColorName::MouseBackgroundColor;
        case 119: return DynamicColorName::HighlightForegroundColor;
        case 117: return DynamicColorName::HighlightBackgroundColor;
        default:
            return DynamicColorName::DefaultForegroundColor;
    }
}

///  Input: CSI 14 t (for text area size)
///  Input: CSI 14; 2 t (for full window size)
/// Output: CSI 14 ; width ; height ; t
struct RequestPixelSize {
    enum class Area {
        TextArea,
        WindowArea, // or: View
    };
    Area area;
};

/// Requests the current color value of a DynamicColorName.
struct RequestDynamicColor {
    DynamicColorName name;
};

/// DECRQSS - Request Status String
struct RequestStatusString {
    enum class Value {
        SGR,
        DECSCL,
        DECSCUSR,
        DECSCA,
        DECSTBM,
        DECSLRM,
        DECSLPP,
        DECSCPP,
        DECSNLS
    };

    Value value;
};

/// DECTABSR - Tab Stop Report
///
/// Requests currently configured tab stops.
struct RequestTabStops {};

/// Sets the DynamicColorName to given color value.
struct SetDynamicColor {
    DynamicColorName name;
    RGBColor color;
};

/// Resets the DynamicColorName to its configuration default.
struct ResetDynamicColor {
    DynamicColorName name;
};

constexpr int setDynamicColorCommand(DynamicColorName name)
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
        default:
            return 0;
    }
}

constexpr int resetDynamicColorCommand(DynamicColorName name)
{
    switch (name)
    {
        case DynamicColorName::DefaultForegroundColor: return 110;
        case DynamicColorName::DefaultBackgroundColor: return 111;
        case DynamicColorName::TextCursorColor: return 112;
        case DynamicColorName::MouseForegroundColor: return 113;
        case DynamicColorName::MouseBackgroundColor: return 114;
        case DynamicColorName::HighlightForegroundColor: return 119;
        case DynamicColorName::HighlightBackgroundColor: return 117;
        default:
            return 0;
    }
}

std::string setDynamicColorValue(RGBColor const& color);

/// OSC 46
///
/// Change Log File to Pt.  This is normally dis-
/// abled by a compile-time option.
struct SetLogFilePathConfig {
    std::string path;
};

// }}}

using Command = std::variant<
    AppendChar,
    ApplicationKeypadMode,
    BackIndex,
    Backspace,
    Bell,
    ChangeIconTitle,
    ChangeWindowTitle,
    ClearLine,
    ClearScreen,
    ClearScrollbackBuffer,
    ClearToBeginOfLine,
    ClearToBeginOfScreen,
    ClearToEndOfLine,
    ClearToEndOfScreen,
    CopyToClipboard,
    CursorBackwardTab,
    CursorNextLine,
    CursorPreviousLine,
    DeleteCharacters,
    DeleteColumns,
    DeleteLines,
    DesignateCharset,
    DeviceStatusReport,
    DumpState,
    EraseCharacters,
    ForwardIndex,
    FullReset,
    HorizontalPositionAbsolute,
    HorizontalPositionRelative,
    HorizontalTabClear,
    HorizontalTabSet,
    Hyperlink,
    Index,
    InsertCharacters,
    InsertColumns,
    InsertLines,
    InvalidCommand,
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
    Notify,
    ReportCursorPosition,
    ReportExtendedCursorPosition,
    RequestDynamicColor,
    RequestMode,
    RequestPixelSize,
    RequestStatusString,
    RequestTabStops,
    ResetDynamicColor,
    ResizeWindow,
    RestoreCursor,
    RestoreMode,
    RestoreWindowTitle,
    ReverseIndex,
    SaveCursor,
    SaveMode,
    SaveWindowTitle,
    ScreenAlignmentPattern,
    ScrollDown,
    ScrollUp,
    SelectConformanceLevel,
    SendDeviceAttributes,
    SendMouseEvents,
    SendTerminalId,
    SetBackgroundColor,
    SetCursorStyle,
    SetDynamicColor,
    SetForegroundColor,
    SetGraphicsRendition,
    SetLeftRightMargin,
    SetMark,
    SetMode,
    SetTopBottomMargin,
    SetUnderlineColor,
    SingleShiftSelect,
    SoftTerminalReset
>;

using CommandList = std::vector<Command>;

std::string to_string(Command const& cmd);
std::string to_mnemonic(Command const& _command, bool _withParameters, bool _withComment);
std::vector<std::string> to_mnemonic(std::vector<Command> const& _commands, bool _withParameters, bool _withComment);

/// Screen Command Execution API.
class CommandVisitor {
  public:
    virtual ~CommandVisitor() = default;

    virtual void visit(AppendChar const& v) = 0;
    virtual void visit(ApplicationKeypadMode const& v) = 0;
    virtual void visit(BackIndex const& v) = 0;
    virtual void visit(Backspace const& v) = 0;
    virtual void visit(Bell const& v) = 0;
    virtual void visit(ChangeIconTitle const& v) = 0;
    virtual void visit(ChangeWindowTitle const& v) = 0;
    virtual void visit(ClearLine const& v) = 0;
    virtual void visit(ClearScreen const& v) = 0;
    virtual void visit(ClearScrollbackBuffer const& v) = 0;
    virtual void visit(ClearToBeginOfLine const& v) = 0;
    virtual void visit(ClearToBeginOfScreen const& v) = 0;
    virtual void visit(ClearToEndOfLine const& v) = 0;
    virtual void visit(ClearToEndOfScreen const& v) = 0;
    virtual void visit(CopyToClipboard const& v) = 0;
    virtual void visit(CursorBackwardTab const& v) = 0;
    virtual void visit(CursorNextLine const& v) = 0;
    virtual void visit(CursorPreviousLine const& v) = 0;
    virtual void visit(DeleteCharacters const& v) = 0;
    virtual void visit(DeleteColumns const& v) = 0;
    virtual void visit(DeleteLines const& v) = 0;
    virtual void visit(DesignateCharset const& v) = 0;
    virtual void visit(DeviceStatusReport const& v) = 0;
    virtual void visit(DumpState const& v) = 0;
    virtual void visit(EraseCharacters const& v) = 0;
    virtual void visit(ForwardIndex const& v) = 0;
    virtual void visit(FullReset const& v) = 0;
    virtual void visit(HorizontalPositionAbsolute const& v) = 0;
    virtual void visit(HorizontalPositionRelative const& v) = 0;
    virtual void visit(HorizontalTabClear const& v) = 0;
    virtual void visit(HorizontalTabSet const& v) = 0;
    virtual void visit(Hyperlink const& v) = 0;
    virtual void visit(Index const& v) = 0;
    virtual void visit(InsertCharacters const& v) = 0;
    virtual void visit(InsertColumns const& v) = 0;
    virtual void visit(InsertLines const& v) = 0;
    virtual void visit(Linefeed const& v) = 0;
    virtual void visit(MoveCursorBackward const& v) = 0;
    virtual void visit(MoveCursorDown const& v) = 0;
    virtual void visit(MoveCursorForward const& v) = 0;
    virtual void visit(MoveCursorTo const& v) = 0;
    virtual void visit(MoveCursorToBeginOfLine const& v) = 0;
    virtual void visit(MoveCursorToColumn const& v) = 0;
    virtual void visit(MoveCursorToLine const& v) = 0;
    virtual void visit(MoveCursorToNextTab const& v) = 0;
    virtual void visit(MoveCursorUp const& v) = 0;
    virtual void visit(Notify const& v) = 0;
    virtual void visit(ReportCursorPosition const& v) = 0;
    virtual void visit(ReportExtendedCursorPosition const& v) = 0;
    virtual void visit(RequestDynamicColor const& v) = 0;
    virtual void visit(RequestMode const& v) = 0;
    virtual void visit(RequestPixelSize const& v) = 0;
    virtual void visit(RequestStatusString const& v) = 0;
    virtual void visit(RequestTabStops const& v) = 0;
    virtual void visit(ResetDynamicColor const& v) = 0;
    virtual void visit(ResizeWindow const& v) = 0;
    virtual void visit(RestoreCursor const& v) = 0;
    virtual void visit(RestoreWindowTitle const& v) = 0;
    virtual void visit(ReverseIndex const& v) = 0;
    virtual void visit(SaveCursor const& v) = 0;
    virtual void visit(SaveWindowTitle const& v) = 0;
    virtual void visit(ScreenAlignmentPattern const& v) = 0;
    virtual void visit(ScrollDown const& v) = 0;
    virtual void visit(ScrollUp const& v) = 0;
    virtual void visit(SelectConformanceLevel const& v) = 0;
    virtual void visit(SendDeviceAttributes const& v) = 0;
    virtual void visit(SendMouseEvents const& v) = 0;
    virtual void visit(SendTerminalId const& v) = 0;
    virtual void visit(SetBackgroundColor const& v) = 0;
    virtual void visit(SetCursorStyle const& v) = 0;
    virtual void visit(SetDynamicColor const& v) = 0;
    virtual void visit(SetForegroundColor const& v) = 0;
    virtual void visit(SetGraphicsRendition const& v) = 0;
    virtual void visit(SetLeftRightMargin const& v) = 0;
    virtual void visit(SetMark const& v) = 0;
    virtual void visit(SetMode const& v) = 0;
    virtual void visit(SetTopBottomMargin const& v) = 0;
    virtual void visit(SetUnderlineColor const& v) = 0;
    virtual void visit(SingleShiftSelect const& v) = 0;
    virtual void visit(SoftTerminalReset const& v) = 0;
    virtual void visit(InvalidCommand const& v) = 0;
    virtual void visit(SaveMode const& v) = 0;
    virtual void visit(RestoreMode const& v) = 0;

    // {{{ Secret std::visit() workaround
    void operator()(AppendChar const& v) { visit(v); }
    void operator()(ApplicationKeypadMode const& v) { visit(v); }
    void operator()(BackIndex const& v) { visit(v); }
    void operator()(Backspace const& v) { visit(v); }
    void operator()(Bell const& v) { visit(v); }
    void operator()(ChangeIconTitle const& v) { visit(v); }
    void operator()(ChangeWindowTitle const& v) { visit(v); }
    void operator()(ClearLine const& v) { visit(v); }
    void operator()(ClearScreen const& v) { visit(v); }
    void operator()(ClearScrollbackBuffer const& v) { visit(v); }
    void operator()(ClearToBeginOfLine const& v) { visit(v); }
    void operator()(ClearToBeginOfScreen const& v) { visit(v); }
    void operator()(ClearToEndOfLine const& v) { visit(v); }
    void operator()(ClearToEndOfScreen const& v) { visit(v); }
    void operator()(CopyToClipboard const& v) { visit(v); }
    void operator()(CursorBackwardTab const& v) { visit(v); }
    void operator()(CursorNextLine const& v) { visit(v); }
    void operator()(CursorPreviousLine const& v) { visit(v); }
    void operator()(DeleteCharacters const& v) { visit(v); }
    void operator()(DeleteColumns const& v) { visit(v); }
    void operator()(DeleteLines const& v) { visit(v); }
    void operator()(DesignateCharset const& v) { visit(v); }
    void operator()(DeviceStatusReport const& v) { visit(v); }
    void operator()(DumpState const& v) { visit(v); }
    void operator()(EraseCharacters const& v) { visit(v); }
    void operator()(ForwardIndex const& v) { visit(v); }
    void operator()(FullReset const& v) { visit(v); }
    void operator()(HorizontalPositionAbsolute const& v) { visit(v); }
    void operator()(HorizontalPositionRelative const& v) { visit(v); }
    void operator()(HorizontalTabClear const& v) { visit(v); }
    void operator()(HorizontalTabSet const& v) { visit(v); }
    void operator()(Hyperlink const& v) { visit(v); }
    void operator()(Index const& v) { visit(v); }
    void operator()(InsertCharacters const& v) { visit(v); }
    void operator()(InsertColumns const& v) { visit(v); }
    void operator()(InsertLines const& v) { visit(v); }
    void operator()(Linefeed const& v) { visit(v); }
    void operator()(MoveCursorBackward const& v) { visit(v); }
    void operator()(MoveCursorDown const& v) { visit(v); }
    void operator()(MoveCursorForward const& v) { visit(v); }
    void operator()(MoveCursorTo const& v) { visit(v); }
    void operator()(MoveCursorToBeginOfLine const& v) { visit(v); }
    void operator()(MoveCursorToColumn const& v) { visit(v); }
    void operator()(MoveCursorToLine const& v) { visit(v); }
    void operator()(MoveCursorToNextTab const& v) { visit(v); }
    void operator()(MoveCursorUp const& v) { visit(v); }
    void operator()(Notify const& v) { visit(v); }
    void operator()(ReportCursorPosition const& v) { visit(v); }
    void operator()(ReportExtendedCursorPosition const& v) { visit(v); }
    void operator()(RequestDynamicColor const& v) { visit(v); }
    void operator()(RequestMode const& v) { visit(v); }
    void operator()(RequestPixelSize const& v) { visit(v); }
    void operator()(RequestStatusString const& v) { visit(v); }
    void operator()(RequestTabStops const& v) { visit(v); }
    void operator()(ResetDynamicColor const& v) { visit(v); }
    void operator()(ResizeWindow const& v) { visit(v); }
    void operator()(RestoreCursor const& v) { visit(v); }
    void operator()(RestoreWindowTitle const& v) { visit(v); }
    void operator()(ReverseIndex const& v) { visit(v); }
    void operator()(SaveCursor const& v) { visit(v); }
    void operator()(SaveWindowTitle const& v) { visit(v); }
    void operator()(ScreenAlignmentPattern const& v) { visit(v); }
    void operator()(ScrollDown const& v) { visit(v); }
    void operator()(ScrollUp const& v) { visit(v); }
    void operator()(SelectConformanceLevel const& v) { visit(v); }
    void operator()(SendDeviceAttributes const& v) { visit(v); }
    void operator()(SendMouseEvents const& v) { visit(v); }
    void operator()(SendTerminalId const& v) { visit(v); }
    void operator()(SetBackgroundColor const& v) { visit(v); }
    void operator()(SetCursorStyle const& v) { visit(v); }
    void operator()(SetDynamicColor const& v) { visit(v); }
    void operator()(SetForegroundColor const& v) { visit(v); }
    void operator()(SetGraphicsRendition const& v) { visit(v); }
    void operator()(SetLeftRightMargin const& v) { visit(v); }
    void operator()(SetMark const& v) { visit(v); }
    void operator()(SetMode const& v) { visit(v); }
    void operator()(SetTopBottomMargin const& v) { visit(v); }
    void operator()(SetUnderlineColor const& v) { visit(v); }
    void operator()(SingleShiftSelect const& v) { visit(v); }
    void operator()(SoftTerminalReset const& v) { visit(v); }
    void operator()(InvalidCommand const& v) { visit(v); }
    void operator()(SaveMode const& v) { visit(v); }
    void operator()(RestoreMode const& v) { visit(v); }
    // }}}
};

}  // namespace terminal

namespace fmt {
    template <>
    struct formatter<terminal::Mode> {
        template <typename ParseContext>
        constexpr auto parse(ParseContext& ctx)
        {
            return ctx.begin();
        }

        template <typename FormatContext>
        auto format(const terminal::Mode _mode, FormatContext& ctx)
        {
            return format_to(ctx.out(), "{}", to_string(_mode));
        }
    };

    template <>
    struct formatter<terminal::MouseProtocol> {
        template <typename ParseContext>
        constexpr auto parse(ParseContext& ctx)
        {
            return ctx.begin();
        }

        template <typename FormatContext>
        auto format(const terminal::MouseProtocol _value, FormatContext& ctx)
        {
            switch (_value)
            {
                case terminal::MouseProtocol::X10:
                    return format_to(ctx.out(), "X10");
                case terminal::MouseProtocol::HighlightTracking:
                    return format_to(ctx.out(), "HighlightTracking");
                case terminal::MouseProtocol::ButtonTracking:
                    return format_to(ctx.out(), "ButtonTracking");
                case terminal::MouseProtocol::NormalTracking:
                    return format_to(ctx.out(), "NormalTracking");
                case terminal::MouseProtocol::AnyEventTracking:
                    return format_to(ctx.out(), "AnyEventTracking");
            }
            return format_to(ctx.out(), "{}", unsigned(_value));
        }
    };

    template <>
    struct formatter<terminal::Coordinate> {
        template <typename ParseContext>
        constexpr auto parse(ParseContext& ctx)
        {
            return ctx.begin();
        }

        template <typename FormatContext>
        auto format(const terminal::Coordinate& coord, FormatContext& ctx)
        {
            return format_to(ctx.out(), "({}:{})", coord.row, coord.column);
        }
    };

    template <>
    struct formatter<terminal::HorizontalTabClear::Which> {
        using Which = terminal::HorizontalTabClear::Which;
        template <typename ParseContext>
        constexpr auto parse(ParseContext& ctx) { return ctx.begin(); }
        template <typename FormatContext>
        auto format(const Which _which, FormatContext& ctx)
        {
            switch (_which)
            {
                case Which::AllTabs:
                    return format_to(ctx.out(), "AllTabs");
                case Which::UnderCursor:
                    return format_to(ctx.out(), "UnderCursor");
            }
            return format_to(ctx.out(), "({})", static_cast<unsigned>(_which));
        }
    };

    template <>
    struct formatter<terminal::DynamicColorName> {
        template <typename ParseContext>
        constexpr auto parse(ParseContext& ctx)
        {
            return ctx.begin();
        }

        template <typename FormatContext>
        auto format(const terminal::DynamicColorName& name, FormatContext& ctx)
        {
            using terminal::DynamicColorName;
            switch (name)
            {
                case DynamicColorName::DefaultForegroundColor:
                    return format_to(ctx.out(), "DefaultForegroundColor");
                case DynamicColorName::DefaultBackgroundColor:
                    return format_to(ctx.out(), "DefaultBackgroundColor");
                case DynamicColorName::TextCursorColor:
                    return format_to(ctx.out(), "TextCursorColor");
                case DynamicColorName::MouseForegroundColor:
                    return format_to(ctx.out(), "MouseForegroundColor");
                case DynamicColorName::MouseBackgroundColor:
                    return format_to(ctx.out(), "MouseBackgroundColor");
                case DynamicColorName::HighlightForegroundColor:
                    return format_to(ctx.out(), "HighlightForegroundColor");
                case DynamicColorName::HighlightBackgroundColor:
                    return format_to(ctx.out(), "HighlightBackgroundColor");
            }
            return format_to(ctx.out(), "({})", static_cast<int>(name));
        }
    };

    template <>
    struct formatter<terminal::InvalidCommand::Reason> {
        template <typename ParseContext>
        constexpr auto parse(ParseContext& ctx) { return ctx.begin(); }
        template <typename FormatContext>
        auto format(terminal::InvalidCommand::Reason _reason, FormatContext& _ctx)
        {
            switch (_reason)
            {
                case terminal::InvalidCommand::Reason::Unsupported:
                    return format_to(_ctx.out(), "unsupported");
                case terminal::InvalidCommand::Reason::Invalid:
                    return format_to(_ctx.out(), "invalid");
                case terminal::InvalidCommand::Reason::Unknown:
                    return format_to(_ctx.out(), "unknown");
                default:
                    return format_to(_ctx.out(), "({})", static_cast<int>(_reason));
            }
        }
    };

    template <>
    struct formatter<terminal::Command> {
        template <typename ParseContext>
        constexpr auto parse(ParseContext& ctx) { return ctx.begin(); }
        template <typename FormatContext>
        auto format(terminal::Command const& _command, FormatContext& _ctx)
        {
            return format_to(_ctx.out(), "{}", terminal::to_string(_command));
        }
    };
}
