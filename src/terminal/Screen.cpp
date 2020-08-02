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
#include <terminal/VTType.h>
#include <terminal/Logger.h>

#include <crispy/algorithm.h>
#include <crispy/escape.h>
#include <crispy/times.h>
#include <crispy/utils.h>

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

Screen::Screen(WindowSize const& _size,
               ScreenEvents& _eventListener,
               Logger const& _logger,
               bool _logRaw,
               bool _logTrace,
               optional<size_t> _maxHistoryLineCount
) :
    eventListener_{ _eventListener },
    logger_{ _logger },
    logRaw_{ _logRaw },
    logTrace_{ _logTrace },
    commandBuilder_{ _logger },
    parser_{
        ref(commandBuilder_),
        [this](string const& _msg) { logger_(ParserErrorEvent{_msg}); }
    },
    modes_{},
    primaryBuffer_{ ScreenBuffer::Type::Main, _size, modes_, _maxHistoryLineCount },
    alternateBuffer_{ ScreenBuffer::Type::Alternate, _size, modes_, nullopt },
    buffer_{ &primaryBuffer_ },
    size_{ _size },
    maxHistoryLineCount_{ _maxHistoryLineCount },
    directExecutor_{ *this },
    synchronizedExecutor_{ *this },
    commandExecutor_ { &directExecutor_ }
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
    visit(*commandExecutor_, _command);

    buffer_->verifyState();
    instructionCounter_++;

    eventListener_.commands({_command});
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

    // #if defined(LIBTERMINAL_LOG_TRACE)
    // if (logTrace_ && logger_)
    // {
    //     auto const traces = to_mnemonic(commandBuilder_.commands(), true, true);
    //     for (auto const& trace : traces)
    //         logger_(TraceOutputEvent{trace});
    // }
    // #endif

    for_each(
        commandBuilder_.commands(),
        [&](Command const& _command) {
            buffer_->verifyState();
#if defined(LIBTERMINAL_LOG_TRACE)
            auto const trace = to_mnemonic(_command, true, true);
            logger_(TraceOutputEvent{trace});
#endif
            visit(*commandExecutor_, _command);
            instructionCounter_++;
            buffer_->verifyState();
        }
    );

    eventListener_.commands(commandBuilder_.commands());
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

string Screen::renderHistoryTextLine(cursor_pos_t _lineNumberIntoHistory) const
{
    assert(1 <= _lineNumberIntoHistory && _lineNumberIntoHistory <= buffer_->historyLineCount());
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

void Screen::renderSelection(terminal::Screen::Renderer const& _render) const
{
    if (selector_)
        selector_->render(_render);
}

vector<Selector::Range> Screen::selection() const
{
    if (selector_)
        return selector_->selection();
    else
        return {};
}

// {{{ viewport management
bool Screen::isLineVisible(cursor_pos_t _row) const noexcept
{
    return crispy::ascending(1 - scrollOffset_, _row, size_.rows - scrollOffset_);
}

bool Screen::scrollUp(int _numLines)
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

bool Screen::scrollDown(int _numLines)
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
    if (auto const newScrollOffset = buffer_->findMarkerBackward(-scrollOffset_); newScrollOffset.has_value())
    {
        scrollOffset_ = 1 - newScrollOffset.value();
        return true;
    }

    return false;
}

bool Screen::scrollMarkDown()
{
    if (auto const newScrollOffset = buffer_->findMarkerForward(1 - scrollOffset_); newScrollOffset.has_value())
    {
        scrollOffset_ = *newScrollOffset < 0 ? 1 - newScrollOffset.value() : 0;
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

// {{{ others
void Screen::saveCursor()
{
    // https://vt100.net/docs/vt510-rm/DECSC.html

    savedCursors_.emplace(currentBuffer().cursor);
}

void Screen::restoreCursor()
{
    if (!savedCursors_.empty())
    {
        auto const& saved = savedCursors_.top();
        moveCursorTo(saved.position);
        setMode(Mode::AutoWrap, saved.autoWrap);
        setMode(Mode::Origin, saved.originMode);
        savedCursors_.pop();
    }
}

void Screen::resetSoft()
{
    (*this)(SetMode{Mode::BatchedRendering, false});
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
                eventListener_.setMouseWheelMode(InputGenerator::MouseWheelMode::Default);
                buffer_ = &primaryBuffer_;
                break;
            case ScreenBuffer::Type::Alternate:
                if (buffer_->isModeEnabled(Mode::MouseAlternateScroll))
                    eventListener_.setMouseWheelMode(InputGenerator::MouseWheelMode::ApplicationCursorKeys);
                else
                    eventListener_.setMouseWheelMode(InputGenerator::MouseWheelMode::NormalCursorKeys);
                buffer_ = &alternateBuffer_;
                break;
        }

        if (selector_)
            selector_.reset();

        eventListener_.bufferChanged(_type);
    }
}
// }}}

// {{{ ops
void Screen::operator()(Bell const&)
{
    eventListener_.bell();
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

void Screen::operator()(SelectConformanceLevel const& v)
{
    // Don't enforce the selected conformance level, just remember it.
    terminalId_ = v.level;
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

void Screen::operator()(CopyToClipboard const& v)
{
    eventListener_.copyToClipboard(v.data);
}

void Screen::operator()(ClearToEndOfScreen const&)
{
    if (isAlternateScreen() && buffer_->cursor.position.row == 1 && buffer_->cursor.position.column == 1)
        buffer_->hyperlinks.clear();

    (*this)(ClearToEndOfLine{});

    for_each(
        LIBTERMINAL_EXECUTION_COMMA(par)
        next(buffer_->currentLine),
        end(buffer_->lines),
        [&](ScreenBuffer::Line& line) {
            fill(begin(line), end(line), Cell{{}, buffer_->cursor.graphicsRendition});
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
            fill(begin(line), end(line), Cell{{}, buffer_->cursor.graphicsRendition});
        }
    );
}

void Screen::operator()(ClearScreen const&)
{
    // Instead of *just* clearing the screen, and thus, losing potential important content,
    // we scroll up by RowCount number of lines, so move it all into history, so the user can scroll
    // up in case the content is still needed.
    buffer_->scrollUp(size().rows);
}

void Screen::operator()(ClearScrollbackBuffer const&)
{
    if (selector_)
        selector_.reset();

    buffer_->savedLines.clear();
}

void Screen::operator()(EraseCharacters const& v)
{
    // Spec: https://vt100.net/docs/vt510-rm/ECH.html
    // It's not clear from the spec how to perform erase when inside margin and number of chars to be erased would go outside margins.
    // TODO: See what xterm does ;-)
    size_t const n = min(buffer_->size_.columns - realCursorPosition().column + 1, v.n == 0 ? 1 : v.n);
    fill_n(buffer_->currentColumn, n, Cell{{}, buffer_->cursor.graphicsRendition});
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
        Cell{{}, buffer_->cursor.graphicsRendition}
    );
}

void Screen::operator()(ClearToBeginOfLine const&)
{
    fill(
        begin(*buffer_->currentLine),
        next(buffer_->currentColumn),
        Cell{{}, buffer_->cursor.graphicsRendition}
    );
}

void Screen::operator()(ClearLine const&)
{
    fill(
        begin(*buffer_->currentLine),
        end(*buffer_->currentLine),
        Cell{{}, buffer_->cursor.graphicsRendition}
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
                { buffer_->cursor.position.row, buffer_->margin_.vertical.to },
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
                { buffer_->cursor.position.row, buffer_->margin_.vertical.to },
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
    auto const n = min(
        v.n,
        cursorPosition().row > buffer_->margin_.vertical.from
            ? cursorPosition().row - buffer_->margin_.vertical.from
            : cursorPosition().row
    );

    buffer_->cursor.position.row -= n;
    buffer_->currentLine = prev(buffer_->currentLine, n);
    buffer_->setCurrentColumn(cursorPosition().column);
    buffer_->verifyState();
}

void Screen::operator()(MoveCursorDown const& v)
{
    auto const n = min(v.n, size_.rows - cursorPosition().row);
    buffer_->cursor.position.row += n;
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
    auto const n = min(v.n, buffer_->cursor.position.column - 1);
    buffer_->setCurrentColumn(buffer_->cursor.position.column - n);
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
    moveCursorTo({v.row, buffer_->cursor.position.column});
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
                buffer_->tabWidth - (buffer_->cursor.position.column - 1) % buffer_->tabWidth,
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
    eventListener_.notify(_notify.title, _notify.content);
}

void Screen::operator()(CursorBackwardTab const& v)
{
    if (v.count == 0)
        return;

    if (!buffer_->tabs.empty())
    {
        for (int k = 0; k < v.count; ++k)
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
        if (buffer_->cursor.position.column <= buffer_->tabWidth)
            (*this)(MoveCursorToBeginOfLine{});
        else
        {
            auto const m = buffer_->cursor.position.column % buffer_->tabWidth;
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
    saveCursor();
}

void Screen::operator()(RestoreCursor const&)
{
    restoreCursor();
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
    buffer_->cursor.graphicsRendition.foregroundColor = v.color;
}

void Screen::operator()(SetBackgroundColor const& v)
{
    buffer_->cursor.graphicsRendition.backgroundColor = v.color;
}

void Screen::operator()(SetUnderlineColor const& v)
{
    buffer_->cursor.graphicsRendition.underlineColor = v.color;
}

void Screen::operator()(SetCursorStyle const& v)
{
    eventListener_.setCursorStyle(v.display, v.shape);
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
            buffer_->cursor.graphicsRendition = {};
            break;
        case GraphicsRendition::Bold:
            buffer_->cursor.graphicsRendition.styles |= CharacterStyleMask::Bold;
            break;
        case GraphicsRendition::Faint:
            buffer_->cursor.graphicsRendition.styles |= CharacterStyleMask::Faint;
            break;
        case GraphicsRendition::Italic:
            buffer_->cursor.graphicsRendition.styles |= CharacterStyleMask::Italic;
            break;
        case GraphicsRendition::Underline:
            buffer_->cursor.graphicsRendition.styles |= CharacterStyleMask::Underline;
            break;
        case GraphicsRendition::Blinking:
            buffer_->cursor.graphicsRendition.styles |= CharacterStyleMask::Blinking;
            break;
        case GraphicsRendition::Inverse:
            buffer_->cursor.graphicsRendition.styles |= CharacterStyleMask::Inverse;
            break;
        case GraphicsRendition::Hidden:
            buffer_->cursor.graphicsRendition.styles |= CharacterStyleMask::Hidden;
            break;
        case GraphicsRendition::CrossedOut:
            buffer_->cursor.graphicsRendition.styles |= CharacterStyleMask::CrossedOut;
            break;
        case GraphicsRendition::DoublyUnderlined:
            buffer_->cursor.graphicsRendition.styles |= CharacterStyleMask::DoublyUnderlined;
            break;
        case GraphicsRendition::CurlyUnderlined:
            buffer_->cursor.graphicsRendition.styles |= CharacterStyleMask::CurlyUnderlined;
            break;
        case GraphicsRendition::DottedUnderline:
            buffer_->cursor.graphicsRendition.styles |= CharacterStyleMask::DottedUnderline;
            break;
        case GraphicsRendition::DashedUnderline:
            buffer_->cursor.graphicsRendition.styles |= CharacterStyleMask::DashedUnderline;
            break;
        case GraphicsRendition::Framed:
            buffer_->cursor.graphicsRendition.styles |= CharacterStyleMask::Framed;
            break;
        case GraphicsRendition::Overline:
            buffer_->cursor.graphicsRendition.styles |= CharacterStyleMask::Overline;
            break;
        case GraphicsRendition::Normal:
            buffer_->cursor.graphicsRendition.styles &= ~(CharacterStyleMask::Bold | CharacterStyleMask::Faint);
            break;
        case GraphicsRendition::NoItalic:
            buffer_->cursor.graphicsRendition.styles &= ~CharacterStyleMask::Italic;
            break;
        case GraphicsRendition::NoUnderline:
            buffer_->cursor.graphicsRendition.styles &= ~CharacterStyleMask::Underline;
            break;
        case GraphicsRendition::NoBlinking:
            buffer_->cursor.graphicsRendition.styles &= ~CharacterStyleMask::Blinking;
            break;
        case GraphicsRendition::NoInverse:
            buffer_->cursor.graphicsRendition.styles &= ~CharacterStyleMask::Inverse;
            break;
        case GraphicsRendition::NoHidden:
            buffer_->cursor.graphicsRendition.styles &= ~CharacterStyleMask::Hidden;
            break;
        case GraphicsRendition::NoCrossedOut:
            buffer_->cursor.graphicsRendition.styles &= ~CharacterStyleMask::CrossedOut;
            break;
        case GraphicsRendition::NoFramed:
            buffer_->cursor.graphicsRendition.styles &= ~CharacterStyleMask::Framed;
            break;
        case GraphicsRendition::NoOverline:
            buffer_->cursor.graphicsRendition.styles &= ~CharacterStyleMask::Overline;
            break;
    }
}

void Screen::operator()(SetMark const&)
{
    buffer_->currentLine->marked = true;
}

void Screen::operator()(SetMode const& v)
{
    setMode(v.mode, v.enable);
}

void Screen::setMode(Mode _mode, bool _enable)
{
    switch (_mode)
    {
        case Mode::BatchedRendering:
            if (_enable)
                commandExecutor_ = &synchronizedExecutor_;
            else
            {
                commandExecutor_ = &directExecutor_;
                synchronizedExecutor_.flush();
            }
            break;
        case Mode::UseAlternateScreen:
            if (_enable)
                setBuffer(ScreenBuffer::Type::Alternate);
            else
                setBuffer(ScreenBuffer::Type::Main);
            break;
        case Mode::UseApplicationCursorKeys:
            eventListener_.useApplicationCursorKeys(_enable);
            if (isAlternateScreen())
            {
                if (_enable)
                    eventListener_.setMouseWheelMode(InputGenerator::MouseWheelMode::ApplicationCursorKeys);
                else
                    eventListener_.setMouseWheelMode(InputGenerator::MouseWheelMode::NormalCursorKeys);
            }
            break;
        case Mode::BracketedPaste:
            eventListener_.setBracketedPaste(_enable);
            break;
        case Mode::MouseSGR:
            eventListener_.setMouseTransport(MouseTransport::SGR);
            break;
        case Mode::MouseExtended:
            eventListener_.setMouseTransport(MouseTransport::Extended);
            break;
        case Mode::MouseURXVT:
            eventListener_.setMouseTransport(MouseTransport::URXVT);
            break;
        case Mode::MouseAlternateScroll:
            if (_enable)
                eventListener_.setMouseWheelMode(InputGenerator::MouseWheelMode::ApplicationCursorKeys);
            else
                eventListener_.setMouseWheelMode(InputGenerator::MouseWheelMode::NormalCursorKeys);
            break;
        case Mode::FocusTracking:
            eventListener_.setGenerateFocusEvents(_enable);
            break;
        default:
            break;
    }

    modes_.set(_mode, _enable);
    buffer_->setMode(_mode, _enable);
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
                Cell{'X', buffer_->cursor.graphicsRendition}
            );
        }
    );
}

void Screen::operator()(SendMouseEvents const& v)
{
    eventListener_.setMouseProtocol(v.protocol, v.enable);
}

void Screen::operator()(ApplicationKeypadMode const& v)
{
    eventListener_.setApplicationkeypadMode(v.enable);
}

void Screen::operator()(DesignateCharset const& v)
{
    // TODO: unit test SCS and see if they also behave well with reset/softreset
    // Also, is the cursor shared between the two buffers?
    buffer_->cursor.charsets.select(v.table, v.charset);
}

void Screen::operator()(SingleShiftSelect const& v)
{
    // TODO: unit test SS2, SS3
    buffer_->cursor.charsets.singleShift(v.table);
}

void Screen::operator()(SoftTerminalReset const&)
{
    resetSoft();
}

void Screen::operator()(ChangeIconTitle const&)
{
    // Not supported (for now), ignored.
}

void Screen::operator()(ChangeWindowTitle const& v)
{
    windowTitle_ = v.title;

    eventListener_.setWindowTitle(v.title);
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

        eventListener_.setWindowTitle(windowTitle_);
    }
}

void Screen::operator()(ResizeWindow const& v)
{
    eventListener_.resizeWindow(v.width, v.height, v.unit == ResizeWindow::Unit::Pixels);
}

void Screen::operator()(AppendChar const& v)
{
    buffer_->appendChar(v.ch, instructionCounter_ == 1);
    instructionCounter_ = 0;
}

void Screen::operator()(RequestDynamicColor const& v)
{
    if (auto const color = eventListener_.requestDynamicColor(v.name); color.has_value())
    {
        reply(
            "\033]{};{}\x07",
            setDynamicColorCommand(v.name),
            setDynamicColorValue(color.value())
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
        for (int column = 2 * buffer_->tabWidth + 1; column <= size().columns; column += buffer_->tabWidth)
            dcs << '/' << column;
    }
    dcs << '\x5c'; // ST

    reply(dcs.str());
}

void Screen::operator()(ResetDynamicColor const& v)
{
    eventListener_.resetDynamicColor(v.name);
}

void Screen::operator()(SetDynamicColor const& v)
{
    eventListener_.setDynamicColor(v.name, v.color);
}

void Screen::operator()(DumpState const&)
{
    eventListener_.dumpState();
}
// }}}

// {{{ CommandExecutor
void CommandExecutor::visit(AppendChar const& v) { screen_(v); }
void CommandExecutor::visit(ApplicationKeypadMode const& v) { screen_(v); }
void CommandExecutor::visit(BackIndex const& v) { screen_(v); }
void CommandExecutor::visit(Backspace const& v) { screen_(v); }
void CommandExecutor::visit(Bell const& v) { screen_(v); }
void CommandExecutor::visit(ChangeIconTitle const& v) { screen_(v); }
void CommandExecutor::visit(ChangeWindowTitle const& v) { screen_(v); }
void CommandExecutor::visit(ClearLine const& v) { screen_(v); }
void CommandExecutor::visit(ClearScreen const& v) { screen_(v); }
void CommandExecutor::visit(ClearScrollbackBuffer const& v) { screen_(v); }
void CommandExecutor::visit(ClearToBeginOfLine const& v) { screen_(v); }
void CommandExecutor::visit(ClearToBeginOfScreen const& v) { screen_(v); }
void CommandExecutor::visit(ClearToEndOfLine const& v) { screen_(v); }
void CommandExecutor::visit(ClearToEndOfScreen const& v) { screen_(v); }
void CommandExecutor::visit(CopyToClipboard const& v) { screen_(v); }
void CommandExecutor::visit(CursorBackwardTab const& v) { screen_(v); }
void CommandExecutor::visit(CursorNextLine const& v) { screen_(v); }
void CommandExecutor::visit(CursorPreviousLine const& v) { screen_(v); }
void CommandExecutor::visit(DeleteCharacters const& v) { screen_(v); }
void CommandExecutor::visit(DeleteColumns const& v) { screen_(v); }
void CommandExecutor::visit(DeleteLines const& v) { screen_(v); }
void CommandExecutor::visit(DesignateCharset const& v) { screen_(v); }
void CommandExecutor::visit(DeviceStatusReport const& v) { screen_(v); }
void CommandExecutor::visit(DumpState const& v) { screen_(v); }
void CommandExecutor::visit(EraseCharacters const& v) { screen_(v); }
void CommandExecutor::visit(ForwardIndex const& v) { screen_(v); }
void CommandExecutor::visit(FullReset const& v) { screen_(v); }
void CommandExecutor::visit(HorizontalPositionAbsolute const& v) { screen_(v); }
void CommandExecutor::visit(HorizontalPositionRelative const& v) { screen_(v); }
void CommandExecutor::visit(HorizontalTabClear const& v) { screen_(v); }
void CommandExecutor::visit(HorizontalTabSet const& v) { screen_(v); }
void CommandExecutor::visit(Hyperlink const& v) { screen_(v); }
void CommandExecutor::visit(Index const& v) { screen_(v); }
void CommandExecutor::visit(InsertCharacters const& v) { screen_(v); }
void CommandExecutor::visit(InsertColumns const& v) { screen_(v); }
void CommandExecutor::visit(InsertLines const& v) { screen_(v); }
void CommandExecutor::visit(Linefeed const& v) { screen_(v); }
void CommandExecutor::visit(MoveCursorBackward const& v) { screen_(v); }
void CommandExecutor::visit(MoveCursorDown const& v) { screen_(v); }
void CommandExecutor::visit(MoveCursorForward const& v) { screen_(v); }
void CommandExecutor::visit(MoveCursorTo const& v) { screen_(v); }
void CommandExecutor::visit(MoveCursorToBeginOfLine const& v) { screen_(v); }
void CommandExecutor::visit(MoveCursorToColumn const& v) { screen_(v); }
void CommandExecutor::visit(MoveCursorToLine const& v) { screen_(v); }
void CommandExecutor::visit(MoveCursorToNextTab const& v) { screen_(v); }
void CommandExecutor::visit(MoveCursorUp const& v) { screen_(v); }
void CommandExecutor::visit(Notify const& v) { screen_(v); }
void CommandExecutor::visit(ReportCursorPosition const& v) { screen_(v); }
void CommandExecutor::visit(ReportExtendedCursorPosition const& v) { screen_(v); }
void CommandExecutor::visit(RequestDynamicColor const& v) { screen_(v); }
void CommandExecutor::visit(RequestMode const& v) { screen_(v); }
void CommandExecutor::visit(RequestTabStops const& v) { screen_(v); }
void CommandExecutor::visit(ResetDynamicColor const& v) { screen_(v); }
void CommandExecutor::visit(ResizeWindow const& v) { screen_(v); }
void CommandExecutor::visit(RestoreCursor const& v) { screen_(v); }
void CommandExecutor::visit(RestoreWindowTitle const& v) { screen_(v); }
void CommandExecutor::visit(ReverseIndex const& v) { screen_(v); }
void CommandExecutor::visit(SaveCursor const& v) { screen_(v); }
void CommandExecutor::visit(SaveWindowTitle const& v) { screen_(v); }
void CommandExecutor::visit(ScreenAlignmentPattern const& v) { screen_(v); }
void CommandExecutor::visit(ScrollDown const& v) { screen_(v); }
void CommandExecutor::visit(ScrollUp const& v) { screen_(v); }
void CommandExecutor::visit(SelectConformanceLevel const& v) { screen_(v); }
void CommandExecutor::visit(SendDeviceAttributes const& v) { screen_(v); }
void CommandExecutor::visit(SendMouseEvents const& v) { screen_(v); }
void CommandExecutor::visit(SendTerminalId const& v) { screen_(v); }
void CommandExecutor::visit(SetBackgroundColor const& v) { screen_(v); }
void CommandExecutor::visit(SetCursorStyle const& v) { screen_(v); }
void CommandExecutor::visit(SetDynamicColor const& v) { screen_(v); }
void CommandExecutor::visit(SetForegroundColor const& v) { screen_(v); }
void CommandExecutor::visit(SetGraphicsRendition const& v) { screen_(v); }
void CommandExecutor::visit(SetLeftRightMargin const& v) { screen_(v); }
void CommandExecutor::visit(SetMark const& v) { screen_(v); }
void CommandExecutor::visit(SetMode const& v) { screen_(v); }
void CommandExecutor::visit(SetTopBottomMargin const& v) { screen_(v); }
void CommandExecutor::visit(SetUnderlineColor const& v) { screen_(v); }
void CommandExecutor::visit(SingleShiftSelect const& v) { screen_(v); }
void CommandExecutor::visit(SoftTerminalReset const& v) { screen_(v); }
// }}}

// {{{ SynchronizedCommandExecutor
void SynchronizedCommandExecutor::flush()
{
    for (Command const& cmd : queuedCommands_)
        screen_.write(cmd);

    queuedCommands_.clear();
}
/// }}}

} // namespace terminal
