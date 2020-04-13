/**
 * This file is part of the "libterminal" project
 *   Copyright (c) 2019 Christian Parpart <christian@parpart.family>
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
#include <terminal/OutputGenerator.h>
#include <terminal/Util.h>
#include <terminal/util/algorithm.h>
#include <terminal/util/times.h>
#include <terminal/VTType.h>

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
using namespace terminal::support;

namespace terminal {

string to_string(CharacterStyleMask _mask)
{
    string out;
    auto const append = [&](string_view _name) {
        if (!out.empty())
            out += ",";
        out += _name;
    };
    if (_mask & CharacterStyleMask::Bold)
        append("bold");
    if (_mask & CharacterStyleMask::Faint)
        append("faint");
    if (_mask & CharacterStyleMask::Italic)
    if (_mask & CharacterStyleMask::Underline)
        append("underline");
    if (_mask & CharacterStyleMask::Blinking)
        append("blinking");
    if (_mask & CharacterStyleMask::Inverse)
        append("inverse");
    if (_mask & CharacterStyleMask::Hidden)
        append("hidden");
    if (_mask & CharacterStyleMask::CrossedOut)
        append("crossed-out");
    if (_mask & CharacterStyleMask::DoublyUnderlined)
        append("doubly-underlined");
    return out;
}

std::optional<size_t> ScreenBuffer::findPrevMarker(size_t _scrollOffset) const
{
    _scrollOffset = min(_scrollOffset, savedLines.size());
    cursor_pos_t rowNumber = _scrollOffset + 1;

    for (auto line = prev(end(savedLines), _scrollOffset + 1); rowNumber <= savedLines.size(); --line, ++rowNumber)
        if (line->marked)
            return {rowNumber};

    return nullopt;
}

std::optional<size_t> ScreenBuffer::findNextMarker(size_t _scrollOffset) const
{
    _scrollOffset = min(_scrollOffset, savedLines.size());
    cursor_pos_t rowNumber = _scrollOffset - 1;

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
    cursor = clampCoordinate(toRealCoordinate(to));
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
        currentColumn = next(begin(*currentLine), _newColumn - 1);
    }
    verifyState();
}

void ScreenBuffer::appendChar(char32_t ch)
{
    verifyState();

    if (wrapPending && autoWrap)
    {
        assert(cursor.column == size_.columns);
        linefeed(margin_.horizontal.from);
    }

    *currentColumn = {ch, graphicsRendition};

    if (cursor.column < size_.columns)
    {
        cursor.column++;
        currentColumn++;
        verifyState();
    }
    else if (autoWrap)
    {
        wrapPending = true;
    }
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
    auto column = next(begin(*line), realCursorPosition().column - 1);
    auto reverseMarginRight = next(line->rbegin(), size_.columns - margin_.horizontal.to);
    auto reverseColumn = next(line->rbegin(), size_.columns - realCursorPosition().column + 1);

    rotate(
        reverseMarginRight,
        next(reverseMarginRight, n),
        reverseColumn
    );
    updateCursorIterators();
    fill_n(
        column,
        n,
        Cell{L' ', graphicsRendition}
    );
}

void ScreenBuffer::insertColumns(cursor_pos_t _n)
{
    for (cursor_pos_t lineNo = margin_.vertical.from; lineNo <= margin_.vertical.to; ++lineNo)
        insertChars(lineNo, _n);
}

void ScreenBuffer::updateCursorIterators()
{
    // update iterators
    currentLine = next(begin(lines), cursor.row - 1);
    currentColumn = next(begin(*currentLine), cursor.column - 1);

    verifyState();
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
    assert(size_.rows == lines.size());

    // verify cursor positions
    [[maybe_unused]] auto const clampedCursor = clampCoordinate(cursor);
    assert(cursor == clampedCursor);

    // verify iterators
    [[maybe_unused]] auto const line = next(begin(lines), cursor.row - 1);
    [[maybe_unused]] auto const col = next(begin(*line), cursor.column - 1);

    assert(line == currentLine);
    assert(col == currentColumn);
    assert(cursor.column == size_.columns || wrapPending == false);
#endif
}

// ==================================================================================

Screen::Screen(WindowSize const& _size,
               optional<size_t> _maxHistoryLineCount,
               ModeSwitchCallback _useApplicationCursorKeys,
               function<void()> _onWindowTitleChanged,
               ResizeWindowCallback _resizeWindow,
               SetApplicationKeypadMode _setApplicationkeypadMode,
               SetBracketedPaste _setBracketedPaste,
			   OnSetCursorStyle _setCursorStyle,
               Reply reply,
               Logger _logger,
               bool _logRaw,
               bool _logTrace,
               Hook onCommands,
               OnBufferChanged _onBufferChanged,
               std::function<void()> _bell,
               std::function<RGBColor(DynamicColorName)> _requestDynamicColor,
               std::function<void(DynamicColorName)> _resetDynamicColor,
               std::function<void(DynamicColorName, RGBColor const&)> _setDynamicColor
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
	setCursorStyle_{ move(_setCursorStyle) },
    reply_{ move(reply) },
    handler_{ _logger },
    parser_{ ref(handler_), _logger },
    primaryBuffer_{ ScreenBuffer::Type::Main, _size, _maxHistoryLineCount },
    alternateBuffer_{ ScreenBuffer::Type::Alternate, _size, nullopt },
    buffer_{ &primaryBuffer_ },
    size_{ _size },
    maxHistoryLineCount_{ _maxHistoryLineCount },
    onBufferChanged_{ move(_onBufferChanged) },
    bell_{ move(_bell) },
    requestDynamicColor_{ move(_requestDynamicColor) },
    resetDynamicColor_{ move(_resetDynamicColor) },
    setDynamicColor_{ move(_setDynamicColor) }
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

    if (onCommands_)
        onCommands_({_command});
}

void Screen::write(char const * _data, size_t _size)
{
#if defined(LIBTERMINAL_LOG_RAW)
    if (logRaw_ && logger_)
        logger_(RawOutputEvent{ escape(_data, _data + _size) });
#endif

    handler_.commands().clear();
    parser_.parseFragment(_data, _size);

    buffer_->verifyState();

    for_each(
        handler_.commands(),
        [&](Command const& _command) {
            visit(*this, _command);
            buffer_->verifyState();

            #if defined(LIBTERMINAL_LOG_TRACE)
            if (logTrace_ && logger_)
                logger_(TraceOutputEvent{to_mnemonic(_command, true, true)});
            #endif
        }
    );

    if (onCommands_)
        onCommands_(handler_.commands());
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
        if (cell.character)
            line += utf8::to_string(utf8::encode(cell.character));
        else
            line += " "; // fill character

    return line;
}

string Screen::renderTextLine(cursor_pos_t row) const
{
    string line;
    line.reserve(size_.columns);
    for (cursor_pos_t col = 1; col <= size_.columns; ++col)
        if (auto const& cell = at(row, col); cell.character)
            line += utf8::to_string(utf8::encode(at(row, col).character));
        else
            line += " "; // fill character

    return line;
}

string Screen::renderText() const
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

std::string Screen::screenshot() const
{
    auto result = std::stringstream{};
    auto generator = OutputGenerator{ result };

    generator(ClearScreen{});
    generator(MoveCursorTo{ 1, 1 });

    for (cursor_pos_t row = 1; row <= size_.rows; ++row)
    {
        for (cursor_pos_t col = 1; col <= size_.columns; ++col)
        {
            Cell const& cell = at(row, col);

            //TODO: generator(SetGraphicsRendition{ cell.attributes.styles });
            generator(SetForegroundColor{ cell.attributes.foregroundColor });
            generator(SetBackgroundColor{ cell.attributes.backgroundColor });
            generator(AppendChar{ cell.character ? cell.character : L' ' });
        }
        generator(MoveCursorToBeginOfLine{});
        generator(Linefeed{});
    }

    generator(MoveCursorTo{ buffer_->cursor.row, buffer_->cursor.column });
    if (realCursor().visible)
        generator(SetMode{ Mode::VisibleCursor, false });

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
        scrollOffset_ = newScrollOffset.value();
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
    // if (realCursorPosition().row == buffer_->margin_.vertical.to)
    //     buffer_->scrollUp(1);
    // else
    //     moveCursorTo({cursorPosition().row + 1, cursorPosition().column});
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
    reply("\033[?64;{}c",
          to_params(DeviceAttributes::Columns132 | DeviceAttributes::SelectiveErase
                  | DeviceAttributes::UserDefinedKeys | DeviceAttributes::NationalReplacementCharacterSets
                  | DeviceAttributes::TechnicalCharacters | DeviceAttributes::AnsiColor
                  | DeviceAttributes::AnsiTextLocator));
}

void Screen::operator()(SendTerminalId const&)
{
    // terminal protocol type
    auto constexpr Pp = static_cast<unsigned>(VTType::VT420);

    // version number
    // TODO: (PACKAGE_VERSION_MAJOR * 100 + PACKAGE_VERSION_MINOR) * 100 + PACKAGE_VERSION_MICRO
    auto constexpr Pv = 0;

    // ROM cardridge registration number (always 0)
    auto constexpr Pc = 0;

    reply("\033[{};{};{}c", Pp, Pv, Pc);
}

void Screen::operator()(ClearToEndOfScreen const&)
{
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

void Screen::operator()(MoveCursorUp const& v)
{
    auto const n = min(v.n, cursorPosition().row - buffer_->margin_.vertical.from);
    buffer_->cursor.row -= n;
    buffer_->currentLine = prev(buffer_->currentLine, n);
    buffer_->currentColumn = next(begin(*buffer_->currentLine), realCursorPosition().column - 1);
    buffer_->verifyState();
}

void Screen::operator()(MoveCursorDown const& v)
{
    auto const n = min(v.n, size_.rows - cursorPosition().row);
    buffer_->cursor.row += n;
    buffer_->currentLine = next(buffer_->currentLine, n);
    buffer_->currentColumn = next(begin(*buffer_->currentLine), realCursorPosition().column - 1);
    buffer_->verifyState();
}

void Screen::operator()(MoveCursorForward const& v)
{
    auto const n = min(v.n, size_.columns - buffer_->cursor.column);
    buffer_->cursor.column += n;
    buffer_->currentColumn = next(
        buffer_->currentColumn,
        n
    );
    buffer_->verifyState();
}

void Screen::operator()(MoveCursorBackward const& v)
{
    auto const n = min(v.n, buffer_->cursor.column - 1);
    buffer_->cursor.column -= n;
    buffer_->currentColumn = prev(buffer_->currentColumn, n);

    // even if you move to 80th of 80 columns, it'll first write a char and THEN flag wrap pending
    buffer_->wrapPending = false;

    buffer_->verifyState();
}

void Screen::operator()(MoveCursorToColumn const& v)
{
    buffer_->wrapPending = false;
    auto const n = min(v.column, size_.columns);
    buffer_->cursor.column = n;
    buffer_->currentColumn = next(begin(*buffer_->currentLine), n - 1);
    buffer_->verifyState();
}

void Screen::operator()(MoveCursorToBeginOfLine const&)
{
    buffer_->wrapPending = false;
    buffer_->cursor.column = 1;
    buffer_->currentColumn = next(
        begin(*buffer_->currentLine),
        buffer_->cursor.column - 1
    );
    buffer_->verifyState();
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

        if (i < buffer_->tabs.size())
            (*this)(MoveCursorToColumn{buffer_->tabs[i]});
        else if (buffer_->realCursorPosition().column < buffer_->margin_.horizontal.to)
            (*this)(MoveCursorToColumn{buffer_->margin_.horizontal.to});
        else
            (*this)(CursorNextLine{1});
    }
    else if (buffer_->tabWidth)
    {
        // default tab settings
        if (buffer_->realCursorPosition().column < buffer_->margin_.horizontal.to)
        {
            auto const n = 1 + buffer_->tabWidth - buffer_->cursor.column % buffer_->tabWidth;
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

void Screen::operator()(SetCursorStyle const& v)
{
	if (setCursorStyle_)
		setCursorStyle_(v.display, v.shape);
}

void Screen::operator()(SetGraphicsRendition const& v)
{
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
            break;
        case Mode::BracketedPaste:
            if (setBracketedPaste_)
                setBracketedPaste_(v.enable);
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
    // TODO
    cerr << fmt::format("TODO: SendMouseEvents({}, {})",
                        to_string(v.protocol),
                        v.enable ? "enable" : "disable")
         << endl;
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
    buffer_->appendChar(v.ch);
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
        for (size_t column = 2 * buffer_->tabWidth + 1; column <= size().columns; column += buffer_->tabWidth)
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
    return buffer_->at(_rowNr, _colNr);
}

void Screen::moveCursorTo(Coordinate to)
{
    buffer_->moveCursorTo(to);
}

void Screen::setBuffer(ScreenBuffer::Type _type)
{
    if (bufferType() != _type)
    {
        switch (_type)
        {
            case ScreenBuffer::Type::Main:
                buffer_ = &primaryBuffer_;
                break;
            case ScreenBuffer::Type::Alternate:
                buffer_ = &alternateBuffer_;
                break;
        }
        if (onBufferChanged_)
            onBufferChanged_(_type);
    }
}
// }}}

} // namespace terminal
