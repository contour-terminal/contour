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
#include <terminal/VTType.h>

#include <algorithm>
#include <iterator>
#include <sstream>

using namespace std;

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

void ScreenBuffer::resize(WindowSize const& _newSize)
{
    if (_newSize.rows > size_.rows)
    {
        // Grow line count by splicing available lince from history back into buffer, if available,
        // or create new ones until size_.rows == _newSize.rows.
        auto const extendCount = _newSize.rows - size_.rows;
        auto const rowsToTakeFromSavedLines = min(extendCount, static_cast<unsigned int>(std::size(savedLines)));
        lines.splice(
            begin(lines),
            savedLines,
            prev(end(savedLines), rowsToTakeFromSavedLines),
            end(savedLines));

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
            savedLines.splice(
                end(savedLines),
                lines,
                begin(lines),
                next(begin(lines), n)
            );
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
        auto targetLine = next(begin(lines), margin.vertical.to - n);
        auto const bottomLine = next(begin(lines), margin.vertical.to);     // bottom margin's end-line iterator
        for (; targetLine != bottomLine; ++targetLine)
        {
            fill_n(
                next(begin(*targetLine), margin.horizontal.from - 1),
                margin.horizontal.length(),
                Cell{{}, graphicsRendition}
            );
        }
    }
    else if (margin.vertical == Margin::Range{1, size_.rows})
    {
        // full-screen scroll-up
        auto const n = min(v_n, size_.rows);

        if (n > 0)
        {
            savedLines.splice(
                end(savedLines),
                lines,
                begin(lines),
                next(begin(lines), n)
            );

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

        auto const e_i = margin.vertical.to - n;
        for (auto li = next(begin(lines), e_i); li != next(begin(lines), margin.vertical.to); ++li)
            fill(begin(*li), end(*li), Cell{{}, graphicsRendition});
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
        Cell{L' ', {}}
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

void ScreenBuffer::verifyState() const
{
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
}

// ==================================================================================

Screen::Screen(WindowSize const& _size,
               ModeSwitchCallback _useApplicationCursorKeys,
               function<void()> _onWindowTitleChanged,
               ResizeWindowCallback _resizeWindow,
               Reply reply,
               Logger _logger,
               Hook onCommands) :
    onCommands_{ move(onCommands) },
    logger_{ _logger },
    useApplicationCursorKeys_{ move(_useApplicationCursorKeys) },
    onWindowTitleChanged_{ move(_onWindowTitleChanged) },
    resizeWindow_{ move(_resizeWindow) },
    reply_{ move(reply) },
    handler_{ _size.rows, _logger },
    parser_{ ref(handler_), _logger },
    primaryBuffer_{ _size },
    alternateBuffer_{ _size },
    state_{ &primaryBuffer_ },
    size_{ _size }
{
    (*this)(SetMode{Mode::AutoWrap, true});
}

void Screen::resize(WindowSize const& _newSize)
{
    // TODO: only resize current screen buffer, and then make sure we resize the other upon actual switch
    primaryBuffer_.resize(_newSize);
    alternateBuffer_.resize(_newSize);
    size_ = _newSize;
}

void Screen::write(char const * _data, size_t _size)
{
    if (logger_)
        logger_(RawOutputEvent{ escape(_data, _data + _size) });

    handler_.commands().clear();
    parser_.parseFragment(_data, _size);

    state_->verifyState();
    for (Command const& command : handler_.commands())
    {
        visit(*this, command);
        state_->verifyState();
    }

    if (onCommands_)
        onCommands_(handler_.commands());
}

void Screen::render(Renderer const& render) const
{
    for (cursor_pos_t row = 1; row <= size_.rows; ++row)
        for (cursor_pos_t col = 1; col <= size_.columns; ++col)
            render(row, col, at(row, col));
}

string Screen::renderHistoryTextLine(cursor_pos_t _lineNumberIntoHistory) const
{
    assert(1 <= _lineNumberIntoHistory && _lineNumberIntoHistory <= state_->savedLines.size());
    string line;
    line.reserve(size_.columns);
    auto const lineIter = next(state_->savedLines.rbegin(), _lineNumberIntoHistory - 1);
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

    generator(MoveCursorTo{ state_->cursor.row, state_->cursor.column });
    if (realCursor().visible)
        generator(SetMode{ Mode::VisibleCursor, false });

    return result.str();
}

// {{{ ops
void Screen::operator()(Bell const& v)
{
}

void Screen::operator()(FullReset const& v)
{
    resetHard();
}

void Screen::operator()(Linefeed const& v)
{
    // if (realCursorPosition().row == state_->margin_.vertical.to)
    //     state_->scrollUp(1);
    // else
    //     moveCursorTo({cursorPosition().row + 1, cursorPosition().column});
    if (isModeEnabled(Mode::AutomaticNewLine))
        state_->linefeed(state_->margin_.horizontal.from);
    else
        state_->linefeed(realCursorPosition().column);
}

void Screen::operator()(Backspace const& v)
{
    moveCursorTo({cursorPosition().row, cursorPosition().column > 1 ? cursorPosition().column - 1 : 1});
}

void Screen::operator()(DeviceStatusReport const& v)
{
    reply("\033[0n");
}

void Screen::operator()(ReportCursorPosition const& v)
{
    reply("\033[{};{}R", cursorPosition().row, cursorPosition().column);
}

void Screen::operator()(ReportExtendedCursorPosition const& v)
{
    auto const pageNum = 1;
    reply("\033[{};{};{}R", cursorPosition().row, cursorPosition().column, pageNum);
}

void Screen::operator()(SendDeviceAttributes const& v)
{
    // See https://vt100.net/docs/vt510-rm/DA1.html
    reply("\033[?64;{}c",
          to_params(DeviceAttributes::Columns132 | DeviceAttributes::SelectiveErase
                  | DeviceAttributes::UserDefinedKeys | DeviceAttributes::NationalReplacementCharacterSets
                  | DeviceAttributes::TechnicalCharacters | DeviceAttributes::AnsiColor
                  | DeviceAttributes::AnsiTextLocator));
}

void Screen::operator()(SendTerminalId const& v)
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

void Screen::operator()(ClearToEndOfScreen const& v)
{
    for (auto line = state_->currentLine; line != end(state_->lines); ++line)
        fill(begin(*line), end(*line), Cell{{}, state_->graphicsRendition});
}

void Screen::operator()(ClearToBeginOfScreen const& v)
{
    for (auto line = begin(state_->lines); line != next(state_->currentLine); ++line)
        fill(begin(*line), end(*line), Cell{{}, state_->graphicsRendition});
}

void Screen::operator()(ClearScreen const& v)
{
    // https://vt100.net/docs/vt510-rm/ED.html
    for (auto& line : state_->lines)
        fill(begin(line), end(line), Cell{{}, state_->graphicsRendition});
}

void Screen::operator()(ClearScrollbackBuffer const& v)
{
    state_->savedLines.clear();
}

void Screen::operator()(EraseCharacters const& v)
{
    // Spec: https://vt100.net/docs/vt510-rm/ECH.html
    // It's not clear from the spec how to perform erase when inside margin and number of chars to be erased would go outside margins.
    // TODO: See what xterm does ;-)
    size_t const n = min(state_->size_.columns - realCursorPosition().column + 1, v.n == 0 ? 1 : v.n);
    fill_n(state_->currentColumn, n, Cell{{}, state_->graphicsRendition});
}

void Screen::operator()(ScrollUp const& v)
{
    state_->scrollUp(v.n);
}

void Screen::operator()(ScrollDown const& v)
{
    state_->scrollDown(v.n);
}

void Screen::operator()(ClearToEndOfLine const& v)
{
    fill(
        state_->currentColumn,
        end(*state_->currentLine),
        Cell{{}, state_->graphicsRendition}
    );
}

void Screen::operator()(ClearToBeginOfLine const& v)
{
    fill(
        begin(*state_->currentLine),
        next(state_->currentColumn),
        Cell{{}, state_->graphicsRendition}
    );
}

void Screen::operator()(ClearLine const& v)
{
    fill(
        begin(*state_->currentLine),
        end(*state_->currentLine),
        Cell{{}, state_->graphicsRendition}
    );
}

void Screen::operator()(CursorNextLine const& v)
{
    state_->moveCursorTo({cursorPosition().row + v.n, 1});
}

void Screen::operator()(CursorPreviousLine const& v)
{
    auto const n = min(v.n, cursorPosition().row - 1);
    state_->moveCursorTo({cursorPosition().row - n, 1});
}

void Screen::operator()(InsertCharacters const& v)
{
    if (isCursorInsideMargins())
        state_->insertChars(realCursorPosition().row, v.n);
}

void Screen::operator()(InsertLines const& v)
{
    if (isCursorInsideMargins())
    {
        state_->scrollDown(
            v.n,
            Margin{
                { state_->cursor.row, state_->margin_.vertical.to },
                state_->margin_.horizontal
            }
        );
    }
}

void Screen::operator()(InsertColumns const& v)
{
    if (isCursorInsideMargins())
        state_->insertColumns(v.n);
}

void Screen::operator()(DeleteLines const& v)
{
    if (isCursorInsideMargins())
    {
        state_->scrollUp(
            v.n,
            Margin{
                { state_->cursor.row, state_->margin_.vertical.to },
                state_->margin_.horizontal
            }
        );
    }
}

void Screen::operator()(DeleteCharacters const& v)
{
    if (isCursorInsideMargins() && v.n != 0)
        state_->deleteChars(realCursorPosition().row, v.n);
}

void Screen::operator()(DeleteColumns const& v)
{
    if (isCursorInsideMargins())
        for (cursor_pos_t lineNo = state_->margin_.vertical.from; lineNo <= state_->margin_.vertical.to; ++lineNo)
            state_->deleteChars(lineNo, v.n);
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

void Screen::operator()(MoveCursorUp const& v)
{
    auto const n = min(v.n, cursorPosition().row - state_->margin_.vertical.from);
    state_->cursor.row -= n;
    state_->currentLine = prev(state_->currentLine, n);
    state_->currentColumn = next(begin(*state_->currentLine), realCursorPosition().column - 1);
    state_->verifyState();
}

void Screen::operator()(MoveCursorDown const& v)
{
    auto const n = min(v.n, size_.rows - cursorPosition().row);
    state_->cursor.row += n;
    state_->currentLine = next(state_->currentLine, n);
    state_->currentColumn = next(begin(*state_->currentLine), realCursorPosition().column - 1);
    state_->verifyState();
}

void Screen::operator()(MoveCursorForward const& v)
{
    auto const n = min(v.n, size_.columns - state_->cursor.column);
    state_->cursor.column += n;
    state_->currentColumn = next(
        state_->currentColumn,
        n
    );
    state_->verifyState();
}

void Screen::operator()(MoveCursorBackward const& v)
{
    auto const n = min(v.n, state_->cursor.column - 1);
    state_->cursor.column -= n;
    state_->currentColumn = prev(state_->currentColumn, n);

    // even if you move to 80th of 80 columns, it'll first write a char and THEN flag wrap pending
    state_->wrapPending = false;

    state_->verifyState();
}

void Screen::operator()(MoveCursorToColumn const& v)
{
    state_->wrapPending = false;
    auto const n = min(v.column, size_.columns);
    state_->cursor.column = n;
    state_->currentColumn = next(begin(*state_->currentLine), n - 1);
    state_->verifyState();
}

void Screen::operator()(MoveCursorToBeginOfLine const& v)
{
    state_->wrapPending = false;
    state_->cursor.column = 1;
    state_->currentColumn = next(
        begin(*state_->currentLine),
        state_->cursor.column - 1
    );
    state_->verifyState();
}

void Screen::operator()(MoveCursorTo const& v)
{
    moveCursorTo(Coordinate{v.row, v.column});
}

void Screen::operator()(MoveCursorToLine const& v)
{
    moveCursorTo({v.row, state_->cursor.column});
}

void Screen::operator()(MoveCursorToNextTab const& v)
{
    auto const n = 1 + state_->tabWidth - state_->cursor.column % state_->tabWidth;
    (*this)(MoveCursorForward{n});
    // TODO: I guess something must remember when a \t was added, for proper move-back?
}

void Screen::operator()(SaveCursor const& v)
{
    state_->saveState();
}

void Screen::operator()(RestoreCursor const& v)
{
    state_->restoreState();
}

void Screen::operator()(Index const& v)
{
    if (realCursorPosition().row == state_->margin_.vertical.to)
        state_->scrollUp(1);
    else
        moveCursorTo({cursorPosition().row + 1, cursorPosition().column});
}

void Screen::operator()(ReverseIndex const& v)
{
    if (realCursorPosition().row == state_->margin_.vertical.from)
        state_->scrollDown(1);
    else
        moveCursorTo({cursorPosition().row - 1, cursorPosition().column});
}

void Screen::operator()(BackIndex const& v)
{
    if (realCursorPosition().column == state_->margin_.horizontal.from)
        ;// TODO: scrollRight(1);
    else
        moveCursorTo({cursorPosition().row, cursorPosition().column - 1});
}

void Screen::operator()(ForwardIndex const& v)
{
    if (realCursorPosition().column == state_->margin_.horizontal.to)
        ;// TODO: scrollLeft(1);
    else
        moveCursorTo({cursorPosition().row, cursorPosition().column + 1});
}

void Screen::operator()(SetForegroundColor const& v)
{
    state_->graphicsRendition.foregroundColor = v.color;
}

void Screen::operator()(SetBackgroundColor const& v)
{
    state_->graphicsRendition.backgroundColor = v.color;
}

void Screen::operator()(SetGraphicsRendition const& v)
{
    switch (v.rendition)
    {
        case GraphicsRendition::Reset:
            state_->graphicsRendition = {};
            break;
        case GraphicsRendition::Bold:
            state_->graphicsRendition.styles |= CharacterStyleMask::Bold;
            break;
        case GraphicsRendition::Faint:
            state_->graphicsRendition.styles |= CharacterStyleMask::Faint;
            break;
        case GraphicsRendition::Italic:
            state_->graphicsRendition.styles |= CharacterStyleMask::Italic;
            break;
        case GraphicsRendition::Underline:
            state_->graphicsRendition.styles |= CharacterStyleMask::Underline;
            break;
        case GraphicsRendition::Blinking:
            state_->graphicsRendition.styles |= CharacterStyleMask::Blinking;
            break;
        case GraphicsRendition::Inverse:
            state_->graphicsRendition.styles |= CharacterStyleMask::Inverse;
            break;
        case GraphicsRendition::Hidden:
            state_->graphicsRendition.styles |= CharacterStyleMask::Hidden;
            break;
        case GraphicsRendition::CrossedOut:
            state_->graphicsRendition.styles |= CharacterStyleMask::CrossedOut;
            break;
        case GraphicsRendition::DoublyUnderlined:
            state_->graphicsRendition.styles |= CharacterStyleMask::DoublyUnderlined;
            break;
        case GraphicsRendition::Normal:
            state_->graphicsRendition.styles &= ~(CharacterStyleMask::Bold | CharacterStyleMask::Faint);
            break;
        case GraphicsRendition::NoItalic:
            state_->graphicsRendition.styles &= ~CharacterStyleMask::Italic;
            break;
        case GraphicsRendition::NoUnderline:
            state_->graphicsRendition.styles &= ~CharacterStyleMask::Underline;
            break;
        case GraphicsRendition::NoBlinking:
            state_->graphicsRendition.styles &= ~CharacterStyleMask::Blinking;
            break;
        case GraphicsRendition::NoInverse:
            state_->graphicsRendition.styles &= ~CharacterStyleMask::Inverse;
            break;
        case GraphicsRendition::NoHidden:
            state_->graphicsRendition.styles &= ~CharacterStyleMask::Hidden;
            break;
        case GraphicsRendition::NoCrossedOut:
            state_->graphicsRendition.styles &= ~CharacterStyleMask::CrossedOut;
            break;
    }
}

void Screen::operator()(SetMode const& v)
{
    state_->setMode(v.mode, v.enable);

    switch (v.mode)
    {
        case Mode::UseAlternateScreen:
            if (v.enable)
                state_ = &alternateBuffer_;
            else
                state_ = &primaryBuffer_;
            break;
        case Mode::UseApplicationCursorKeys:
            if (useApplicationCursorKeys_)
                useApplicationCursorKeys_(v.enable);
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

void Screen::operator()(SetTopBottomMargin const& margin)
{
    if (auto const bottom = min(margin.bottom, state_->size_.rows); margin.top < bottom)
    {
        state_->margin_.vertical.from = margin.top;
        state_->margin_.vertical.to = bottom;
        state_->moveCursorTo({1, 1});
    }
}

void Screen::operator()(SetLeftRightMargin const& margin)
{
    if (isModeEnabled(Mode::LeftRightMargin))
    {
        if (auto const right = min(margin.right, state_->size_.columns); margin.left + 1 < right)
        {
            state_->margin_.horizontal.from = margin.left;
            state_->margin_.horizontal.to = right;
            state_->moveCursorTo({1, 1});
        }
    }
}

void Screen::operator()(ScreenAlignmentPattern const&)
{
    // sets the margins to the extremes of the page
    state_->margin_.vertical.from = 1;
    state_->margin_.vertical.to = size_.rows;
    state_->margin_.horizontal.from = 1;
    state_->margin_.horizontal.to = size_.columns;

    // and moves the cursor to the home position
    moveCursorTo({1, 1});

    // fills the complete screen area with a test pattern
    for (auto& line: state_->lines)
        for (auto& col: line)
            col.character = 'X';
}

void Screen::operator()(SendMouseEvents const& v)
{
    // TODO
    cerr << fmt::format("TODO: SendMouseEvents({}, {})",
                        to_string(v.protocol),
                        v.enable ? "enable" : "disable")
         << endl;
}

void Screen::operator()(AlternateKeypadMode const& v)
{
    // TODO
    cerr << "TODO: AlternateKeypadMode." << endl;
}

void Screen::operator()(DesignateCharset const& v)
{
    // TODO
}

void Screen::operator()(SingleShiftSelect const& v)
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

void Screen::operator()(SaveWindowTitle const& v)
{
    savedWindowTitles_.push(windowTitle_);
}

void Screen::operator()(RestoreWindowTitle const& v)
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
    state_->appendChar(v.ch);
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
    primaryBuffer_ = ScreenBuffer{size_};
    alternateBuffer_ = ScreenBuffer{size_};
    state_ = &primaryBuffer_;
}

Screen::Cell const& Screen::at(cursor_pos_t rowNr, cursor_pos_t colNr) const noexcept
{
    return state_->at(rowNr, colNr);
}

Screen::Cell& Screen::at(cursor_pos_t rowNr, cursor_pos_t colNr) noexcept
{
    return state_->at(rowNr, colNr);
}

void Screen::moveCursorTo(Coordinate to)
{
    state_->moveCursorTo(to);
}
// }}}

} // namespace terminal
