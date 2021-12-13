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

#include <terminal/Image.h>
#include <terminal/ParserEvents.h>
#include <terminal/ParserExtension.h>
#include <terminal/Functions.h>
#include <terminal/Sequence.h>
#include <terminal/SixelParser.h>
#include <terminal/primitives.h>

#include <unicode/utf8.h>
#include <unicode/convert.h>

#include <cassert>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace terminal {

template <typename EventListener> class Screen;

// {{{ enums
enum class ControlTransmissionMode
{
    S7C1T, // 7-bit controls
    S8C1T, // 8-bit controls
};

enum class GraphicsRendition
{
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

/// Mutualy exclusive mouse protocls.
enum class MouseProtocol
{
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

enum class AnsiMode { // {{{
    KeyboardAction = 2,         // KAM
    Insert = 4,                 // IRM
    SendReceive = 12,           // SRM
    AutomaticNewLine = 20,      // LNM
}; // }}}

enum class DECMode { // {{{
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
    AllowColumns80to132, // ?40
    DebugLogging, // ?46,
    UseAlternateScreen,
    BracketedPaste,
    FocusTracking, // 1004
    SixelScrolling, // ?80
    UsePrivateColorRegisters, // ?1070

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

    // If this mode is unset, text reflow is blocked on on this line and any lines below.
    // If this mode is set, the current line and any line below is allowed to reflow.
    // Default: Enabled (if supported by terminal).
    TextReflow = 2027,
    // }}}
}; // }}}

enum class CharsetTable
{
    G0 = 0,
    G1 = 1,
    G2 = 2,
    G3 = 3
};

enum class CharsetId
{
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
std::string to_string(CharsetTable i);
std::string to_string(CharsetId charset);
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

constexpr bool isValidAnsiMode(int _mode) noexcept
{
    switch (static_cast<AnsiMode>(_mode))
    {
        case AnsiMode::KeyboardAction:
        case AnsiMode::Insert:
        case AnsiMode::SendReceive:
        case AnsiMode::AutomaticNewLine:
            return true;
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
        case DECMode::MouseAlternateScroll: return 1007;
        case DECMode::BatchedRendering: return 2026;
        case DECMode::TextReflow: return 2027;
    }
    return static_cast<unsigned>(m);
}

constexpr bool isValidDECMode(int _mode) noexcept
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
        case DECMode::MouseAlternateScroll:
        case DECMode::BatchedRendering:
        case DECMode::TextReflow:
            return true;
    }
    return false;
}

CursorShape makeCursorShape(std::string const& _name);

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
        default:
            return DynamicColorName::DefaultForegroundColor;
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
        default:
            return 0;
    }
}
// }}}

// {{{ TODO: refactor me
// XTSMGRAPHICS (xterm extension)
// CSI ? Pi ; Pa ; Pv S
namespace XtSmGraphics
{
    enum class Item {
        NumberOfColorRegisters = 1,
        SixelGraphicsGeometry = 2,
        ReGISGraphicsGeometry = 3,
    };

    enum class Action {
        Read = 1,
        ResetToDefault = 2,
        SetToValue = 3,
        ReadLimit = 4
    };

    using Value = std::variant<std::monostate, unsigned, ImageSize>;
}

/// TBC - Tab Clear
///
/// This control function clears tab stops.
enum class HorizontalTabClear {
    /// Ps = 0 (default)
    AllTabs,

    /// Ps = 3
    UnderCursor,
};

/// Input: CSI 16 t
///
///  Input: CSI 14 t (for text area size)
///  Input: CSI 14; 2 t (for full window size)
/// Output: CSI 14 ; width ; height ; t
enum class RequestPixelSize {
    CellArea,
    TextArea,
    WindowArea,
};

/// DECRQSS - Request Status String
enum class RequestStatusString {
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

/// DECSIXEL - Sixel Graphics Image.
struct SixelImage { // TODO: this struct is only used internally in Sequencer, make it private
    /// Size in pixels for this image
    ImageSize size;

    /// RGBA buffer of the image to be rendered
    Image::Data rgba;
};

inline std::string setDynamicColorValue(RGBColor const& color) // TODO: yet another helper. maybe SemanticsUtils static class?
{
    auto const r = static_cast<unsigned>(static_cast<float>(color.red) / 255.0f * 0xFFFF);
    auto const g = static_cast<unsigned>(static_cast<float>(color.green) / 255.0f * 0xFFFF);
    auto const b = static_cast<unsigned>(static_cast<float>(color.blue) / 255.0f * 0xFFFF);
    return fmt::format("rgb:{:04X}/{:04X}/{:04X}", r, g, b);
}

enum class ApplyResult {
    Ok,
    Invalid,
    Unsupported,
};
// }}}

/// Sequencer - The semantic VT analyzer layer.
///
/// Sequencer implements the translation from VT parser events, forming a higher level Sequence,
/// that can be matched against actions to perform on the target Screen.
template <typename EventListener>
class Sequencer {
  public:
    /// Constructs the sequencer stage.
    Sequencer(Screen<EventListener>& _screen,
              ImageSize _maxImageSize,
              RGBAColor _backgroundColor,
              std::shared_ptr<SixelColorPalette> _imageColorPalette);

    void setMaxImageSize(ImageSize _value) { maxImageSize_ = _value; }
    void setMaxImageColorRegisters(unsigned _value) { maxImageRegisterCount_ = _value; }
    void setUsePrivateColorRegisters(bool _value) { usePrivateColorRegisters_ = _value; }

    uint64_t instructionCounter() const noexcept { return instructionCounter_; }
    void resetInstructionCounter() noexcept { instructionCounter_ = 0; }
    char32_t precedingGraphicCharacter() const noexcept { return precedingGraphicCharacter_; }

    // ParserEvents
    //
    void error(std::string_view _errorString);
    void print(char _text);
    void print(std::string_view _chars);
    void execute(char _controlCode);
    void clear();
    void collect(char _char);
    void collectLeader(char _leader);
    void param(char _char);
    void dispatchESC(char _function);
    void dispatchCSI(char _function);
    void startOSC();
    void putOSC(char _char);
    void dispatchOSC();
    void hook(char _function);
    void put(char _char);
    void unhook();
    void startAPC() {}
    void putAPC(char) {}
    void dispatchAPC() {}
    void startPM() {}
    void putPM(char) {}
    void dispatchPM() {}

  private:
    void executeControlFunction(char _c0);
    void handleSequence();

    [[nodiscard]] std::unique_ptr<ParserExtension> hookSTP(Sequence const& _ctx);
    [[nodiscard]] std::unique_ptr<ParserExtension> hookSixel(Sequence const& _ctx);
    [[nodiscard]] std::unique_ptr<ParserExtension> hookDECRQSS(Sequence const& _ctx);
    [[nodiscard]] std::unique_ptr<ParserExtension> hookXTGETTCAP(Sequence const& /*_seq*/);

    void applyAndLog(FunctionDefinition const& _function, Sequence const& _context);
    ApplyResult apply(FunctionDefinition const& _function, Sequence const& _context);

    // private data
    //
    Sequence sequence_{};
    Screen<EventListener>& screen_;
    char32_t precedingGraphicCharacter_ = {};
    uint64_t instructionCounter_ = 0;
    unicode::utf8_decoder_state utf8DecoderState_ = {};

    std::unique_ptr<ParserExtension> hookedParser_;
    std::unique_ptr<SixelImageBuilder> sixelImageBuilder_;
    std::shared_ptr<SixelColorPalette> imageColorPalette_;
    bool usePrivateColorRegisters_ = false;
    ImageSize maxImageSize_;
    unsigned maxImageRegisterCount_;
    RGBAColor backgroundColor_;
};

}  // namespace terminal

namespace fmt { // {{{
    template <>
    struct formatter<terminal::AnsiMode> {
        template <typename ParseContext>
        constexpr auto parse(ParseContext& ctx) { return ctx.begin(); }
        template <typename FormatContext>
        auto format(terminal::AnsiMode _mode, FormatContext& ctx) { return format_to(ctx.out(), "{}", to_string(_mode)); }
    };

    template <>
    struct formatter<terminal::DECMode> {
        template <typename ParseContext>
        constexpr auto parse(ParseContext& ctx) { return ctx.begin(); }
        template <typename FormatContext>
        auto format(terminal::DECMode _mode, FormatContext& ctx) { return format_to(ctx.out(), "{}", to_string(_mode)); }
    };

    template <>
    struct formatter<terminal::MouseProtocol> {
        template <typename ParseContext>
        constexpr auto parse(ParseContext& ctx)
        {
            return ctx.begin();
        }

        template <typename FormatContext>
        auto format(terminal::MouseProtocol _value, FormatContext& ctx)
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
    struct formatter<terminal::DynamicColorName> {
        template <typename ParseContext>
        constexpr auto parse(ParseContext& ctx)
        {
            return ctx.begin();
        }

        template <typename FormatContext>
        auto format(terminal::DynamicColorName name, FormatContext& ctx)
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
            return format_to(ctx.out(), "({})", static_cast<unsigned>(name));
        }
    };

    template <>
    struct formatter<terminal::Sequence> {
        template <typename ParseContext>
        constexpr auto parse(ParseContext& ctx) { return ctx.begin(); }
        template <typename FormatContext>
        auto format(terminal::Sequence const& seq, FormatContext& ctx)
        {
            return format_to(ctx.out(), "{}", seq.text());
        }
    };
} // }}}
