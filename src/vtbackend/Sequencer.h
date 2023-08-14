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

#include <vtbackend/Functions.h>
#include <vtbackend/Image.h>
#include <vtbackend/Sequence.h>
#include <vtbackend/SixelParser.h>
#include <vtbackend/primitives.h>

#include <vtparser/ParserEvents.h>
#include <vtparser/ParserExtension.h>

#include <cassert>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include <libunicode/convert.h>
#include <libunicode/utf8.h>

namespace terminal
{

// {{{ TODO: refactor me
// XTSMGRAPHICS (xterm extension)
// CSI ? Pi ; Pa ; Pv S
namespace XtSmGraphics
{
    enum class item
    {
        NumberOfColorRegisters = 1,
        SixelGraphicsGeometry = 2,
        ReGISGraphicsGeometry = 3,
    };

    enum class action
    {
        Read = 1,
        ResetToDefault = 2,
        SetToValue = 3,
        ReadLimit = 4
    };

    using value = std::variant<std::monostate, unsigned, image_size>;
} // namespace XtSmGraphics

/// TBC - Tab Clear
///
/// This control function clears tab stops.
enum class horizontal_tab_clear
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
enum class request_pixel_size // TODO: rename RequestPixelSize to RequestArea?
{
    CellArea,
    TextArea,
    WindowArea,
};

/// DECRQSS - Request Status String
enum class request_status_string
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
struct sixel_image
{ // TODO: this struct is only used internally in Sequencer, make it private
    /// Size in pixels for this image
    image_size size;

    /// RGBA buffer of the image to be rendered
    image::data rgba;
};

inline std::string setDynamicColorValue(
    rgb_color const& color) // TODO: yet another helper. maybe SemanticsUtils static class?
{
    auto const r = static_cast<unsigned>(static_cast<float>(color.red) / 255.0f * 0xFFFF);
    auto const g = static_cast<unsigned>(static_cast<float>(color.green) / 255.0f * 0xFFFF);
    auto const b = static_cast<unsigned>(static_cast<float>(color.blue) / 255.0f * 0xFFFF);
    return fmt::format("rgb:{:04X}/{:04X}/{:04X}", r, g, b);
}

enum class apply_result
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
class sequencer
{
  public:
    /// Constructs the sequencer stage.
    explicit sequencer(Terminal& terminal);

    // ParserEvents
    //
    void error(std::string_view errorString);
    void print(char32_t codepoint);
    size_t print(std::string_view chars, size_t cellCount);
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
    void startOSC();
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
    sequence _sequence {};
    sequence_parameter_builder _parameterBuilder;

    std::unique_ptr<ParserExtension> _hookedParser;
    std::unique_ptr<sixel_image_builder> _sixelImageBuilder;
};

// {{{ inlines
inline void sequencer::clear() noexcept
{
    _sequence.clearExceptParameters();
    _parameterBuilder.reset();
}

inline void sequencer::paramDigit(char ch) noexcept
{
    _parameterBuilder.multiplyBy10AndAdd(static_cast<uint8_t>(ch - '0'));
}

inline void sequencer::paramSeparator() noexcept
{
    _parameterBuilder.nextParameter();
}

inline void sequencer::paramSubSeparator() noexcept
{
    _parameterBuilder.nextSubParameter();
}
// }}}

} // namespace terminal

// {{{ fmt formatter
template <>
struct fmt::formatter<terminal::request_status_string>: formatter<std::string_view>
{
    auto format(terminal::request_status_string value, format_context& ctx) noexcept
        -> format_context::iterator
    {
        string_view name;
        switch (value)
        {
            case terminal::request_status_string::SGR: name = "SGR"; break;
            case terminal::request_status_string::DECSCL: name = "DECSCL"; break;
            case terminal::request_status_string::DECSCUSR: name = "DECSCUSR"; break;
            case terminal::request_status_string::DECSCA: name = "DECSCA"; break;
            case terminal::request_status_string::DECSTBM: name = "DECSTBM"; break;
            case terminal::request_status_string::DECSLRM: name = "DECSLRM"; break;
            case terminal::request_status_string::DECSLPP: name = "DECSLPP"; break;
            case terminal::request_status_string::DECSCPP: name = "DECSCPP"; break;
            case terminal::request_status_string::DECSNLS: name = "DECSNLS"; break;
            case terminal::request_status_string::DECSASD: name = "DECSASD"; break;
            case terminal::request_status_string::DECSSDT: name = "DECSSDT"; break;
        }
        return formatter<string_view>::format(name, ctx);
    }
};

template <>
struct fmt::formatter<terminal::sequence>
{
    template <typename ParseContext>
    constexpr auto parse(ParseContext& ctx)
    {
        return ctx.begin();
    }
    template <typename FormatContext>
    auto format(terminal::sequence const& seq, FormatContext& ctx)
    {
        return fmt::format_to(ctx.out(), "{}", seq.text());
    }
};
// }}}
