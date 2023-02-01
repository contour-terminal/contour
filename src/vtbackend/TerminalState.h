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

#include <vtbackend/Charset.h>
#include <vtbackend/ColorPalette.h>
#include <vtbackend/GraphicsAttributes.h>
#include <vtbackend/Grid.h>
#include <vtbackend/Hyperlink.h>
#include <vtbackend/InputGenerator.h>
#include <vtbackend/InputHandler.h>
#include <vtbackend/ScreenEvents.h> // ScreenType
#include <vtbackend/Sequencer.h>
#include <vtbackend/Settings.h>
#include <vtbackend/ViCommands.h>
#include <vtbackend/ViInputHandler.h>
#include <vtbackend/cell/CellConfig.h>
#include <vtbackend/primitives.h>

#include <vtparser/Parser.h>

#include <unicode/utf8.h>

#include <fmt/format.h>

#include <atomic>
#include <bitset>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <stack>
#include <vector>

namespace terminal
{

class Terminal;

// {{{ Modes
/// API for setting/querying terminal modes.
///
/// This abstracts away the actual implementation for more intuitive use and easier future adaptability.
class Modes
{
  public:
    void set(AnsiMode mode, bool enabled) { _ansi.set(static_cast<size_t>(mode), enabled); }

    void set(DECMode mode, bool enabled) { _dec.set(static_cast<size_t>(mode), enabled); }

    [[nodiscard]] bool enabled(AnsiMode mode) const noexcept { return _ansi.test(static_cast<size_t>(mode)); }

    [[nodiscard]] bool enabled(DECMode mode) const noexcept { return _dec.test(static_cast<size_t>(mode)); }

    void save(std::vector<DECMode> const& modes)
    {
        for (DECMode const mode: modes)
            _savedModes[mode].push_back(enabled(mode));
    }

    void restore(std::vector<DECMode> const& modes)
    {
        for (DECMode const mode: modes)
        {
            if (auto i = _savedModes.find(mode); i != _savedModes.end() && !i->second.empty())
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
    std::bitset<32> _ansi;                            // AnsiMode
    std::bitset<8452 + 1> _dec;                       // DECMode
    std::map<DECMode, std::vector<bool>> _savedModes; //!< saved DEC modes
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
    bool wrapPending = false;
    GraphicsAttributes graphicsRendition {};
    CharsetMapping charsets {};
    HyperlinkId hyperlink {};
    // TODO: selective erase attribute
    // TODO: SS2/SS3 states
    // TODO: CharacterSet for GL and GR
};
// }}}

struct Search
{
    std::u32string pattern;
    ScrollOffset initialScrollOffset {};
    bool initiatedByDoubleClick = false;
};

// Mandates what execution mode the terminal will take to process VT sequences.
//
enum class ExecutionMode
{
    // Normal execution mode, with no tracing enabled.
    Normal,

    // Trace mode is enabled and waiting for command to continue execution.
    Waiting,

    // Tracing mode is enabled and execution is stopped after each VT sequence.
    SingleStep,

    // Tracing mode is enabled, execution is stopped after queue of pending VT sequences is empty.
    BreakAtEmptyQueue,

    // Trace mode is enabled and execution is stopped at frame marker.
    // TODO: BreakAtFrame,
};

enum class WrapPending
{
    Yes,
    No,
};

/**
 * Defines the state of a terminal.
 * All those data members used to live in Screen, but are moved
 * out with the goal to move all shared state up to Terminal later
 * and have Screen API maintain only *one* screen.
 *
 * TODO: Let's move all shared data into one place,
 * ultimatively ending up in Terminal (or keep TerminalState).
 */
struct TerminalState
{
    explicit TerminalState(Terminal& terminal);

    Settings& settings;

    std::atomic<ExecutionMode> executionMode = ExecutionMode::Normal;
    std::mutex breakMutex;
    std::condition_variable breakCondition;

    /// contains the pixel size of a single cell, or area(cellPixelSize_) == 0 if unknown.
    ImageSize cellPixelSize;

    ColorPalette defaultColorPalette;
    ColorPalette colorPalette;
    std::vector<ColorPalette> savedColorPalettes;
    size_t lastSavedColorPalette = 0;

    bool focused = true;

    VTType terminalId = VTType::VT525;

    Modes modes;
    std::map<DECMode, std::vector<bool>> savedModes; //!< saved DEC modes

    unsigned maxImageColorRegisters = 256;
    ImageSize effectiveImageCanvasSize;
    std::shared_ptr<SixelColorPalette> imageColorPalette;
    ImagePool imagePool;

    std::vector<ColumnOffset> tabs;

    ScreenType screenType = ScreenType::Primary;
    StatusDisplayType statusDisplayType = StatusDisplayType::None;
    std::optional<StatusDisplayType> savedStatusDisplayType = std::nullopt;
    ActiveStatusDisplay activeStatusDisplay = ActiveStatusDisplay::Main;

    Search searchMode;

    CursorDisplay cursorDisplay = CursorDisplay::Steady;
    CursorShape cursorShape = CursorShape::Block;

    std::string currentWorkingDirectory = {};

    unsigned maxImageRegisterCount = 256;
    bool usePrivateColorRegisters = false;

    bool usingStdoutFastPipe = false;

    // Hyperlink related
    //
    HyperlinkStorage hyperlinks {};

    std::string windowTitle {};
    std::stack<std::string> savedWindowTitles {};

    Sequencer sequencer;
    parser::Parser<Sequencer, false> parser;
    uint64_t instructionCounter = 0;

    InputGenerator inputGenerator {};

    ViCommands viCommands;
    ViInputHandler inputHandler;
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
    auto format(terminal::AnsiMode mode, FormatContext& ctx)
    {
        return fmt::format_to(ctx.out(), "{}", to_string(mode));
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
    auto format(terminal::DECMode mode, FormatContext& ctx)
    {
        return fmt::format_to(ctx.out(), "{}", to_string(mode));
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
        return fmt::format_to(ctx.out(), "{}", cursor.position);
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
        case DynamicColorName::DefaultForegroundColor: return fmt::format_to(ctx.out(), "DefaultForegroundColor");
        case DynamicColorName::DefaultBackgroundColor: return fmt::format_to(ctx.out(), "DefaultBackgroundColor");
        case DynamicColorName::TextCursorColor: return fmt::format_to(ctx.out(), "TextCursorColor");
        case DynamicColorName::MouseForegroundColor: return fmt::format_to(ctx.out(), "MouseForegroundColor");
        case DynamicColorName::MouseBackgroundColor: return fmt::format_to(ctx.out(), "MouseBackgroundColor");
        case DynamicColorName::HighlightForegroundColor: return fmt::format_to(ctx.out(), "HighlightForegroundColor");
        case DynamicColorName::HighlightBackgroundColor: return fmt::format_to(ctx.out(), "HighlightBackgroundColor");
        }
        return fmt::format_to(ctx.out(), "({})", static_cast<unsigned>(name));
        // clang-format on
    }
};

template <>
struct formatter<terminal::ExecutionMode>
{
    template <typename ParseContext>
    constexpr auto parse(ParseContext& ctx)
    {
        return ctx.begin();
    }
    template <typename FormatContext>
    auto format(terminal::ExecutionMode value, FormatContext& ctx)
    {
        // clang-format off
        switch (value)
        {
            case terminal::ExecutionMode::Normal: return fmt::format_to(ctx.out(), "NORMAL");
            case terminal::ExecutionMode::Waiting: return fmt::format_to(ctx.out(), "WAITING");
            case terminal::ExecutionMode::SingleStep: return fmt::format_to(ctx.out(), "SINGLE STEP");
            case terminal::ExecutionMode::BreakAtEmptyQueue: return fmt::format_to(ctx.out(), "BREAK AT EMPTY");
        }
        // clang-format on
        return fmt::format_to(ctx.out(), "UNKNOWN");
    }
};

} // namespace fmt
// }}}
