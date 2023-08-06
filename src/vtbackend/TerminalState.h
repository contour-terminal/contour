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

#include <fmt/format.h>

#include <atomic>
#include <bitset>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <stack>
#include <vector>

#include <libunicode/utf8.h>

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
    void set(ansi_mode mode, bool enabled) { _ansi.set(static_cast<size_t>(mode), enabled); }

    void set(dec_mode mode, bool enabled) { _dec.set(static_cast<size_t>(mode), enabled); }

    [[nodiscard]] bool enabled(ansi_mode mode) const noexcept
    {
        return _ansi.test(static_cast<size_t>(mode));
    }

    [[nodiscard]] bool enabled(dec_mode mode) const noexcept { return _dec.test(static_cast<size_t>(mode)); }

    void save(std::vector<dec_mode> const& modes)
    {
        for (dec_mode const mode: modes)
            _savedModes[mode].push_back(enabled(mode));
    }

    void restore(std::vector<dec_mode> const& modes)
    {
        for (dec_mode const mode: modes)
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
    std::bitset<32> _ansi;                             // AnsiMode
    std::bitset<8452 + 1> _dec;                        // DECMode
    std::map<dec_mode, std::vector<bool>> _savedModes; //!< saved DEC modes
};
// }}}

// {{{ Cursor
/// Terminal cursor data structure.
///
/// NB: Take care what to store here, as DECSC/DECRC will save/restore this struct.
struct Cursor
{
    cell_location position { line_offset(0), column_offset(0) };
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
    scroll_offset initialScrollOffset {};
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
    image_size cellPixelSize;

    ColorPalette defaultColorPalette;
    ColorPalette colorPalette;
    std::vector<ColorPalette> savedColorPalettes;
    size_t lastSavedColorPalette = 0;

    bool focused = true;

    VTType terminalId = VTType::VT525;

    Modes modes;
    std::map<dec_mode, std::vector<bool>> savedModes; //!< saved DEC modes

    unsigned maxImageColorRegisters = 256;
    image_size effectiveImageCanvasSize;
    std::shared_ptr<SixelColorPalette> imageColorPalette;
    ImagePool imagePool;

    std::vector<column_offset> tabs;

    screen_type screenType = screen_type::Primary;
    status_display_type statusDisplayType = status_display_type::None;
    bool syncWindowTitleWithHostWritableStatusDisplay = false;
    std::optional<status_display_type> savedStatusDisplayType = std::nullopt;
    active_status_display activeStatusDisplay = active_status_display::Main;

    Search searchMode;

    cursor_display cursorDisplay = cursor_display::Steady;
    cursor_shape cursorShape = cursor_shape::Block;

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
template <>
struct fmt::formatter<terminal::ansi_mode>: fmt::formatter<std::string>
{
    auto format(terminal::ansi_mode mode, format_context& ctx) -> format_context::iterator
    {
        return formatter<std::string>::format(to_string(mode), ctx);
    }
};

template <>
struct fmt::formatter<terminal::dec_mode>: fmt::formatter<std::string>
{
    auto format(terminal::dec_mode mode, format_context& ctx) -> format_context::iterator
    {
        return formatter<std::string>::format(to_string(mode), ctx);
    }
};

template <>
struct fmt::formatter<terminal::Cursor>: fmt::formatter<terminal::cell_location>
{
    auto format(const terminal::Cursor cursor, format_context& ctx) -> format_context::iterator
    {
        return formatter<terminal::cell_location>::format(cursor.position, ctx);
    }
};

template <>
struct fmt::formatter<terminal::dynamic_color_name>: formatter<std::string_view>
{
    template <typename FormatContext>
    auto format(terminal::dynamic_color_name value, FormatContext& ctx)
    {
        using terminal::dynamic_color_name;
        string_view name;
        switch (value)
        {
            case dynamic_color_name::DefaultForegroundColor: name = "DefaultForegroundColor"; break;
            case dynamic_color_name::DefaultBackgroundColor: name = "DefaultBackgroundColor"; break;
            case dynamic_color_name::TextCursorColor: name = "TextCursorColor"; break;
            case dynamic_color_name::MouseForegroundColor: name = "MouseForegroundColor"; break;
            case dynamic_color_name::MouseBackgroundColor: name = "MouseBackgroundColor"; break;
            case dynamic_color_name::HighlightForegroundColor: name = "HighlightForegroundColor"; break;
            case dynamic_color_name::HighlightBackgroundColor: name = "HighlightBackgroundColor"; break;
        }
        return formatter<string_view>::format(name, ctx);
    }
};

template <>
struct fmt::formatter<terminal::ExecutionMode>: formatter<std::string_view>
{
    auto format(terminal::ExecutionMode value, format_context& ctx) -> format_context::iterator
    {
        string_view name;
        switch (value)
        {
            case terminal::ExecutionMode::Normal: name = "NORMAL"; break;
            case terminal::ExecutionMode::Waiting: name = "WAITING"; break;
            case terminal::ExecutionMode::SingleStep: name = "SINGLE STEP"; break;
            case terminal::ExecutionMode::BreakAtEmptyQueue: name = "BREAK AT EMPTY"; break;
        }
        return formatter<string_view>::format(name, ctx);
    }
};

// }}}
