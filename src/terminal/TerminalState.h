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

#include <terminal/Cell.h>
#include <terminal/Charset.h>
#include <terminal/ColorPalette.h>
#include <terminal/GraphicsAttributes.h>
#include <terminal/Grid.h>
#include <terminal/Hyperlink.h>
#include <terminal/Parser.h>
#include <terminal/ScreenEvents.h> // ScreenType
#include <terminal/Sequencer.h>
#include <terminal/primitives.h>

#include <unicode/utf8.h>

#include <fmt/format.h>

#include <bitset>
#include <functional>
#include <memory>
#include <stack>
#include <vector>

namespace terminal
{

template <typename EventListener>
class Screen;

// {{{ enums
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
    SixelScrolling,           // ?80
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

    // If this mode is unset, text reflow is blocked on on this line and any lines below.
    // If this mode is set, the current line and any line below is allowed to reflow.
    // Default: Enabled (if supported by terminal).
    TextReflow = 2027,

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
// }}}

// {{{ enum helper free functions
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

std::string to_string(DECMode _mode);
std::string to_string(AnsiMode _mode);

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
    case DECMode::SixelScrolling: return 80;
    case DECMode::UsePrivateColorRegisters: return 1070;
    case DECMode::MouseExtended: return 1005;
    case DECMode::MouseSGR: return 1006;
    case DECMode::MouseURXVT: return 1015;
    case DECMode::MouseSGRPixels: return 1016;
    case DECMode::MouseAlternateScroll: return 1007;
    case DECMode::BatchedRendering: return 2026;
    case DECMode::TextReflow: return 2027;
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
    case DECMode::SixelScrolling:
    case DECMode::UsePrivateColorRegisters:
    case DECMode::MouseExtended:
    case DECMode::MouseSGR:
    case DECMode::MouseURXVT:
    case DECMode::MouseSGRPixels:
    case DECMode::MouseAlternateScroll:
    case DECMode::BatchedRendering:
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
// }}}

// {{{ Modes
/// API for setting/querying terminal modes.
///
/// This abstracts away the actual implementation for more intuitive use and easier future adaptability.
class Modes
{
  public:
    void set(AnsiMode _mode, bool _enabled) { ansi_.set(static_cast<size_t>(_mode), _enabled); }

    void set(DECMode _mode, bool _enabled) { dec_.set(static_cast<size_t>(_mode), _enabled); }

    bool enabled(AnsiMode _mode) const noexcept { return ansi_.test(static_cast<size_t>(_mode)); }

    bool enabled(DECMode _mode) const noexcept { return dec_.test(static_cast<size_t>(_mode)); }

    void save(std::vector<DECMode> const& _modes)
    {
        for (DECMode const mode: _modes)
            savedModes_[mode].push_back(enabled(mode));
    }

    void restore(std::vector<DECMode> const& _modes)
    {
        for (DECMode const mode: _modes)
        {
            if (auto i = savedModes_.find(mode); i != savedModes_.end() && !i->second.empty())
            {
                auto& saved = i->second;
                set(mode, saved.back());
                saved.pop_back();
            }
        }
    }

  private:
    // TODO: make this a vector<bool> by casting from Mode, but that requires ensured small linearity in Mode
    // enum values.
    std::bitset<32> ansi_;                            // AnsiMode
    std::bitset<8452 + 1> dec_;                       // DECMode
    std::map<DECMode, std::vector<bool>> savedModes_; //!< saved DEC modes
};
// }}}

// {{{ Cursor
/// Terminal cursor data structure.
///
/// NB: Take care what to store here, as DECSC/DECRC will save/restore this struct.
struct Cursor
{
    CellLocation position { LineOffset(0), ColumnOffset(0) };
    bool autoWrap = true; // false;
    bool originMode = false;
    bool visible = true;
    GraphicsAttributes graphicsRendition {};
    CharsetMapping charsets {};
    HyperlinkId hyperlink {};
    // TODO: selective erase attribute
    // TODO: SS2/SS3 states
    // TODO: CharacterSet for GL and GR
};
// }}}

/**
 * Defines the state of a terminal.
 * All those data members used to live in Screen, but are moved
 * out with the goal to move all shared state up to Terminal later
 * and have Screen API maintain only *one* screen.
 *
 * TODO: Let's move all shared data into one place,
 * ultimatively ending up in Terminal (or keep TerminalState).
 */
template <typename TheTerminal>
struct TerminalState
{
    TerminalState(TheTerminal& _terminal,
                  PageSize _pageSize,
                  LineCount _maxHistoryLineCount,
                  ImageSize _maxImageSize,
                  unsigned _maxImageColorRegisters,
                  bool _sixelCursorConformance,
                  ColorPalette const& _colorPalette,
                  bool _allowReflowOnResize):
        terminal { _terminal },
        pageSize { _pageSize },
        cellPixelSize {},
        margin { Margin::Vertical { {}, pageSize.lines.as<LineOffset>() - LineOffset(1) },
                 Margin::Horizontal { {}, pageSize.columns.as<ColumnOffset>() - ColumnOffset(1) } },
        defaultColorPalette { _colorPalette },
        colorPalette { _colorPalette },
        maxImageColorRegisters { _maxImageColorRegisters },
        maxImageSize { _maxImageSize },
        maxImageSizeLimit { _maxImageSize },
        imageColorPalette { std::make_shared<SixelColorPalette>(maxImageColorRegisters,
                                                                maxImageColorRegisters) },
        imagePool { [this](Image const* _image) {
            terminal.discardImage(*_image);
        } },
        sixelCursorConformance { _sixelCursorConformance },
        allowReflowOnResize { _allowReflowOnResize },
        grids { Grid<Cell>(_pageSize, _allowReflowOnResize, _maxHistoryLineCount),
                Grid<Cell>(_pageSize, false, LineCount(0)) },
        activeGrid { &grids[0] },
        cursor {},
        lastCursorPosition {},
        hyperlinks { HyperlinkCache { 1024 } },
        respondToTCapQuery { true },
        sequencer { _terminal, imageColorPalette },
        parser { std::ref(sequencer) }
    {
    }

    TheTerminal& terminal;

    PageSize pageSize;
    ImageSize cellPixelSize; ///< contains the pixel size of a single cell, or area(cellPixelSize_) == 0 if
                             ///< unknown.
    Margin margin;

    ColorPalette defaultColorPalette;
    ColorPalette colorPalette;

    bool focused = true;

    VTType terminalId = VTType::VT525;

    Modes modes;
    std::map<DECMode, std::vector<bool>> savedModes; //!< saved DEC modes

    unsigned maxImageColorRegisters;
    ImageSize maxImageSize;
    ImageSize maxImageSizeLimit;
    std::shared_ptr<SixelColorPalette> imageColorPalette;
    ImagePool imagePool;

    bool sixelCursorConformance = true;

    ColumnCount tabWidth { 8 };
    std::vector<ColumnOffset> tabs;

    bool allowReflowOnResize;

    ScreenType screenType = ScreenType::Main;
    std::array<Grid<Cell>, 2> grids;
    Grid<Cell>* activeGrid;

    // cursor related
    //
    Cursor cursor;
    Cursor savedCursor;
    Cursor savedPrimaryCursor; //!< saved cursor of primary-screen when switching to alt-screen.
    CellLocation lastCursorPosition;
    bool wrapPending = false;

    CursorDisplay cursorDisplay = CursorDisplay::Steady;
    CursorShape cursorShape = CursorShape::Block;

    std::string currentWorkingDirectory = {};

    unsigned maxImageRegisterCount = 256;
    bool usePrivateColorRegisters = false;

    // Hyperlink related
    //
    HyperlinkStorage hyperlinks {};

    // experimental features
    //
    bool respondToTCapQuery = true;

    std::string windowTitle {};
    std::stack<std::string> savedWindowTitles {};

    Sequencer<TheTerminal> sequencer;
    parser::Parser<Sequencer<TheTerminal>> parser;
    uint64_t instructionCounter = 0;

    char32_t precedingGraphicCharacter = {};
    unicode::utf8_decoder_state utf8DecoderState = {};
    bool terminating = false;
};

} // namespace terminal

// {{{ fmt formatters
namespace fmt
{

template <>
struct formatter<terminal::AnsiMode>
{
    template <typename ParseContext>
    constexpr auto parse(ParseContext& ctx)
    {
        return ctx.begin();
    }
    template <typename FormatContext>
    auto format(terminal::AnsiMode _mode, FormatContext& ctx)
    {
        return format_to(ctx.out(), "{}", to_string(_mode));
    }
};

template <>
struct formatter<terminal::DECMode>
{
    template <typename ParseContext>
    constexpr auto parse(ParseContext& ctx)
    {
        return ctx.begin();
    }
    template <typename FormatContext>
    auto format(terminal::DECMode _mode, FormatContext& ctx)
    {
        return format_to(ctx.out(), "{}", to_string(_mode));
    }
};

template <>
struct formatter<terminal::Cursor>
{
    template <typename ParseContext>
    constexpr auto parse(ParseContext& ctx)
    {
        return ctx.begin();
    }
    template <typename FormatContext>
    auto format(const terminal::Cursor cursor, FormatContext& ctx)
    {
        return format_to(ctx.out(),
                         "({}:{}{})",
                         cursor.position.line,
                         cursor.position.column,
                         cursor.visible ? "" : ", (invis)");
    }
};

template <>
struct formatter<terminal::DynamicColorName>
{
    template <typename ParseContext>
    constexpr auto parse(ParseContext& ctx)
    {
        return ctx.begin();
    }
    template <typename FormatContext>
    auto format(terminal::DynamicColorName name, FormatContext& ctx)
    {
        // clang-format off
        using terminal::DynamicColorName;
        switch (name)
        {
        case DynamicColorName::DefaultForegroundColor: return format_to(ctx.out(), "DefaultForegroundColor");
        case DynamicColorName::DefaultBackgroundColor: return format_to(ctx.out(), "DefaultBackgroundColor");
        case DynamicColorName::TextCursorColor: return format_to(ctx.out(), "TextCursorColor");
        case DynamicColorName::MouseForegroundColor: return format_to(ctx.out(), "MouseForegroundColor");
        case DynamicColorName::MouseBackgroundColor: return format_to(ctx.out(), "MouseBackgroundColor");
        case DynamicColorName::HighlightForegroundColor: return format_to(ctx.out(), "HighlightForegroundColor");
        case DynamicColorName::HighlightBackgroundColor: return format_to(ctx.out(), "HighlightBackgroundColor");
        }
        return format_to(ctx.out(), "({})", static_cast<unsigned>(name));
        // clang-format on
    }
};

} // namespace fmt
// }}}
