// SPDX-License-Identifier: Apache-2.0
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
#include <vtbackend/logging.h>
#include <vtbackend/primitives.h>

#include <vtparser/Parser.h>

#include <fmt/format.h>

#include <atomic>
#include <bitset>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <stack>
#include <vector>

#include <libunicode/utf8.h>

namespace vtbackend
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

    bool set(DECMode mode, bool enabled)
    {
        if (_decFrozen[static_cast<size_t>(mode)])
        {
            errorLog()("Attempt to change frozen DEC mode {}. Ignoring.", mode);
            return false;
        }
        _dec.set(static_cast<size_t>(mode), enabled);
        return true;
    }

    [[nodiscard]] bool enabled(AnsiMode mode) const noexcept { return _ansi[static_cast<size_t>(mode)]; }
    [[nodiscard]] bool enabled(DECMode mode) const noexcept { return _dec[static_cast<size_t>(mode)]; }

    [[nodiscard]] bool frozen(DECMode mode) const noexcept
    {
        assert(isValidDECMode(static_cast<unsigned int>(mode)));
        return _decFrozen.test(static_cast<size_t>(mode));
    }

    void freeze(DECMode mode)
    {
        assert(isValidDECMode(static_cast<unsigned int>(mode)));

        if (mode == DECMode::BatchedRendering)
        {
            errorLog()("Attempt to freeze batched rendering. Ignoring.");
            return;
        }

        _decFrozen.set(static_cast<size_t>(mode), true);
        terminalLog()("Freezing {} DEC mode to permanently-{}.", mode, enabled(mode) ? "set" : "reset");
    }

    void unfreeze(DECMode mode)
    {
        assert(isValidDECMode(static_cast<unsigned int>(mode)));
        _decFrozen.set(static_cast<size_t>(mode), false);
        terminalLog()("Unfreezing permanently-{} DEC mode {}.", mode, enabled(mode));
    }

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
    std::bitset<32> _ansi;                            // AnsiMode
    std::bitset<8452 + 1> _dec;                       // DECMode
    std::bitset<8452 + 1> _decFrozen;                 // DECMode
    std::map<DECMode, std::vector<bool>> _savedModes; // saved DEC modes
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
    bool syncWindowTitleWithHostWritableStatusDisplay = false;
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
    vtparser::Parser<Sequencer, false> parser;
    uint64_t instructionCounter = 0;

    InputGenerator inputGenerator {};

    ViCommands viCommands;
    ViInputHandler inputHandler;
};

} // namespace vtbackend

// {{{ fmt formatters
template <>
struct fmt::formatter<vtbackend::AnsiMode>: fmt::formatter<std::string>
{
    auto format(vtbackend::AnsiMode mode, format_context& ctx) -> format_context::iterator
    {
        return formatter<std::string>::format(to_string(mode), ctx);
    }
};

template <>
struct fmt::formatter<vtbackend::DECMode>: fmt::formatter<std::string>
{
    auto format(vtbackend::DECMode mode, format_context& ctx) -> format_context::iterator
    {
        return formatter<std::string>::format(to_string(mode), ctx);
    }
};

template <>
struct fmt::formatter<vtbackend::Cursor>: fmt::formatter<vtbackend::CellLocation>
{
    auto format(const vtbackend::Cursor cursor, format_context& ctx) -> format_context::iterator
    {
        return formatter<vtbackend::CellLocation>::format(cursor.position, ctx);
    }
};

template <>
struct fmt::formatter<vtbackend::DynamicColorName>: formatter<std::string_view>
{
    template <typename FormatContext>
    auto format(vtbackend::DynamicColorName value, FormatContext& ctx)
    {
        using vtbackend::DynamicColorName;
        string_view name;
        switch (value)
        {
            case DynamicColorName::DefaultForegroundColor: name = "DefaultForegroundColor"; break;
            case DynamicColorName::DefaultBackgroundColor: name = "DefaultBackgroundColor"; break;
            case DynamicColorName::TextCursorColor: name = "TextCursorColor"; break;
            case DynamicColorName::MouseForegroundColor: name = "MouseForegroundColor"; break;
            case DynamicColorName::MouseBackgroundColor: name = "MouseBackgroundColor"; break;
            case DynamicColorName::HighlightForegroundColor: name = "HighlightForegroundColor"; break;
            case DynamicColorName::HighlightBackgroundColor: name = "HighlightBackgroundColor"; break;
        }
        return formatter<string_view>::format(name, ctx);
    }
};

template <>
struct fmt::formatter<vtbackend::ExecutionMode>: formatter<std::string_view>
{
    auto format(vtbackend::ExecutionMode value, format_context& ctx) -> format_context::iterator
    {
        string_view name;
        switch (value)
        {
            case vtbackend::ExecutionMode::Normal: name = "NORMAL"; break;
            case vtbackend::ExecutionMode::Waiting: name = "WAITING"; break;
            case vtbackend::ExecutionMode::SingleStep: name = "SINGLE STEP"; break;
            case vtbackend::ExecutionMode::BreakAtEmptyQueue: name = "BREAK AT EMPTY"; break;
        }
        return formatter<string_view>::format(name, ctx);
    }
};

// }}}
