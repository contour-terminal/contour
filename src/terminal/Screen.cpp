#include <terminal/Screen.h>
#include <terminal/Util.h>
#include <terminal/VTType.h>

#include <algorithm>
#include <iterator>

using namespace std;

namespace terminal {

void Screen::Buffer::resize(size_t newColumnCount, size_t newRowCount)
{
    if (margin_.horizontal == Range{1, numColumns_} && margin_.vertical == Range{1, numLines_})
    {
        // full screen margins adapt implicitely to remain full-size
        margin_ = Margin{
            {1, newRowCount},
            {1, newColumnCount}
        };
    }
    else
    {
        // clamp margin
        margin_.horizontal.from = min(margin_.horizontal.from, newColumnCount);
        margin_.horizontal.to = min(margin_.horizontal.to, newColumnCount);
        margin_.vertical.from = min(margin_.vertical.from, newRowCount);
        margin_.vertical.to = min(margin_.vertical.to, newRowCount);
    }

    if (newColumnCount > numColumns_)
    {
        // TODO: grow existing lines to newColumnCount
    }
    else if (newColumnCount < numColumns_)
    {
        // TODO: shrink existing lines to newColumnCount
    }

    if (newRowCount > numLines_)
    {
        auto const extendCount = newRowCount - numLines_;
        auto const rowsToTakeFromSavedLines = min(extendCount, size(savedLines));
        lines.splice(
            begin(lines),
            savedLines,
            prev(end(savedLines), rowsToTakeFromSavedLines),
            end(savedLines));

        auto const fillLineCount = extendCount - rowsToTakeFromSavedLines;
        generate_n(
            back_inserter(lines),
            fillLineCount,
            [=]() { return Line{newColumnCount, Cell{}}; });
    }
    else
    {
        // TODO: shrink existing line count to newRowCount
        // by splicing the diff count lines into savedLines bottom
    }
}

Screen::Cell& Screen::Buffer::at(cursor_pos_t row, cursor_pos_t col)
{
    return (*next(begin(lines), row - 1))[col - 1];
}

Screen::Cell const& Screen::Buffer::at(cursor_pos_t row, cursor_pos_t col) const
{
    return (*next(begin(lines), row - 1))[col - 1];
}

void Screen::Buffer::linefeed()
{
    wrapPending = false;

    if (cursor.row < numLines())
    {
        cursor.row++;
        cursor.column = 1;
        currentLine++;
        currentColumn = begin(*currentLine);
    }
    else
    {
        cursor.column = 1;
        savedLines.splice(
            end(savedLines),
            lines,
            begin(lines)
        );
        lines.emplace_back(numColumns(), Cell{});
        currentLine = prev(end(lines));
        currentColumn = begin(*currentLine);
    }

    verifyCursorIterators();
}

void Screen::Buffer::appendChar(wchar_t ch)
{
    verifyCursorIterators();

    if (wrapPending && autoWrap)
    {
        assert(cursor.column == numColumns());
        linefeed();
    }

    *currentColumn = {ch, graphicsRendition};

    if (cursor.column < numColumns())
    {
        cursor.column++;
        currentColumn++;
        verifyCursorIterators();
    }
    else if (autoWrap)
    {
        wrapPending = true;
    }
}

void Screen::Buffer::scrollUp(size_t v_n)
{
    scrollUp(v_n, margin_);
}

void Screen::Buffer::scrollUp(size_t v_n, Margin const& margin)
{
    if (margin.horizontal != Range{1, numColumns()})
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
                Cell{}
            );
        }
    }
    else if (margin.vertical == Range{1, numLines()})
    {
        // full-screen scroll-up
        auto const n = min(v_n, numLines());

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
                [this]() { return Line{numColumns(), Cell{}}; }
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
            fill(begin(*li), end(*li), Cell{});
    }

    updateCursorIterators();
}

void Screen::Buffer::scrollDown(size_t v_n)
{
    auto const marginHeight = margin_.vertical.length();
    auto const n = min(v_n, marginHeight);

    if (margin_.horizontal != Range{1, numColumns()})
    {
        // full "inside" scroll-down
        if (n < marginHeight)
        {
            auto sourceLine = next(begin(lines), margin_.vertical.to - n - 1);
            auto targetLine = next(begin(lines), margin_.vertical.to - 1);
            auto const sourceEndLine = next(begin(lines), margin_.vertical.from - 1);

            while (sourceLine != sourceEndLine)
            {
                copy_n(
                    next(begin(*sourceLine), margin_.horizontal.from - 1),
                    margin_.horizontal.length(),
                    next(begin(*targetLine), margin_.horizontal.from - 1)
                );
                --targetLine;
                --sourceLine;
            }

            copy_n(
                next(begin(*sourceLine), margin_.horizontal.from - 1),
                margin_.horizontal.length(),
                next(begin(*targetLine), margin_.horizontal.from - 1)
            );

            for_each(
                next(begin(lines), margin_.vertical.from - 1),
                next(begin(lines), margin_.vertical.from - 1 + n),
                [this](Line& line) {
                    fill_n(
                        next(begin(line), margin_.horizontal.from - 1),
                        margin_.horizontal.length(),
                        Cell{}
                    );
                }
            );
        }
        else
        {
            // clear everything in margin
            for_each(
                next(begin(lines), margin_.vertical.from - 1),
                next(begin(lines), margin_.vertical.to),
                [this](Line& line) {
                    fill_n(
                        next(begin(line), margin_.horizontal.from - 1),
                        margin_.horizontal.length(),
                        Cell{}
                    );
                }
            );
        }
    }
    else if (margin_.vertical == Range{1, numLines()})
    {
        rotate(
            begin(lines),
            next(begin(lines), marginHeight - n),
            end(lines)
        );

        for_each(
            begin(lines),
            next(begin(lines), n),
            [](Line& line) {
                fill(
                    begin(line),
                    end(line),
                    Cell{}
                );
            }
        );
    }
    else
    {
        // scroll down only inside vertical margin with full horizontal extend
        rotate(
            next(begin(lines), margin_.vertical.from - 1),
            next(begin(lines), margin_.vertical.to - n),
            next(begin(lines), margin_.vertical.to)
        );

        for_each(
            next(begin(lines), margin_.vertical.from - 1),
            next(begin(lines), margin_.vertical.from - 1 + n),
            [](Line& line) {
                fill(
                    begin(line),
                    end(line),
                    Cell{}
                );
            }
        );
    }

    updateCursorIterators();
}

void Screen::Buffer::updateCursorIterators()
{
    // update iterators
    currentLine = next(begin(lines), cursor.row - 1);
    currentColumn = next(begin(*currentLine), cursor.column - 1);

    verifyCursorIterators();
}

void Screen::Buffer::verifyCursorIterators() const
{
    auto const line = next(begin(lines), cursor.row - 1);
    auto const col = next(begin(*line), cursor.column - 1);

    assert(line == currentLine);
    assert(col == currentColumn);
    assert(cursor.column == numColumns() || wrapPending == false);
}

// ==================================================================================

Screen::Screen(size_t columnCount,
               size_t rowCount,
               Reply reply,
               Logger logger,
               Hook onCommands) :
    logger_{move(logger)},
    onCommands_{move(onCommands)},
    handler_{rowCount},
    parser_{
        ref(handler_),
        [this](string const& msg) { log("debug.parser: " + msg); },
        [this](string const& msg) { log("trace.parser: " + msg); }
    },
    primaryBuffer_{columnCount, rowCount},
    alternateBuffer_{columnCount, rowCount},
    state_{&primaryBuffer_},
    enabledModes_{},
    columnCount_{columnCount},
    rowCount_{rowCount},
    reply_{move(reply)}
{
}

void Screen::resize(size_t newColumnCount, size_t newRowCount)
{
    // TODO: only resize current screen buffer, and then make sure we resize the other upon actual switch

    primaryBuffer_.resize(newColumnCount, newRowCount);
    alternateBuffer_.resize(newColumnCount, newRowCount);

    rowCount_ = newRowCount;
    columnCount_ = newColumnCount;
}

void Screen::write(char const *data, size_t size)
{
    handler_.commands().clear();
    parser_.parseFragment(data, size);

    for (Command const& command : handler_.commands())
    {
        log("write: {}", to_string(command));
        visit(*this, command);
    }

    if (onCommands_)
        onCommands_(handler_.commands());
}

void Screen::render(Renderer const& render)
{
    for (cursor_pos_t row = 1; row <= rowCount(); ++row)
        for (cursor_pos_t col = 1; col <= columnCount(); ++col)
            render(row, col, at(row, col));
}

string Screen::renderTextLine(size_t row) const
{
    string line;
    line.reserve(columnCount());
    for (cursor_pos_t col = 1; col <= columnCount(); ++col)
        if (auto const& cell = at(row, col); cell.character)
            line += utf8::to_string(utf8::encode(at(row, col).character));
        else
            line += " "; // fill character

    return line;
}

string Screen::renderText() const
{
    string text;
    text.reserve(rowCount_ * (columnCount_ + 1));

    for (size_t row = 1; row <= rowCount_; ++row)
    {
        text += renderTextLine(row);
        text += '\n';
    }

    return text;
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
    state_->linefeed();
}

void Screen::operator()(Backspace const& v)
{
    moveCursorTo(currentRow(), currentColumn() > 1 ? currentColumn() - 1 : 1);
}

void Screen::operator()(DeviceStatusReport const& v)
{
    reply("\033[0n");
}

void Screen::operator()(ReportCursorPosition const& v)
{
    reply("\033[?{};{}R", currentRow(), currentColumn());
}

void Screen::operator()(SendDeviceAttributes const& v)
{
    reply("\033[{}c",
          to_params(DeviceAttributes::Columns132 | DeviceAttributes::SelectiveErase
                  | DeviceAttributes::UserDefinedKeys | DeviceAttributes::NationalReplacementCharacterSets
                  | DeviceAttributes::TechnicalCharacters | DeviceAttributes::AnsiColor
                  | DeviceAttributes::AnsiTextLocator));
}

void Screen::operator()(SendTerminalId const& v)
{
    // terminal protocol type
    auto constexpr Pp = static_cast<unsigned>(VTType::VT220);

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
        fill(begin(*line), end(*line), Cell{});
}

void Screen::operator()(ClearToBeginOfScreen const& v)
{
    for (auto line = begin(state_->lines); line != next(state_->currentLine); ++line)
        fill(begin(*line), end(*line), Cell{});
}

void Screen::operator()(ClearScreen const& v)
{
    // https://vt100.net/docs/vt510-rm/ED.html
    for (auto& line : state_->lines)
        fill(begin(line), end(line), Cell{});
}

void Screen::operator()(ClearScrollbackBuffer const& v)
{
    state_->savedLines.clear();
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
        Cell{}
    );
}

void Screen::operator()(ClearToBeginOfLine const& v)
{
    fill(
        begin(*state_->currentLine),
        next(state_->currentColumn),
        Cell{}
    );
}

void Screen::operator()(ClearLine const& v)
{
    fill(
        begin(*state_->currentLine),
        end(*state_->currentLine),
        Cell{}
    );
}

void Screen::operator()(InsertLines const& v)
{
    for (cursor_pos_t i = 0; i < v.n; ++i)
        state_->lines.emplace(state_->currentLine, Buffer::Line{rowCount(), Cell{}});
}

void Screen::operator()(DeleteLines const& v)
{
    if (state_->margin_.vertical.from <= state_->cursor.row &&
        state_->cursor.row <= state_->margin_.vertical.to)
    {
        auto const marginTopAdjust = size_t{state_->cursor.row - state_->margin_.vertical.from};
        auto const margin = Buffer::Margin{
            { state_->margin_.vertical.from + marginTopAdjust, state_->margin_.vertical.to },
            state_->margin_.horizontal
        };

        state_->scrollUp(v.n, margin);
    }
}

void Screen::operator()(DeleteCharacters const& v)
{
    fill(
        state_->currentColumn,
        next(
            state_->currentColumn,
            min(
                static_cast<long int>(v.n),
                distance(state_->currentColumn, end(*state_->currentLine))
            )
        ),
        Cell{}
    );
}

void Screen::operator()(MoveCursorUp const& v)
{
    auto const n = min(v.n, currentRow() - 1);
    state_->cursor.row -= n;
    state_->currentLine = prev(state_->currentLine, n);
    state_->currentColumn = next(begin(*state_->currentLine), currentColumn() - 1);
    state_->verifyCursorIterators();
}

void Screen::operator()(MoveCursorDown const& v)
{
    auto const n = min(v.n, rowCount() - currentRow());
    state_->cursor.row += n;
    state_->currentLine = next(state_->currentLine, n);
    state_->currentColumn = next(begin(*state_->currentLine), currentColumn() - 1);
    state_->verifyCursorIterators();
}

void Screen::operator()(MoveCursorForward const& v)
{
    auto const n = min(v.n, columnCount_ - state_->cursor.column);
    state_->cursor.column += n;
    state_->currentColumn = next(
        state_->currentColumn,
        n
    );
    state_->verifyCursorIterators();
}

void Screen::operator()(MoveCursorBackward const& v)
{
    auto const n = min(v.n, state_->cursor.column - 1);
    state_->cursor.column -= n;
    state_->currentColumn = prev(state_->currentColumn, n);

    // even if you move to 80th of 80 columns, it'll first write a char and THEN flag wrap pending
    state_->wrapPending = false;

    state_->verifyCursorIterators();
}

void Screen::operator()(MoveCursorToColumn const& v)
{
    auto const n = min(v.column, columnCount());
    state_->cursor.column = n;
    state_->currentColumn = next(begin(*state_->currentLine), n - 1);
    state_->verifyCursorIterators();
}

void Screen::operator()(MoveCursorToBeginOfLine const& v)
{
    state_->wrapPending = false;
    state_->cursor.column = 1;
    state_->currentColumn = next(
        begin(*state_->currentLine),
        state_->cursor.column - 1
    );
    state_->verifyCursorIterators();
}

void Screen::operator()(MoveCursorTo const& v)
{
    moveCursorTo(v.row, v.column);
}

void Screen::operator()(MoveCursorToNextTab const& v)
{
    auto constexpr TabWidth = 8;
    auto const n = 1 + TabWidth - state_->cursor.column % TabWidth;
    (*this)(MoveCursorForward{n});
    // TODO: I guess something must remember when a \t was added, for proper move-back?
}

void Screen::operator()(HideCursor const& v)
{
}

void Screen::operator()(ShowCursor const& v)
{
}

void Screen::operator()(SaveCursor const& v)
{
    // https://vt100.net/docs/vt510-rm/DECSC.html
    // TODO: this doesn't just save the cursor, but:
    // * cursor x,y
    // * character attributes (SGR)
    // * character sets
    // * wrap flag
    // * state of origin mode
    // * selective erase attribute
    // * SS2/SS3 states

    // state_->saveStack.emplace(state_->cursor);
}

void Screen::operator()(RestoreCursor const& v)
{
    if (!state_->saveStack.empty())
    {
        auto& save = state_->saveStack.top();
        state_->cursor = save.cursor;
        state_->saveStack.pop();
    }
}

void Screen::operator()(Index const& v)
{
    if (currentRow() == state_->margin_.vertical.to)
        state_->scrollUp(1);
    else
        moveCursorTo(currentRow() + 1, currentColumn());
}

void Screen::operator()(ReverseIndex const& v)
{
    if (currentRow() == state_->margin_.vertical.from)
        state_->scrollDown(1);
    else
        moveCursorTo(currentRow() - 1, currentColumn());
}

void Screen::operator()(BackIndex const& v)
{
    if (currentColumn() == state_->margin_.horizontal.from)
        ;// scrollRight(1);
    else
        moveCursorTo(currentRow(), currentColumn() - 1);
}

void Screen::operator()(ForwardIndex const& v)
{
    if (currentColumn() == state_->margin_.horizontal.to)
        ;// scrollLeft(1);
    else
        moveCursorTo(currentRow(), currentColumn() + 1);
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
    setMode(v.mode, v.enable);
    switch (v.mode)
    {
        case Mode::UseAlternateScreen:
            if (v.enable)
                state_ = &alternateBuffer_;
            else
                state_ = &primaryBuffer_;
            break;
        case Mode::AutoWrap:
            state_->autoWrap = v.enable;
            break;
        default:
            break;
    }
}

void Screen::setMode(Mode _mode, bool _enable)
{
    if (_enable)
        enabledModes_.insert(_mode);
    else if (auto i = enabledModes_.find(_mode); i != enabledModes_.end())
        enabledModes_.erase(i);
}

void Screen::operator()(SetTopBottomMargin const& margin)
{
    state_->margin_.vertical.from = margin.top;
    state_->margin_.vertical.to = margin.bottom;
}

void Screen::operator()(SetLeftRightMargin const& margin)
{
    state_->margin_.horizontal.from = margin.left;
    state_->margin_.horizontal.to = margin.right;
}

void Screen::operator()(ScreenAlignmentPattern const&)
{
    // sets the margins to the extremes of the page
    state_->margin_.vertical.from = 1;
    state_->margin_.vertical.to = rowCount_;
    state_->margin_.horizontal.from = 1;
    state_->margin_.horizontal.to = columnCount_;

    // and moves the cursor to the home position
    moveCursorTo(1, 1);

    // fills the complete screen area with a test pattern
    for (auto& line: state_->lines)
        for (auto& col: line)
            col.character = 'X';
}

void Screen::operator()(SendMouseEvents const& v)
{
}

void Screen::operator()(AlternateKeypadMode const& v)
{
}

void Screen::operator()(DesignateCharset const& v)
{
}

void Screen::operator()(SingleShiftSelect const& v)
{
}

void Screen::operator()(ChangeWindowTitle const& v)
{
}

void Screen::operator()(ChangeIconName const& v)
{
}

void Screen::operator()(AppendChar const& v)
{
    state_->appendChar(v.ch);
}
// }}}

// {{{ others
void Screen::reset()
{
    primaryBuffer_ = Buffer{columnCount_, rowCount_};
    alternateBuffer_ = Buffer{columnCount_, rowCount_};
    state_ = &primaryBuffer_;
    // TODO: is this right? reverting to primary screen buffer?
}

void Screen::resetHard()
{
    reset(); // really?
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
    state_->cursor.row = clamp(to.row, cursor_pos_t{1}, rowCount());
    state_->cursor.column = clamp(to.column, cursor_pos_t{1}, columnCount());
    log("moveCursorTo: {}:{}", state_->cursor.row, state_->cursor.column);

    state_->updateCursorIterators();
}
// }}}

} // namespace terminal
