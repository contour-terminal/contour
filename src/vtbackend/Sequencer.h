// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <vtbackend/Functions.h>
#include <vtbackend/Image.h>
#include <vtbackend/Sequence.h>
#include <vtbackend/SixelParser.h>
#include <vtbackend/primitives.h>

#include <vtparser/ParserEvents.h>
#include <vtparser/ParserExtension.h>

#include <libunicode/convert.h>
#include <libunicode/utf8.h>

#include <memory>
#include <string>
#include <string_view>

namespace vtbackend
{

// {{{ TODO: refactor me
// XTSMGRAPHICS (xterm extension)
// CSI ? Pi ; Pa ; Pv S
namespace XtSmGraphics
{
    enum class Item
    {
        NumberOfColorRegisters = 1,
        SixelGraphicsGeometry = 2,
        ReGISGraphicsGeometry = 3,
    };

    enum class Action
    {
        Read = 1,
        ResetToDefault = 2,
        SetToValue = 3,
        ReadLimit = 4
    };

    using Value = std::variant<std::monostate, unsigned, ImageSize>;
} // namespace XtSmGraphics

/// TBC - Tab Clear
///
/// This control function clears tab stops.
enum class HorizontalTabClear
{
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
enum class RequestPixelSize // TODO: rename RequestPixelSize to RequestArea?
{
    CellArea,
    TextArea,
    WindowArea,
};

/// DECRQSS - Request Status String
enum class RequestStatusString
{
    SGR,
    DECSCL,
    DECSCUSR,
    DECSCA,
    DECSTBM,
    DECSLRM,
    DECSLPP,
    DECSCPP,
    DECSNLS,
    DECSASD,
    DECSSDT,
};

/// DECSIXEL - Sixel Graphics Image.
struct SixelImage
{ // TODO: this struct is only used internally in Sequencer, make it private
    /// Size in pixels for this image
    ImageSize size;

    /// RGBA buffer of the image to be rendered
    Image::Data rgba;
};

inline std::string setDynamicColorValue(
    RGBColor const& color) // TODO: yet another helper. maybe SemanticsUtils static class?
{
    auto const r = static_cast<unsigned>(static_cast<float>(color.red) / 255.0f * 0xFFFF);
    auto const g = static_cast<unsigned>(static_cast<float>(color.green) / 255.0f * 0xFFFF);
    auto const b = static_cast<unsigned>(static_cast<float>(color.blue) / 255.0f * 0xFFFF);
    return fmt::format("rgb:{:04X}/{:04X}/{:04X}", r, g, b);
}

enum class ApplyResult
{
    Ok,
    Invalid,
    Unsupported,
};
// }}}

class Terminal;

/// Sequencer - The semantic VT analyzer layer.
///
/// Sequencer implements the translation from VT parser events, forming a higher level Sequence,
/// that can be matched against actions to perform on the target Screen.
class Sequencer
{
  public:
    /// Constructs the sequencer stage.
    explicit Sequencer(Terminal& terminal);

    // ParserEvents
    //
    void error(std::string_view errorString);
    void print(char32_t codepoint);
    size_t print(std::string_view chars, size_t cellCount);
    void printEnd() noexcept;
    void execute(char controlCode);
    void clear() noexcept;
    void collect(char ch);
    void collectLeader(char leader) noexcept;
    void param(char ch) noexcept;
    void paramDigit(char ch) noexcept;
    void paramSeparator() noexcept;
    void paramSubSeparator() noexcept;
    void dispatchESC(char finalChar);
    void dispatchCSI(char finalChar);
    void startOSC() noexcept;
    void putOSC(char ch);
    void dispatchOSC();
    void hook(char finalChar);
    void put(char ch);
    void unhook();
    void startAPC() {}
    void putAPC(char) {}
    void dispatchAPC() {}
    void startPM() {}
    void putPM(char) {}
    void dispatchPM() {}

    void hookParser(std::unique_ptr<ParserExtension> parserExtension) noexcept
    {
        _hookedParser = std::move(parserExtension);
    }

    [[nodiscard]] size_t maxBulkTextSequenceWidth() const noexcept;

  private:
    void handleSequence();

    // private data
    //
    Terminal& _terminal;
    Sequence _sequence {};
    SequenceParameterBuilder _parameterBuilder;

    std::unique_ptr<ParserExtension> _hookedParser;
    std::unique_ptr<SixelImageBuilder> _sixelImageBuilder;
};

// {{{ inlines
inline void Sequencer::clear() noexcept
{
    _sequence.clearExceptParameters();
    _parameterBuilder.reset();
}

inline void Sequencer::paramDigit(char ch) noexcept
{
    _parameterBuilder.multiplyBy10AndAdd(static_cast<uint8_t>(ch - '0'));
}

inline void Sequencer::paramSeparator() noexcept
{
    _parameterBuilder.nextParameter();
}

inline void Sequencer::paramSubSeparator() noexcept
{
    _parameterBuilder.nextSubParameter();
}
// }}}

} // namespace vtbackend

// {{{ fmt formatter
template <>
struct fmt::formatter<vtbackend::RequestStatusString>: formatter<std::string_view>
{
    auto format(vtbackend::RequestStatusString value, format_context& ctx) noexcept
        -> format_context::iterator
    {
        string_view name;
        switch (value)
        {
            case vtbackend::RequestStatusString::SGR: name = "SGR"; break;
            case vtbackend::RequestStatusString::DECSCL: name = "DECSCL"; break;
            case vtbackend::RequestStatusString::DECSCUSR: name = "DECSCUSR"; break;
            case vtbackend::RequestStatusString::DECSCA: name = "DECSCA"; break;
            case vtbackend::RequestStatusString::DECSTBM: name = "DECSTBM"; break;
            case vtbackend::RequestStatusString::DECSLRM: name = "DECSLRM"; break;
            case vtbackend::RequestStatusString::DECSLPP: name = "DECSLPP"; break;
            case vtbackend::RequestStatusString::DECSCPP: name = "DECSCPP"; break;
            case vtbackend::RequestStatusString::DECSNLS: name = "DECSNLS"; break;
            case vtbackend::RequestStatusString::DECSASD: name = "DECSASD"; break;
            case vtbackend::RequestStatusString::DECSSDT: name = "DECSSDT"; break;
        }
        return formatter<string_view>::format(name, ctx);
    }
};

template <>
struct fmt::formatter<vtbackend::Sequence>
{
    template <typename ParseContext>
    constexpr auto parse(ParseContext& ctx)
    {
        return ctx.begin();
    }
    template <typename FormatContext>
    auto format(vtbackend::Sequence const& seq, FormatContext& ctx)
    {
        return fmt::format_to(ctx.out(), "{}", seq.text());
    }
};
// }}}
