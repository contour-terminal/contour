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
#include <terminal/ControlCode.h>
#include <terminal/InputGenerator.h>
#include <terminal/Screen.h>
#include <terminal/Terminal.h>
#include <terminal/VTType.h>
#include <terminal/VTWriter.h>
#include <terminal/logging.h>

#include <crispy/App.h>
#include <crispy/Comparison.h>
#include <crispy/algorithm.h>
#include <crispy/base64.h>
#include <crispy/escape.h>
#include <crispy/size.h>
#include <crispy/times.h>
#include <crispy/utils.h>

#include <unicode/convert.h>
#include <unicode/emoji_segmenter.h>
#include <unicode/grapheme_segmenter.h>
#include <unicode/word_segmenter.h>

#include <range/v3/view.hpp>

#include <algorithm>
#include <cassert>
#include <iostream>
#include <iterator>
#include <sstream>
#include <string_view>
#include <tuple>
#include <variant>

using namespace std::string_view_literals;

using crispy::escape;
using crispy::for_each;
using crispy::times;
using crispy::toHexString;

using gsl::span;

using std::accumulate;
using std::array;
using std::clamp;
using std::distance;
using std::endl;
using std::fill;
using std::function;
using std::get;
using std::holds_alternative;
using std::make_shared;
using std::make_unique;
using std::max;
using std::min;
using std::monostate;
using std::move;
using std::next;
using std::nullopt;
using std::optional;
using std::ostringstream;
using std::pair;
using std::prev;
using std::ref;
using std::rotate;
using std::shared_ptr;
using std::string;
using std::string_view;
using std::tuple;
using std::unique_ptr;
using std::vector;

namespace terminal
{

auto constexpr inline TabWidth = ColumnCount(8);

auto const inline VTCaptureBufferLog = logstore::Category("vt.ext.capturebuffer",
                                                          "Capture Buffer debug logging.",
                                                          logstore::Category::State::Disabled,
                                                          logstore::Category::Visibility::Hidden);

namespace // {{{ helper
{
    std::string vtSequenceParameterString(GraphicsAttributes const& _sgr)
    {
        std::string output;

        auto const sgrSep = [&]() {
            if (!output.empty())
                output += ';';
        };
        auto const sgrAdd = [&](unsigned _value) {
            sgrSep();
            output += std::to_string(_value);
        };
        auto const sgrAddStr = [&](string_view _value) {
            sgrSep();
            output += _value;
        };
        auto const sgrAddSub = [&](unsigned _value) {
            output += std::to_string(_value);
        };

        if (isIndexedColor(_sgr.foregroundColor))
        {
            auto const colorValue = getIndexedColor(_sgr.foregroundColor);
            if (static_cast<unsigned>(colorValue) < 8)
                sgrAdd(30 + static_cast<unsigned>(colorValue));
            else
            {
                sgrAdd(38);
                sgrAddSub(5);
                sgrAddSub(static_cast<unsigned>(colorValue));
            }
        }
        else if (isDefaultColor(_sgr.foregroundColor))
            sgrAdd(39);
        else if (isBrightColor(_sgr.foregroundColor))
            sgrAdd(90 + static_cast<unsigned>(getBrightColor(_sgr.foregroundColor)));
        else if (isRGBColor(_sgr.foregroundColor))
        {
            auto const rgb = getRGBColor(_sgr.foregroundColor);
            sgrAdd(38);
            sgrAddSub(2);
            sgrAddSub(static_cast<unsigned>(rgb.red));
            sgrAddSub(static_cast<unsigned>(rgb.green));
            sgrAddSub(static_cast<unsigned>(rgb.blue));
        }

        if (isIndexedColor(_sgr.backgroundColor))
        {
            auto const colorValue = getIndexedColor(_sgr.backgroundColor);
            if (static_cast<unsigned>(colorValue) < 8)
                sgrAdd(40 + static_cast<unsigned>(colorValue));
            else
            {
                sgrAdd(48);
                sgrAddSub(5);
                sgrAddSub(static_cast<unsigned>(colorValue));
            }
        }
        else if (isDefaultColor(_sgr.backgroundColor))
            sgrAdd(49);
        else if (isBrightColor(_sgr.backgroundColor))
            sgrAdd(100 + getBrightColor(_sgr.backgroundColor));
        else if (isRGBColor(_sgr.backgroundColor))
        {
            auto const& rgb = getRGBColor(_sgr.backgroundColor);
            sgrAdd(48);
            sgrAddSub(2);
            sgrAddSub(static_cast<unsigned>(rgb.red));
            sgrAddSub(static_cast<unsigned>(rgb.green));
            sgrAddSub(static_cast<unsigned>(rgb.blue));
        }

        if (isRGBColor(_sgr.underlineColor))
        {
            auto const& rgb = getRGBColor(_sgr.underlineColor);
            sgrAdd(58);
            sgrAddSub(2);
            sgrAddSub(static_cast<unsigned>(rgb.red));
            sgrAddSub(static_cast<unsigned>(rgb.green));
            sgrAddSub(static_cast<unsigned>(rgb.blue));
        }

        // TODO: _sgr.styles;
        auto constexpr masks = array {
            pair { CellFlags::Bold, "1"sv },
            pair { CellFlags::Faint, "2"sv },
            pair { CellFlags::Italic, "3"sv },
            pair { CellFlags::Underline, "4"sv },
            pair { CellFlags::Blinking, "5"sv },
            pair { CellFlags::Inverse, "7"sv },
            pair { CellFlags::Hidden, "8"sv },
            pair { CellFlags::CrossedOut, "9"sv },
            pair { CellFlags::DoublyUnderlined, "4:2"sv },
            pair { CellFlags::CurlyUnderlined, "4:3"sv },
            pair { CellFlags::DottedUnderline, "4:4"sv },
            pair { CellFlags::DashedUnderline, "4:5"sv },
            pair { CellFlags::Framed, "51"sv },
            // TODO(impl or completely remove): pair{CellFlags::Encircled, ""sv},
            pair { CellFlags::Overline, "53"sv },
        };

        for (auto const& mask: masks)
            if (_sgr.styles & mask.first)
                sgrAddStr(mask.second);

        return output;
    }

    template <typename T, typename U>
    std::optional<crispy::boxed<T, U>> decr(std::optional<crispy::boxed<T, U>> v)
    {
        if (v.has_value())
            --*v;
        return v;
    }

    // optional<CharsetTable> getCharsetTableForCode(std::string const& _intermediate)
    // {
    //     if (_intermediate.size() != 1)
    //         return nullopt;
    //
    //     char const code = _intermediate[0];
    //     switch (code)
    //     {
    //         case '(':
    //             return {CharsetTable::G0};
    //         case ')':
    //         case '-':
    //             return {CharsetTable::G1};
    //         case '*':
    //         case '.':
    //             return {CharsetTable::G2};
    //         case '+':
    //         case '/':
    //             return {CharsetTable::G3};
    //         default:
    //             return nullopt;
    //     }
    // }
} // namespace
// }}}

template <typename Cell, ScreenType TheScreenType>
Screen<Cell, TheScreenType>::Screen(TerminalState& terminalState, ScreenType screenType, Grid<Cell>& grid):
    _terminal { terminalState.terminal }, _state { terminalState }, _screenType { screenType }, _grid { grid }
{
    updateCursorIterator();
}

template <typename Cell, ScreenType TheScreenType>
unsigned Screen<Cell, TheScreenType>::numericCapability(capabilities::Code _cap) const
{
    using namespace capabilities::literals;

    switch (_cap)
    {
        case "li"_tcap: return unbox<unsigned>(_state.pageSize.lines);
        case "co"_tcap: return unbox<unsigned>(_state.pageSize.columns);
        case "it"_tcap: return unbox<unsigned>(TabWidth);
        default: return StaticDatabase::numericCapability(_cap);
    }
}

template <typename Cell, ScreenType TheScreenType>
void Screen<Cell, TheScreenType>::verifyState() const
{
    grid().verifyState();
}

template <typename Cell, ScreenType TheScreenType>
void Screen<Cell, TheScreenType>::fail(std::string const& _message) const
{
    inspect(_message, std::cerr);
    abort();
}

template <typename Cell, ScreenType TheScreenType>
string_view Screen<Cell, TheScreenType>::tryEmplaceChars(string_view _chars, size_t cellCount) noexcept
{
    if (!_terminal.isFullHorizontalMargins())
        return _chars;

    // In case the charset has been altered, no
    // optimization can be applied.
    // Unless we're storing the charset in the TriviallyStyledLineBuffer, too.
    // But for now that's too rare to be beneficial.
    if (!_state.cursor.charsets.isSelected(CharsetId::USASCII))
        return _chars;

    crlfIfWrapPending();

    auto const columnsAvailable = pageSize().columns.value - _state.cursor.position.column.value;

#if defined(LIBTERMINAL_SCAN_UNICODE)
    assert(cellCount <= static_cast<size_t>(columnsAvailable));
#endif

    if (!_terminal.isModeEnabled(DECMode::AutoWrap) && cellCount > static_cast<size_t>(columnsAvailable))
        // With AutoWrap on, we can only emplace if it fits the line.
        return _chars;

    if (_state.cursor.position.column.value == 0)
    {
        if (currentLine().empty())
        {
            _chars.remove_prefix(emplaceCharsIntoCurrentLine(_chars, cellCount));
            _chars = tryEmplaceContinuousChars(_chars, cellCount);
            _terminal.currentPtyBuffer()->advanceHotEndUntil(_chars.data());
            return _chars;
        }
        return _chars;
    }

    if (canResumeEmplace(_chars))
    {
        auto const charsToWrite = static_cast<size_t>(min(columnsAvailable, static_cast<int>(_chars.size())));
        auto& lineBuffer = currentLine().trivialBuffer();
        lineBuffer.text.growBy(charsToWrite);
        lineBuffer.usedColumns += ColumnCount::cast_from(cellCount);
        advanceCursorAfterWrite(ColumnCount::cast_from(charsToWrite));
        _chars.remove_prefix(charsToWrite);
        _chars = tryEmplaceContinuousChars(_chars, cellCount);
        _terminal.currentPtyBuffer()->advanceHotEndUntil(_chars.data());
        return _chars;
    }

    return _chars;
}

template <typename Cell, ScreenType TheScreenType>
string_view Screen<Cell, TheScreenType>::tryEmplaceContinuousChars(string_view _chars,
                                                                   size_t cellCount) noexcept
{
    while (!_chars.empty())
    {
        crlf();
        if (!currentLine().empty())
            break;

        _chars.remove_prefix(emplaceCharsIntoCurrentLine(_chars, cellCount));
    }

    return _chars;
}

template <typename Cell, ScreenType TheScreenType>
size_t Screen<Cell, TheScreenType>::emplaceCharsIntoCurrentLine(string_view _chars, size_t cellCount) noexcept
{
    auto columnsAvailable = (_state.margin.horizontal.to.value + 1) - _state.cursor.position.column.value;
    assert(cellCount <= static_cast<size_t>(columnsAvailable));
    auto const charsToWrite = static_cast<size_t>(min(columnsAvailable, static_cast<int>(_chars.size())));

    Line<Cell>& line = currentLine();
    if (!line.isInflatedBuffer() && line.empty())
    {
        // Only use fastpath if the currently line hasn't been inflated already.
        // Because we might lose prior-written textual/SGR information otherwise.
        line.reset(_state.cursor.graphicsRendition,
                   _state.cursor.hyperlink,
                   crispy::BufferFragment {
                       _terminal.currentPtyBuffer(),
                       _chars.substr(0, charsToWrite),
                   },
                   ColumnCount::cast_from(cellCount));
        advanceCursorAfterWrite(ColumnCount::cast_from(cellCount));
    }
    else
    {
        // Transforming _chars input from UTF-8 to UTF-32 even though right now it should only
        // be containing US-ASCII, but soon it'll be any arbitrary textual Unicode codepoints.
        auto utf8DecoderState = unicode::utf8_decoder_state {};
        for (char const ch: _chars)
        {
            auto const result = unicode::from_utf8(utf8DecoderState, static_cast<uint8_t>(ch));
            if (holds_alternative<unicode::Success>(result))
                writeText(get<unicode::Success>(result).value);
            else if (holds_alternative<unicode::Invalid>(result))
                writeText(U'\uFFFE'); // U+FFFE (Not a Character)
        }
    }
    // fmt::print("emplaceCharsIntoCurrentLine ({} cols, {} bytes): \"{}\"\n", cellCount, _chars.size(),
    // _chars);
    return charsToWrite;
}

template <typename Cell, ScreenType TheScreenType>
void Screen<Cell, TheScreenType>::advanceCursorAfterWrite(ColumnCount n) noexcept
{
    assert(_state.cursor.position.column.value + n.value <= _state.margin.horizontal.to.value + 1);
    //  + 1 here because `to` is inclusive.

    if (_state.cursor.position.column.value + n.value < _state.pageSize.columns.value)
        _state.cursor.position.column.value += n.value;
    else
    {
        _state.cursor.position.column.value += n.value - 1;
        _state.wrapPending = true;
    }
}

template <typename Cell, ScreenType TheScreenType>
bool Screen<Cell, TheScreenType>::canResumeEmplace(std::string_view continuationChars) const noexcept
{
    auto& line = currentLine();
    if (!line.isTrivialBuffer())
        return false;
    TriviallyStyledLineBuffer const& buffer = line.trivialBuffer();
    return buffer.text.view().end() == continuationChars.begin()
           && buffer.attributes == _state.cursor.graphicsRendition
           && buffer.hyperlink == _state.cursor.hyperlink
           && buffer.text.owner() == _terminal.currentPtyBuffer();
}

template <typename Cell, ScreenType TheScreenType>
void Screen<Cell, TheScreenType>::writeText(string_view _chars, size_t cellCount)
{
#if defined(LIBTERMINAL_LOG_TRACE)
    if (VTTraceSequenceLog)
        VTTraceSequenceLog()("text({} bytes): \"{}\"", _chars.size(), _chars);
#endif

#if defined(LIBTERMINAL_PTY_BUFFER_OBJECTS)
    _chars = tryEmplaceChars(_chars, cellCount);
    if (_chars.empty())
        return;
#endif

    for (char const ch: _chars)
        writeText(static_cast<char32_t>(ch));
}

template <typename Cell, ScreenType TheScreenType>
void Screen<Cell, TheScreenType>::crlfIfWrapPending()
{
    if (_state.wrapPending && _state.cursor.autoWrap) // && !_terminal.isModeEnabled(DECMode::TextReflow))
    {
        bool const lineWrappable = currentLine().wrappable();
        crlf();
        if (lineWrappable)
            currentLine().setFlag(LineFlags::Wrappable | LineFlags::Wrapped, true);
    }
}

template <typename Cell, ScreenType TheScreenType>
void Screen<Cell, TheScreenType>::writeText(char32_t _char)
{
#if defined(LIBTERMINAL_LOG_TRACE)
    if (VTTraceSequenceLog)
        VTTraceSequenceLog()("char: \'{}\'", unicode::convert_to<char>(_char));
#endif

    crlfIfWrapPending();

    char32_t const codepoint = _state.cursor.charsets.map(_char);

    auto const lastChar = precedingGraphicCharacter();
    auto const isAsciiBreakable = lastChar < 128 && codepoint < 128;
    // NB: This is an optimization for US-ASCII text versus grapheme cluster segmentation.

    if (isAsciiBreakable || !lastChar || unicode::grapheme_segmenter::breakable(lastChar, codepoint))
    {
        writeCharToCurrentAndAdvance(codepoint);
    }
    else
    {
        auto const extendedWidth = usePreviousCell().appendCharacter(codepoint);
        if (extendedWidth > 0)
            clearAndAdvance(extendedWidth);
        _terminal.markCellDirty(_state.lastCursorPosition);
    }

    resetInstructionCounter();
}

template <typename Cell, ScreenType TheScreenType>
void Screen<Cell, TheScreenType>::writeCharToCurrentAndAdvance(char32_t _character) noexcept
{
    Line<Cell>& line = currentLine();

    Cell& cell = line.useCellAt(_state.cursor.position.column);

#if defined(LINE_AVOID_CELL_RESET)
    bool const consecutiveTextWrite = _state.sequencer.instructionCounter() == 1;
    if (!consecutiveTextWrite)
        cell.reset();
#endif

    cell.write(_state.cursor.graphicsRendition,
               _character,
               (uint8_t) unicode::width(_character),
               _state.cursor.hyperlink);

    _state.lastCursorPosition = _state.cursor.position;

#if 1
    clearAndAdvance(cell.width());
#else
    bool const cursorInsideMargin =
        _terminal.isModeEnabled(DECMode::LeftRightMargin) && _terminal.isCursorInsideMargins();
    auto const cellsAvailable = cursorInsideMargin
                                    ? *(_state.margin.horizontal.to - _state.cursor.position.column) - 1
                                    : *_state.pageSize.columns - *_state.cursor.position.column - 1;

    auto const n = min(cell.width(), cellsAvailable);

    if (n == cell.width())
    {
        assert(n > 0);
        _state.cursor.position.column++;
        for (int i = 1; i < n; ++i)
        {
            currentCell().reset(_state.cursor.graphicsRendition, _state.cursor.hyperlink);
            _state.cursor.position.column++;
        }
    }
    else if (_state.cursor.autoWrap)
        _state.wrapPending = true;
#endif

    // TODO: maybe move selector API up? So we can make this call conditional,
    //       and only call it when something is selected?
    //       Alternatively we could add a boolean to make this callback
    //       conditional, something like: setReportDamage(bool);
    //       The latter is probably the easiest.
    _terminal.markCellDirty(_state.cursor.position);
}

template <typename Cell, ScreenType TheScreenType>
void Screen<Cell, TheScreenType>::clearAndAdvance(int _graphemeClusterWidth) noexcept
{
    if (_graphemeClusterWidth == 0)
        return;

    bool const cursorInsideMargin =
        _terminal.isModeEnabled(DECMode::LeftRightMargin) && _terminal.isCursorInsideMargins();
    auto const cellsAvailable = cursorInsideMargin
                                    ? *(_state.margin.horizontal.to - _state.cursor.position.column) - 1
                                    : *_state.pageSize.columns - *_state.cursor.position.column - 1;
    auto const n = min(_graphemeClusterWidth, cellsAvailable);

    if (n == _graphemeClusterWidth)
    {
        _state.cursor.position.column++;
        auto& line = currentLine();
        for (int i = 1; i < n; ++i) // XXX It's not even clear if other TEs are doing that, too.
        {
            line.useCellAt(_state.cursor.position.column)
                .reset(_state.cursor.graphicsRendition, _state.cursor.hyperlink);
            _state.cursor.position.column++;
        }
    }
    else if (_state.cursor.autoWrap)
    {
        _state.wrapPending = true;
    }
}

template <typename Cell, ScreenType TheScreenType>
std::string Screen<Cell, TheScreenType>::screenshot(function<string(LineOffset)> const& _postLine) const
{
    auto result = std::stringstream {};
    auto writer = VTWriter(result);

    for (int const line: ranges::views::iota(0, *_state.pageSize.lines))
    {
        writer.write(grid().lineAt(LineOffset(line)));
        if (_postLine)
            writer.write(_postLine(LineOffset(line)));
        writer.crlf();
    }

    return result.str();
}

template <typename Cell, ScreenType TheScreenType>
optional<LineOffset> Screen<Cell, TheScreenType>::findMarkerUpwards(LineOffset _startLine) const
{
    // XXX _startLine is an absolute history line coordinate
    if (_state.screenType != ScreenType::Primary)
        return nullopt;
    if (*_startLine <= -*historyLineCount())
        return nullopt;

    _startLine = min(_startLine, boxed_cast<LineOffset>(_state.pageSize.lines - 1));

    for (LineOffset i = _startLine - 1; i >= -boxed_cast<LineOffset>(historyLineCount()); --i)
        if (grid().lineAt(i).marked())
            return { i };

    return nullopt;
}

template <typename Cell, ScreenType TheScreenType>
optional<LineOffset> Screen<Cell, TheScreenType>::findMarkerDownwards(LineOffset _lineOffset) const
{
    if (_state.screenType != ScreenType::Primary)
        return nullopt;

    auto const top = std::clamp(_lineOffset,
                                -boxed_cast<LineOffset>(historyLineCount()),
                                +boxed_cast<LineOffset>(_state.pageSize.lines) - 1);

    auto const bottom = LineOffset(0);

    for (LineOffset i = top + 1; i <= bottom; ++i)
        if (grid().lineAt(i).marked())
            return { i };

    return nullopt;
}

// {{{ tabs related
template <typename Cell, ScreenType TheScreenType>
void Screen<Cell, TheScreenType>::clearAllTabs()
{
    _state.tabs.clear();
}

template <typename Cell, ScreenType TheScreenType>
void Screen<Cell, TheScreenType>::clearTabUnderCursor()
{
    // populate tabs vector in case of default tab width is used (until now).
    if (_state.tabs.empty() && *TabWidth != 0)
        for (auto column = boxed_cast<ColumnOffset>(TabWidth);
             column < boxed_cast<ColumnOffset>(_state.pageSize.columns);
             column += boxed_cast<ColumnOffset>(TabWidth))
            _state.tabs.emplace_back(column - 1);

    // erase the specific tab underneath
    for (auto i = begin(_state.tabs); i != end(_state.tabs); ++i)
    {
        if (*i == realCursorPosition().column)
        {
            _state.tabs.erase(i);
            break;
        }
    }
}

template <typename Cell, ScreenType TheScreenType>
void Screen<Cell, TheScreenType>::setTabUnderCursor()
{
    _state.tabs.emplace_back(realCursorPosition().column);
    sort(begin(_state.tabs), end(_state.tabs));
}
// }}}

// {{{ others
template <typename Cell, ScreenType TheScreenType>
void Screen<Cell, TheScreenType>::moveCursorTo(LineOffset _line, ColumnOffset _column)
{
    auto const [line, column] = [&]() {
        if (!_state.cursor.originMode)
            return pair { _line, _column };
        else
            return pair { _line + _state.margin.vertical.from, _column + _state.margin.horizontal.from };
    }();

    _state.wrapPending = false;
    _state.cursor.position = clampToScreen({ line, column });
    updateCursorIterator();
}

template <typename Cell, ScreenType TheScreenType>
void Screen<Cell, TheScreenType>::linefeed(ColumnOffset _newColumn)
{
    _state.wrapPending = false;
    _state.cursor.position.column = _newColumn;

    if (*realCursorPosition().line == *_state.margin.vertical.to)
    {
        // TODO(perf) if we know that we text is following this LF
        // (i.e. parser state will be ground state),
        // then invoke scrollUpUninitialized instead
        // and make sure the subsequent text write will
        // possibly also reset remaining grid cells in that line
        // if the incoming text did not write to the full line
        scrollUp(LineCount(1), {}, _state.margin);
    }
    else
    {
        // using moveCursorTo() would embrace code reusage,
        // but due to the fact that it's fully recalculating iterators,
        // it may be faster to just incrementally update them.
        // moveCursorTo({logicalCursorPosition().line + 1, _state.margin.horizontal.from});
        _state.cursor.position.line++;
        updateCursorIterator();
    }
}

template <typename Cell, ScreenType TheScreenType>
void Screen<Cell, TheScreenType>::scrollUp(LineCount n, GraphicsAttributes sgr, Margin margin)
{
    auto const scrollCount = grid().scrollUp(n, sgr, margin);
    updateCursorIterator();
    // TODO only call onBufferScrolled if full page margin
    _terminal.onBufferScrolled(scrollCount);
}

template <typename Cell, ScreenType TheScreenType>
void Screen<Cell, TheScreenType>::scrollDown(LineCount _n, Margin _margin)
{
    grid().scrollDown(_n, cursor().graphicsRendition, _margin);
    updateCursorIterator();
}

template <typename Cell, ScreenType TheScreenType>
void Screen<Cell, TheScreenType>::setCurrentColumn(ColumnOffset _n)
{
    auto const col = _state.cursor.originMode ? _state.margin.horizontal.from + _n : _n;
    auto const clampedCol = min(col, boxed_cast<ColumnOffset>(_state.pageSize.columns) - 1);
    _state.wrapPending = false;
    _state.cursor.position.column = clampedCol;
}

template <typename Cell, ScreenType TheScreenType>
string Screen<Cell, TheScreenType>::renderMainPageText() const
{
    return grid().renderMainPageText();
}
// }}}

// {{{ ops
template <typename Cell, ScreenType TheScreenType>
void Screen<Cell, TheScreenType>::linefeed()
{
    // If coming through stdout-fastpipe, the LF acts like CRLF.
    if (_state.usingStdoutFastPipe)
        linefeed(_state.margin.horizontal.from);
    else
        linefeed(_state.cursor.position.column);
}

template <typename Cell, ScreenType TheScreenType>
void Screen<Cell, TheScreenType>::backspace()
{
    if (_state.cursor.position.column.value)
        _state.cursor.position.column--;
}

template <typename Cell, ScreenType TheScreenType>
void Screen<Cell, TheScreenType>::deviceStatusReport()
{
    _terminal.reply("\033[0n");
}

template <typename Cell, ScreenType TheScreenType>
void Screen<Cell, TheScreenType>::reportCursorPosition()
{
    _terminal.reply("\033[{};{}R", logicalCursorPosition().line + 1, logicalCursorPosition().column + 1);
}

template <typename Cell, ScreenType TheScreenType>
void Screen<Cell, TheScreenType>::reportExtendedCursorPosition()
{
    auto const pageNum = 1;
    _terminal.reply(
        "\033[{};{};{}R", logicalCursorPosition().line + 1, logicalCursorPosition().column + 1, pageNum);
}

template <typename Cell, ScreenType TheScreenType>
void Screen<Cell, TheScreenType>::selectConformanceLevel(VTType _level)
{
    // Don't enforce the selected conformance level, just remember it.
    _state.terminalId = _level;
}

template <typename Cell, ScreenType TheScreenType>
void Screen<Cell, TheScreenType>::sendDeviceAttributes()
{
    // See https://vt100.net/docs/vt510-rm/DA1.html

    auto const id = [&]() -> string_view {
        switch (_state.terminalId)
        {
            case VTType::VT100: return "1";
            case VTType::VT220:
            case VTType::VT240: return "62";
            case VTType::VT320:
            case VTType::VT330:
            case VTType::VT340: return "63";
            case VTType::VT420: return "64";
            case VTType::VT510:
            case VTType::VT520:
            case VTType::VT525: return "65";
        }
        return "1"; // Should never be reached.
    }();

    auto const attrs = to_params(DeviceAttributes::AnsiColor |
                                 // DeviceAttributes::AnsiTextLocator |
                                 DeviceAttributes::CaptureScreenBuffer | DeviceAttributes::Columns132 |
                                 // TODO: DeviceAttributes::NationalReplacementCharacterSets |
                                 DeviceAttributes::RectangularEditing |
                                 // TODO: DeviceAttributes::SelectiveErase |
                                 DeviceAttributes::SixelGraphics |
                                 // TODO: DeviceAttributes::TechnicalCharacters |
                                 DeviceAttributes::UserDefinedKeys);

    _terminal.reply("\033[?{};{}c", id, attrs);
}

template <typename Cell, ScreenType TheScreenType>
void Screen<Cell, TheScreenType>::sendTerminalId()
{
    // Note, this is "Secondary DA".
    // It requests for the terminalID

    // terminal protocol type
    auto const Pp = static_cast<unsigned>(_state.terminalId);

    // version number
    // TODO: (PACKAGE_VERSION_MAJOR * 100 + PACKAGE_VERSION_MINOR) * 100 + PACKAGE_VERSION_MICRO
    auto constexpr Pv =
        (LIBTERMINAL_VERSION_MAJOR * 100 + LIBTERMINAL_VERSION_MINOR) * 100 + LIBTERMINAL_VERSION_PATCH;

    // ROM cardridge registration number (always 0)
    auto constexpr Pc = 0;

    _terminal.reply("\033[>{};{};{}c", Pp, Pv, Pc);
}

template <typename Cell, ScreenType TheScreenType>
void Screen<Cell, TheScreenType>::clearToEndOfScreen()
{
    clearToEndOfLine();

    for (auto const lineOffset:
         ranges::views::iota(unbox<int>(_state.cursor.position.line) + 1, unbox<int>(_state.pageSize.lines)))
    {
        Line<Cell>& line = grid().lineAt(LineOffset::cast_from(lineOffset));
        line.reset(grid().defaultLineFlags(), _state.cursor.graphicsRendition);
    }
}

template <typename Cell, ScreenType TheScreenType>
void Screen<Cell, TheScreenType>::clearToBeginOfScreen()
{
    clearToBeginOfLine();

    for (auto const lineOffset: ranges::views::iota(0, *_state.cursor.position.line))
    {
        Line<Cell>& line = grid().lineAt(LineOffset::cast_from(lineOffset));
        line.reset(grid().defaultLineFlags(), _state.cursor.graphicsRendition);
    }
}

template <typename Cell, ScreenType TheScreenType>
void Screen<Cell, TheScreenType>::clearScreen()
{
    // Instead of *just* clearing the screen, and thus, losing potential important content,
    // we scroll up by RowCount number of lines, so move it all into history, so the user can scroll
    // up in case the content is still needed.
    scrollUp(_state.pageSize.lines);
}

template <typename Cell, ScreenType TheScreenType>
void Screen<Cell, TheScreenType>::eraseCharacters(ColumnCount _n)
{
    // Spec: https://vt100.net/docs/vt510-rm/ECH.html
    // It's not clear from the spec how to perform erase when inside margin and number of chars to be erased
    // would go outside margins.
    // TODO: See what xterm does ;-)

    // erase characters from current colum to the right
    auto const columnsAvailable =
        _state.pageSize.columns - boxed_cast<ColumnCount>(realCursorPosition().column);
    auto const n = unbox<long>(clamp(_n, ColumnCount(1), columnsAvailable));

    auto& line = currentLine();
    for (int i = 0; i < n; ++i)
        line.useCellAt(_state.cursor.position.column + i).reset(_state.cursor.graphicsRendition);
}

template <typename Cell, ScreenType TheScreenType>
void Screen<Cell, TheScreenType>::clearToEndOfLine()
{
    if (_terminal.isFullHorizontalMargins() && _state.cursor.position.column.value == 0)
    {
        currentLine().reset(currentLine().flags(), _state.cursor.graphicsRendition);
        return;
    }

    Cell* i = &at(_state.cursor.position);
    Cell* e = i + unbox<int>(_state.pageSize.columns) - unbox<int>(_state.cursor.position.column);
    while (i != e)
    {
        i->reset(_state.cursor.graphicsRendition);
        ++i;
    }

    auto const line = _state.cursor.position.line;
    auto const left = _state.cursor.position.column;
    auto const right = boxed_cast<ColumnOffset>(_state.pageSize.columns - 1);
    auto const area = Rect { Top(*line), Left(*left), Bottom(*line), Right(*right) };
    _terminal.markRegionDirty(area);
}

template <typename Cell, ScreenType TheScreenType>
void Screen<Cell, TheScreenType>::clearToBeginOfLine()
{
    Cell* i = &at(_state.cursor.position.line, ColumnOffset(0));
    Cell* e = i + unbox<int>(_state.cursor.position.column) + 1;
    while (i != e)
    {
        i->reset(_state.cursor.graphicsRendition);
        ++i;
    }

    auto const line = _state.cursor.position.line;
    auto const left = ColumnOffset(0);
    auto const right = _state.cursor.position.column;
    auto const area = Rect { Top(*line), Left(*left), Bottom(*line), Right(*right) };
    _terminal.markRegionDirty(area);
}

template <typename Cell, ScreenType TheScreenType>
void Screen<Cell, TheScreenType>::clearLine()
{
    currentLine().reset(grid().defaultLineFlags(), _state.cursor.graphicsRendition);

    auto const line = _state.cursor.position.line;
    auto const left = ColumnOffset(0);
    auto const right = boxed_cast<ColumnOffset>(_state.pageSize.columns - 1);
    auto const area = Rect { Top(*line), Left(*left), Bottom(*line), Right(*right) };
    _terminal.markRegionDirty(area);
}

template <typename Cell, ScreenType TheScreenType>
void Screen<Cell, TheScreenType>::moveCursorToNextLine(LineCount _n)
{
    moveCursorTo(logicalCursorPosition().line + _n.as<LineOffset>(), ColumnOffset(0));
}

template <typename Cell, ScreenType TheScreenType>
void Screen<Cell, TheScreenType>::moveCursorToPrevLine(LineCount _n)
{
    auto const n = min(_n.as<LineOffset>(), logicalCursorPosition().line);
    moveCursorTo(logicalCursorPosition().line - n, ColumnOffset(0));
}

template <typename Cell, ScreenType TheScreenType>
void Screen<Cell, TheScreenType>::insertCharacters(ColumnCount _n)
{
    if (_terminal.isCursorInsideMargins())
        insertChars(realCursorPosition().line, _n);
}

/// Inserts @p _n characters at given line @p _lineNo.
template <typename Cell, ScreenType TheScreenType>
void Screen<Cell, TheScreenType>::insertChars(LineOffset _lineNo, ColumnCount _n)
{
    auto const n = min(*_n, *_state.margin.horizontal.to - *logicalCursorPosition().column + 1);

    auto column0 = grid().lineAt(_lineNo).inflatedBuffer().begin() + *realCursorPosition().column;
    auto column1 = grid().lineAt(_lineNo).inflatedBuffer().begin() + *_state.margin.horizontal.to - n + 1;
    auto column2 = grid().lineAt(_lineNo).inflatedBuffer().begin() + *_state.margin.horizontal.to + 1;

    rotate(column0, column1, column2);

    for (Cell& cell: grid().lineAt(_lineNo).useRange(boxed_cast<ColumnOffset>(_state.cursor.position.column),
                                                     ColumnCount::cast_from(n)))
    {
        cell.write(_state.cursor.graphicsRendition, L' ', 1);
    }
}

template <typename Cell, ScreenType TheScreenType>
void Screen<Cell, TheScreenType>::insertLines(LineCount _n)
{
    if (_terminal.isCursorInsideMargins())
    {
        scrollDown(_n,
                   Margin { Margin::Vertical { _state.cursor.position.line, _state.margin.vertical.to },
                            _state.margin.horizontal });
        updateCursorIterator();
    }
}

template <typename Cell, ScreenType TheScreenType>
void Screen<Cell, TheScreenType>::insertColumns(ColumnCount _n)
{
    if (_terminal.isCursorInsideMargins())
        for (auto lineNo = _state.margin.vertical.from; lineNo <= _state.margin.vertical.to; ++lineNo)
            insertChars(lineNo, _n);
}

template <typename Cell, ScreenType TheScreenType>
void Screen<Cell, TheScreenType>::copyArea(Rect _sourceArea,
                                           int _page,
                                           CellLocation _targetTopLeft,
                                           int _targetPage)
{
    (void) _page;
    (void) _targetPage;

    // The space at https://vt100.net/docs/vt510-rm/DECCRA.html states:
    // "If Pbs is greater than Pts, // or Pls is greater than Prs, the terminal ignores DECCRA."
    //
    // However, the first part "Pbs is greater than Pts" does not make sense.
    if (*_sourceArea.bottom < *_sourceArea.top || *_sourceArea.right < *_sourceArea.left)
        return;

    if (*_sourceArea.top == *_targetTopLeft.line && *_sourceArea.left == *_targetTopLeft.column)
        // Copy to its own location => no-op.
        return;

    auto const [x0, xInc, xEnd] = [&]() {
        if (*_targetTopLeft.column > *_sourceArea.left) // moving right
            return std::tuple { *_sourceArea.right - *_sourceArea.left, -1, -1 };
        else
            return std::tuple { 0, +1, *_sourceArea.right - *_sourceArea.left + 1 };
    }();

    auto const [y0, yInc, yEnd] = [&]() {
        if (*_targetTopLeft.line > *_sourceArea.top) // moving down
            return std::tuple { *_sourceArea.bottom - *_sourceArea.top, -1, -1 };
        else
            return std::tuple { 0, +1, *_sourceArea.bottom - *_sourceArea.top + 1 };
    }();

    for (auto y = y0; y != yEnd; y += yInc)
    {
        for (auto x = x0; x != xEnd; x += xInc)
        {
            Cell const& sourceCell = at(LineOffset::cast_from(*_sourceArea.top + y),
                                        ColumnOffset::cast_from(_sourceArea.left + x));
            Cell& targetCell = at(LineOffset::cast_from(_targetTopLeft.line + y),
                                  ColumnOffset::cast_from(_targetTopLeft.column + x));
            targetCell = sourceCell;
        }
    }
}

template <typename Cell, ScreenType TheScreenType>
void Screen<Cell, TheScreenType>::eraseArea(int _top, int _left, int _bottom, int _right)
{
    assert(_right <= unbox<int>(_state.pageSize.columns));
    assert(_bottom <= unbox<int>(_state.pageSize.lines));

    if (_top > _bottom || _left > _right)
        return;

    for (int y = _top; y <= _bottom; ++y)
    {
        for (Cell& cell: grid()
                             .lineAt(LineOffset::cast_from(y))
                             .useRange(ColumnOffset(_left), ColumnCount(_right - _left + 1)))
        {
            cell.write(_state.cursor.graphicsRendition, L' ', 1);
        }
    }
}

template <typename Cell, ScreenType TheScreenType>
void Screen<Cell, TheScreenType>::fillArea(char32_t _ch, int _top, int _left, int _bottom, int _right)
{
    // "Pch can be any value from 32 to 126 or from 160 to 255."
    if (!(32 <= _ch && _ch <= 126) && !(160 <= _ch && _ch <= 255))
        return;

    auto const w = static_cast<uint8_t>(unicode::width(_ch));
    for (int y = _top; y <= _bottom; ++y)
    {
        for (Cell& cell:
             grid()
                 .lineAt(LineOffset::cast_from(y))
                 .useRange(ColumnOffset::cast_from(_left), ColumnCount::cast_from(_right - _left + 1)))
        {
            cell.write(cursor().graphicsRendition, _ch, w);
        }
    }
}

template <typename Cell, ScreenType TheScreenType>
void Screen<Cell, TheScreenType>::deleteLines(LineCount _n)
{
    if (_terminal.isCursorInsideMargins())
    {
        scrollUp(_n,
                 Margin { Margin::Vertical { _state.cursor.position.line, _state.margin.vertical.to },
                          _state.margin.horizontal });
    }
}

template <typename Cell, ScreenType TheScreenType>
void Screen<Cell, TheScreenType>::deleteCharacters(ColumnCount _n)
{
    if (_terminal.isCursorInsideMargins() && *_n != 0)
        deleteChars(realCursorPosition().line, realCursorPosition().column, _n);
}

template <typename Cell, ScreenType TheScreenType>
void Screen<Cell, TheScreenType>::deleteChars(LineOffset _line, ColumnOffset _column, ColumnCount _n)
{
    auto& line = grid().lineAt(_line);
    auto lineBuffer = line.cells();

    Cell* left = const_cast<Cell*>(lineBuffer.data() + _column.as<size_t>());
    Cell* right = const_cast<Cell*>(lineBuffer.data() + *_state.margin.horizontal.to + 1);
    long const n = min(_n.as<long>(), static_cast<long>(distance(left, right)));
    Cell* mid = left + n;

    rotate(left, mid, right);

    for (Cell& cell: gsl::make_span(right - n, right))
    {
        cell.write(_state.cursor.graphicsRendition, L' ', 1);
    }
}

template <typename Cell, ScreenType TheScreenType>
void Screen<Cell, TheScreenType>::deleteColumns(ColumnCount _n)
{
    if (_terminal.isCursorInsideMargins())
        for (auto lineNo = _state.margin.vertical.from; lineNo <= _state.margin.vertical.to; ++lineNo)
            deleteChars(lineNo, realCursorPosition().column, _n);
}

template <typename Cell, ScreenType TheScreenType>
void Screen<Cell, TheScreenType>::horizontalTabClear(HorizontalTabClear _which)
{
    switch (_which)
    {
        case HorizontalTabClear::AllTabs: clearAllTabs(); break;
        case HorizontalTabClear::UnderCursor: clearTabUnderCursor(); break;
    }
}

template <typename Cell, ScreenType TheScreenType>
void Screen<Cell, TheScreenType>::horizontalTabSet()
{
    setTabUnderCursor();
}

template <typename Cell, ScreenType TheScreenType>
void Screen<Cell, TheScreenType>::setCurrentWorkingDirectory(string const& _url)
{
    _state.currentWorkingDirectory = _url;
}

template <typename Cell, ScreenType TheScreenType>
void Screen<Cell, TheScreenType>::hyperlink(string _id, string _uri)
{
    if (_uri.empty())
        _state.cursor.hyperlink = {};
    else if (!_id.empty())
        _state.cursor.hyperlink = _state.hyperlinks.hyperlinkIdByUserId(_id);
    else
    {
        _state.cursor.hyperlink = _state.hyperlinks.nextHyperlinkId++;
        _state.hyperlinks.cache.emplace(_state.cursor.hyperlink,
                                        make_shared<HyperlinkInfo>(HyperlinkInfo { move(_id), move(_uri) }));
    }
    // TODO:
    // Care about eviction.
    // Move hyperlink store into ScreenBuffer, so it gets reset upon every switch into
    // alternate screen (not for main screen!)
}

template <typename Cell, ScreenType TheScreenType>
void Screen<Cell, TheScreenType>::moveCursorUp(LineCount _n)
{
    _state.wrapPending = 0;
    auto const n = min(_n.as<LineOffset>(),
                       logicalCursorPosition().line > _state.margin.vertical.from
                           ? logicalCursorPosition().line - _state.margin.vertical.from
                           : logicalCursorPosition().line);

    _state.cursor.position.line -= n;
    updateCursorIterator();
}

template <typename Cell, ScreenType TheScreenType>
void Screen<Cell, TheScreenType>::moveCursorDown(LineCount _n)
{
    _state.wrapPending = 0;
    auto const currentLineNumber = logicalCursorPosition().line;
    auto const n = min(_n.as<LineOffset>(),
                       currentLineNumber <= _state.margin.vertical.to
                           ? _state.margin.vertical.to - currentLineNumber
                           : (boxed_cast<LineOffset>(_state.pageSize.lines) - 1) - currentLineNumber);

    _state.cursor.position.line += n;
    updateCursorIterator();
}

template <typename Cell, ScreenType TheScreenType>
void Screen<Cell, TheScreenType>::moveCursorForward(ColumnCount _n)
{
    _state.wrapPending = 0;
    _state.cursor.position.column =
        min(_state.cursor.position.column + _n.as<ColumnOffset>(), _state.margin.horizontal.to);
}

template <typename Cell, ScreenType TheScreenType>
void Screen<Cell, TheScreenType>::moveCursorBackward(ColumnCount _n)
{
    // even if you move to 80th of 80 columns, it'll first write a char and THEN flag wrap pending
    _state.wrapPending = false;

    // TODO: skip cells that in counting when iterating backwards over a wide cell (such as emoji)
    auto const n = min(_n.as<ColumnOffset>(), _state.cursor.position.column);
    setCurrentColumn(_state.cursor.position.column - n);
}

template <typename Cell, ScreenType TheScreenType>
void Screen<Cell, TheScreenType>::moveCursorToColumn(ColumnOffset _column)
{
    setCurrentColumn(_column);
}

template <typename Cell, ScreenType TheScreenType>
void Screen<Cell, TheScreenType>::moveCursorToBeginOfLine()
{
    setCurrentColumn(ColumnOffset(0));
}

template <typename Cell, ScreenType TheScreenType>
void Screen<Cell, TheScreenType>::moveCursorToLine(LineOffset _row)
{
    moveCursorTo(_row, _state.cursor.position.column);
}

template <typename Cell, ScreenType TheScreenType>
void Screen<Cell, TheScreenType>::moveCursorToNextTab()
{
    // TODO: I guess something must remember when a \t was added, for proper move-back?
    // TODO: respect HTS/TBC

    if (!_state.tabs.empty())
    {
        // advance to the next tab
        size_t i = 0;
        while (i < _state.tabs.size() && realCursorPosition().column >= _state.tabs[i])
            ++i;

        auto const currentCursorColumn = logicalCursorPosition().column;

        if (i < _state.tabs.size())
            moveCursorForward(boxed_cast<ColumnCount>(_state.tabs[i] - currentCursorColumn));
        else if (realCursorPosition().column < _state.margin.horizontal.to)
            moveCursorForward(boxed_cast<ColumnCount>(_state.margin.horizontal.to - currentCursorColumn));
        else
            moveCursorToNextLine(LineCount(1));
    }
    else if (TabWidth.value)
    {
        // default tab settings
        if (realCursorPosition().column < _state.margin.horizontal.to)
        {
            auto const n =
                min((TabWidth - boxed_cast<ColumnCount>(_state.cursor.position.column) % TabWidth),
                    _state.pageSize.columns - boxed_cast<ColumnCount>(logicalCursorPosition().column));
            moveCursorForward(n);
        }
        else
            moveCursorToNextLine(LineCount(1));
    }
    else
    {
        // no tab stops configured
        if (realCursorPosition().column < _state.margin.horizontal.to)
            // then TAB moves to the end of the screen
            moveCursorToColumn(_state.margin.horizontal.to);
        else
            // then TAB moves to next line left margin
            moveCursorToNextLine(LineCount(1));
    }
}

template <typename Cell, ScreenType TheScreenType>
void Screen<Cell, TheScreenType>::notify(string const& _title, string const& _content)
{
    std::cout << "Screen.NOTIFY: title: '" << _title << "', content: '" << _content << "'\n";
    _terminal.notify(_title, _content);
}

template <typename Cell, ScreenType TheScreenType>
void Screen<Cell, TheScreenType>::captureBuffer(LineCount _lineCount, bool _logicalLines)
{
    // TODO: Unit test case! (for ensuring line numbering and limits are working as expected)

    auto capturedBuffer = std::string();

    // TODO: when capturing _lineCount < screenSize.lines, start at the lowest non-empty line.
    auto const relativeStartLine =
        _logicalLines ? grid().computeLogicalLineNumberFromBottom(LineCount::cast_from(_lineCount))
                      : unbox<int>(_state.pageSize.lines - _lineCount);
    auto const startLine = LineOffset::cast_from(
        clamp(relativeStartLine, -unbox<int>(historyLineCount()), unbox<int>(_state.pageSize.lines)));

    VTCaptureBufferLog()("Capture buffer: {} lines {}", _lineCount, _logicalLines ? "logical" : "actual");

    size_t constexpr MaxChunkSize = 4096;
    size_t currentChunkSize = 0;
    auto const pushContent = [&](auto const data) -> void {
        if (data.empty())
            return;
        if (currentChunkSize == 0) // initiate chunk
            _terminal.reply("\033^{};", CaptureBufferCode);
        else if (currentChunkSize + data.size() >= MaxChunkSize)
        {
            VTCaptureBufferLog()("Transferred chunk of {} bytes.", currentChunkSize);
            _terminal.reply("\033\\"); // ST
            _terminal.reply("\033^{};", CaptureBufferCode);
            currentChunkSize = 0;
        }
        _terminal.reply(data);
        currentChunkSize += data.size();
    };
    LineOffset const bottomLine = boxed_cast<LineOffset>(_state.pageSize.lines - 1);
    VTCaptureBufferLog()("Capturing buffer. top: {}, bottom: {}", relativeStartLine, bottomLine);

    for (LineOffset line = startLine; line <= bottomLine; ++line)
    {
        if (_logicalLines && grid().lineAt(line).wrapped() && !capturedBuffer.empty())
            capturedBuffer.pop_back();

        auto const& lineBuffer = grid().lineAt(line);
        auto lineCellsTrimmed = lineBuffer.trim_blank_right();
        if (lineCellsTrimmed.empty())
        {
            VTCaptureBufferLog()("Skipping blank line {}", line);
            continue;
        }
        auto const tl = lineCellsTrimmed.size();
        while (!lineCellsTrimmed.empty())
        {
            auto const available = MaxChunkSize - currentChunkSize;
            auto const n = min(available, lineCellsTrimmed.size());
            for (auto const& cell: lineCellsTrimmed.subspan(0, n))
                pushContent(cell.toUtf8());
            lineCellsTrimmed = lineCellsTrimmed.subspan(n);
        }
        VTCaptureBufferLog()("NL ({} len)", tl);
        pushContent("\n"sv);
    }

    if (currentChunkSize != 0)
        _terminal.reply("\033\\"); // ST

    VTCaptureBufferLog()("Capturing buffer finished.");
    _terminal.reply("\033^{};\033\\", CaptureBufferCode); // mark the end
}

template <typename Cell, ScreenType TheScreenType>
void Screen<Cell, TheScreenType>::cursorForwardTab(TabStopCount _count)
{
    for (int i = 0; i < unbox<int>(_count); ++i)
        moveCursorToNextTab();
}

template <typename Cell, ScreenType TheScreenType>
void Screen<Cell, TheScreenType>::cursorBackwardTab(TabStopCount _count)
{
    if (!_count)
        return;

    if (!_state.tabs.empty())
    {
        for (unsigned k = 0; k < unbox<unsigned>(_count); ++k)
        {
            auto const i =
                std::find_if(rbegin(_state.tabs), rend(_state.tabs), [&](ColumnOffset tabPos) -> bool {
                    return tabPos < logicalCursorPosition().column;
                });
            if (i != rend(_state.tabs))
            {
                // prev tab found -> move to prev tab
                moveCursorToColumn(*i);
            }
            else
            {
                moveCursorToColumn(_state.margin.horizontal.from);
                break;
            }
        }
    }
    else if (TabWidth.value)
    {
        // default tab settings
        if (*_state.cursor.position.column < *TabWidth)
            moveCursorToBeginOfLine();
        else
        {
            auto const m = (*_state.cursor.position.column + 1) % *TabWidth;
            auto const n = m ? (*_count - 1) * *TabWidth + m : *_count * *TabWidth + m;
            moveCursorBackward(ColumnCount(n - 1));
        }
    }
    else
    {
        // no tab stops configured
        moveCursorToBeginOfLine();
    }
}

template <typename Cell, ScreenType TheScreenType>
void Screen<Cell, TheScreenType>::index()
{
    if (*realCursorPosition().line == *_state.margin.vertical.to)
        scrollUp(LineCount(1));
    else
        moveCursorDown(LineCount(1));
}

template <typename Cell, ScreenType TheScreenType>
void Screen<Cell, TheScreenType>::reverseIndex()
{
    if (unbox<int>(realCursorPosition().line) == unbox<int>(_state.margin.vertical.from))
        scrollDown(LineCount(1));
    else
        moveCursorUp(LineCount(1));
}

template <typename Cell, ScreenType TheScreenType>
void Screen<Cell, TheScreenType>::backIndex()
{
    if (realCursorPosition().column == _state.margin.horizontal.from)
        ; // TODO: scrollRight(1);
    else
        moveCursorForward(ColumnCount(1));
}

template <typename Cell, ScreenType TheScreenType>
void Screen<Cell, TheScreenType>::forwardIndex()
{
    if (*realCursorPosition().column == *_state.margin.horizontal.to)
        grid().scrollLeft(GraphicsAttributes {}, _state.margin);
    else
        moveCursorForward(ColumnCount(1));
}

template <typename Cell, ScreenType TheScreenType>
void Screen<Cell, TheScreenType>::setForegroundColor(Color _color)
{
    _state.cursor.graphicsRendition.foregroundColor = _color;
}

template <typename Cell, ScreenType TheScreenType>
void Screen<Cell, TheScreenType>::setBackgroundColor(Color _color)
{
    _state.cursor.graphicsRendition.backgroundColor = _color;
}

template <typename Cell, ScreenType TheScreenType>
void Screen<Cell, TheScreenType>::setUnderlineColor(Color _color)
{
    _state.cursor.graphicsRendition.underlineColor = _color;
}

template <typename Cell, ScreenType TheScreenType>
void Screen<Cell, TheScreenType>::setCursorStyle(CursorDisplay _display, CursorShape _shape)
{
    _state.cursorDisplay = _display;
    _state.cursorShape = _shape;

    _terminal.setCursorStyle(_display, _shape);
}

template <typename Cell, ScreenType TheScreenType>
void Screen<Cell, TheScreenType>::setGraphicsRendition(GraphicsRendition _rendition)
{
    _terminal.setGraphicsRendition(_rendition);
}

template <typename Cell, ScreenType TheScreenType>
void Screen<Cell, TheScreenType>::setMark()
{
    currentLine().setMarked(true);
}

template <typename Cell, ScreenType TheScreenType>
void Screen<Cell, TheScreenType>::saveModes(std::vector<DECMode> const& _modes)
{
    _state.modes.save(_modes);
}

template <typename Cell, ScreenType TheScreenType>
void Screen<Cell, TheScreenType>::restoreModes(std::vector<DECMode> const& _modes)
{
    _state.modes.restore(_modes);
}

enum class ModeResponse
{ // TODO: respect response 0, 3, 4.
    NotRecognized = 0,
    Set = 1,
    Reset = 2,
    PermanentlySet = 3,
    PermanentlyReset = 4
};

template <typename Cell, ScreenType TheScreenType>
void Screen<Cell, TheScreenType>::requestAnsiMode(unsigned int _mode)
{
    ModeResponse const modeResponse =
        isValidAnsiMode(_mode)
            ? _terminal.isModeEnabled(static_cast<AnsiMode>(_mode)) ? ModeResponse::Set : ModeResponse::Reset
            : ModeResponse::NotRecognized;

    auto const code = toAnsiModeNum(static_cast<AnsiMode>(_mode));

    _terminal.reply("\033[{};{}$y", code, static_cast<unsigned>(modeResponse));
}

template <typename Cell, ScreenType TheScreenType>
void Screen<Cell, TheScreenType>::requestDECMode(unsigned int _mode)
{
    ModeResponse const modeResponse =
        isValidDECMode(_mode)
            ? _terminal.isModeEnabled(static_cast<DECMode>(_mode)) ? ModeResponse::Set : ModeResponse::Reset
            : ModeResponse::NotRecognized;

    auto const code = toDECModeNum(static_cast<DECMode>(_mode));

    _terminal.reply("\033[?{};{}$y", code, static_cast<unsigned>(modeResponse));
}

template <typename Cell, ScreenType TheScreenType>
void Screen<Cell, TheScreenType>::screenAlignmentPattern()
{
    // sets the margins to the extremes of the page
    _state.margin.vertical.from = LineOffset(0);
    _state.margin.vertical.to = boxed_cast<LineOffset>(_state.pageSize.lines) - LineOffset(1);
    _state.margin.horizontal.from = ColumnOffset(0);
    _state.margin.horizontal.to = boxed_cast<ColumnOffset>(_state.pageSize.columns) - ColumnOffset(1);

    // and moves the cursor to the home position
    moveCursorTo({}, {});

    // fills the complete screen area with a test pattern
    for (auto& line: grid().mainPage())
    {
        line.fill(grid().defaultLineFlags(), GraphicsAttributes {}, U'E', 1);
    }
}

template <typename Cell, ScreenType TheScreenType>
void Screen<Cell, TheScreenType>::applicationKeypadMode(bool _enable)
{
    _terminal.setApplicationkeypadMode(_enable);
}

template <typename Cell, ScreenType TheScreenType>
void Screen<Cell, TheScreenType>::designateCharset(CharsetTable _table, CharsetId _charset)
{
    // TODO: unit test SCS and see if they also behave well with reset/softreset
    // Also, is the cursor shared between the two buffers?
    _state.cursor.charsets.select(_table, _charset);
}

template <typename Cell, ScreenType TheScreenType>
void Screen<Cell, TheScreenType>::singleShiftSelect(CharsetTable _table)
{
    // TODO: unit test SS2, SS3
    _state.cursor.charsets.singleShift(_table);
}

template <typename Cell, ScreenType TheScreenType>
void Screen<Cell, TheScreenType>::sixelImage(ImageSize _pixelSize, Image::Data&& _data)
{
    auto const columnCount =
        ColumnCount::cast_from(ceilf(float(*_pixelSize.width) / float(*_state.cellPixelSize.width)));
    auto const lineCount =
        LineCount::cast_from(ceilf(float(*_pixelSize.height) / float(*_state.cellPixelSize.height)));
    auto const extent = GridSize { lineCount, columnCount };
    auto const autoScrollAtBottomMargin = _terminal.isModeEnabled(
        DECMode::SixelScrolling); // If DECSDM is enabled, scrolling is meant to be disabled.
    auto const topLeft = autoScrollAtBottomMargin ? logicalCursorPosition() : CellLocation {};

    auto const alignmentPolicy = ImageAlignment::TopStart;
    auto const resizePolicy = ImageResize::NoResize;

    auto const imageOffset = PixelCoordinate {};
    auto const imageSize = _pixelSize;

    shared_ptr<Image const> imageRef = uploadImage(ImageFormat::RGBA, _pixelSize, move(_data));
    renderImage(imageRef,
                topLeft,
                extent,
                imageOffset,
                imageSize,
                alignmentPolicy,
                resizePolicy,
                autoScrollAtBottomMargin);

    if (!_terminal.isModeEnabled(DECMode::SixelCursorNextToGraphic))
        linefeed(topLeft.column);
}

template <typename Cell, ScreenType TheScreenType>
shared_ptr<Image const> Screen<Cell, TheScreenType>::uploadImage(ImageFormat _format,
                                                                 ImageSize _imageSize,
                                                                 Image::Data&& _pixmap)
{
    return _state.imagePool.create(_format, _imageSize, move(_pixmap));
}

template <typename Cell, ScreenType TheScreenType>
void Screen<Cell, TheScreenType>::renderImage(shared_ptr<Image const> _image,
                                              CellLocation _topLeft,
                                              GridSize _gridSize,
                                              PixelCoordinate _imageOffset,
                                              ImageSize _imageSize,
                                              ImageAlignment _alignmentPolicy,
                                              ImageResize _resizePolicy,
                                              bool _autoScroll)
{
    // TODO: make use of _imageOffset and _imageSize
    (void) _imageOffset;
    (void) _imageSize;

    auto const linesAvailable = _state.pageSize.lines - _topLeft.line.as<LineCount>();
    auto const linesToBeRendered = min(_gridSize.lines, linesAvailable);
    auto const columnsAvailable = *_state.pageSize.columns - *_topLeft.column;
    auto const columnsToBeRendered = ColumnCount(min(columnsAvailable, *_gridSize.columns));
    auto const gapColor = RGBAColor {}; // TODO: _state.cursor.graphicsRendition.backgroundColor;

    // TODO: make use of _imageOffset and _imageSize
    auto const rasterizedImage = _state.imagePool.rasterize(
        move(_image), _alignmentPolicy, _resizePolicy, gapColor, _gridSize, _state.cellPixelSize);

    if (*linesToBeRendered)
    {
        for (GridSize::Offset const offset: GridSize { linesToBeRendered, columnsToBeRendered })
        {
            Cell& cell = at(_topLeft + offset);
            cell.setImageFragment(rasterizedImage, CellLocation { offset.line, offset.column });
            cell.setHyperlink(_state.cursor.hyperlink);
        };
        moveCursorTo(_topLeft.line + unbox<int>(linesToBeRendered) - 1, _topLeft.column);
    }

    // If there're lines to be rendered missing (because it didn't fit onto the screen just yet)
    // AND iff sixel !sixelScrolling  is enabled, then scroll as much as needed to render the remaining
    // lines.
    if (linesToBeRendered != _gridSize.lines && _autoScroll)
    {
        auto const remainingLineCount = _gridSize.lines - linesToBeRendered;
        for (auto const lineOffset: crispy::times(*remainingLineCount))
        {
            linefeed(_topLeft.column);
            for (auto const columnOffset: crispy::views::iota_as<ColumnOffset>(*columnsToBeRendered))
            {
                auto const offset =
                    CellLocation { boxed_cast<LineOffset>(linesToBeRendered) + lineOffset, columnOffset };
                Cell& cell =
                    at(boxed_cast<LineOffset>(_state.pageSize.lines) - 1, _topLeft.column + columnOffset);
                cell.setImageFragment(rasterizedImage, offset);
                cell.setHyperlink(_state.cursor.hyperlink);
            };
        }
    }
    // move ansi text cursor to position of the sixel cursor
    moveCursorToColumn(_topLeft.column + _gridSize.columns.as<ColumnOffset>());
}

template <typename Cell, ScreenType TheScreenType>
void Screen<Cell, TheScreenType>::requestDynamicColor(DynamicColorName _name)
{
    auto const color = [&]() -> optional<RGBColor> {
        switch (_name)
        {
            case DynamicColorName::DefaultForegroundColor: return _state.colorPalette.defaultForeground;
            case DynamicColorName::DefaultBackgroundColor: return _state.colorPalette.defaultBackground;
            case DynamicColorName::TextCursorColor:
                if (holds_alternative<CellForegroundColor>(_state.colorPalette.cursor.color))
                    return _state.colorPalette.defaultForeground;
                else if (holds_alternative<CellBackgroundColor>(_state.colorPalette.cursor.color))
                    return _state.colorPalette.defaultBackground;
                else
                    return get<RGBColor>(_state.colorPalette.cursor.color);
            case DynamicColorName::MouseForegroundColor: return _state.colorPalette.mouseForeground;
            case DynamicColorName::MouseBackgroundColor: return _state.colorPalette.mouseBackground;
            case DynamicColorName::HighlightForegroundColor:
                if (_state.colorPalette.selectionForeground.has_value())
                    return _state.colorPalette.selectionForeground.value();
                else
                    return nullopt;
            case DynamicColorName::HighlightBackgroundColor:
                if (_state.colorPalette.selectionBackground.has_value())
                    return _state.colorPalette.selectionBackground.value();
                else
                    return nullopt;
        }
        return nullopt; // should never happen
    }();

    if (color.has_value())
    {
        _terminal.reply(
            "\033]{};{}\033\\", setDynamicColorCommand(_name), setDynamicColorValue(color.value()));
    }
}

template <typename Cell, ScreenType TheScreenType>
void Screen<Cell, TheScreenType>::requestPixelSize(RequestPixelSize _area)
{
    switch (_area)
    {
        case RequestPixelSize::WindowArea: [[fallthrough]]; // TODO
        case RequestPixelSize::TextArea: {
            // Result is CSI  4 ;  height ;  width t
            _terminal.reply("\033[4;{};{}t", pixelSize().height, pixelSize().width);
            break;
        }
        case RequestPixelSize::CellArea:
            // Result is CSI  6 ;  height ;  width t
            _terminal.reply("\033[6;{};{}t", _state.cellPixelSize.height, _state.cellPixelSize.width);
            break;
    }
}

template <typename Cell, ScreenType TheScreenType>
void Screen<Cell, TheScreenType>::requestCharacterSize(
    RequestPixelSize _area) // TODO: rename RequestPixelSize to RequestArea?
{
    switch (_area)
    {
        case RequestPixelSize::TextArea:
            _terminal.reply("\033[8;{};{}t", _state.pageSize.lines, _state.pageSize.columns);
            break;
        case RequestPixelSize::WindowArea:
            _terminal.reply("\033[9;{};{}t", _state.pageSize.lines, _state.pageSize.columns);
            break;
        case RequestPixelSize::CellArea:
            Guarantee(false
                      && "Screen.requestCharacterSize: Doesn't make sense, and cannot be called, therefore, "
                         "fortytwo.");
            break;
    }
}

template <typename Cell, ScreenType TheScreenType>
void Screen<Cell, TheScreenType>::requestStatusString(RequestStatusString _value)
{
    // xterm responds with DCS 1 $ r Pt ST for valid requests
    // or DCS 0 $ r Pt ST for invalid requests.
    auto const response = [&](RequestStatusString _value) -> optional<string> {
        switch (_value)
        {
            case RequestStatusString::DECSCL: {
                auto level = 61;
                switch (_state.terminalId)
                {
                    case VTType::VT525:
                    case VTType::VT520:
                    case VTType::VT510: level = 65; break;
                    case VTType::VT420: level = 64; break;
                    case VTType::VT340:
                    case VTType::VT330:
                    case VTType::VT320: level = 63; break;
                    case VTType::VT240:
                    case VTType::VT220: level = 62; break;
                    case VTType::VT100: level = 61; break;
                }

                auto const c1TransmittionMode = ControlTransmissionMode::S7C1T;
                auto const c1t = c1TransmittionMode == ControlTransmissionMode::S7C1T ? 1 : 0;

                return fmt::format("{};{}\"p", level, c1t);
            }
            case RequestStatusString::DECSCUSR: // Set cursor style (DECSCUSR), VT520
            {
                int const blinkingOrSteady = _state.cursorDisplay == CursorDisplay::Steady ? 1 : 0;
                int const shape = [&]() {
                    switch (_state.cursorShape)
                    {
                        case CursorShape::Block: return 1;
                        case CursorShape::Underscore: return 3;
                        case CursorShape::Bar: return 5;
                        case CursorShape::Rectangle: return 7;
                    }
                    return 1;
                }();
                return fmt::format("{} q", shape + blinkingOrSteady);
            }
            case RequestStatusString::DECSLPP:
                // Ps >= 2 4  -> Resize to Ps lines (DECSLPP), VT340 and VT420.
                // xterm adapts this by resizing its window.
                if (*_state.pageSize.lines >= 24)
                    return fmt::format("{}t", _state.pageSize.lines);
                errorlog()("Requesting device status for {} not with line count < 24 is undefined.");
                return nullopt;
            case RequestStatusString::DECSTBM:
                return fmt::format("{};{}r", 1 + *_state.margin.vertical.from, *_state.margin.vertical.to);
            case RequestStatusString::DECSLRM:
                return fmt::format(
                    "{};{}s", 1 + *_state.margin.horizontal.from, *_state.margin.horizontal.to);
            case RequestStatusString::DECSCPP:
                // EXTENSION: Usually DECSCPP only knows about 80 and 132, but we take any.
                return fmt::format("{}|$", _state.pageSize.columns);
            case RequestStatusString::DECSNLS: return fmt::format("{}*|", _state.pageSize.lines);
            case RequestStatusString::SGR:
                return fmt::format("0;{}m", vtSequenceParameterString(_state.cursor.graphicsRendition));
            case RequestStatusString::DECSCA: // TODO
                errorlog()(fmt::format("Requesting device status for {} not implemented yet.", _value));
                break;
        }
        return nullopt;
    }(_value);

    _terminal.reply("\033P{}$r{}\033\\", response.has_value() ? 1 : 0, response.value_or(""), "\"p");
}

template <typename Cell, ScreenType TheScreenType>
void Screen<Cell, TheScreenType>::requestTabStops()
{
    // Response: `DCS 2 $ u Pt ST`
    ostringstream dcs;
    dcs << "\033P2$u"sv; // DCS
    if (!_state.tabs.empty())
    {
        for (size_t const i: times(_state.tabs.size()))
        {
            if (i)
                dcs << '/';
            dcs << *_state.tabs[i] + 1;
        }
    }
    else if (*TabWidth != 0)
    {
        dcs << 1;
        for (auto column = *TabWidth + 1; column <= *_state.pageSize.columns; column += *TabWidth)
            dcs << '/' << column;
    }
    dcs << "\033\\"sv; // ST

    _terminal.reply(dcs.str());
}

namespace
{
    std::string asHex(std::string_view _value)
    {
        std::string output;
        for (char const ch: _value)
            output += fmt::format("{:02X}", unsigned(ch));
        return output;
    }
} // namespace

template <typename Cell, ScreenType TheScreenType>
void Screen<Cell, TheScreenType>::requestCapability(std::string_view _name)
{
    if (booleanCapability(_name))
        _terminal.reply("\033P1+r{}\033\\", toHexString(_name));
    else if (auto const value = numericCapability(_name); value != Database::npos)
    {
        auto hexValue = fmt::format("{:X}", value);
        if (hexValue.size() % 2)
            hexValue.insert(hexValue.begin(), '0');
        _terminal.reply("\033P1+r{}={}\033\\", toHexString(_name), hexValue);
    }
    else if (auto const value = stringCapability(_name); !value.empty())
        _terminal.reply("\033P1+r{}={}\033\\", toHexString(_name), asHex(value));
    else
        _terminal.reply("\033P0+r\033\\");
}

template <typename Cell, ScreenType TheScreenType>
void Screen<Cell, TheScreenType>::requestCapability(capabilities::Code _code)
{
    if (booleanCapability(_code))
        _terminal.reply("\033P1+r{}\033\\", _code.hex());
    else if (auto const value = numericCapability(_code); value >= 0)
    {
        auto hexValue = fmt::format("{:X}", value);
        if (hexValue.size() % 2)
            hexValue.insert(hexValue.begin(), '0');
        _terminal.reply("\033P1+r{}={}\033\\", _code.hex(), hexValue);
    }
    else if (auto const value = stringCapability(_code); !value.empty())
        _terminal.reply("\033P1+r{}={}\033\\", _code.hex(), asHex(value));
    else
        _terminal.reply("\033P0+r\033\\");
}

template <typename Cell, ScreenType TheScreenType>
void Screen<Cell, TheScreenType>::resetDynamicColor(DynamicColorName _name)
{
    switch (_name)
    {
        case DynamicColorName::DefaultForegroundColor:
            _state.colorPalette.defaultForeground = _state.defaultColorPalette.defaultForeground;
            break;
        case DynamicColorName::DefaultBackgroundColor:
            _state.colorPalette.defaultBackground = _state.defaultColorPalette.defaultBackground;
            break;
        case DynamicColorName::TextCursorColor:
            _state.colorPalette.cursor = _state.defaultColorPalette.cursor;
            break;
        case DynamicColorName::MouseForegroundColor:
            _state.colorPalette.mouseForeground = _state.defaultColorPalette.mouseForeground;
            break;
        case DynamicColorName::MouseBackgroundColor:
            _state.colorPalette.mouseBackground = _state.defaultColorPalette.mouseBackground;
            break;
        case DynamicColorName::HighlightForegroundColor:
            _state.colorPalette.selectionForeground = _state.defaultColorPalette.selectionForeground;
            break;
        case DynamicColorName::HighlightBackgroundColor:
            _state.colorPalette.selectionBackground = _state.defaultColorPalette.selectionBackground;
            break;
    }
}

template <typename Cell, ScreenType TheScreenType>
void Screen<Cell, TheScreenType>::setDynamicColor(DynamicColorName _name, RGBColor _value)
{
    switch (_name)
    {
        case DynamicColorName::DefaultForegroundColor: _state.colorPalette.defaultForeground = _value; break;
        case DynamicColorName::DefaultBackgroundColor: _state.colorPalette.defaultBackground = _value; break;
        case DynamicColorName::TextCursorColor: _state.colorPalette.cursor.color = _value; break;
        case DynamicColorName::MouseForegroundColor: _state.colorPalette.mouseForeground = _value; break;
        case DynamicColorName::MouseBackgroundColor: _state.colorPalette.mouseBackground = _value; break;
        case DynamicColorName::HighlightForegroundColor:
            _state.colorPalette.selectionForeground = _value;
            break;
        case DynamicColorName::HighlightBackgroundColor:
            _state.colorPalette.selectionBackground = _value;
            break;
    }
}

template <typename Cell, ScreenType TheScreenType>
void Screen<Cell, TheScreenType>::inspect()
{
    _terminal.inspect();
}

template <typename Cell, ScreenType TheScreenType>
void Screen<Cell, TheScreenType>::inspect(std::string const& _message, std::ostream& _os) const
{
    auto const hline = [&]() {
        for_each(crispy::times(*_state.pageSize.columns), [&](auto) { _os << '='; });
        _os << endl;
    };

    auto const gridInfoLine = [&](Grid<Cell> const& grid) {
        return fmt::format("main page lines: scrollback cur {} max {}, main page lines {}, used lines "
                           "{}, zero index {}\n",
                           grid.historyLineCount(),
                           grid.maxHistoryLineCount(),
                           grid.pageSize().lines,
                           grid.linesUsed(),
                           grid.zero_index());
    };

    if (!_message.empty())
    {
        hline();
        _os << "\033[1;37;41m" << _message << "\033[m" << endl;
        hline();
    }

    _os << fmt::format("Rendered screen at the time of failure\n");
    _os << fmt::format("main page size       : {}\n", _state.pageSize);
    _os << fmt::format("history line count   : {} (max {})\n",
                       _terminal.primaryScreen().historyLineCount(),
                       _terminal.maxHistoryLineCount());
    _os << fmt::format("cursor position      : {}\n", _state.cursor);
    if (_state.cursor.originMode)
        _os << fmt::format("real cursor position : {})\n", toRealCoordinate(_state.cursor.position));
    _os << fmt::format("vertical margins     : {}\n", _state.margin.vertical);
    _os << fmt::format("horizontal margins   : {}\n", _state.margin.horizontal);
    _os << gridInfoLine(grid());

    hline();
    _os << screenshot([this](LineOffset _lineNo) -> string {
        // auto const absoluteLine = grid().toAbsoluteLine(_lineNo);
        return fmt::format("{} {:>4}: {}",
                           grid().lineAt(_lineNo).isTrivialBuffer() ? "|" : ":",
                           _lineNo.value,
                           grid().lineAt(_lineNo).flags());
    });
    hline();
    _state.imagePool.inspect(_os);
    hline();

    // TODO: print more useful debug information
    // - screen size
    // - left/right margin
    // - top/down margin
    // - cursor position
    // - autoWrap
    // - ... other output related modes
}

template <typename Cell, ScreenType TheScreenType>
void Screen<Cell, TheScreenType>::smGraphics(XtSmGraphics::Item _item,
                                             XtSmGraphics::Action _action,
                                             XtSmGraphics::Value _value)
{
    using Item = XtSmGraphics::Item;
    using Action = XtSmGraphics::Action;

    constexpr auto NumberOfColorRegistersItem = 1;
    constexpr auto SixelItem = 2;

    constexpr auto Success = 0;
    constexpr auto Failure = 3;

    switch (_item)
    {
        case Item::NumberOfColorRegisters:
            switch (_action)
            {
                case Action::Read: {
                    auto const value = _state.imageColorPalette->size();
                    _terminal.reply("\033[?{};{};{}S", NumberOfColorRegistersItem, Success, value);
                    break;
                }
                case Action::ReadLimit: {
                    auto const value = _state.imageColorPalette->maxSize();
                    _terminal.reply("\033[?{};{};{}S", NumberOfColorRegistersItem, Success, value);
                    break;
                }
                case Action::ResetToDefault: {
                    auto const value = _state.maxImageColorRegisters;
                    _state.imageColorPalette->setSize(value);
                    _terminal.reply("\033[?{};{};{}S", NumberOfColorRegistersItem, Success, value);
                    break;
                }
                case Action::SetToValue:
                    visit(overloaded {
                              [&](int _number) {
                                  _state.imageColorPalette->setSize(static_cast<unsigned>(_number));
                                  _terminal.reply(
                                      "\033[?{};{};{}S", NumberOfColorRegistersItem, Success, _number);
                              },
                              [&](ImageSize) {
                                  _terminal.reply("\033[?{};{};{}S", NumberOfColorRegistersItem, Failure, 0);
                              },
                              [&](monostate) {
                                  _terminal.reply("\033[?{};{};{}S", NumberOfColorRegistersItem, Failure, 0);
                              },
                          },
                          _value);
                    break;
            }
            break;

        case Item::SixelGraphicsGeometry:
            switch (_action)
            {
                case Action::Read: {
                    auto const viewportSize = pixelSize();
                    _terminal.reply("\033[?{};{};{};{}S",
                                    SixelItem,
                                    Success,
                                    min(viewportSize.width, _state.maxImageSize.width),
                                    min(viewportSize.height, _state.maxImageSize.height));
                }
                break;
                case Action::ReadLimit:
                    _terminal.reply("\033[?{};{};{};{}S",
                                    SixelItem,
                                    Success,
                                    _state.maxImageSizeLimit.width,
                                    _state.maxImageSizeLimit.height);
                    break;
                case Action::ResetToDefault:
                    // The limit is the default at the same time.
                    _state.maxImageSize = _state.maxImageSizeLimit;
                    break;
                case Action::SetToValue:
                    if (holds_alternative<ImageSize>(_value))
                    {
                        auto size = get<ImageSize>(_value);
                        size.width = min(size.width, _state.maxImageSizeLimit.width);
                        size.height = min(size.height, _state.maxImageSizeLimit.height);
                        _state.maxImageSize = size;
                        _terminal.reply("\033[?{};{};{};{}S", SixelItem, Success, size.width, size.height);
                    }
                    else
                        _terminal.reply("\033[?{};{};{}S", SixelItem, Failure, 0);
                    break;
            }
            break;

        case Item::ReGISGraphicsGeometry: // Surely, we don't do ReGIS just yet. :-)
            break;
    }
}
// }}}

// {{{ impl namespace (some command generator helpers)
namespace impl
{
    ApplyResult setAnsiMode(Sequence const& _seq, size_t _modeIndex, bool _enable, Terminal& term)
    {
        switch (_seq.param(_modeIndex))
        {
            case 2: // (AM) Keyboard Action Mode
                return ApplyResult::Unsupported;
            case 4: // (IRM) Insert Mode
                term.setMode(AnsiMode::Insert, _enable);
                return ApplyResult::Ok;
            case 12: // (SRM) Send/Receive Mode
            case 20: // (LNM) Automatic Newline
            default: return ApplyResult::Unsupported;
        }
    }

    optional<DECMode> toDECMode(unsigned _value)
    {
        switch (_value)
        {
            case 1: return DECMode::UseApplicationCursorKeys;
            case 2: return DECMode::DesignateCharsetUSASCII;
            case 3: return DECMode::Columns132;
            case 4: return DECMode::SmoothScroll;
            case 5: return DECMode::ReverseVideo;
            case 6: return DECMode::Origin;
            case 7: return DECMode::AutoWrap;
            // TODO: Ps = 8  -> Auto-repeat Keys (DECARM), VT100.
            case 9: return DECMode::MouseProtocolX10;
            case 10: return DECMode::ShowToolbar;
            case 12: return DECMode::BlinkingCursor;
            case 19: return DECMode::PrinterExtend;
            case 25: return DECMode::VisibleCursor;
            case 30: return DECMode::ShowScrollbar;
            // TODO: Ps = 3 5  -> Enable font-shifting functions (rxvt).
            // IGNORE? Ps = 3 8  -> Enter Tektronix Mode (DECTEK), VT240, xterm.
            // TODO: Ps = 4 0  -> Allow 80 -> 132 Mode, xterm.
            case 40: return DECMode::AllowColumns80to132;
            // IGNORE: Ps = 4 1  -> more(1) fix (see curses resource).
            // TODO: Ps = 4 2  -> Enable National Replacement Character sets (DECNRCM), VT220.
            // TODO: Ps = 4 4  -> Turn On Margin Bell, xterm.
            // TODO: Ps = 4 5  -> Reverse-wraparound Mode, xterm.
            case 46: return DECMode::DebugLogging;
            case 47: return DECMode::UseAlternateScreen;
            // TODO: Ps = 6 6  -> Application keypad (DECNKM), VT320.
            // TODO: Ps = 6 7  -> Backarrow key sends backspace (DECBKM), VT340, VT420.  This sets the
            // backarrowKey resource to "true".
            case 69: return DECMode::LeftRightMargin;
            case 80: return DECMode::SixelScrolling;
            case 1000: return DECMode::MouseProtocolNormalTracking;
            case 1001: return DECMode::MouseProtocolHighlightTracking;
            case 1002: return DECMode::MouseProtocolButtonTracking;
            case 1003: return DECMode::MouseProtocolAnyEventTracking;
            case 1004: return DECMode::FocusTracking;
            case 1005: return DECMode::MouseExtended;
            case 1006: return DECMode::MouseSGR;
            case 1007: return DECMode::MouseAlternateScroll;
            case 1015: return DECMode::MouseURXVT;
            case 1016: return DECMode::MouseSGRPixels;
            case 1047: return DECMode::UseAlternateScreen;
            case 1048: return DECMode::SaveCursor;
            case 1049: return DECMode::ExtendedAltScreen;
            case 2004: return DECMode::BracketedPaste;
            case 2026: return DECMode::BatchedRendering;
            case 2027: return DECMode::TextReflow;
            case 8452: return DECMode::SixelCursorNextToGraphic;
        }
        return nullopt;
    }

    ApplyResult setModeDEC(Sequence const& _seq, size_t _modeIndex, bool _enable, Terminal& term)
    {
        if (auto const modeOpt = toDECMode(_seq.param(_modeIndex)); modeOpt.has_value())
        {
            term.setMode(modeOpt.value(), _enable);
            return ApplyResult::Ok;
        }
        return ApplyResult::Invalid;
    }

    optional<RGBColor> parseColor(string_view const& _value)
    {
        try
        {
            // "rgb:RR/GG/BB"
            //  0123456789a
            if (_value.size() == 12 && _value.substr(0, 4) == "rgb:" && _value[6] == '/' && _value[9] == '/')
            {
                auto const r = crispy::to_integer<16, uint8_t>(_value.substr(4, 2));
                auto const g = crispy::to_integer<16, uint8_t>(_value.substr(7, 2));
                auto const b = crispy::to_integer<16, uint8_t>(_value.substr(10, 2));
                return RGBColor { r.value(), g.value(), b.value() };
            }

            // "#RRGGBB"
            if (_value.size() == 7 && _value[0] == '#')
            {
                auto const r = crispy::to_integer<16, uint8_t>(_value.substr(1, 2));
                auto const g = crispy::to_integer<16, uint8_t>(_value.substr(3, 2));
                auto const b = crispy::to_integer<16, uint8_t>(_value.substr(5, 2));
                return RGBColor { r.value(), g.value(), b.value() };
            }

            // "#RGB"
            if (_value.size() == 4 && _value[0] == '#')
            {
                auto const r = crispy::to_integer<16, uint8_t>(_value.substr(1, 1));
                auto const g = crispy::to_integer<16, uint8_t>(_value.substr(2, 1));
                auto const b = crispy::to_integer<16, uint8_t>(_value.substr(3, 1));
                auto const rr = static_cast<uint8_t>(r.value() << 4);
                auto const gg = static_cast<uint8_t>(g.value() << 4);
                auto const bb = static_cast<uint8_t>(b.value() << 4);
                return RGBColor { rr, gg, bb };
            }

            return std::nullopt;
        }
        catch (...)
        {
            // that will be a formatting error in stoul() then.
            return std::nullopt;
        }
    }

    Color parseColor(Sequence const& _seq, size_t* pi)
    {
        // We are at parameter index `i`.
        //
        // It may now follow:
        // - ":2::r:g:b"        RGB color
        // - ":3:F:C:M:Y"       CMY color  (F is scaling factor, what is max? 100 or 255?)
        // - ":4:F:C:M:Y:K"     CMYK color (F is scaling factor, what is max? 100 or 255?)
        // - ":5:P"
        // Sub-parameters can also be delimited with ';' and thus are no sub-parameters per-se.
        size_t i = *pi;
        auto const len = _seq.subParameterCount(i);
        if (_seq.subParameterCount(i) >= 1)
        {
            switch (_seq.param(i + 1))
            {
                case 2: // ":2::R:G:B" and ":2:R:G:B"
                {
                    if (len == 4 || len == 5)
                    {
                        // NB: subparam(i, 1) may be ignored
                        auto const r = _seq.subparam(i, len - 2);
                        auto const g = _seq.subparam(i, len - 1);
                        auto const b = _seq.subparam(i, len - 0);
                        if (r <= 255 && g <= 255 && b <= 255)
                        {
                            *pi += len;
                            return Color { RGBColor {
                                static_cast<uint8_t>(r), static_cast<uint8_t>(g), static_cast<uint8_t>(b) } };
                        }
                    }
                    break;
                }
                case 3: // ":3:F:C:M:Y" (TODO)
                case 4: // ":4:F:C:M:Y:K" (TODO)
                    *pi += len;
                    break;
                case 5: // ":5:P"
                    if (auto const P = _seq.subparam(i, 2); P <= 255)
                    {
                        *pi += len;
                        return static_cast<IndexedColor>(P);
                    }
                    break;
                default:
                    // XXX invalid sub parameter
                    break;
            }
        }

        // Compatibility mode, colors using ';' instead of ':'.
        if (i + 1 < _seq.parameterCount())
        {
            ++i;
            auto const mode = _seq.param(i);
            if (mode == 5)
            {
                if (i + 1 < _seq.parameterCount())
                {
                    ++i;
                    auto const value = _seq.param(i);
                    if (i <= 255)
                    {
                        *pi = i;
                        return static_cast<IndexedColor>(value);
                    }
                    else
                    {
                    } // TODO: _seq.logInvalidCSI("Invalid color indexing.");
                }
                else
                {
                } // TODO: _seq.logInvalidCSI("Missing color index.");
            }
            else if (mode == 2)
            {
                if (i + 3 < _seq.parameterCount())
                {
                    auto const r = _seq.param(i + 1);
                    auto const g = _seq.param(i + 2);
                    auto const b = _seq.param(i + 3);
                    i += 3;
                    if (r <= 255 && g <= 255 && b <= 255)
                    {
                        *pi = i;
                        return RGBColor { static_cast<uint8_t>(r),
                                          static_cast<uint8_t>(g),
                                          static_cast<uint8_t>(b) };
                    }
                    else
                    {
                    } // TODO: _seq.logInvalidCSI("RGB color out of range.");
                }
                else
                {
                } // TODO: _seq.logInvalidCSI("Invalid color mode.");
            }
            else
            {
            } // TODO: _seq.logInvalidCSI("Invalid color mode.");
        }
        else
        {
        } // TODO: _seq.logInvalidCSI("Invalid color indexing.");

        // failure case, skip this argument
        *pi = i + 1;
        return Color {};
    }

    template <typename Target>
    ApplyResult applySGR(Target& target, Sequence const& seq, size_t parameterStart, size_t parameterEnd)
    {
        if (parameterStart == parameterEnd)
        {
            target.setGraphicsRendition(GraphicsRendition::Reset);
            return ApplyResult::Ok;
        }

        for (size_t i = parameterStart; i < parameterEnd; ++i)
        {
            switch (seq.param(i))
            {
                case 0: target.setGraphicsRendition(GraphicsRendition::Reset); break;
                case 1: target.setGraphicsRendition(GraphicsRendition::Bold); break;
                case 2: target.setGraphicsRendition(GraphicsRendition::Faint); break;
                case 3: target.setGraphicsRendition(GraphicsRendition::Italic); break;
                case 4:
                    if (seq.subParameterCount(i) == 1)
                    {
                        switch (seq.subparam(i, 1))
                        {
                            case 0: target.setGraphicsRendition(GraphicsRendition::NoUnderline); break;
                            case 1: target.setGraphicsRendition(GraphicsRendition::Underline); break;
                            case 2: target.setGraphicsRendition(GraphicsRendition::DoublyUnderlined); break;
                            case 3: target.setGraphicsRendition(GraphicsRendition::CurlyUnderlined); break;
                            case 4: target.setGraphicsRendition(GraphicsRendition::DottedUnderline); break;
                            case 5: target.setGraphicsRendition(GraphicsRendition::DashedUnderline); break;
                            default: target.setGraphicsRendition(GraphicsRendition::Underline); break;
                        }
                        ++i;
                    }
                    else
                        target.setGraphicsRendition(GraphicsRendition::Underline);
                    break;
                case 5: target.setGraphicsRendition(GraphicsRendition::Blinking); break;
                case 7: target.setGraphicsRendition(GraphicsRendition::Inverse); break;
                case 8: target.setGraphicsRendition(GraphicsRendition::Hidden); break;
                case 9: target.setGraphicsRendition(GraphicsRendition::CrossedOut); break;
                case 21: target.setGraphicsRendition(GraphicsRendition::DoublyUnderlined); break;
                case 22: target.setGraphicsRendition(GraphicsRendition::Normal); break;
                case 23: target.setGraphicsRendition(GraphicsRendition::NoItalic); break;
                case 24: target.setGraphicsRendition(GraphicsRendition::NoUnderline); break;
                case 25: target.setGraphicsRendition(GraphicsRendition::NoBlinking); break;
                case 27: target.setGraphicsRendition(GraphicsRendition::NoInverse); break;
                case 28: target.setGraphicsRendition(GraphicsRendition::NoHidden); break;
                case 29: target.setGraphicsRendition(GraphicsRendition::NoCrossedOut); break;
                case 30: target.setForegroundColor(IndexedColor::Black); break;
                case 31: target.setForegroundColor(IndexedColor::Red); break;
                case 32: target.setForegroundColor(IndexedColor::Green); break;
                case 33: target.setForegroundColor(IndexedColor::Yellow); break;
                case 34: target.setForegroundColor(IndexedColor::Blue); break;
                case 35: target.setForegroundColor(IndexedColor::Magenta); break;
                case 36: target.setForegroundColor(IndexedColor::Cyan); break;
                case 37: target.setForegroundColor(IndexedColor::White); break;
                case 38: target.setForegroundColor(parseColor(seq, &i)); break;
                case 39: target.setForegroundColor(DefaultColor()); break;
                case 40: target.setBackgroundColor(IndexedColor::Black); break;
                case 41: target.setBackgroundColor(IndexedColor::Red); break;
                case 42: target.setBackgroundColor(IndexedColor::Green); break;
                case 43: target.setBackgroundColor(IndexedColor::Yellow); break;
                case 44: target.setBackgroundColor(IndexedColor::Blue); break;
                case 45: target.setBackgroundColor(IndexedColor::Magenta); break;
                case 46: target.setBackgroundColor(IndexedColor::Cyan); break;
                case 47: target.setBackgroundColor(IndexedColor::White); break;
                case 48: target.setBackgroundColor(parseColor(seq, &i)); break;
                case 49: target.setBackgroundColor(DefaultColor()); break;
                case 51: target.setGraphicsRendition(GraphicsRendition::Framed); break;
                case 53: target.setGraphicsRendition(GraphicsRendition::Overline); break;
                case 54: target.setGraphicsRendition(GraphicsRendition::NoFramed); break;
                case 55: target.setGraphicsRendition(GraphicsRendition::NoOverline); break;
                // 58 is reserved, but used for setting underline/decoration colors by some other VTEs
                // (such as mintty, kitty, libvte)
                case 58: target.setUnderlineColor(parseColor(seq, &i)); break;
                case 90: target.setForegroundColor(BrightColor::Black); break;
                case 91: target.setForegroundColor(BrightColor::Red); break;
                case 92: target.setForegroundColor(BrightColor::Green); break;
                case 93: target.setForegroundColor(BrightColor::Yellow); break;
                case 94: target.setForegroundColor(BrightColor::Blue); break;
                case 95: target.setForegroundColor(BrightColor::Magenta); break;
                case 96: target.setForegroundColor(BrightColor::Cyan); break;
                case 97: target.setForegroundColor(BrightColor::White); break;
                case 100: target.setBackgroundColor(BrightColor::Black); break;
                case 101: target.setBackgroundColor(BrightColor::Red); break;
                case 102: target.setBackgroundColor(BrightColor::Green); break;
                case 103: target.setBackgroundColor(BrightColor::Yellow); break;
                case 104: target.setBackgroundColor(BrightColor::Blue); break;
                case 105: target.setBackgroundColor(BrightColor::Magenta); break;
                case 106: target.setBackgroundColor(BrightColor::Cyan); break;
                case 107: target.setBackgroundColor(BrightColor::White); break;
                default: break; // TODO: logInvalidCSI("Invalid SGR number: {}", seq.param(i));
            }
        }
        return ApplyResult::Ok;
    }

    template <typename Cell, ScreenType ST>
    ApplyResult CPR(Sequence const& _seq, Screen<Cell, ST>& _screen)
    {
        switch (_seq.param(0))
        {
            case 5: _screen.deviceStatusReport(); return ApplyResult::Ok;
            case 6: _screen.reportCursorPosition(); return ApplyResult::Ok;
            default: return ApplyResult::Unsupported;
        }
    }

    template <typename Cell, ScreenType ST>
    ApplyResult DECRQPSR(Sequence const& _seq, Screen<Cell, ST>& _screen)
    {
        if (_seq.parameterCount() != 1)
            return ApplyResult::Invalid; // -> error
        else if (_seq.param(0) == 1)
            // TODO: https://vt100.net/docs/vt510-rm/DECCIR.html
            // TODO return emitCommand<RequestCursorState>(); // or call it with ...Detailed?
            return ApplyResult::Invalid;
        else if (_seq.param(0) == 2)
        {
            _screen.requestTabStops();
            return ApplyResult::Ok;
        }
        else
            return ApplyResult::Invalid;
    }

    template <typename Cell, ScreenType ST>
    ApplyResult DECSCUSR(Sequence const& _seq, Screen<Cell, ST>& _screen)
    {
        if (_seq.parameterCount() <= 1)
        {
            switch (_seq.param_or(0, Sequence::Parameter { 1 }))
            {
                case 0:
                case 1: _screen.setCursorStyle(CursorDisplay::Blink, CursorShape::Block); break;
                case 2: _screen.setCursorStyle(CursorDisplay::Steady, CursorShape::Block); break;
                case 3: _screen.setCursorStyle(CursorDisplay::Blink, CursorShape::Underscore); break;
                case 4: _screen.setCursorStyle(CursorDisplay::Steady, CursorShape::Underscore); break;
                case 5: _screen.setCursorStyle(CursorDisplay::Blink, CursorShape::Bar); break;
                case 6: _screen.setCursorStyle(CursorDisplay::Steady, CursorShape::Bar); break;
                default: return ApplyResult::Invalid;
            }
            return ApplyResult::Ok;
        }
        else
            return ApplyResult::Invalid;
    }

    template <typename Cell, ScreenType ST>
    ApplyResult EL(Sequence const& _seq, Screen<Cell, ST>& _screen)
    {
        switch (_seq.param_or(0, Sequence::Parameter { 0 }))
        {
            case 0: _screen.clearToEndOfLine(); break;
            case 1: _screen.clearToBeginOfLine(); break;
            case 2: _screen.clearLine(); break;
            default: return ApplyResult::Invalid;
        }
        return ApplyResult::Ok;
    }

    template <typename Cell, ScreenType ST>
    ApplyResult TBC(Sequence const& _seq, Screen<Cell, ST>& _screen)
    {
        if (_seq.parameterCount() != 1)
        {
            _screen.horizontalTabClear(HorizontalTabClear::UnderCursor);
            return ApplyResult::Ok;
        }

        switch (_seq.param(0))
        {
            case 0: _screen.horizontalTabClear(HorizontalTabClear::UnderCursor); break;
            case 3: _screen.horizontalTabClear(HorizontalTabClear::AllTabs); break;
            default: return ApplyResult::Invalid;
        }
        return ApplyResult::Ok;
    }

    inline std::unordered_map<std::string_view, std::string_view> parseSubParamKeyValuePairs(
        std::string_view const& s)
    {
        return crispy::splitKeyValuePairs(s, ':');
    }

    template <typename Cell, ScreenType ST>
    ApplyResult setOrRequestDynamicColor(Sequence const& _seq,
                                         Screen<Cell, ST>& _screen,
                                         DynamicColorName _name)
    {
        auto const& value = _seq.intermediateCharacters();
        if (value == "?")
            _screen.requestDynamicColor(_name);
        else if (auto color = parseColor(value); color.has_value())
            _screen.setDynamicColor(_name, color.value());
        else
            return ApplyResult::Invalid;

        return ApplyResult::Ok;
    }

    bool queryOrSetColorPalette(string_view _text,
                                std::function<void(uint8_t)> _queryColor,
                                std::function<void(uint8_t, RGBColor)> _setColor)
    {
        // Sequence := [Param (';' Param)*]
        // Param    := Index ';' Query | Set
        // Index    := DIGIT+
        // Query    := ?'
        // Set      := 'rgb:' Hex8 '/' Hex8 '/' Hex8
        // Hex8     := [0-9A-Za-z] [0-9A-Za-z]
        // DIGIT    := [0-9]
        int index = -1;
        return crispy::split(_text, ';', [&](string_view value) {
            if (index < 0)
            {
                index = crispy::to_integer<10, int>(value).value_or(-1);
                if (!(0 <= index && index <= 0xFF))
                    return false;
            }
            else if (value == "?"sv)
            {
                _queryColor((uint8_t) index);
                index = -1;
            }
            else if (auto const color = parseColor(value))
            {
                _setColor((uint8_t) index, color.value());
                index = -1;
            }
            else
                return false;

            return true;
        });
    }

    template <typename Cell, ScreenType ST>
    ApplyResult RCOLPAL(Sequence const& _seq, Screen<Cell, ST>& _screen)
    {
        if (_seq.intermediateCharacters().empty())
        {
            _screen.colorPalette() = _screen.defaultColorPalette();
            return ApplyResult::Ok;
        }

        auto const index = crispy::to_integer<10, uint8_t>(_seq.intermediateCharacters());
        if (!index.has_value())
            return ApplyResult::Invalid;

        _screen.colorPalette().palette[*index] = _screen.defaultColorPalette().palette[*index];

        return ApplyResult::Ok;
    }

    ApplyResult SETCOLPAL(Sequence const& _seq, Terminal& _terminal)
    {
        bool const ok = queryOrSetColorPalette(
            _seq.intermediateCharacters(),
            [&](uint8_t index) {
                auto const color = _terminal.colorPalette().palette.at(index);
                _terminal.reply("\033]4;{};rgb:{:04x}/{:04x}/{:04x}\033\\",
                                index,
                                static_cast<uint16_t>(color.red) << 8 | color.red,
                                static_cast<uint16_t>(color.green) << 8 | color.green,
                                static_cast<uint16_t>(color.blue) << 8 | color.blue);
            },
            [&](uint8_t index, RGBColor color) { _terminal.colorPalette().palette.at(index) = color; });

        return ok ? ApplyResult::Ok : ApplyResult::Invalid;
    }

    int toInt(string_view _value)
    {
        int out = 0;
        for (auto const ch: _value)
        {
            if (!(ch >= '0' && ch <= '9'))
                return 0;

            out = out * 10 + (ch - '0');
        }
        return out;
    }

    string autoFontFace(string_view _value, string_view _regular, string_view _style)
    {
        (void) _style;
        (void) _regular;
        return string(_value);
        // if (!_value.empty() && _value != "auto")
        //     return string(_value);
        // else
        //     return fmt::format("{}:style={}", _regular, _style);
    }

    ApplyResult setAllFont(Sequence const& _seq, Terminal& terminal)
    {
        // [read]  OSC 60 ST
        // [write] OSC 60 ; size ; regular ; bold ; italic ; bold italic ST
        auto const& params = _seq.intermediateCharacters();
        auto const splits = crispy::split(params, ';');
        auto const param = [&](unsigned _index) -> string_view {
            if (_index < splits.size())
                return splits.at(_index);
            else
                return {};
        };
        auto const emptyParams = [&]() -> bool {
            for (auto const& x: splits)
                if (!x.empty())
                    return false;
            return true;
        }();
        if (emptyParams)
        {
            auto const fonts = terminal.getFontDef();
            terminal.reply("\033]60;{};{};{};{};{};{}\033\\",
                           int(fonts.size * 100), // precission-shift
                           fonts.regular,
                           fonts.bold,
                           fonts.italic,
                           fonts.boldItalic,
                           fonts.emoji);
        }
        else
        {
            auto const size = double(toInt(param(0))) / 100.0;
            auto const regular = string(param(1));
            auto const bold = string(param(2));
            auto const italic = string(param(3));
            auto const boldItalic = string(param(4));
            auto const emoji = string(param(5));
            terminal.setFontDef(FontDef { size, regular, bold, italic, boldItalic, emoji });
        }
        return ApplyResult::Ok;
    }

    ApplyResult setFont(Sequence const& _seq, Terminal& terminal)
    {
        auto const& params = _seq.intermediateCharacters();
        auto const splits = crispy::split(params, ';');

        if (splits.size() != 1)
            return ApplyResult::Invalid;

        if (splits[0] != "?"sv)
        {
            auto fontDef = FontDef {};
            fontDef.regular = splits[0];
            terminal.setFontDef(fontDef);
        }
        else
        {
            auto const fonts = terminal.getFontDef();
            terminal.reply("\033]50;{}\033\\", fonts.regular);
        }

        return ApplyResult::Ok;
    }

    ApplyResult clipboard(Sequence const& _seq, Terminal& terminal)
    {
        // Only setting clipboard contents is supported, not reading.
        auto const& params = _seq.intermediateCharacters();
        if (auto const splits = crispy::split(params, ';'); splits.size() == 2 && splits[0] == "c")
        {
            terminal.copyToClipboard(crispy::base64::decode(splits[1]));
            return ApplyResult::Ok;
        }
        else
            return ApplyResult::Invalid;
    }

    template <typename Cell, ScreenType ST>
    ApplyResult NOTIFY(Sequence const& _seq, Screen<Cell, ST>& _screen)
    {
        auto const& value = _seq.intermediateCharacters();
        if (auto const splits = crispy::split(value, ';'); splits.size() == 3 && splits[0] == "notify")
        {
            _screen.notify(string(splits[1]), string(splits[2]));
            return ApplyResult::Ok;
        }
        else
            return ApplyResult::Unsupported;
    }

    template <typename Cell, ScreenType ST>
    ApplyResult SETCWD(Sequence const& _seq, Screen<Cell, ST>& _screen)
    {
        string const& url = _seq.intermediateCharacters();
        _screen.setCurrentWorkingDirectory(url);
        return ApplyResult::Ok;
    }

    ApplyResult CAPTURE(Sequence const& _seq, Terminal& terminal)
    {
        // CSI Mode ; [; Count] t
        //
        // Mode: 0 = physical lines
        //       1 = logical lines (unwrapped)
        //
        // Count: number of lines to capture from main page aera's bottom upwards
        //        If omitted or 0, the main page area's line count will be used.

        auto const logicalLines = _seq.param_or(0, 0);
        if (logicalLines != 0 && logicalLines != 1)
            return ApplyResult::Invalid;

        auto const lineCount = LineCount(_seq.param_or(1, *terminal.pageSize().lines));

        terminal.requestCaptureBuffer(lineCount, logicalLines);

        return ApplyResult::Ok;
    }

    template <typename Cell, ScreenType ST>
    ApplyResult HYPERLINK(Sequence const& _seq, Screen<Cell, ST>& _screen)
    {
        auto const& value = _seq.intermediateCharacters();
        // hyperlink_OSC ::= OSC '8' ';' params ';' URI
        // params := pair (':' pair)*
        // pair := TEXT '=' TEXT
        if (auto const pos = value.find(';'); pos != value.npos)
        {
            auto const paramsStr = value.substr(0, pos);
            auto const params = parseSubParamKeyValuePairs(paramsStr);

            auto id = string {};
            if (auto const p = params.find("id"); p != params.end())
                id = p->second;

            if (pos + 1 != value.size())
                _screen.hyperlink(id, value.substr(pos + 1));
            else
                _screen.hyperlink(string { id }, string {});

            return ApplyResult::Ok;
        }
        else
            _screen.hyperlink(string {}, string {});

        return ApplyResult::Ok;
    }

    template <typename Cell, ScreenType ST>
    ApplyResult saveDECModes(Sequence const& _seq, Screen<Cell, ST>& _screen)
    {
        vector<DECMode> modes;
        for (size_t i = 0; i < _seq.parameterCount(); ++i)
            if (optional<DECMode> mode = toDECMode(_seq.param(i)); mode.has_value())
                modes.push_back(mode.value());
        _screen.saveModes(modes);
        return ApplyResult::Ok;
    }

    template <typename Cell, ScreenType ST>
    ApplyResult restoreDECModes(Sequence const& _seq, Screen<Cell, ST>& _screen)
    {
        vector<DECMode> modes;
        for (size_t i = 0; i < _seq.parameterCount(); ++i)
            if (optional<DECMode> mode = toDECMode(_seq.param(i)); mode.has_value())
                modes.push_back(mode.value());
        _screen.restoreModes(modes);
        return ApplyResult::Ok;
    }

    ApplyResult WINDOWMANIP(Sequence const& _seq, Terminal& terminal)
    {
        if (_seq.parameterCount() == 3)
        {
            switch (_seq.param(0))
            {
                case 4: // resize in pixel units
                    terminal.resizeWindow(ImageSize { Width(_seq.param(2)), Height(_seq.param(1)) });
                    break;
                case 8: // resize in cell units
                    terminal.resizeWindow(PageSize { LineCount::cast_from(_seq.param(1)),
                                                     ColumnCount::cast_from(_seq.param(2)) });
                    break;
                case 22: terminal.saveWindowTitle(); break;
                case 23: terminal.restoreWindowTitle(); break;
                default: return ApplyResult::Unsupported;
            }
            return ApplyResult::Ok;
        }
        else if (_seq.parameterCount() == 2 || _seq.parameterCount() == 1)
        {
            switch (_seq.param(0))
            {
                case 4:
                case 8:
                    // this means, resize to full display size
                    // TODO: just create a dedicated callback for fulscreen resize!
                    terminal.resizeWindow(ImageSize {});
                    break;
                case 14:
                    if (_seq.parameterCount() == 2 && _seq.param(1) == 2)
                        terminal.primaryScreen().requestPixelSize(
                            RequestPixelSize::WindowArea); // CSI 14 ; 2 t
                    else
                        terminal.primaryScreen().requestPixelSize(RequestPixelSize::TextArea); // CSI 14 t
                    break;
                case 16: terminal.primaryScreen().requestPixelSize(RequestPixelSize::CellArea); break;
                case 18: terminal.primaryScreen().requestCharacterSize(RequestPixelSize::TextArea); break;
                case 19: terminal.primaryScreen().requestCharacterSize(RequestPixelSize::WindowArea); break;
                case 22: {
                    switch (_seq.param(1))
                    {
                        case 0: terminal.saveWindowTitle(); break; // CSI 22 ; 0 t | save icon & window title
                        case 1: return ApplyResult::Unsupported;   // CSI 22 ; 1 t | save icon title
                        case 2: terminal.saveWindowTitle(); break; // CSI 22 ; 2 t | save window title
                        default: return ApplyResult::Unsupported;
                    }
                    return ApplyResult::Ok;
                }
                case 23: {
                    switch (_seq.param(1))
                    {
                        case 0:
                            terminal.restoreWindowTitle();
                            break;                               // CSI 22 ; 0 t | save icon & window title
                        case 1: return ApplyResult::Unsupported; // CSI 22 ; 1 t | save icon title
                        case 2: terminal.restoreWindowTitle(); break; // CSI 22 ; 2 t | save window title
                        default: return ApplyResult::Unsupported;
                    }
                    return ApplyResult::Ok;
                }
            }
            return ApplyResult::Ok;
        }
        else
            return ApplyResult::Unsupported;
    }

    template <typename Cell, ScreenType ST>
    ApplyResult XTSMGRAPHICS(Sequence const& _seq, Screen<Cell, ST>& _screen)
    {
        auto const Pi = _seq.param<unsigned>(0);
        auto const Pa = _seq.param<unsigned>(1);
        auto const Pv = _seq.param_or<unsigned>(2, 0);
        auto const Pu = _seq.param_or<unsigned>(3, 0);

        auto const item = [&]() -> optional<XtSmGraphics::Item> {
            switch (Pi)
            {
                case 1: return XtSmGraphics::Item::NumberOfColorRegisters;
                case 2: return XtSmGraphics::Item::SixelGraphicsGeometry;
                case 3: return XtSmGraphics::Item::ReGISGraphicsGeometry;
                default: return nullopt;
            }
        }();
        if (!item.has_value())
            return ApplyResult::Invalid;

        auto const action = [&]() -> optional<XtSmGraphics::Action> {
            switch (Pa)
            {
                case 1: return XtSmGraphics::Action::Read;
                case 2: return XtSmGraphics::Action::ResetToDefault;
                case 3: return XtSmGraphics::Action::SetToValue;
                case 4: return XtSmGraphics::Action::ReadLimit;
                default: return nullopt;
            }
        }();
        if (!action.has_value())
            return ApplyResult::Invalid;

        if (*item != XtSmGraphics::Item::NumberOfColorRegisters && *action == XtSmGraphics::Action::SetToValue
            && (!Pv || !Pu))
            return ApplyResult::Invalid;

        auto const value = [&]() -> XtSmGraphics::Value {
            using Action = XtSmGraphics::Action;
            switch (*action)
            {
                case Action::Read:
                case Action::ResetToDefault:
                case Action::ReadLimit: return std::monostate {};
                case Action::SetToValue:
                    return *item == XtSmGraphics::Item::NumberOfColorRegisters
                               ? XtSmGraphics::Value { Pv }
                               : XtSmGraphics::Value { ImageSize { Width(Pv), Height(Pu) } };
            }
            return std::monostate {};
        }();

        _screen.smGraphics(*item, *action, value);

        return ApplyResult::Ok;
    }
} // namespace impl
// }}}

template <typename Cell, ScreenType TheScreenType>
void Screen<Cell, TheScreenType>::executeControlCode(char controlCode)
{
#if defined(LIBTERMINAL_LOG_TRACE)
    if (VTTraceSequenceLog)
        VTTraceSequenceLog()(
            "control U+{:02X} ({})", controlCode, to_string(static_cast<ControlCode::C0>(controlCode)));
#endif

    _terminal.state().instructionCounter++;
    switch (controlCode)
    {
        case 0x00: // NUL
            break;
        case 0x07: // BEL
            _terminal.bell();
            break;
        case 0x08: // BS
            backspace();
            break;
        case 0x09: // TAB
            moveCursorToNextTab();
            break;
        case 0x0A: // LF
            linefeed();
            break;
        case 0x0B: // VT
            // Even though VT means Vertical Tab, it seems that xterm is doing an IND instead.
            [[fallthrough]];
        case 0x0C: // FF
            // Even though FF means Form Feed, it seems that xterm is doing an IND instead.
            index();
            break;
        case 0x0D: moveCursorToBeginOfLine(); break;
        case 0x37: _terminal.saveCursor(); break;
        case 0x38: _terminal.restoreCursor(); break;
        default:
            // if (VTParserLog)
            //     VTParserLog()("Unsupported C0 sequence: {}", crispy::escape((uint8_t) controlCode));
            break;
    }
}

template <typename Cell, ScreenType TheScreenType>
void Screen<Cell, TheScreenType>::processSequence(Sequence const& seq)
{
#if defined(LIBTERMINAL_LOG_TRACE)
    if (VTTraceSequenceLog)
        VTTraceSequenceLog()("Handle VT sequence: {}", seq);
#endif

    // std::cerr << fmt::format("\t{} \t; {}\n", sequence_,
    //         sequence_.functionDefinition() ? sequence_.functionDefinition()->comment : ""sv);

    _terminal.state().instructionCounter++;
    if (FunctionDefinition const* funcSpec = seq.functionDefinition(); funcSpec != nullptr)
        applyAndLog(*funcSpec, seq);
    else if (VTParserLog)
        VTParserLog()("Unknown VT sequence: {}", seq);
}

template <typename Cell, ScreenType TheScreenType>
void Screen<Cell, TheScreenType>::applyAndLog(FunctionDefinition const& _function, Sequence const& _seq)
{
    auto const result = apply(_function, _seq);
    switch (result)
    {
        case ApplyResult::Invalid: {
            VTParserLog()("Invalid VT sequence: {}", _seq);
            break;
        }
        case ApplyResult::Unsupported: {
            VTParserLog()("Unsupported VT sequence: {}", _seq);
            break;
        }
        case ApplyResult::Ok: {
            _terminal.verifyState();
            break;
        }
    }
}

template <typename Cell, ScreenType TheScreenType>
ApplyResult Screen<Cell, TheScreenType>::apply(FunctionDefinition const& function, Sequence const& seq)
{
    // This function assumed that the incoming instruction has been already resolved to a given
    // FunctionDefinition
    switch (function)
    {
        // C0
        case BEL: _terminal.bell(); break;
        case BS: backspace(); break;
        case TAB: moveCursorToNextTab(); break;
        case LF: linefeed(); break;
        case VT: [[fallthrough]];
        case FF: index(); break;
        case CR: moveCursorToBeginOfLine(); break;

        // ESC
        case SCS_G0_SPECIAL: designateCharset(CharsetTable::G0, CharsetId::Special); break;
        case SCS_G0_USASCII: designateCharset(CharsetTable::G0, CharsetId::USASCII); break;
        case SCS_G1_SPECIAL: designateCharset(CharsetTable::G1, CharsetId::Special); break;
        case SCS_G1_USASCII: designateCharset(CharsetTable::G1, CharsetId::USASCII); break;
        case DECALN: screenAlignmentPattern(); break;
        case DECBI: backIndex(); break;
        case DECFI: forwardIndex(); break;
        case DECKPAM: applicationKeypadMode(true); break;
        case DECKPNM: applicationKeypadMode(false); break;
        case DECRS: _terminal.restoreCursor(); break;
        case DECSC: _terminal.saveCursor(); break;
        case HTS: horizontalTabSet(); break;
        case IND: index(); break;
        case NEL: moveCursorToNextLine(LineCount(1)); break;
        case RI: reverseIndex(); break;
        case RIS: _terminal.hardReset(); break;
        case SS2: singleShiftSelect(CharsetTable::G2); break;
        case SS3: singleShiftSelect(CharsetTable::G3); break;

        // CSI
        case ANSISYSSC: _terminal.restoreCursor(); break;
        case CBT:
            cursorBackwardTab(TabStopCount::cast_from(seq.param_or(0, Sequence::Parameter { 1 })));
            break;
        case CHA: moveCursorToColumn(seq.param_or<ColumnOffset>(0, ColumnOffset { 1 }) - 1); break;
        case CHT:
            cursorForwardTab(TabStopCount::cast_from(seq.param_or(0, Sequence::Parameter { 1 })));
            break;
        case CNL:
            moveCursorToNextLine(LineCount::cast_from(seq.param_or(0, Sequence::Parameter { 1 })));
            break;
        case CPL:
            moveCursorToPrevLine(LineCount::cast_from(seq.param_or(0, Sequence::Parameter { 1 })));
            break;
        case CPR: return impl::CPR(seq, *this);
        case CUB: moveCursorBackward(seq.param_or<ColumnCount>(0, ColumnCount { 1 })); break;
        case CUD: moveCursorDown(seq.param_or<LineCount>(0, LineCount { 1 })); break;
        case CUF: moveCursorForward(seq.param_or<ColumnCount>(0, ColumnCount { 1 })); break;
        case CUP:
            moveCursorTo(LineOffset::cast_from(seq.param_or<int>(0, 1) - 1),
                         ColumnOffset::cast_from(seq.param_or<int>(1, 1) - 1));
            break;
        case CUU: moveCursorUp(seq.param_or<LineCount>(0, LineCount { 1 })); break;
        case DA1: sendDeviceAttributes(); break;
        case DA2: sendTerminalId(); break;
        case DA3:
            // terminal identification, 4 hex codes
            _terminal.reply("\033P!|C0000000\033\\");
            break;
        case DCH: deleteCharacters(seq.param_or<ColumnCount>(0, ColumnCount { 1 })); break;
        case DECCARA: {
            auto const origin = this->origin();
            auto const top = LineOffset(seq.param_or(0, *origin.line + 1) - 1);
            auto const left = ColumnOffset(seq.param_or(1, *origin.column + 1) - 1);
            auto const bottom = LineOffset(seq.param_or(2, *pageSize().lines) - 1);
            auto const right = ColumnOffset(seq.param_or(3, *pageSize().columns) - 1);
            for (auto row = top; row <= bottom; ++row)
            {
                for (auto column = left; column <= right; ++column)
                {
                    auto& cell = at(row, column);
                    impl::applySGR(cell, seq, 4, seq.parameterCount());
                    // Maybe move setGraphicsRendition to Screen::cursor() ?
                }
            }
        }
        break;
        case DECCRA: {
            // The coordinates of the rectangular area are affected by the setting of origin mode (DECOM).
            // DECCRA is not affected by the page margins.
            auto const origin = this->origin();
            auto const top = Top(seq.param_or(0, *origin.line + 1) - 1);
            auto const left = Left(seq.param_or(1, *origin.column + 1) - 1);
            auto const bottom = Bottom(seq.param_or(2, *pageSize().lines) - 1);
            auto const right = Right(seq.param_or(3, *pageSize().columns) - 1);
            auto const page = seq.param_or(4, 0);

            auto const targetTop = LineOffset(seq.param_or(5, *origin.line + 1) - 1);
            auto const targetLeft = ColumnOffset(seq.param_or(6, *origin.column + 1) - 1);
            auto const targetTopLeft = CellLocation { targetTop, targetLeft };
            auto const targetPage = seq.param_or(7, 0);

            copyArea(Rect { top, left, bottom, right }, page, targetTopLeft, targetPage);
        }
        break;
        case DECERA: {
            // The coordinates of the rectangular area are affected by the setting of origin mode (DECOM).
            auto const origin = this->origin();
            auto const top = seq.param_or(0, *origin.line + 1) - 1;
            auto const left = seq.param_or(1, *origin.column + 1) - 1;

            // If the value of Pt, Pl, Pb, or Pr exceeds the width or height of the active page, then the
            // value is treated as the width or height of that page.
            auto const size = pageSize();
            auto const bottom = min(seq.param_or(2, unbox<int>(size.lines)), unbox<int>(size.lines)) - 1;
            auto const right = min(seq.param_or(3, unbox<int>(size.columns)), unbox<int>(size.columns)) - 1;

            eraseArea(top, left, bottom, right);
        }
        break;
        case DECFRA: {
            auto const ch = seq.param_or(0, Sequence::Parameter { 0 });
            // The coordinates of the rectangular area are affected by the setting of origin mode (DECOM).
            auto const origin = this->origin();
            auto const top = seq.param_or(0, origin.line);
            auto const left = seq.param_or(1, origin.column);

            // If the value of Pt, Pl, Pb, or Pr exceeds the width or height of the active page, then the
            // value is treated as the width or height of that page.
            auto const size = pageSize();
            auto const bottom = min(seq.param_or(2, *size.lines), *size.lines);
            auto const right = min(seq.param_or(3, *size.columns), *size.columns);

            fillArea(ch, *top, *left, bottom, right);
        }
        break;
        case DECDC: deleteColumns(seq.param_or(0, ColumnCount(1))); break;
        case DECIC: insertColumns(seq.param_or(0, ColumnCount(1))); break;
        case DECRM: {
            ApplyResult r = ApplyResult::Ok;
            crispy::for_each(crispy::times(seq.parameterCount()), [&](size_t i) {
                auto const t = impl::setModeDEC(seq, i, false, _terminal);
                r = max(r, t);
            });
            return r;
        }
        break;
        case DECRQM:
            if (seq.parameterCount() != 1)
                return ApplyResult::Invalid;
            requestDECMode(seq.param(0));
            return ApplyResult::Ok;
        case DECRQM_ANSI:
            if (seq.parameterCount() != 1)
                return ApplyResult::Invalid;
            requestAnsiMode(seq.param(0));
            return ApplyResult::Ok;
        case DECRQPSR: return impl::DECRQPSR(seq, *this);
        case DECSCUSR: return impl::DECSCUSR(seq, *this);
        case DECSCPP:
            if (auto const columnCount = seq.param_or(0, 80); columnCount == 80 || columnCount == 132)
            {
                // EXTENSION: only 80 and 132 are specced, but we allow any.
                _terminal.resizeColumns(ColumnCount(columnCount), false);
                return ApplyResult::Ok;
            }
            else
                return ApplyResult::Invalid;
        case DECSNLS:
            _terminal.resizeScreen(PageSize { pageSize().lines, seq.param<ColumnCount>(0) });
            return ApplyResult::Ok;
        case DECSLRM: {
            auto l = decr(seq.param_opt<ColumnOffset>(0));
            auto r = decr(seq.param_opt<ColumnOffset>(1));
            _terminal.setLeftRightMargin(l, r);
        }
        break;
        case DECSM: {
            ApplyResult r = ApplyResult::Ok;
            crispy::for_each(crispy::times(seq.parameterCount()), [&](size_t i) {
                auto const t = impl::setModeDEC(seq, i, true, _terminal);
                r = max(r, t);
            });
            return r;
        }
        case DECSTBM:
            _terminal.setTopBottomMargin(decr(seq.param_opt<LineOffset>(0)),
                                         decr(seq.param_opt<LineOffset>(1)));
            break;
        case DECSTR: _terminal.softReset(); break;
        case DECXCPR: reportExtendedCursorPosition(); break;
        case DL: deleteLines(seq.param_or(0, LineCount(1))); break;
        case ECH: eraseCharacters(seq.param_or(0, ColumnCount(1))); break;
        case ED:
            if (seq.parameterCount() == 0)
                clearToEndOfScreen();
            else
            {
                for (size_t i = 0; i < seq.parameterCount(); ++i)
                {
                    switch (seq.param(i))
                    {
                        case 0: clearToEndOfScreen(); break;
                        case 1: clearToBeginOfScreen(); break;
                        case 2: clearScreen(); break;
                        case 3:
                            grid().clearHistory();
                            _terminal.scrollbackBufferCleared();
                            break;
                    }
                }
            }
            break;
        case EL: return impl::EL(seq, *this);
        case HPA: moveCursorToColumn(seq.param<ColumnOffset>(0) - 1); break;
        case HPR: moveCursorForward(seq.param<ColumnCount>(0)); break;
        case HVP:
            moveCursorTo(seq.param_or(0, LineOffset(1)) - 1, seq.param_or(1, ColumnOffset(1)) - 1);
            break; // YES, it's like a CUP!
        case ICH: insertCharacters(seq.param_or(0, ColumnCount { 1 })); break;
        case IL: insertLines(seq.param_or(0, LineCount { 1 })); break;
        case REP:
            if (_terminal.state().precedingGraphicCharacter)
            {
                auto const requestedCount = seq.param<size_t>(0);
                auto const availableColumns =
                    (margin().horizontal.to - cursor().position.column).template as<size_t>();
                auto const effectiveCount = min(requestedCount, availableColumns);
                for (size_t i = 0; i < effectiveCount; i++)
                    writeText(_terminal.state().precedingGraphicCharacter);
            }
            break;
        case RM: {
            ApplyResult r = ApplyResult::Ok;
            crispy::for_each(crispy::times(seq.parameterCount()), [&](size_t i) {
                auto const t = impl::setAnsiMode(seq, i, false, _terminal);
                r = max(r, t);
            });
            return r;
        }
        break;
        case SCOSC: _terminal.saveCursor(); break;
        case SD: scrollDown(seq.param_or<LineCount>(0, LineCount { 1 })); break;
        case SETMARK: setMark(); break;
        case SGR: return impl::applySGR(_terminal, seq, 0, seq.parameterCount());
        case SM: {
            ApplyResult r = ApplyResult::Ok;
            crispy::for_each(crispy::times(seq.parameterCount()), [&](size_t i) {
                auto const t = impl::setAnsiMode(seq, i, true, _terminal);
                r = max(r, t);
            });
            return r;
        }
        case SU: scrollUp(seq.param_or<LineCount>(0, LineCount(1))); break;
        case TBC: return impl::TBC(seq, *this);
        case VPA: moveCursorToLine(seq.param_or<LineOffset>(0, LineOffset { 1 }) - 1); break;
        case WINMANIP: return impl::WINDOWMANIP(seq, _terminal);
        case DECMODERESTORE: return impl::restoreDECModes(seq, *this);
        case DECMODESAVE: return impl::saveDECModes(seq, *this);
        case XTSMGRAPHICS: return impl::XTSMGRAPHICS(seq, *this);
        case XTVERSION:
            _terminal.reply(fmt::format("\033P>|{} {}\033\\", LIBTERMINAL_NAME, LIBTERMINAL_VERSION_STRING));
            return ApplyResult::Ok;

        // OSC
        case SETTITLE:
            //(not supported) ChangeIconTitle(seq.intermediateCharacters());
            _terminal.setWindowTitle(seq.intermediateCharacters());
            return ApplyResult::Ok;
        case SETICON: return ApplyResult::Ok; // NB: Silently ignore!
        case SETWINTITLE: _terminal.setWindowTitle(seq.intermediateCharacters()); break;
        case SETXPROP: return ApplyResult::Unsupported;
        case SETCOLPAL: return impl::SETCOLPAL(seq, _terminal);
        case RCOLPAL: return impl::RCOLPAL(seq, *this);
        case SETCWD: return impl::SETCWD(seq, *this);
        case HYPERLINK: return impl::HYPERLINK(seq, *this);
        case CAPTURE: return impl::CAPTURE(seq, _terminal);
        case COLORFG:
            return impl::setOrRequestDynamicColor(seq, *this, DynamicColorName::DefaultForegroundColor);
        case COLORBG:
            return impl::setOrRequestDynamicColor(seq, *this, DynamicColorName::DefaultBackgroundColor);
        case COLORCURSOR:
            return impl::setOrRequestDynamicColor(seq, *this, DynamicColorName::TextCursorColor);
        case COLORMOUSEFG:
            return impl::setOrRequestDynamicColor(seq, *this, DynamicColorName::MouseForegroundColor);
        case COLORMOUSEBG:
            return impl::setOrRequestDynamicColor(seq, *this, DynamicColorName::MouseBackgroundColor);
        case SETFONT: return impl::setFont(seq, _terminal);
        case SETFONTALL: return impl::setAllFont(seq, _terminal);
        case CLIPBOARD: return impl::clipboard(seq, _terminal);
        // TODO: case COLORSPECIAL: return impl::setOrRequestDynamicColor(seq, _output,
        // DynamicColorName::HighlightForegroundColor);
        case RCOLORFG: resetDynamicColor(DynamicColorName::DefaultForegroundColor); break;
        case RCOLORBG: resetDynamicColor(DynamicColorName::DefaultBackgroundColor); break;
        case RCOLORCURSOR: resetDynamicColor(DynamicColorName::TextCursorColor); break;
        case RCOLORMOUSEFG: resetDynamicColor(DynamicColorName::MouseForegroundColor); break;
        case RCOLORMOUSEBG: resetDynamicColor(DynamicColorName::MouseBackgroundColor); break;
        case RCOLORHIGHLIGHTFG: resetDynamicColor(DynamicColorName::HighlightForegroundColor); break;
        case RCOLORHIGHLIGHTBG: resetDynamicColor(DynamicColorName::HighlightBackgroundColor); break;
        case NOTIFY: return impl::NOTIFY(seq, *this);
        case DUMPSTATE: inspect(); break;

        // hooks
        case DECSIXEL: _state.sequencer.hookParser(hookSixel(seq)); break;
        case STP: _state.sequencer.hookParser(hookSTP(seq)); break;
        case DECRQSS: _state.sequencer.hookParser(hookDECRQSS(seq)); break;
        case XTGETTCAP: _state.sequencer.hookParser(hookXTGETTCAP(seq)); break;

        default: return ApplyResult::Unsupported;
    }
    return ApplyResult::Ok;
}

template <typename Cell, ScreenType TheScreenType>
unique_ptr<ParserExtension> Screen<Cell, TheScreenType>::hookSixel(Sequence const& _seq)
{
    auto const Pa = _seq.param_or(0, 1);
    auto const Pb = _seq.param_or(1, 2);

    auto const aspectVertical = [](int Pa) {
        switch (Pa)
        {
            case 9:
            case 8:
            case 7: return 1;
            case 6:
            case 5: return 2;
            case 4:
            case 3: return 3;
            case 2: return 5;
            case 1:
            case 0:
            default: return 2;
        }
    }(Pa);

    auto const aspectHorizontal = 1;
    auto const transparentBackground = Pb == 1;

    sixelImageBuilder_ = make_unique<SixelImageBuilder>(
        _terminal.state().maxImageSize,
        aspectVertical,
        aspectHorizontal,
        transparentBackground ? RGBAColor { 0, 0, 0, 0 } : _terminal.state().colorPalette.defaultBackground,
        _terminal.state().usePrivateColorRegisters
            ? make_shared<SixelColorPalette>(_terminal.state().maxImageRegisterCount,
                                             clamp(_terminal.state().maxImageRegisterCount, 0u, 16384u))
            : _terminal.state().imageColorPalette);

    return make_unique<SixelParser>(*sixelImageBuilder_, [this]() {
        {
            sixelImage(sixelImageBuilder_->size(), move(sixelImageBuilder_->data()));
        }
    });
}

template <typename Cell, ScreenType TheScreenType>
unique_ptr<ParserExtension> Screen<Cell, TheScreenType>::hookSTP(Sequence const& /*_seq*/)
{
    return make_unique<SimpleStringCollector>(
        [this](string_view const& _data) { _terminal.setTerminalProfile(unicode::convert_to<char>(_data)); });
}

template <typename Cell, ScreenType TheScreenType>
unique_ptr<ParserExtension> Screen<Cell, TheScreenType>::hookXTGETTCAP(Sequence const& /*_seq*/)
{
    // DCS + q Pt ST
    //           Request Termcap/Terminfo String (XTGETTCAP), xterm.  The
    //           string following the "q" is a list of names encoded in
    //           hexadecimal (2 digits per character) separated by ; which
    //           correspond to termcap or terminfo key names.
    //           A few special features are also recognized, which are not key
    //           names:
    //
    //           o   Co for termcap colors (or colors for terminfo colors), and
    //
    //           o   TN for termcap name (or name for terminfo name).
    //
    //           o   RGB for the ncurses direct-color extension.
    //               Only a terminfo name is provided, since termcap
    //               applications cannot use this information.
    //
    //           xterm responds with
    //           DCS 1 + r Pt ST for valid requests, adding to Pt an = , and
    //           the value of the corresponding string that xterm would send,
    //           or
    //           DCS 0 + r Pt ST for invalid requests.
    //           The strings are encoded in hexadecimal (2 digits per
    //           character).

    return make_unique<SimpleStringCollector>([this](string_view const& _data) {
        auto const capsInHex = crispy::split(_data, ';');
        for (auto hexCap: capsInHex)
        {
            auto const hexCap8 = unicode::convert_to<char>(hexCap);
            if (auto const capOpt = crispy::fromHexString(string_view(hexCap8.data(), hexCap8.size())))
                requestCapability(capOpt.value());
        }
    });
}

template <typename Cell, ScreenType TheScreenType>
unique_ptr<ParserExtension> Screen<Cell, TheScreenType>::hookDECRQSS(Sequence const& /*_seq*/)
{
    return make_unique<SimpleStringCollector>([this](string_view const& _data) {
        auto const s = [](string_view _dataString) -> optional<RequestStatusString> {
            auto const mappings = array<pair<string_view, RequestStatusString>, 9> {
                pair { "m", RequestStatusString::SGR },       pair { "\"p", RequestStatusString::DECSCL },
                pair { " q", RequestStatusString::DECSCUSR }, pair { "\"q", RequestStatusString::DECSCA },
                pair { "r", RequestStatusString::DECSTBM },   pair { "s", RequestStatusString::DECSLRM },
                pair { "t", RequestStatusString::DECSLPP },   pair { "$|", RequestStatusString::DECSCPP },
                pair { "*|", RequestStatusString::DECSNLS }
            };
            for (auto const& mapping: mappings)
                if (_dataString == mapping.first)
                    return mapping.second;
            return nullopt;
        }(_data);

        if (s.has_value())
            requestStatusString(s.value());

        // TODO: handle batching
    });
}

} // namespace terminal

#include <terminal/Cell.h>
template class terminal::Screen<terminal::Cell, terminal::ScreenType::Primary>;
template class terminal::Screen<terminal::Cell, terminal::ScreenType::Alternate>;
