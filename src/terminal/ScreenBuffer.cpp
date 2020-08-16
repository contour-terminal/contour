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
#include <terminal/ScreenBuffer.h>
#include <terminal/OutputGenerator.h>

#include <unicode/grapheme_segmenter.h>
#include <unicode/utf8.h>

#include <crispy/times.h>
#include <crispy/algorithm.h>
#include <crispy/utils.h>

#include <algorithm>
#include <stdexcept>
#include <iostream>
#include <optional>

#if defined(LIBTERMINAL_EXECUTION_PAR)
#include <execution>
#define LIBTERMINAL_EXECUTION_COMMA(par) (std::execution:: par),
#else
#define LIBTERMINAL_EXECUTION_COMMA(par) /*!*/
#endif

using std::cerr;
using std::endl;
using std::min;
using std::max;
using std::next;
using std::nullopt;
using std::optional;
using std::string;

using crispy::for_each;

namespace terminal {

std::string Cell::toUtf8() const
{
    return unicode::to_utf8(codepoints_.data(), codepointCount_);
}

std::optional<int> ScreenBuffer::findMarkerBackward(int _currentCursorLine) const
{
    // TODO: unit- tests for all cases.

    if (_currentCursorLine > size().height || (_currentCursorLine < 0 && -_currentCursorLine >= historyLineCount()))
        return nullopt;

    // we start looking in the main screen area only if @p _currentCursorLine is at least 2,
    // i.e. when there is at least one line to check.
    if (_currentCursorLine >= 2) // main screen area
    {
        int row = _currentCursorLine - 1;
        while (row > 0)
        {
            auto const currentLine = next(begin(lines), row - 1);
            if (currentLine->marked)
                return {row};

            --row;
        }
    }

    // saved-lines area
    auto const scrollOffset = _currentCursorLine <= 0 ? -_currentCursorLine + 1 : 0;

    for (int i = scrollOffset; i < historyLineCount(); ++i)
        if (Line const& line = savedLines.at(historyLineCount() - i - 1); line.marked)
            return -i;

    return nullopt;
}

std::optional<int> ScreenBuffer::findMarkerForward(int _currentCursorLine) const
{
    for (int i = _currentCursorLine + 1; i <= 0; ++i)
        if (int const ri = historyLineCount() + i - 1; savedLines.at(ri).marked)
            return {i};

    for (int i = max(_currentCursorLine + 1, 1); i <= size_.height; ++i)
        if (Line const& line = lines.at(i - 1); line.marked)
            return {i};

    return nullopt;
}

void ScreenBuffer::resize(WindowSize const& _newSize)
{
    if (_newSize.height > size_.height)
    {
        // Grow line count by splicing available lines from history back into buffer, if available,
        // or create new ones until size_.height == _newSize.height.
        auto const extendCount = _newSize.height - size_.height;
        auto const rowsToTakeFromSavedLines = min(extendCount, static_cast<int>(std::size(savedLines)));

        for_each(
            crispy::times(rowsToTakeFromSavedLines),
            [&](auto) {
                savedLines.back().resize(_newSize.width);
                lines.emplace_front(std::move(savedLines.back()));
                savedLines.pop_back();
            }
        );

        cursor.position.row += rowsToTakeFromSavedLines;

        auto const fillLineCount = extendCount - rowsToTakeFromSavedLines;
        generate_n(
            back_inserter(lines),
            fillLineCount,
            [=]() { return Line{static_cast<size_t>(_newSize.width), Cell{}}; });
    }
    else if (_newSize.height < size_.height)
    {
        // Shrink existing line count to _newSize.height
        // by splicing the number of lines to be shrinked by into savedLines bottom.
        if (cursor.position.row == size_.height)
        {
            auto const n = size_.height - _newSize.height;
            for_each(
                crispy::times(n),
                [&](auto) {
                    lines.front().resize(_newSize.width);
                    savedLines.emplace_back(std::move(lines.front()));
                    lines.pop_front();
                }
            );
            clampSavedLines();
        }
        else
            // Hard-cut below cursor by the number of lines to shrink.
            lines.resize(_newSize.height);

        assert(lines.size() == static_cast<size_t>(_newSize.height));
    }

    if (_newSize.width > size_.width)
    {
        // Grow existing columns to _newSize.width.
        std::for_each(
            begin(lines),
            end(lines),
            [=](auto& line) { line.resize(_newSize.width); }
        );
        if (wrapPending)
            cursor.position.column++;
        wrapPending = false;
    }
    else if (_newSize.width < size_.width)
    {
        // Shrink existing columns to _newSize.width.
        // Nothing should be done, I think, as we preserve prior (now exceeding) content.
        if (cursor.position.column == size_.width)
            wrapPending = true;

        // truncating tabs
        while (!tabs.empty() && tabs.back() > _newSize.width)
            tabs.pop_back();
    }

    // Reset margin to their default.
    margin_ = Margin{
		Margin::Range{1, _newSize.height},
        Margin::Range{1, _newSize.width}
    };
    // TODO: find out what to do with DECOM mode. Reset it to?

    size_ = _newSize;

    lastCursorPosition = clampCoordinate(lastCursorPosition);
    auto lastLine = next(begin(lines), lastCursorPosition.row - 1);
    lastColumn = columnIteratorAt(begin(*lastLine), lastCursorPosition.column);

    cursor.position = clampCoordinate(cursor.position);
    updateCursorIterators();
}

void ScreenBuffer::setMode(Mode _mode, bool _enable)
{
    // TODO: rename this function  to indicate this is more an event to be act upon.

    // TODO: thse member variables aren't really needed anymore (are they?), remove them.
    switch (_mode)
    {
        case Mode::AutoWrap:
            cursor.autoWrap = _enable;
            break;
        case Mode::LeftRightMargin:
            // Resetting DECLRMM also resets the horizontal margins back to screen size.
            if (!_enable)
                margin_.horizontal = {1, size_.width};
            break;
        case Mode::Origin:
            cursor.originMode = _enable;
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
    cursor.position = clampToScreen(toRealCoordinate(to));
    updateCursorIterators();
}

Cell& ScreenBuffer::at(Coordinate const& _pos) noexcept
{
    assert(crispy::ascending(1 - historyLineCount(), _pos.row, size_.height));
    assert(crispy::ascending(1, _pos.column, size_.width));

    if (_pos.row > 0)
        return (*next(begin(lines), _pos.row - 1))[_pos.column - 1];
    else
        return (*next(rbegin(savedLines), -_pos.row))[_pos.column - 1];
}

void ScreenBuffer::linefeed(cursor_pos_t _newColumn)
{
    wrapPending = false;

    if (realCursorPosition().row == margin_.vertical.to ||
        realCursorPosition().row == size_.height)
    {
        scrollUp(1);
        moveCursorTo({cursorPosition().row, _newColumn});
    }
    else
    {
        // using moveCursorTo() would embrace code reusage, but due to the fact that it's fully recalculating iterators,
        // it may be faster to just incrementally update them.
        // moveCursorTo({cursorPosition().row + 1, margin_.horizontal.from});
        cursor.position.row++;
        cursor.position.column = _newColumn;
        currentLine++;
        updateColumnIterator();
    }
    verifyState();
}

void ScreenBuffer::appendChar(char32_t _ch, bool _consecutive)
{
    verifyState();

    if (wrapPending && cursor.autoWrap)
        linefeed(margin_.horizontal.from);

    auto const ch =
        _ch < 127 ? cursor.charsets.map(static_cast<char>(_ch))
                  : _ch == 0x7F ? ' ' : _ch;

    bool const insertToPrev =
        _consecutive
        && !lastColumn->empty()
        && unicode::grapheme_segmenter::nonbreakable(lastColumn->codepoint(lastColumn->codepointCount() - 1), ch);

    if (!insertToPrev)
        writeCharToCurrentAndAdvance(ch);
    else
    {
        auto const extendedWidth = lastColumn->appendCharacter(ch);

        if (extendedWidth > 0)
            clearAndAdvance(extendedWidth);
    }
}

void ScreenBuffer::clearAndAdvance(int _offset)
{
    if (_offset == 0)
        return;

    auto const availableColumnCount = margin_.horizontal.length() - cursor.position.column;
    auto const n = min(_offset, availableColumnCount);

    if (n == _offset)
    {
        assert(n > 0);
        cursor.position.column += n;
        for (auto i = 0; i < n; ++i)
            (currentColumn++)->reset(cursor.graphicsRendition, currentHyperlink);
    }
    else if (cursor.autoWrap)
    {
        wrapPending = true;
    }
}

void ScreenBuffer::writeCharToCurrentAndAdvance(char32_t _character)
{
    Cell& cell = *currentColumn;
    cell.setCharacter(_character);
    cell.attributes() = cursor.graphicsRendition;
    cell.setHyperlink(currentHyperlink);

    lastColumn = currentColumn;
    lastCursorPosition = cursor.position;

    bool const cursorInsideMargin = isModeEnabled(Mode::LeftRightMargin) && isCursorInsideMargins();
    auto const cellsAvailable = cursorInsideMargin ? margin_.horizontal.to - cursor.position.column
                                                   : size_.width - cursor.position.column;

    auto const n = min(cell.width(), cellsAvailable);

    if (n == cell.width())
    {
        assert(n > 0);
        cursor.position.column += n;
        currentColumn++;
        for (int i = 1; i < n; ++i)
            (currentColumn++)->reset(cursor.graphicsRendition, currentHyperlink);
        verifyState();
    }
    else if (cursor.autoWrap)
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
    if (margin.horizontal != Margin::Range{1, size_.width})
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
                    Cell{{}, cursor.graphicsRendition}
                );
            }
        );
    }
    else if (margin.vertical == Margin::Range{1, size_.height})
    {
        // full-screen scroll-up
        auto const n = min(v_n, size_.height);

        if (n > 0)
        {
            for_each(
                crispy::times(n),
                [&](auto) {
                    savedLines.emplace_back(std::move(lines.front()));
                    lines.pop_front();
                }
            );

            clampSavedLines();

            generate_n(
                back_inserter(lines),
                n,
                [this]() { return Line{static_cast<size_t>(size_.width), Cell{{}, cursor.graphicsRendition}}; }
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
                fill(begin(line), end(line), Cell{{}, cursor.graphicsRendition});
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

    if (_margin.horizontal != Margin::Range{1, size_.width})
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
                        Cell{{}, cursor.graphicsRendition}
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
                        Cell{{}, cursor.graphicsRendition}
                    );
                }
            );
        }
    }
    else if (_margin.vertical == Margin::Range{1, size_.height})
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
                    Cell{{}, cursor.graphicsRendition}
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
                    Cell{{}, cursor.graphicsRendition}
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
        Cell{L' ', cursor.graphicsRendition}
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
        columnIteratorAt(begin(*line), cursor.position.column),
        n,
        Cell{L' ', cursor.graphicsRendition}
    );
}

void ScreenBuffer::insertColumns(cursor_pos_t _n)
{
    for (cursor_pos_t lineNo = margin_.vertical.from; lineNo <= margin_.vertical.to; ++lineNo)
        insertChars(lineNo, _n);
}

void ScreenBuffer::setCurrentColumn(cursor_pos_t _n)
{
    auto const col = cursor.originMode ? margin_.horizontal.from + _n - 1 : _n;
    auto const clampedCol = min(col, size_.width);
    cursor.position.column = clampedCol;
    updateColumnIterator();

    verifyState();
}

bool ScreenBuffer::incrementCursorColumn(cursor_pos_t _n)
{
    auto const n = min(_n,  margin_.horizontal.length() - cursor.position.column);
    cursor.position.column += n;
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
        for (cursor_pos_t column = tabWidth; column <= size().width; column += tabWidth)
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
            ((lrmm && cursor.position.column != margin_.horizontal.to)
            || (!lrmm && cursor.position.column != size_.width)))
    {
        fail(fmt::format(
            "Wrap is pending but cursor's column ({}) is not at right side of margin ({}) or screen ({}).",
            cursor.position.column, margin_.horizontal.to, size_.width
        ));
    }

    if (static_cast<size_t>(size_.height) != lines.size())
        fail(fmt::format("Line count mismatch. Actual line count {} but should be {}.", lines.size(), size_.height));

    // verify cursor positions
    [[maybe_unused]] auto const clampedCursorPos = clampToScreen(cursor.position);
    if (cursor.position != clampedCursorPos)
        fail(fmt::format("Cursor {} does not match clamp to screen {}.", cursor, clampedCursorPos));
    // FIXME: the above triggers on tmux vertical screen split (cursor.column off-by-one)

    // verify iterators
    [[maybe_unused]] auto const line = next(begin(lines), cursor.position.row - 1);
    [[maybe_unused]] auto const col = columnIteratorAt(cursor.position.column);

    if (line != currentLine)
        fail(fmt::format("Calculated current line does not match."));
    else if (col != currentColumn)
        fail(fmt::format("Calculated current column does not match."));

    if (wrapPending && cursor.position.column != size_.width && cursor.position.column != margin_.horizontal.to)
        fail(fmt::format("wrapPending flag set when cursor is not in last column."));
#endif
}

void ScreenBuffer::dumpState(std::string const& _message) const
{
    auto const hline = [&]() {
        for_each(crispy::times(size_.width), [](auto) { cerr << '='; });
        cerr << endl;
    };

    hline();
    cerr << "\033[1;37;41m" << _message << "\033[m" << endl;
    hline();

    cerr << fmt::format("Rendered screen at the time of failure: {}\n", size_);
    cerr << fmt::format("cursor position      : {}\n", cursor);
    if (cursor.originMode)
        cerr << fmt::format("real cursor position : {})\n", toRealCoordinate(cursor.position));
    cerr << fmt::format("vertical margins     : {}\n", margin_.vertical);
    cerr << fmt::format("horizontal margins   : {}\n", margin_.horizontal);

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

string ScreenBuffer::renderTextLine(cursor_pos_t row) const
{
    string line;
    line.reserve(size_.width);
    for (cursor_pos_t col = 1; col <= size_.width; ++col)
        if (auto const& cell = at({row, col}); cell.codepointCount())
            line += cell.toUtf8();
        else
            line += " "; // fill character

    return line;
}

string ScreenBuffer::renderText() const
{
    string text;
    text.reserve(size_.height * (size_.width + 1));

    for (cursor_pos_t row = 1; row <= size_.height; ++row)
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

    for (cursor_pos_t const row : crispy::times(1, size_.height))
    {
        for (cursor_pos_t const col : crispy::times(1, size_.width))
        {
            Cell const& cell = at({row, col});

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

} // end namespace
