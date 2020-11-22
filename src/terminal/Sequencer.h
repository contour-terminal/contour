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

#include <terminal/Logger.h>
#include <terminal/ParserEvents.h>
#include <terminal/ParserExtension.h>
#include <terminal/Functions.h>
#include <terminal/SixelParser.h>

#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace terminal {

class Screen;

// {{{ enums
enum class CursorDisplay {
    Steady,
    Blink
};

enum class CursorShape {
    Block,
    Rectangle,
    Underscore,
    Bar,
};

enum class ControlTransmissionMode {
    S7C1T, // 7-bit controls
    S8C1T, // 8-bit controls
};

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
    SixelScrolling, // ?80
    UsePrivateColorRegisters, // ?1070
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

enum class CharsetTable {
    G0 = 0,
    G1 = 1,
    G2 = 2,
    G3 = 3
};

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
// }}}

// {{{ enum helper free functions
std::string to_string(Mode m);
std::string to_string(CharsetTable i);
std::string to_string(CharsetId charset);
std::string to_string(GraphicsRendition s);

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
        case Mode::SixelScrolling:
        case Mode::UsePrivateColorRegisters:
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
        case Mode::SixelScrolling: return "?80";
        case Mode::UsePrivateColorRegisters: return "?1070";
        case Mode::MouseExtended: return "?1005";
        case Mode::MouseSGR: return "?1006";
        case Mode::MouseURXVT: return "?1015";
        case Mode::MouseAlternateScroll: return "?1007";
        case Mode::BatchedRendering: return "2026";
    }
    return "0";
}

CursorShape makeCursorShape(std::string const& _name);

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
// }}}

// {{{ TODO: refactor me
// XTSMGRAPHICS (xterm extension)
// CSI ? Pi ; Pa ; Pv S
struct XtSmGraphics
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

    using Value = std::variant<std::monostate, int, Size>;

    Item item;
    Action action;
    Value value;
};

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

/// DECSIXEL - Sixel Graphics Image.
struct SixelImage { // TODO: this struct is only used internally in Sequencer, make it private
    /// Size in pixels for this image
    Size size;
    /// RGBA buffer of the image to be rendered
    std::vector<uint8_t> rgba;
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

/// Helps constructing VT functions as they're being parsed by the VT parser.
class Sequence {
  public:
    using Parameter = int;
    using ParameterList = std::vector<std::vector<Parameter>>;
    using Intermediaries = std::string;
    using DataString = std::string;

  private:
    FunctionCategory category_;
    char leaderSymbol_ = 0;
    ParameterList parameters_;
    Intermediaries intermediateCharacters_;
    char finalChar_ = 0;
    DataString dataString_;

  public:
    size_t constexpr static MaxParameters = 16;
    size_t constexpr static MaxSubParameters = 8;

    Sequence()
    {
        parameters_.resize(MaxParameters);
        for (auto& param : parameters_)
            param.reserve(MaxSubParameters);
        parameters_.clear();
    }

    // mutators
    //
    void clear()
    {
        category_ = FunctionCategory::C0;
        leaderSymbol_ = 0;
        intermediateCharacters_.clear();
        parameters_.clear();
        finalChar_ = 0;
        dataString_.clear();
    }

    void setCategory(FunctionCategory _cat) noexcept { category_ = _cat; }
    void setLeader(char _ch) noexcept { leaderSymbol_ = _ch; }
    ParameterList& parameters() noexcept { return parameters_; }
    Intermediaries& intermediateCharacters() noexcept { return intermediateCharacters_; }
    void setFinalChar(char _ch) noexcept { finalChar_ = _ch; }

    DataString const& dataString() const noexcept { return dataString_; }
    DataString& dataString() noexcept { return dataString_; }

    /// @returns this VT-sequence into a human readable string form.
    std::string text() const;

    /// @returns the raw VT-sequence string.
    std::string raw() const;

    /// Converts a FunctionSpinto a FunctionSelector, applicable for finding the corresponding FunctionDefinition.
    FunctionSelector selector() const noexcept
    {
        switch (category_)
        {
            case FunctionCategory::OSC:
                return FunctionSelector{category_, 0, parameters_[0][0], 0, 0};
            default:
            {
                // Only support CSI sequences with 0 or 1 intermediate characters.
                char const intermediate = intermediateCharacters_.size() == 1
                    ? static_cast<char>(intermediateCharacters_[0])
                    : char{};

                return FunctionSelector{category_, leaderSymbol_, static_cast<int>(parameters_.size()), intermediate, finalChar_};
            }
        }
    }

    // accessors
    //
    FunctionCategory category() const noexcept { return category_; }
    Intermediaries const& intermediateCharacters() const noexcept { return intermediateCharacters_; }
    char finalChar() const noexcept { return finalChar_; }

    ParameterList const& parameters() const noexcept { return parameters_; }
    size_t parameterCount() const noexcept { return parameters_.size(); }
    size_t subParameterCount(size_t _index) const noexcept { return parameters_[_index].size() - 1; }

    std::optional<Parameter> param_opt(size_t _index) const noexcept
    {
        if (_index < parameters_.size() && parameters_[_index][0])
            return {parameters_[_index][0]};
        else
            return std::nullopt;
    }

    Parameter param_or(size_t _index, Parameter _defaultValue) const noexcept
    {
        return param_opt(_index).value_or(_defaultValue);
    }

    int param(size_t _index) const noexcept
    {
        assert(_index < parameters_.size());
        assert(0 < parameters_[_index].size());
        return parameters_[_index][0];
    }

    int subparam(size_t _index, size_t _subIndex) const noexcept
    {
        assert(_index < parameters_.size());
        assert(_subIndex + 1 < parameters_[_index].size());
        return parameters_[_index][_subIndex + 1];
    }

    bool containsParameter(int _value) const noexcept
    {
        for (size_t i = 0; i < parameterCount(); ++i)
            if (param(i) == _value)
                return true;
        return false;
    }
};

/// Sequencer - The semantic VT analyzer layer.
///
/// Sequencer implements the translation from VT parser events, forming a higher level Sequence,
/// that can be matched against actions to perform on the target Screen.
class Sequencer : public ParserEvents {
  public:
    /// Constructs the sequencer stage.
    Sequencer(Screen& _screen,
              Logger _logger,
              Size _maxImageSize,
              RGBAColor _backgroundColor,
              std::shared_ptr<ColorPalette> _imageColorPalette);

    /// Constructs a very primitive Sequencer, SHOULD be used for testing only.
    Sequencer(Screen& _screen, Logger _logger)
        : Sequencer(_screen,
                    std::move(_logger),
                    Size{800, 600},
                    RGBAColor{},
                    std::make_shared<ColorPalette>()) {}

    void setMaxImageSize(Size _value) { maxImageSize_ = _value; }
    void setMaxImageColorRegisters(int _value) { maxImageRegisterCount_ = _value; }
    void setUsePrivateColorRegisters(bool _value) { usePrivateColorRegisters_ = _value; }

    int64_t instructionCounter() const noexcept { return instructionCounter_; }
    void resetInstructionCounter() noexcept { instructionCounter_ = 0; }

    // helper methods
    //
    static std::optional<RGBColor> parseColor(std::string_view const& _value);

    // ParserEvents
    //
    void error(std::string_view const& _errorString) override;
    void print(char32_t _text) override;
    void execute(char _controlCode) override;
    void clear() override;
    void collect(char _char) override;
    void collectLeader(char _leader) override;
    void param(char _char) override;
    void dispatchESC(char _function) override;
    void dispatchCSI(char _function) override;
    void startOSC() override;
    void putOSC(char32_t _char) override;
    void dispatchOSC() override;
    void hook(char _function) override;
    void put(char32_t _char) override;
    void unhook() override;

  private:
    void executeControlFunction(char _c0);
    void handleSequence();

    void hookSixel(Sequence const& _ctx);
    void hookDECRQSS(Sequence const& _ctx);

    ApplyResult apply(FunctionDefinition const& _function, Sequence const& _context);

    template <typename Event, typename... Args>
    void log(Args&&... args) const
    {
        if (logger_)
            logger_(Event{ std::forward<Args>(args)... });
    }

  private:
    Sequence sequence_{};
    Screen& screen_;
    bool batching_ = false;
    int64_t instructionCounter_ = 0;
    using Batchable = std::variant<Sequence, SixelImage>;
    std::vector<Batchable> batchedSequences_;

    Logger const logger_;

    std::unique_ptr<ParserExtension> hookedParser_;
    std::unique_ptr<SixelImageBuilder> sixelImageBuilder_;
    std::shared_ptr<ColorPalette> imageColorPalette_;
    bool usePrivateColorRegisters_ = false;
    Size maxImageSize_;
    int maxImageRegisterCount_;
    RGBAColor backgroundColor_;
};

}  // namespace terminal

namespace fmt { // {{{
    template <>
    struct formatter<terminal::Mode> {
        template <typename ParseContext>
        constexpr auto parse(ParseContext& ctx)
        {
            return ctx.begin();
        }

        template <typename FormatContext>
        auto format(terminal::Mode _mode, FormatContext& ctx)
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
            return format_to(ctx.out(), "({})", static_cast<int>(name));
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
