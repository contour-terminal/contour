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
#include <terminal/Screen.h>
#include <terminal/Commands.h>
#include <terminal/OutputGenerator.h>
#include <terminal/VTType.h>
#include <terminal/Logger.h>

#include <crispy/algorithm.h>
#include <crispy/escape.h>
#include <crispy/times.h>

#include <unicode/emoji_segmenter.h>
#include <unicode/word_segmenter.h>
#include <unicode/grapheme_segmenter.h>
#include <unicode/utf8.h>

#include <algorithm>
#include <iterator>
#include <sstream>

#if defined(LIBTERMINAL_EXECUTION_PAR)
#include <execution>
#define LIBTERMINAL_EXECUTION_COMMA(par) (std::execution:: par),
#else
#define LIBTERMINAL_EXECUTION_COMMA(par) /*!*/
#endif

using namespace std;
using namespace crispy;

namespace terminal {

string to_string(CharacterStyleMask _mask)
{
    return fmt::format("{}", _mask);
}

std::optional<size_t> ScreenBuffer::findPrevMarker(size_t _scrollOffset) const
{
    _scrollOffset = min(_scrollOffset, savedLines.size());

    if (_scrollOffset + 1 < savedLines.size())
    {
        auto const i = find_if(
            next(rbegin(savedLines), _scrollOffset + 1),
            rend(savedLines),
            [](Line const& line) -> bool { return line.marked; }
        );
        if (i != rend(savedLines))
            return distance(rbegin(savedLines), i);
    }

    return nullopt;
}

std::optional<size_t> ScreenBuffer::findNextMarker(size_t _scrollOffset) const
{
    _scrollOffset = min(_scrollOffset, savedLines.size());
    cursor_pos_t rowNumber = static_cast<cursor_pos_t>(_scrollOffset) - 1;

    if (rowNumber < savedLines.size())
        for (auto line = prev(end(savedLines), rowNumber); rowNumber > 0; ++line, --rowNumber)
            if (line->marked)
                return {rowNumber};

    // default to bottom
    return 0;
}

void ScreenBuffer::resize(WindowSize const& _newSize)
{
    if (_newSize.rows > size_.rows)
    {
        // Grow line count by splicing available lines from history back into buffer, if available,
        // or create new ones until size_.rows == _newSize.rows.
        auto const extendCount = _newSize.rows - size_.rows;
        auto const rowsToTakeFromSavedLines = min(extendCount, static_cast<unsigned int>(std::size(savedLines)));

        for_each(
            times(rowsToTakeFromSavedLines),
            [&](auto) {
                savedLines.back().resize(_newSize.columns);
                lines.emplace_front(std::move(savedLines.back()));
                savedLines.pop_back();
            }
        );

        cursor.row += rowsToTakeFromSavedLines;

        auto const fillLineCount = extendCount - rowsToTakeFromSavedLines;
        generate_n(
            back_inserter(lines),
            fillLineCount,
            [=]() { return Line{_newSize.columns, Cell{}}; });
    }
    else if (_newSize.rows < size_.rows)
    {
        // Shrink existing line count to _newSize.rows
        // by splicing the number of lines to be shrinked by into savedLines bottom.
        if (cursor.row == size_.rows)
        {
            auto const n = size_.rows - _newSize.rows;
            for_each(
                times(n),
                [&](auto) {
                    lines.front().resize(_newSize.columns);
                    savedLines.emplace_back(std::move(lines.front()));
                    lines.pop_front();
                }
            );
            clampSavedLines();
        }
        else
            // Hard-cut below cursor by the number of lines to shrink.
            lines.resize(_newSize.rows);

        assert(lines.size() == _newSize.rows);
    }

    if (_newSize.columns > size_.columns)
    {
        // Grow existing columns to _newSize.columns.
        std::for_each(
            begin(lines),
            end(lines),
            [=](auto& line) { line.resize(_newSize.columns); }
        );
        if (wrapPending)
            cursor.column++;
        wrapPending = false;
    }
    else if (_newSize.columns < size_.columns)
    {
        // Shrink existing columns to _newSize.columns.
        // Nothing should be done, I think, as we preserve prior (now exceeding) content.
        if (cursor.column == size_.columns)
            wrapPending = true;

        // truncating tabs
        while (!tabs.empty() && tabs.back() > _newSize.columns)
            tabs.pop_back();
    }

    // Reset margin to their default.
    margin_ = Margin{
		Margin::Range{1, _newSize.rows},
        Margin::Range{1, _newSize.columns}
    };
    // TODO: find out what to do with DECOM mode. Reset it to?

    size_ = _newSize;

    lastCursor = clampCoordinate(lastCursor);
    auto lastLine = next(begin(lines), lastCursor.row - 1);
    lastColumn = columnIteratorAt(begin(*lastLine), lastCursor.column);

    cursor = clampCoordinate(cursor);
    updateCursorIterators();
}

void ScreenBuffer::saveState()
{
    // https://vt100.net/docs/vt510-rm/DECSC.html

    // TODO: character sets
    // TODO: selective erase attribute
    // TODO: SS2/SS3 states
    savedStates.emplace(SavedState{
        realCursorPosition(),
        graphicsRendition,
        autoWrap,
        cursorRestrictedToMargin
    });
}

void ScreenBuffer::restoreState()
{
    if (!savedStates.empty())
    {
        auto const& saved = savedStates.top();
        moveCursorTo(saved.cursorPosition);
        setMode(Mode::AutoWrap, saved.autowrap);
        setMode(Mode::Origin, saved.originMode);
        savedStates.pop();
    }
}

void ScreenBuffer::setMode(Mode _mode, bool _enable)
{
    if (_mode != Mode::UseAlternateScreen)
    {
        if (_enable)
            enabledModes_.insert(_mode);
        else if (auto i = enabledModes_.find(_mode); i != enabledModes_.end())
            enabledModes_.erase(i);
    }

    // TODO: thse member variables aren't really needed anymore (are they?), remove them.
    switch (_mode)
    {
        case Mode::AutoWrap:
            autoWrap = _enable;
            break;
        case Mode::LeftRightMargin:
            // Resetting DECLRMM also resets the horizontal margins back to screen size.
            if (!_enable)
                margin_.horizontal = {1, size_.columns};
            break;
        case Mode::Origin:
            cursorRestrictedToMargin = _enable;
            break;
        case Mode::VisibleCursor:
            cursor.visible = _enable;
            break;
        default:
            break;
    }
}

void ScreenBuffer::moveCursorTo(Coordinate to)
{
    wrapPending = false;
    cursor = clampToScreen(toRealCoordinate(to));
    updateCursorIterators();
}

Screen::Cell& ScreenBuffer::withOriginAt(cursor_pos_t row, cursor_pos_t col)
{
    if (cursorRestrictedToMargin)
    {
        row += margin_.vertical.from - 1;
        col += margin_.horizontal.from - 1;
    }
    return at(row, col);
}

Screen::Cell& ScreenBuffer::at(cursor_pos_t _row, cursor_pos_t _col)
{
    assert(_row >= 1 && _row <= size_.rows);
    assert(_col >= 1 && _col <= size_.columns);
    assert(size_.rows == lines.size());

    return (*next(begin(lines), _row - 1))[_col - 1];
}

Screen::Cell const& ScreenBuffer::at(cursor_pos_t _row, cursor_pos_t _col) const
{
    return const_cast<ScreenBuffer*>(this)->at(_row, _col);
}

Screen::Cell& ScreenBuffer::at(Coordinate const& _coord)
{
    assert(_coord.row >= 1 && _coord.row <= size_.rows);
    assert(_coord.column >= 1 && _coord.column <= size_.columns);
    assert(size_.rows == lines.size());

    return (*next(begin(lines), _coord.row - 1))[_coord.column - 1];
}

Screen::Cell const& ScreenBuffer::at(Coordinate const& _coord) const
{
    return const_cast<ScreenBuffer*>(this)->at(_coord);
}

void ScreenBuffer::linefeed(cursor_pos_t _newColumn)
{
    wrapPending = false;

    if (realCursorPosition().row == margin_.vertical.to)
    {
        scrollUp(1);
        moveCursorTo({cursorPosition().row, _newColumn});
    }
    else
    {
        // using moveCursorTo() would embrace code reusage, but due to the fact that it's fully recalculating iterators,
        // it may be faster to just incrementally update them.
        // moveCursorTo({cursorPosition().row + 1, margin_.horizontal.from});
        cursor.row++;
        cursor.column = _newColumn;
        currentLine++;
        updateColumnIterator();
    }
    verifyState();
}

void ScreenBuffer::appendChar(char32_t _ch, bool _consecutive)
{
    verifyState();

    if (wrapPending && autoWrap)
        linefeed(margin_.horizontal.from);

    auto const ch = _ch == 0x7F ? ' ' : _ch;

    bool const insertToPrev =
        _consecutive
        && !lastColumn->empty()
        && unicode::grapheme_segmenter::nonbreakable(lastColumn->codepoint(lastColumn->codepointCount() - 1), ch);

    if (!insertToPrev)
        writeCharToCurrentAndAdvance(ch);
    else
    {
        auto const extendedWidth = lastColumn->appendCharacter(ch);
        lastColumn->setHyperlink(currentHyperlink);
        if (extendedWidth != 0)
            clearAndAdvance(extendedWidth);
    }
}

void ScreenBuffer::clearAndAdvance(unsigned _offset)
{
    if (_offset == 0)
        return;

    auto const n = min(_offset, margin_.horizontal.length() - cursor.column);
    if (n == _offset)
    {
        assert(n > 0);
        cursor.column += n;
        for (unsigned i = 0; i < n; ++i)
            (currentColumn++)->reset(graphicsRendition, currentHyperlink);
    }
    else if (autoWrap)
        wrapPending = true;
}

void ScreenBuffer::writeCharToCurrentAndAdvance(char32_t ch)
{
    Cell& cell = *currentColumn;
    cell.setCharacter(ch);
    cell.attributes() = graphicsRendition;
    cell.setHyperlink(currentHyperlink);

    lastColumn = currentColumn;
    lastCursor = cursor;

    bool const cursorInsideMargin = isModeEnabled(Mode::LeftRightMargin) && isCursorInsideMargins();
    auto const cellsAvailable = cursorInsideMargin ? margin_.horizontal.to - cursor.column
                                                   : size_.columns - cursor.column;

    auto const n = min(cell.width(), cellsAvailable);

    if (n == cell.width())
    {
        assert(n > 0);
        cursor.column += n;
        currentColumn++;
        for (unsigned i = 1; i < n; ++i)
            (currentColumn++)->reset(graphicsRendition, currentHyperlink);
        verifyState();
    }
    else if (autoWrap)
    {
        wrapPending = true;
        verifyState();
    }
    else
        verifyState();

}

void ScreenBuffer::scrollUp(cursor_pos_t v_n)
{
    scrollUp(v_n, margin_);
}

void ScreenBuffer::scrollUp(cursor_pos_t v_n, Margin const& margin)
{
    if (margin.horizontal != Margin::Range{1, size_.columns})
    {
        // a full "inside" scroll-up
        auto const marginHeight = margin.vertical.length();
        auto const n = min(v_n, marginHeight);

        if (n < marginHeight)
        {
            auto targetLine = next(begin(lines), margin.vertical.from - 1);     // target line
            auto sourceLine = next(begin(lines), margin.vertical.from - 1 + n); // source line
            auto const bottomLine = next(begin(lines), margin.vertical.to);     // bottom margin's end-line iterator

            for (; sourceLine != bottomLine; ++sourceLine, ++targetLine)
            {
                copy_n(
                    next(begin(*sourceLine), margin.horizontal.from - 1),
                    margin.horizontal.length(),
                    next(begin(*targetLine), margin.horizontal.from - 1)
                );
            }
        }

        // clear bottom n lines in margin.
        auto const topLine = next(begin(lines), margin.vertical.to - n);
        auto const bottomLine = next(begin(lines), margin.vertical.to);     // bottom margin's end-line iterator
        for_each(
            topLine,
            bottomLine,
            [&](ScreenBuffer::Line& line) {
                fill_n(
                    next(begin(line), margin.horizontal.from - 1),
                    margin.horizontal.length(),
                    Cell{{}, graphicsRendition}
                );
            }
        );
    }
    else if (margin.vertical == Margin::Range{1, size_.rows})
    {
        // full-screen scroll-up
        auto const n = min(v_n, size_.rows);

        if (n > 0)
        {
            for_each(
                times(n),
                [&](auto) {
                    savedLines.emplace_back(std::move(lines.front()));
                    lines.pop_front();
                }
            );

            clampSavedLines();

            generate_n(
                back_inserter(lines),
                n,
                [this]() { return Line{size_.columns, Cell{{}, graphicsRendition}}; }
            );
        }
    }
    else
    {
        // scroll up only inside vertical margin with full horizontal extend
        auto const marginHeight = margin.vertical.length();
        auto const n = min(v_n, marginHeight);
        if (n < marginHeight)
        {
            rotate(
                next(begin(lines), margin.vertical.from - 1),
                next(begin(lines), margin.vertical.from - 1 + n),
                next(begin(lines), margin.vertical.to)
            );
        }

        for_each(
            LIBTERMINAL_EXECUTION_COMMA(par)
            next(begin(lines), margin.vertical.to - n),
            next(begin(lines), margin.vertical.to),
            [&](Line& line) {
                fill(begin(line), end(line), Cell{{}, graphicsRendition});
            }
        );
    }

    updateCursorIterators();
}

void ScreenBuffer::scrollDown(cursor_pos_t v_n)
{
    scrollDown(v_n, margin_);
}

void ScreenBuffer::scrollDown(cursor_pos_t v_n, Margin const& _margin)
{
    auto const marginHeight = _margin.vertical.length();
    auto const n = min(v_n, marginHeight);

    if (_margin.horizontal != Margin::Range{1, size_.columns})
    {
        // full "inside" scroll-down
        if (n < marginHeight)
        {
            auto sourceLine = next(begin(lines), _margin.vertical.to - n - 1);
            auto targetLine = next(begin(lines), _margin.vertical.to - 1);
            auto const sourceEndLine = next(begin(lines), _margin.vertical.from - 1);

            while (sourceLine != sourceEndLine)
            {
                copy_n(
                    next(begin(*sourceLine), _margin.horizontal.from - 1),
                    _margin.horizontal.length(),
                    next(begin(*targetLine), _margin.horizontal.from - 1)
                );
                --targetLine;
                --sourceLine;
            }

            copy_n(
                next(begin(*sourceLine), _margin.horizontal.from - 1),
                _margin.horizontal.length(),
                next(begin(*targetLine), _margin.horizontal.from - 1)
            );

            for_each(
                next(begin(lines), _margin.vertical.from - 1),
                next(begin(lines), _margin.vertical.from - 1 + n),
                [_margin, this](Line& line) {
                    fill_n(
                        next(begin(line), _margin.horizontal.from - 1),
                        _margin.horizontal.length(),
                        Cell{{}, graphicsRendition}
                    );
                }
            );
        }
        else
        {
            // clear everything in margin
            for_each(
                next(begin(lines), _margin.vertical.from - 1),
                next(begin(lines), _margin.vertical.to),
                [_margin, this](Line& line) {
                    fill_n(
                        next(begin(line), _margin.horizontal.from - 1),
                        _margin.horizontal.length(),
                        Cell{{}, graphicsRendition}
                    );
                }
            );
        }
    }
    else if (_margin.vertical == Margin::Range{1, size_.rows})
    {
        rotate(
            begin(lines),
            next(begin(lines), marginHeight - n),
            end(lines)
        );

        for_each(
            begin(lines),
            next(begin(lines), n),
            [this](Line& line) {
                fill(
                    begin(line),
                    end(line),
                    Cell{{}, graphicsRendition}
                );
            }
        );
    }
    else
    {
        // scroll down only inside vertical margin with full horizontal extend
        rotate(
            next(begin(lines), _margin.vertical.from - 1),
            next(begin(lines), _margin.vertical.to - n),
            next(begin(lines), _margin.vertical.to)
        );

        for_each(
            next(begin(lines), _margin.vertical.from - 1),
            next(begin(lines), _margin.vertical.from - 1 + n),
            [this](Line& line) {
                fill(
                    begin(line),
                    end(line),
                    Cell{{}, graphicsRendition}
                );
            }
        );
    }

    updateCursorIterators();
}

void ScreenBuffer::deleteChars(cursor_pos_t _lineNo, cursor_pos_t _n)
{
    auto line = next(begin(lines), _lineNo - 1);
    auto column = next(begin(*line), realCursorPosition().column - 1);
    auto rightMargin = next(begin(*line), margin_.horizontal.to);
    auto const n = min(_n, static_cast<cursor_pos_t>(distance(column, rightMargin)));
    rotate(
        column,
        next(column, n),
        rightMargin
    );
    updateCursorIterators();
    rightMargin = next(begin(*line), margin_.horizontal.to);
    fill(
        prev(rightMargin, n),
        rightMargin,
        Cell{L' ', graphicsRendition}
    );
}

/// Inserts @p _n characters at given line @p _lineNo.
void ScreenBuffer::insertChars(cursor_pos_t _lineNo, cursor_pos_t _n)
{
    auto const n = min(_n, margin_.horizontal.to - cursorPosition().column + 1);

    auto line = next(begin(lines), _lineNo - 1);
    auto column0 = next(begin(*line), realCursorPosition().column - 1);
    auto column1 = next(begin(*line), margin_.horizontal.to - n);
    auto column2 = next(begin(*line), margin_.horizontal.to);

    rotate(
        column0,
        column1,
        column2
    );

    if (line == currentLine)
        updateColumnIterator();

    fill_n(
        columnIteratorAt(begin(*line), cursor.column),
        n,
        Cell{L' ', graphicsRendition}
    );
}

void ScreenBuffer::insertColumns(cursor_pos_t _n)
{
    for (cursor_pos_t lineNo = margin_.vertical.from; lineNo <= margin_.vertical.to; ++lineNo)
        insertChars(lineNo, _n);
}

void ScreenBuffer::setCurrentColumn(cursor_pos_t _n)
{
    auto const col = cursorRestrictedToMargin ? margin_.horizontal.from + _n - 1 : _n;
    auto const clampedCol = min(col, size_.columns);
    cursor.column = clampedCol;
    updateColumnIterator();

    verifyState();
}

bool ScreenBuffer::incrementCursorColumn(cursor_pos_t _n)
{
    auto const n = min(_n,  margin_.horizontal.length() - cursor.column);
    cursor.column += n;
    updateColumnIterator();
    verifyState();
    return n == _n;
}

void ScreenBuffer::clampSavedLines()
{
    if (maxHistoryLineCount_.has_value())
        while (savedLines.size() > maxHistoryLineCount_.value())
            savedLines.pop_front();
}

void ScreenBuffer::clearAllTabs()
{
    tabs.clear();
    tabWidth = 0;
}

void ScreenBuffer::clearTabUnderCursor()
{
    // populate tabs vector in case of default tabWidth is used (until now).
    if (tabs.empty() && tabWidth != 0)
        for (cursor_pos_t column = tabWidth; column <= size().columns; column += tabWidth)
            tabs.emplace_back(column);

    // erase the specific tab underneath
    if (auto i = find(begin(tabs), end(tabs), realCursorPosition().column); i != end(tabs))
        tabs.erase(i);
}

void ScreenBuffer::setTabUnderCursor()
{
    tabs.emplace_back(realCursorPosition().column);
    sort(begin(tabs), end(tabs));
}

void ScreenBuffer::verifyState() const
{
#if !defined(NDEBUG)
    auto const lrmm = isModeEnabled(Mode::LeftRightMargin);
    if (wrapPending &&
            ((lrmm && cursor.column != margin_.horizontal.to)
            || (!lrmm && cursor.column != size_.columns)))
    {
        fail(fmt::format(
            "Wrap is pending but cursor's column ({}) is not at right side of margin ({}) or screen ({}).",
            cursor.column, margin_.horizontal.to, size_.columns
        ));
    }

    if (size_.rows != lines.size())
        fail(fmt::format("Line count mismatch. Actual line count {} but should be {}.", lines.size(), size_.rows));

    // verify cursor positions
    [[maybe_unused]] auto const clampedCursor = clampToScreen(cursor);
    if (cursor != clampedCursor)
        fail(fmt::format("Cursor {} does not match clamp to screen {}.", cursor, clampedCursor));
    // FIXME: the above triggers on tmux vertical screen split (cursor.column off-by-one)

    // verify iterators
    [[maybe_unused]] auto const line = next(begin(lines), cursor.row - 1);
    [[maybe_unused]] auto const col = columnIteratorAt(cursor.column);

    if (line != currentLine)
        fail(fmt::format("Calculated current line does not match."));
    else if (col != currentColumn)
        fail(fmt::format("Calculated current column does not match."));

    if (wrapPending && cursor.column != size_.columns && cursor.column != margin_.horizontal.to)
        fail(fmt::format("wrapPending flag set when cursor is not in last column."));
#endif
}

void ScreenBuffer::dumpState(std::string const& _message) const
{
    auto const hline = [&]() {
        for_each(times(size_.columns), [](auto) { cerr << '='; });
        cerr << endl;
    };

    hline();
    cerr << "\033[1;37;41m" << _message << "\033[m" << endl;
    hline();

    cerr << fmt::format("Rendered screen at the time of failure: {}, cursor at {}", size_, cursor);
    if (cursorRestrictedToMargin)
        cerr << fmt::format(" (real: {})", toRealCoordinate(cursor));
    cerr << '\n';

    hline();
    cerr << screenshot();
    hline();

    // TODO: print more useful debug information
    // - screen size
    // - left/right margin
    // - top/down margin
    // - cursor position
    // - autoWrap
    // - wrapPending
    // - ... other output related modes

}

void ScreenBuffer::fail(std::string const& _message) const
{
    dumpState(_message);
    assert(false);
}

// ==================================================================================

Screen::Screen(WindowSize const& _size,
               optional<size_t> _maxHistoryLineCount,
               ModeSwitchCallback _useApplicationCursorKeys,
               function<void()> _onWindowTitleChanged,
               ResizeWindowCallback _resizeWindow,
               SetApplicationKeypadMode _setApplicationkeypadMode,
               SetBracketedPaste _setBracketedPaste,
               SetMouseProtocol _setMouseProtocol,
               SetMouseTransport _setMouseTransport,
               SetMouseWheelMode _setMouseWheelMode,
			   OnSetCursorStyle _setCursorStyle,
               Reply reply,
               Logger const& _logger,
               bool _logRaw,
               bool _logTrace,
               Hook onCommands,
               OnBufferChanged _onBufferChanged,
               std::function<void()> _bell,
               std::function<RGBColor(DynamicColorName)> _requestDynamicColor,
               std::function<void(DynamicColorName)> _resetDynamicColor,
               std::function<void(DynamicColorName, RGBColor const&)> _setDynamicColor,
               std::function<void(bool)> _setGenerateFocusEvents,
               NotifyCallback _notify
) :
    onCommands_{ move(onCommands) },
    logger_{ _logger },
    logRaw_{ _logRaw },
    logTrace_{ _logTrace },
    useApplicationCursorKeys_{ move(_useApplicationCursorKeys) },
    onWindowTitleChanged_{ move(_onWindowTitleChanged) },
    resizeWindow_{ move(_resizeWindow) },
    setApplicationkeypadMode_{ move(_setApplicationkeypadMode) },
    setBracketedPaste_{ move(_setBracketedPaste) },
    setMouseProtocol_{ move(_setMouseProtocol) },
    setMouseTransport_{ move(_setMouseTransport) },
    setMouseWheelMode_{ move(_setMouseWheelMode) },
	setCursorStyle_{ move(_setCursorStyle) },
    reply_{ move(reply) },
    commandBuilder_{ _logger },
    parser_{
        ref(commandBuilder_),
        [this](string const& _msg) { logger_(ParserErrorEvent{_msg}); }
    },
    primaryBuffer_{ ScreenBuffer::Type::Main, _size, _maxHistoryLineCount },
    alternateBuffer_{ ScreenBuffer::Type::Alternate, _size, nullopt },
    buffer_{ &primaryBuffer_ },
    size_{ _size },
    maxHistoryLineCount_{ _maxHistoryLineCount },
    onBufferChanged_{ move(_onBufferChanged) },
    bell_{ move(_bell) },
    requestDynamicColor_{ move(_requestDynamicColor) },
    resetDynamicColor_{ move(_resetDynamicColor) },
    setDynamicColor_{ move(_setDynamicColor) },
    setGenerateFocusEvents_{ move(_setGenerateFocusEvents) },
    notify_{ move(_notify) }
{
    (*this)(SetMode{Mode::AutoWrap, true});
}

void Screen::setMaxHistoryLineCount(std::optional<size_t> _maxHistoryLineCount)
{
    maxHistoryLineCount_ = _maxHistoryLineCount;

    primaryBuffer_.maxHistoryLineCount_ = _maxHistoryLineCount;
    primaryBuffer_.clampSavedLines();

    // Alternate buffer does not have a history usually (and for now we keep it that way).
}

size_t Screen::historyLineCount() const noexcept
{
    return buffer_->savedLines.size();
}

void Screen::resize(WindowSize const& _newSize)
{
    // TODO: only resize current screen buffer, and then make sure we resize the other upon actual switch
    primaryBuffer_.resize(_newSize);
    alternateBuffer_.resize(_newSize);
    size_ = _newSize;
}

void Screen::write(Command const& _command)
{
    buffer_->verifyState();
    visit(*this, _command);
    buffer_->verifyState();
    instructionCounter_++;

    if (onCommands_)
        onCommands_({_command});
}

void Screen::write(char const * _data, size_t _size)
{
#if defined(LIBTERMINAL_LOG_RAW)
    if (logRaw_ && logger_)
        logger_(RawOutputEvent{ escape(_data, _data + _size) });
#endif

    commandBuilder_.commands().clear();
    parser_.parseFragment(_data, _size);

    buffer_->verifyState();

    #if defined(LIBTERMINAL_LOG_TRACE)
    if (logTrace_ && logger_)
    {
        auto const traces = to_mnemonic(commandBuilder_.commands(), true, true);
        for (auto const& trace : traces)
            logger_(TraceOutputEvent{trace});
    }
    #endif

    for_each(
        commandBuilder_.commands(),
        [&](Command const& _command) {
            visit(*this, _command);
            instructionCounter_++;
            buffer_->verifyState();
        }
    );

    if (onCommands_)
        onCommands_(commandBuilder_.commands());
}

void Screen::write(std::u32string_view const& _text)
{
    for (char32_t codepoint : _text)
    {
        uint8_t bytes[4];
        auto const len = unicode::to_utf8(codepoint, bytes);
        write((char const*) bytes, len);
    }
}

void Screen::render(Renderer const& _render, size_t _scrollOffset) const
{
    if (!_scrollOffset)
    {
        for_each(
            times(1, size_.rows) * times(1, size_.columns),
            [&](auto pos) {
                auto const [row, col] = pos;
                _render(row, col, at(row, col));
            }
        );
    }
    else
    {
        _scrollOffset = min(_scrollOffset, buffer_->savedLines.size());
        auto const historyLineCount = min(size_.rows, static_cast<unsigned int>(_scrollOffset));
        auto const mainLineCount = size_.rows - historyLineCount;

        cursor_pos_t rowNumber = 1;
        for (auto line = prev(end(buffer_->savedLines), _scrollOffset); rowNumber <= historyLineCount; ++line, ++rowNumber)
        {
            if (line->size() < size_.columns)
                line->resize(size_.columns);

            auto column = begin(*line);
            for (cursor_pos_t colNumber = 1; colNumber <= size_.columns; ++colNumber, ++column)
                _render(rowNumber, colNumber, *column);
        }

        for (auto line = begin(buffer_->lines); line != next(begin(buffer_->lines), mainLineCount); ++line, ++rowNumber)
        {
            auto column = begin(*line);
            for (cursor_pos_t colNumber = 1; colNumber <= size_.columns; ++colNumber, ++column)
                _render(rowNumber, colNumber, *column);
        }
    }
}

string Screen::renderHistoryTextLine(cursor_pos_t _lineNumberIntoHistory) const
{
    assert(1 <= _lineNumberIntoHistory && _lineNumberIntoHistory <= buffer_->savedLines.size());
    string line;
    line.reserve(size_.columns);
    auto const lineIter = next(buffer_->savedLines.rbegin(), _lineNumberIntoHistory - 1);
    for (Cell const& cell : *lineIter)
        if (cell.codepointCount())
            line += cell.toUtf8();
        else
            line += " "; // fill character

    return line;
}

string ScreenBuffer::renderTextLine(cursor_pos_t row) const
{
    string line;
    line.reserve(size_.columns);
    for (cursor_pos_t col = 1; col <= size_.columns; ++col)
        if (auto const& cell = at(row, col); cell.codepointCount())
            line += cell.toUtf8();
        else
            line += " "; // fill character

    return line;
}

string ScreenBuffer::renderText() const
{
    string text;
    text.reserve(size_.rows * (size_.columns + 1));

    for (cursor_pos_t row = 1; row <= size_.rows; ++row)
    {
        text += renderTextLine(row);
        text += '\n';
    }

    return text;
}

std::string ScreenBuffer::screenshot() const
{
    auto result = std::stringstream{};
    auto generator = OutputGenerator{ result };

    for (cursor_pos_t const row : times(1, size_.rows))
    {
        for (cursor_pos_t const col : times(1, size_.columns))
        {
            Cell const& cell = at(row, col);

            //TODO: some kind of: generator(SetGraphicsRendition{ cell.attributes().styles });
            if (cell.attributes().styles & CharacterStyleMask::Bold)
                generator(SetGraphicsRendition{GraphicsRendition::Bold});
            else
                generator(SetGraphicsRendition{GraphicsRendition::Normal});

            generator(SetForegroundColor{ cell.attributes().foregroundColor });
            generator(SetBackgroundColor{ cell.attributes().backgroundColor });

            if (!cell.codepointCount())
                generator(AppendChar{ U' ' });
            else
                for (char32_t const ch : cell.codepoints())
                    generator(AppendChar{ ch });
        }
        generator(SetGraphicsRendition{GraphicsRendition::Reset});
        generator(MoveCursorToBeginOfLine{});
        generator(Linefeed{});
    }

    return result.str();
}

// {{{ viewport management
bool Screen::isAbsoluteLineVisible(cursor_pos_t _row) const noexcept
{
    return _row >= historyLineCount() - scrollOffset_
        && _row <= historyLineCount() - scrollOffset_ + size().rows;
}

bool Screen::scrollUp(size_t _numLines)
{
    if (isAlternateScreen()) // TODO: make configurable
        return false;

    if (auto const newOffset = min(scrollOffset_ + _numLines, historyLineCount()); newOffset != scrollOffset_)
    {
        scrollOffset_ = newOffset;
        return true;
    }
    else
        return false;
}

bool Screen::scrollDown(size_t _numLines)
{
    if (isAlternateScreen()) // TODO: make configurable
        return false;

    if (auto const newOffset = scrollOffset_ >= _numLines ? scrollOffset_ - _numLines : 0; newOffset != scrollOffset_)
    {
        scrollOffset_ = newOffset;
        return true;
    }
    else
        return false;
}

bool Screen::scrollMarkUp()
{
    if (auto const newScrollOffset = findPrevMarker(scrollOffset_); newScrollOffset.has_value())
    {
        scrollOffset_ = 1 + newScrollOffset.value();
        return true;
    }

    return false;
}

bool Screen::scrollMarkDown()
{
    if (auto const newScrollOffset = findNextMarker(scrollOffset_); newScrollOffset.has_value())
    {
        scrollOffset_ = newScrollOffset.value();
        return true;
    }

    return false;
}

bool Screen::scrollToTop()
{
    if (auto top = historyLineCount(); top != scrollOffset_)
    {
        scrollOffset_ = top;
        return true;
    }
    else
        return false;
}

bool Screen::scrollToBottom()
{
    if (scrollOffset_ != 0)
    {
        scrollOffset_ = 0;
        return true;
    }
    else
        return false;
}
// }}}

// {{{ ops
void Screen::operator()(Bell const&)
{
    if (bell_)
        bell_();
}

void Screen::operator()(FullReset const&)
{
    resetHard();
}

void Screen::operator()(Linefeed const&)
{
    if (isModeEnabled(Mode::AutomaticNewLine))
        buffer_->linefeed(buffer_->margin_.horizontal.from);
    else
        buffer_->linefeed(realCursorPosition().column);
}

void Screen::operator()(Backspace const&)
{
    moveCursorTo({cursorPosition().row, cursorPosition().column > 1 ? cursorPosition().column - 1 : 1});
}

void Screen::operator()(DeviceStatusReport const&)
{
    reply("\033[0n");
}

void Screen::operator()(ReportCursorPosition const&)
{
    reply("\033[{};{}R", cursorPosition().row, cursorPosition().column);
}

void Screen::operator()(ReportExtendedCursorPosition const&)
{
    auto const pageNum = 1;
    reply("\033[{};{};{}R", cursorPosition().row, cursorPosition().column, pageNum);
}

void Screen::operator()(SendDeviceAttributes const&)
{
    // See https://vt100.net/docs/vt510-rm/DA1.html

    auto const id = [&]() -> string_view {
        switch (terminalId_)
        {
            case VTType::VT100:
                return "1";
            case VTType::VT220:
            case VTType::VT240:
                return "62";
            case VTType::VT320:
            case VTType::VT330:
            case VTType::VT340:
                return "63";
            case VTType::VT420:
                return "64";
            case VTType::VT510:
            case VTType::VT520:
            case VTType::VT525:
                return "65";
        }
        return "1"; // Should never be reached.
    }();

    auto const attrs = to_params(
        DeviceAttributes::AnsiColor |
        DeviceAttributes::AnsiTextLocator |
        DeviceAttributes::Columns132 |
        //TODO: DeviceAttributes::NationalReplacementCharacterSets |
        //TODO: DeviceAttributes::RectangularEditing |
        //TODO: DeviceAttributes::SelectiveErase |
        //TODO: DeviceAttributes::SixelGraphics |
        //TODO: DeviceAttributes::TechnicalCharacters |
        DeviceAttributes::UserDefinedKeys
    );

    reply("\033[?{};{}c", id, attrs);
}

void Screen::operator()(SendTerminalId const&)
{
    // Note, this is "Secondary DA".
    // It requests for the terminalID

    // terminal protocol type
    auto const Pp = static_cast<unsigned>(terminalId_);

    // version number
    // TODO: (PACKAGE_VERSION_MAJOR * 100 + PACKAGE_VERSION_MINOR) * 100 + PACKAGE_VERSION_MICRO
    auto constexpr Pv = (LIBTERMINAL_VERSION_MAJOR * 100 + LIBTERMINAL_VERSION_MINOR) * 100 + LIBTERMINAL_VERSION_PATCH;

    // ROM cardridge registration number (always 0)
    auto constexpr Pc = 0;

    reply("\033[>{};{};{}c", Pp, Pv, Pc);
}

void Screen::operator()(ClearToEndOfScreen const&)
{
    if (isAlternateScreen() && buffer_->cursor.row == 1 && buffer_->cursor.column == 1)
        buffer_->hyperlinks.clear();

    (*this)(ClearToEndOfLine{});

    for_each(
        LIBTERMINAL_EXECUTION_COMMA(par)
        next(buffer_->currentLine),
        end(buffer_->lines),
        [&](ScreenBuffer::Line& line) {
            fill(begin(line), end(line), Cell{{}, buffer_->graphicsRendition});
        }
    );
}

void Screen::operator()(ClearToBeginOfScreen const&)
{
    (*this)(ClearToBeginOfLine{});

    for_each(
        LIBTERMINAL_EXECUTION_COMMA(par)
        begin(buffer_->lines),
        buffer_->currentLine,
        [&](ScreenBuffer::Line& line) {
            fill(begin(line), end(line), Cell{{}, buffer_->graphicsRendition});
        }
    );
}

void Screen::operator()(ClearScreen const&)
{
    for_each(
        LIBTERMINAL_EXECUTION_COMMA(par)
        begin(buffer_->lines),
        end(buffer_->lines),
        [&](ScreenBuffer::Line& line) {
            fill(begin(line), end(line), Cell{{}, buffer_->graphicsRendition});
        }
    );
}

void Screen::operator()(ClearScrollbackBuffer const&)
{
    buffer_->savedLines.clear();
}

void Screen::operator()(EraseCharacters const& v)
{
    // Spec: https://vt100.net/docs/vt510-rm/ECH.html
    // It's not clear from the spec how to perform erase when inside margin and number of chars to be erased would go outside margins.
    // TODO: See what xterm does ;-)
    size_t const n = min(buffer_->size_.columns - realCursorPosition().column + 1, v.n == 0 ? 1 : v.n);
    fill_n(buffer_->currentColumn, n, Cell{{}, buffer_->graphicsRendition});
}

void Screen::operator()(ScrollUp const& v)
{
    buffer_->scrollUp(v.n);
}

void Screen::operator()(ScrollDown const& v)
{
    buffer_->scrollDown(v.n);
}

void Screen::operator()(ClearToEndOfLine const&)
{
    fill(
        buffer_->currentColumn,
        end(*buffer_->currentLine),
        Cell{{}, buffer_->graphicsRendition}
    );
}

void Screen::operator()(ClearToBeginOfLine const&)
{
    fill(
        begin(*buffer_->currentLine),
        next(buffer_->currentColumn),
        Cell{{}, buffer_->graphicsRendition}
    );
}

void Screen::operator()(ClearLine const&)
{
    fill(
        begin(*buffer_->currentLine),
        end(*buffer_->currentLine),
        Cell{{}, buffer_->graphicsRendition}
    );
}

void Screen::operator()(CursorNextLine const& v)
{
    buffer_->moveCursorTo({cursorPosition().row + v.n, 1});
}

void Screen::operator()(CursorPreviousLine const& v)
{
    auto const n = min(v.n, cursorPosition().row - 1);
    buffer_->moveCursorTo({cursorPosition().row - n, 1});
}

void Screen::operator()(InsertCharacters const& v)
{
    if (isCursorInsideMargins())
        buffer_->insertChars(realCursorPosition().row, v.n);
}

void Screen::operator()(InsertLines const& v)
{
    if (isCursorInsideMargins())
    {
        buffer_->scrollDown(
            v.n,
            Margin{
                { buffer_->cursor.row, buffer_->margin_.vertical.to },
                buffer_->margin_.horizontal
            }
        );
    }
}

void Screen::operator()(InsertColumns const& v)
{
    if (isCursorInsideMargins())
        buffer_->insertColumns(v.n);
}

void Screen::operator()(DeleteLines const& v)
{
    if (isCursorInsideMargins())
    {
        buffer_->scrollUp(
            v.n,
            Margin{
                { buffer_->cursor.row, buffer_->margin_.vertical.to },
                buffer_->margin_.horizontal
            }
        );
    }
}

void Screen::operator()(DeleteCharacters const& v)
{
    if (isCursorInsideMargins() && v.n != 0)
        buffer_->deleteChars(realCursorPosition().row, v.n);
}

void Screen::operator()(DeleteColumns const& v)
{
    if (isCursorInsideMargins())
        for (cursor_pos_t lineNo = buffer_->margin_.vertical.from; lineNo <= buffer_->margin_.vertical.to; ++lineNo)
            buffer_->deleteChars(lineNo, v.n);
}

void Screen::operator()(HorizontalPositionAbsolute const& v)
{
    // HPA: We only care about column-mode (not pixel/inches) for now.
    (*this)(MoveCursorToColumn{v.n});
}

void Screen::operator()(HorizontalPositionRelative const& v)
{
    // HPR: We only care about column-mode (not pixel/inches) for now.
    (*this)(MoveCursorForward{v.n});
}

void Screen::operator()(HorizontalTabClear const& v)
{
    switch (v.which)
    {
        case HorizontalTabClear::AllTabs:
            buffer_->clearAllTabs();
            break;
        case HorizontalTabClear::UnderCursor:
            buffer_->clearTabUnderCursor();
            break;
    }
}

void Screen::operator()(HorizontalTabSet const&)
{
    buffer_->setTabUnderCursor();
}

void Screen::operator()(Hyperlink const& v)
{
    if (v.uri.empty())
        buffer_->currentHyperlink = nullptr;
    else if (v.id.empty())
        buffer_->currentHyperlink = make_shared<HyperlinkInfo>(HyperlinkInfo{v.id, v.uri});
    else if (auto i = buffer_->hyperlinks.find(v.id); i != buffer_->hyperlinks.end())
        buffer_->currentHyperlink = i->second;
    else
    {
        buffer_->currentHyperlink = make_shared<HyperlinkInfo>(HyperlinkInfo{v.id, v.uri});
        buffer_->hyperlinks[v.id] = buffer_->currentHyperlink;
    }
    // TODO:
    // Care about eviction.
    // Move hyperlink store into ScreenBuffer, so it gets reset upon every switch into
    // alternate screen (not for main screen!)
}

void Screen::operator()(MoveCursorUp const& v)
{
    auto const n = min(v.n, cursorPosition().row - buffer_->margin_.vertical.from);
    buffer_->cursor.row -= n;
    buffer_->currentLine = prev(buffer_->currentLine, n);
    buffer_->setCurrentColumn(cursorPosition().column);
    buffer_->verifyState();
}

void Screen::operator()(MoveCursorDown const& v)
{
    auto const n = min(v.n, size_.rows - cursorPosition().row);
    buffer_->cursor.row += n;
    buffer_->currentLine = next(buffer_->currentLine, n);
    buffer_->setCurrentColumn(cursorPosition().column);
}

void Screen::operator()(MoveCursorForward const& v)
{
    buffer_->incrementCursorColumn(v.n);
}

void Screen::operator()(MoveCursorBackward const& v)
{
    // even if you move to 80th of 80 columns, it'll first write a char and THEN flag wrap pending
    buffer_->wrapPending = false;

    // TODO: skip cells that in counting when iterating backwards over a wide cell (such as emoji)
    auto const n = min(v.n, buffer_->cursor.column - 1);
    buffer_->setCurrentColumn(buffer_->cursor.column - n);
}

void Screen::operator()(MoveCursorToColumn const& v)
{
    buffer_->wrapPending = false;

    buffer_->setCurrentColumn(v.column);
}

void Screen::operator()(MoveCursorToBeginOfLine const&)
{
    buffer_->wrapPending = false;

    buffer_->setCurrentColumn(1);
}

void Screen::operator()(MoveCursorTo const& v)
{
    moveCursorTo(Coordinate{v.row, v.column});
}

void Screen::operator()(MoveCursorToLine const& v)
{
    moveCursorTo({v.row, buffer_->cursor.column});
}

void Screen::operator()(MoveCursorToNextTab const&)
{
    // TODO: I guess something must remember when a \t was added, for proper move-back?
    // TODO: respect HTS/TBC

    if (!buffer_->tabs.empty())
    {
        // advance to the next tab
        size_t i = 0;
        while (i < buffer_->tabs.size() && buffer_->realCursorPosition().column >= buffer_->tabs[i])
            ++i;

        auto const currentCursorColumn = cursorPosition().column;

        if (i < buffer_->tabs.size())
            (*this)(MoveCursorForward{buffer_->tabs[i] - currentCursorColumn});
        else if (buffer_->realCursorPosition().column < buffer_->margin_.horizontal.to)
            (*this)(MoveCursorForward{buffer_->margin_.horizontal.to - currentCursorColumn});
        else
            (*this)(CursorNextLine{1});
    }
    else if (buffer_->tabWidth)
    {
        // default tab settings
        if (buffer_->realCursorPosition().column < buffer_->margin_.horizontal.to)
        {
            auto const n = min(
                buffer_->tabWidth - (buffer_->cursor.column - 1) % buffer_->tabWidth,
                size_.columns - cursorPosition().column
            );
            (*this)(MoveCursorForward{n});
        }
        else
            (*this)(CursorNextLine{1});
    }
    else
    {
        // no tab stops configured
        if (buffer_->realCursorPosition().column < buffer_->margin_.horizontal.to)
            // then TAB moves to the end of the screen
            (*this)(MoveCursorToColumn{buffer_->margin_.horizontal.to});
        else
            // then TAB moves to next line left margin
            (*this)(CursorNextLine{1});
    }
}

void Screen::operator()(Notify const& _notify)
{
    cout << "Screen.NOTIFY: title: '" << _notify.title << "', content: '" << _notify.content << "'\n";
    if (notify_)
        notify_(_notify.title, _notify.content);
}

void Screen::operator()(CursorBackwardTab const& v)
{
    if (v.count == 0)
        return;

    if (!buffer_->tabs.empty())
    {
        for (unsigned k = 0; k < v.count; ++k)
        {
            auto const i = std::find_if(rbegin(buffer_->tabs), rend(buffer_->tabs),
                                        [&](auto tabPos) -> bool {
                                            return tabPos <= cursorPosition().column - 1;
                                        });
            if (i != rend(buffer_->tabs))
            {
                // prev tab found -> move to prev tab
                (*this)(MoveCursorToColumn{*i});
            }
            else
            {
                (*this)(MoveCursorToColumn{buffer_->margin_.horizontal.from});
                break;
            }
        }
    }
    else if (buffer_->tabWidth)
    {
        // default tab settings
        if (buffer_->cursor.column <= buffer_->tabWidth)
            (*this)(MoveCursorToBeginOfLine{});
        else
        {
            auto const m = buffer_->cursor.column % buffer_->tabWidth;
            auto const n = m
                         ? (v.count - 1) * buffer_->tabWidth + m
                         : v.count * buffer_->tabWidth + m;
            (*this)(MoveCursorBackward{n - 1});
        }
    }
    else
    {
        // no tab stops configured
        (*this)(MoveCursorToBeginOfLine{});
    }
}

void Screen::operator()(SaveCursor const&)
{
    buffer_->saveState();
}

void Screen::operator()(RestoreCursor const&)
{
    buffer_->restoreState();
}

void Screen::operator()(Index const&)
{
    if (realCursorPosition().row == buffer_->margin_.vertical.to)
        buffer_->scrollUp(1);
    else
        moveCursorTo({cursorPosition().row + 1, cursorPosition().column});
}

void Screen::operator()(ReverseIndex const&)
{
    if (realCursorPosition().row == buffer_->margin_.vertical.from)
        buffer_->scrollDown(1);
    else
        moveCursorTo({cursorPosition().row - 1, cursorPosition().column});
}

void Screen::operator()(BackIndex const&)
{
    if (realCursorPosition().column == buffer_->margin_.horizontal.from)
        ;// TODO: scrollRight(1);
    else
        moveCursorTo({cursorPosition().row, cursorPosition().column - 1});
}

void Screen::operator()(ForwardIndex const&)
{
    if (realCursorPosition().column == buffer_->margin_.horizontal.to)
        ;// TODO: scrollLeft(1);
    else
        moveCursorTo({cursorPosition().row, cursorPosition().column + 1});
}

void Screen::operator()(SetForegroundColor const& v)
{
    buffer_->graphicsRendition.foregroundColor = v.color;
}

void Screen::operator()(SetBackgroundColor const& v)
{
    buffer_->graphicsRendition.backgroundColor = v.color;
}

void Screen::operator()(SetUnderlineColor const& v)
{
    buffer_->graphicsRendition.underlineColor = v.color;
}

void Screen::operator()(SetCursorStyle const& v)
{
	if (setCursorStyle_)
		setCursorStyle_(v.display, v.shape);
}

void Screen::operator()(SetGraphicsRendition const& v)
{
    // TODO: optimize this as there are only 3 cases
    // 1.) reset
    // 2.) set some bits |=
    // 3.) clear some bits &= ~
    switch (v.rendition)
    {
        case GraphicsRendition::Reset:
            buffer_->graphicsRendition = {};
            break;
        case GraphicsRendition::Bold:
            buffer_->graphicsRendition.styles |= CharacterStyleMask::Bold;
            break;
        case GraphicsRendition::Faint:
            buffer_->graphicsRendition.styles |= CharacterStyleMask::Faint;
            break;
        case GraphicsRendition::Italic:
            buffer_->graphicsRendition.styles |= CharacterStyleMask::Italic;
            break;
        case GraphicsRendition::Underline:
            buffer_->graphicsRendition.styles |= CharacterStyleMask::Underline;
            break;
        case GraphicsRendition::Blinking:
            buffer_->graphicsRendition.styles |= CharacterStyleMask::Blinking;
            break;
        case GraphicsRendition::Inverse:
            buffer_->graphicsRendition.styles |= CharacterStyleMask::Inverse;
            break;
        case GraphicsRendition::Hidden:
            buffer_->graphicsRendition.styles |= CharacterStyleMask::Hidden;
            break;
        case GraphicsRendition::CrossedOut:
            buffer_->graphicsRendition.styles |= CharacterStyleMask::CrossedOut;
            break;
        case GraphicsRendition::DoublyUnderlined:
            buffer_->graphicsRendition.styles |= CharacterStyleMask::DoublyUnderlined;
            break;
        case GraphicsRendition::CurlyUnderlined:
            buffer_->graphicsRendition.styles |= CharacterStyleMask::CurlyUnderlined;
            break;
        case GraphicsRendition::DottedUnderline:
            buffer_->graphicsRendition.styles |= CharacterStyleMask::DottedUnderline;
            break;
        case GraphicsRendition::DashedUnderline:
            buffer_->graphicsRendition.styles |= CharacterStyleMask::DashedUnderline;
            break;
        case GraphicsRendition::Normal:
            buffer_->graphicsRendition.styles &= ~(CharacterStyleMask::Bold | CharacterStyleMask::Faint);
            break;
        case GraphicsRendition::NoItalic:
            buffer_->graphicsRendition.styles &= ~CharacterStyleMask::Italic;
            break;
        case GraphicsRendition::NoUnderline:
            buffer_->graphicsRendition.styles &= ~CharacterStyleMask::Underline;
            break;
        case GraphicsRendition::NoBlinking:
            buffer_->graphicsRendition.styles &= ~CharacterStyleMask::Blinking;
            break;
        case GraphicsRendition::NoInverse:
            buffer_->graphicsRendition.styles &= ~CharacterStyleMask::Inverse;
            break;
        case GraphicsRendition::NoHidden:
            buffer_->graphicsRendition.styles &= ~CharacterStyleMask::Hidden;
            break;
        case GraphicsRendition::NoCrossedOut:
            buffer_->graphicsRendition.styles &= ~CharacterStyleMask::CrossedOut;
            break;
    }
}

void Screen::operator()(SetMark const&)
{
    buffer_->currentLine->marked = true;
}

void Screen::operator()(SetMode const& v)
{
    buffer_->setMode(v.mode, v.enable);

    switch (v.mode)
    {
        case Mode::UseAlternateScreen:
            if (v.enable)
                setBuffer(ScreenBuffer::Type::Alternate);
            else
                setBuffer(ScreenBuffer::Type::Main);
            break;
        case Mode::UseApplicationCursorKeys:
            if (useApplicationCursorKeys_)
                useApplicationCursorKeys_(v.enable);
            if (isAlternateScreen() && setMouseWheelMode_)
            {
                if (v.enable)
                    setMouseWheelMode_(InputGenerator::MouseWheelMode::ApplicationCursorKeys);
                else
                    setMouseWheelMode_(InputGenerator::MouseWheelMode::NormalCursorKeys);
            }
            break;
        case Mode::BracketedPaste:
            if (setBracketedPaste_)
                setBracketedPaste_(v.enable);
            break;
        case Mode::MouseSGR:
            if (setMouseTransport_)
                setMouseTransport_(MouseTransport::SGR);
            break;
        case Mode::MouseExtended:
            if (setMouseTransport_)
                setMouseTransport_(MouseTransport::Extended);
            break;
        case Mode::MouseURXVT:
            if (setMouseTransport_)
                setMouseTransport_(MouseTransport::URXVT);
            break;
        case Mode::MouseAlternateScroll:
            if (setMouseWheelMode_)
            {
                if (v.enable)
                    setMouseWheelMode_(InputGenerator::MouseWheelMode::ApplicationCursorKeys);
                else
                    setMouseWheelMode_(InputGenerator::MouseWheelMode::NormalCursorKeys);
            }
            break;
        case Mode::FocusTracking:
            if (setGenerateFocusEvents_)
                setGenerateFocusEvents_(v.enable);
            break;
        default:
            break;
    }
}

void Screen::operator()(RequestMode const& v)
{
    enum class ModeResponse { // TODO: respect response 0, 3, 4.
        NotRecognized = 0,
        Set = 1,
        Reset = 2,
        PermanentlySet = 3,
        PermanentlyReset = 4
    };

    ModeResponse const modeResponse = isModeEnabled(v.mode)
        ? ModeResponse::Set
        : ModeResponse::Reset;

    if (isAnsiMode(v.mode))
        reply("\033[{};{}$y", to_code(v.mode), static_cast<unsigned>(modeResponse));
    else
        reply("\033[?{};{}$y", to_code(v.mode), static_cast<unsigned>(modeResponse));
}

void Screen::operator()(SetTopBottomMargin const& _margin)
{
	auto const bottom = _margin.bottom.has_value()
		? min(_margin.bottom.value(), size_.rows)
		: size_.rows;

	auto const top = _margin.top.value_or(1);

	if (top < bottom)
    {
        buffer_->margin_.vertical.from = top;
        buffer_->margin_.vertical.to = bottom;
        buffer_->moveCursorTo({1, 1});
    }
}

void Screen::operator()(SetLeftRightMargin const& margin)
{
    if (isModeEnabled(Mode::LeftRightMargin))
    {
		auto const right = margin.right.has_value()
			? min(margin.right.value(), size_.columns)
			: size_.columns;
		auto const left = margin.left.value_or(1);
		if (left + 1 < right)
        {
            buffer_->margin_.horizontal.from = left;
            buffer_->margin_.horizontal.to = right;
            buffer_->moveCursorTo({1, 1});
        }
    }
}

void Screen::operator()(ScreenAlignmentPattern const&)
{
    // sets the margins to the extremes of the page
    buffer_->margin_.vertical.from = 1;
    buffer_->margin_.vertical.to = size_.rows;
    buffer_->margin_.horizontal.from = 1;
    buffer_->margin_.horizontal.to = size_.columns;

    // and moves the cursor to the home position
    moveCursorTo({1, 1});

    // fills the complete screen area with a test pattern
    for_each(
        LIBTERMINAL_EXECUTION_COMMA(par)
        begin(buffer_->lines),
        end(buffer_->lines),
        [&](ScreenBuffer::Line& line) {
            fill(
                LIBTERMINAL_EXECUTION_COMMA(par)
                begin(line),
                end(line),
                ScreenBuffer::Cell{'X', buffer_->graphicsRendition}
            );
        }
    );
}

void Screen::operator()(SendMouseEvents const& v)
{
    if (setMouseProtocol_)
        setMouseProtocol_(v.protocol, v.enable);
}

void Screen::operator()(ApplicationKeypadMode const& v)
{
    if (setApplicationkeypadMode_)
        setApplicationkeypadMode_(v.enable);
}

void Screen::operator()(DesignateCharset const&)
{
    // TODO
}

void Screen::operator()(SingleShiftSelect const&)
{
    // TODO
}

void Screen::operator()(SoftTerminalReset const&)
{
    resetSoft();
}

void Screen::operator()(ChangeWindowTitle const& v)
{
    windowTitle_ = v.title;

    if (onWindowTitleChanged_)
        onWindowTitleChanged_();
}

void Screen::operator()(SaveWindowTitle const&)
{
    savedWindowTitles_.push(windowTitle_);
}

void Screen::operator()(RestoreWindowTitle const&)
{
    if (!savedWindowTitles_.empty())
    {
        windowTitle_ = savedWindowTitles_.top();
        savedWindowTitles_.pop();

        if (onWindowTitleChanged_)
            onWindowTitleChanged_();
    }
}

void Screen::operator()(ResizeWindow const& v)
{
    if (resizeWindow_)
        resizeWindow_(v.width, v.height, v.unit == ResizeWindow::Unit::Pixels);
}

void Screen::operator()(AppendChar const& v)
{
    buffer_->appendChar(v.ch, instructionCounter_ == 1);
    instructionCounter_ = 0;
}

void Screen::operator()(RequestDynamicColor const& v)
{
    if (requestDynamicColor_)
    {
        reply("\033]{};{}\x07",
            setDynamicColorCommand(v.name),
            setDynamicColorValue(requestDynamicColor_(v.name))
        );
    }
}

void Screen::operator()(RequestTabStops const&)
{
    // Response: `DCS 2 $ u Pt ST`
    ostringstream dcs;
    dcs << "\033P2$u"; // DCS
    if (!buffer_->tabs.empty())
    {
        for (size_t const i : times(buffer_->tabs.size()))
        {
            if (i)
                dcs << '/';
            dcs << buffer_->tabs[i];
        }
    }
    else if (buffer_->tabWidth != 0)
    {
        dcs << buffer_->tabWidth + 1;
        for (unsigned column = 2 * buffer_->tabWidth + 1; column <= size().columns; column += buffer_->tabWidth)
            dcs << '/' << column;
    }
    dcs << '\x5c'; // ST

    reply(dcs.str());
}

void Screen::operator()(ResetDynamicColor const& v)
{
    if (resetDynamicColor_)
        resetDynamicColor_(v.name);
}

void Screen::operator()(SetDynamicColor const& v)
{
    if (setDynamicColor_)
        setDynamicColor_(v.name, v.color);
}

void Screen::operator()(DumpState const&)
{
    buffer_->dumpState("Dumping screen state");
}

// }}}

// {{{ others
void Screen::resetSoft()
{
    (*this)(SetGraphicsRendition{GraphicsRendition::Reset}); // SGR
    (*this)(MoveCursorTo{1, 1}); // DECSC (Save cursor state)
    (*this)(SetMode{Mode::VisibleCursor, true}); // DECTCEM (Text cursor enable)
    (*this)(SetMode{Mode::Origin, false}); // DECOM
    (*this)(SetMode{Mode::KeyboardAction, false}); // KAM
    (*this)(SetMode{Mode::AutoWrap, false}); // DECAWM
    (*this)(SetMode{Mode::Insert, false}); // IRM
    (*this)(SetMode{Mode::UseApplicationCursorKeys, false}); // DECCKM (Cursor keys)
    (*this)(SetTopBottomMargin{1, size().rows}); // DECSTBM
    (*this)(SetLeftRightMargin{1, size().columns}); // DECRLM

    // TODO: DECNKM (Numeric keypad)
    // TODO: DECSCA (Select character attribute)
    // TODO: DECNRCM (National replacement character set)
    // TODO: GL, GR (G0, G1, G2, G3)
    // TODO: DECAUPSS (Assign user preference supplemental set)
    // TODO: DECSASD (Select active status display)
    // TODO: DECKPM (Keyboard position mode)
    // TODO: DECPCTERM (PCTerm mode)
}

void Screen::resetHard()
{
    primaryBuffer_.reset();
    alternateBuffer_.reset();
    setBuffer(ScreenBuffer::Type::Main);
}

Screen::Cell const& Screen::absoluteAt(Coordinate const& _coord) const
{
    if (_coord.row <= buffer_->savedLines.size())
        return *next(begin(*next(begin(buffer_->savedLines), _coord.row - 1)), _coord.column - 1);
    else if (auto const rowNr = _coord.row - static_cast<cursor_pos_t>(buffer_->savedLines.size()); rowNr <= size_.rows)
        return at(rowNr, _coord.column);
    else
        throw invalid_argument{"Row number exceeds boundaries."};
}

Screen::Cell const& Screen::at(cursor_pos_t _rowNr, cursor_pos_t _colNr) const noexcept
{
    return buffer_->at(Coordinate{_rowNr, _colNr});
}

void Screen::moveCursorTo(Coordinate to)
{
    buffer_->wrapPending = false;
    buffer_->moveCursorTo(to);
}

void Screen::setBuffer(ScreenBuffer::Type _type)
{
    if (bufferType() != _type)
    {
        switch (_type)
        {
            case ScreenBuffer::Type::Main:
                if (setMouseWheelMode_)
                    setMouseWheelMode_(InputGenerator::MouseWheelMode::Default);
                buffer_ = &primaryBuffer_;
                break;
            case ScreenBuffer::Type::Alternate:
                if (buffer_->isModeEnabled(Mode::MouseAlternateScroll))
                    setMouseWheelMode_(InputGenerator::MouseWheelMode::ApplicationCursorKeys);
                else
                    setMouseWheelMode_(InputGenerator::MouseWheelMode::NormalCursorKeys);
                buffer_ = &alternateBuffer_;
                break;
        }
        if (onBufferChanged_)
            onBufferChanged_(_type);
    }
}
// }}}

} // namespace terminal
