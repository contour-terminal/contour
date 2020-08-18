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
#include <terminal/Debugger.h>
#include <terminal/Size.h>

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

Screen::Screen(Size const& _size,
               ScreenEvents& _eventListener,
               Logger const& _logger,
               bool _logRaw,
               bool _logTrace,
               optional<size_t> _maxHistoryLineCount,
               Size _maxImageSize,
               int _maxImageColorRegisters,
               bool _sixelCursorConformance
) :
    eventListener_{ _eventListener },
    logger_{ _logger },
    logRaw_{ _logRaw },
    logTrace_{ _logTrace },
    modes_{},
    maxImageColorRegisters_{ _maxImageColorRegisters },
    imageColorPalette_(make_shared<ColorPalette>(16, maxImageColorRegisters_)),
    imagePool_{
        [this](Image const* _image) { eventListener_.discardImage(*_image); },
        1
    },
    commandBuilder_{
        _logger,
        _maxImageSize,
        RGBAColor{}, // TODO
        imageColorPalette_
    },
    parser_{
        ref(commandBuilder_),
        [this](string const& _msg) { logger_(ParserErrorEvent{_msg}); }
    },
    primaryBuffer_{ ScreenBuffer::Type::Main, _size, modes_, _maxHistoryLineCount },
    alternateBuffer_{ ScreenBuffer::Type::Alternate, _size, modes_, nullopt },
    buffer_{ &primaryBuffer_ },
    size_{ _size },
    maxHistoryLineCount_{ _maxHistoryLineCount },
    directExecutor_{ *this, _logger },
    synchronizedExecutor_{ *this, _logger },
    debugExecutor_{},
    commandExecutor_ {&directExecutor_},
    sixelCursorConformance_{_sixelCursorConformance}
{
    setMode(Mode::AutoWrap, true);
}

Debugger* Screen::debugger() noexcept
{
    return static_cast<Debugger*>(debugExecutor_.get());
}

void Screen::setMaxHistoryLineCount(std::optional<size_t> _maxHistoryLineCount)
{
    maxHistoryLineCount_ = _maxHistoryLineCount;

    primaryBuffer_.maxHistoryLineCount_ = _maxHistoryLineCount;
    primaryBuffer_.clampSavedLines();

    // Alternate buffer does not have a history usually (and for now we keep it that way).
}

void Screen::resize(Size const& _newSize)
{
    // TODO: only resize current screen buffer, and then make sure we resize the other upon actual switch
    primaryBuffer_.resize(_newSize);
    alternateBuffer_.resize(_newSize);
    size_ = _newSize;
}

void Screen::write(Command const& _command)
{
    buffer_->verifyState();

    if (debugging())
        visit(directExecutor_, _command);
    else
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

void Screen::writeText(char32_t _char)
{
    buffer_->appendChar(_char, instructionCounter_ == 1);
    instructionCounter_ = 0;
}

string Screen::renderHistoryTextLine(cursor_pos_t _lineNumberIntoHistory) const
{
    assert(1 <= _lineNumberIntoHistory && _lineNumberIntoHistory <= buffer_->historyLineCount());
    string line;
    line.reserve(size_.width);
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
    return crispy::ascending(1 - relativeScrollOffset(), _row, size_.height - relativeScrollOffset());
}

bool Screen::scrollUp(int _numLines)
{
    if (isAlternateScreen()) // TODO: make configurable
        return false;

    if (_numLines <= 0)
        return false;

    if (auto const newOffset = max(absoluteScrollOffset().value_or(historyLineCount()) - _numLines, 0); newOffset != absoluteScrollOffset())
    {
        scrollOffset_.emplace(newOffset);
        return true;
    }
    else
        return false;
}

bool Screen::scrollDown(int _numLines)
{
    if (isAlternateScreen()) // TODO: make configurable
        return false;

    if (_numLines <= 0)
        return false;

    auto const newOffset = absoluteScrollOffset().value_or(historyLineCount()) + _numLines;
    if (newOffset < historyLineCount())
    {
        scrollOffset_.emplace(newOffset);
        return true;
    }
    else if (newOffset == historyLineCount() || scrollOffset_.has_value())
    {
        scrollOffset_.reset();
        return true;
    }
    else
        return false;
}

bool Screen::scrollMarkUp()
{
    auto const newScrollOffset = buffer_->findMarkerBackward(absoluteScrollOffset().value_or(historyLineCount()));
    if (newScrollOffset.has_value())
    {
        scrollOffset_.emplace(newScrollOffset.value());
        return true;
    }

    return false;
}

bool Screen::scrollMarkDown()
{
    auto const newScrollOffset = buffer_->findMarkerForward(absoluteScrollOffset().value_or(historyLineCount()));

    if (!newScrollOffset.has_value())
        return false;

    if (*newScrollOffset < historyLineCount())
        scrollOffset_.emplace(*newScrollOffset);
    else
        scrollOffset_.reset();

    return true;
}

bool Screen::scrollToTop()
{
    if (absoluteScrollOffset() != 0)
    {
        scrollOffset_.emplace(0);
        return true;
    }
    else
        return false;
}

bool Screen::scrollToBottom()
{
    if (scrollOffset_.has_value())
    {
        scrollOffset_.reset();
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
    currentBuffer().savedCursor = currentBuffer().cursor;
}

void Screen::restoreCursor()
{
    // https://vt100.net/docs/vt510-rm/DECRC.html
    buffer_->setCursor(currentBuffer().savedCursor);
    setMode(Mode::AutoWrap, currentBuffer().savedCursor.autoWrap);
    setMode(Mode::Origin, currentBuffer().savedCursor.originMode);
}

void Screen::resetSoft()
{
    setMode(Mode::BatchedRendering, false);
    setGraphicsRendition(GraphicsRendition::Reset); // SGR
    moveCursorTo({1, 1}); // DECSC (Save cursor state)
    setMode(Mode::VisibleCursor, true); // DECTCEM (Text cursor enable)
    setMode(Mode::Origin, false); // DECOM
    setMode(Mode::KeyboardAction, false); // KAM
    setMode(Mode::AutoWrap, false); // DECAWM
    setMode(Mode::Insert, false); // IRM
    setMode(Mode::UseApplicationCursorKeys, false); // DECCKM (Cursor keys)
    setTopBottomMargin(1, size().height); // DECSTBM
    setLeftRightMargin(1, size().width); // DECRLM

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
        if (selector_)
            selector_.reset();

        switch (_type)
        {
            case ScreenBuffer::Type::Main:
                eventListener_.setMouseWheelMode(InputGenerator::MouseWheelMode::Default);
                buffer_ = &primaryBuffer_;
                break;
            case ScreenBuffer::Type::Alternate:
                scrollToBottom();
                if (buffer_->isModeEnabled(Mode::MouseAlternateScroll))
                    eventListener_.setMouseWheelMode(InputGenerator::MouseWheelMode::ApplicationCursorKeys);
                else
                    eventListener_.setMouseWheelMode(InputGenerator::MouseWheelMode::NormalCursorKeys);
                buffer_ = &alternateBuffer_;
                break;
        }

        eventListener_.bufferChanged(_type);
    }
}
// }}}

// {{{ ops
void Screen::linefeed()
{
    if (isModeEnabled(Mode::AutomaticNewLine))
        buffer_->linefeed(buffer_->margin_.horizontal.from);
    else
        buffer_->linefeed(realCursorPosition().column);
}

void Screen::backspace()
{
    moveCursorTo({cursorPosition().row, cursorPosition().column > 1 ? cursorPosition().column - 1 : 1});
}

void Screen::deviceStatusReport()
{
    reply("\033[0n");
}

void Screen::reportCursorPosition()
{
    reply("\033[{};{}R", cursorPosition().row, cursorPosition().column);
}

void Screen::reportExtendedCursorPosition()
{
    auto const pageNum = 1;
    reply("\033[{};{};{}R", cursorPosition().row, cursorPosition().column, pageNum);
}

void Screen::selectConformanceLevel(VTType _level)
{
    // Don't enforce the selected conformance level, just remember it.
    terminalId_ = _level;
}

void Screen::sendDeviceAttributes()
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
        DeviceAttributes::SixelGraphics |
        //TODO: DeviceAttributes::TechnicalCharacters |
        DeviceAttributes::UserDefinedKeys
    );

    reply("\033[?{};{}c", id, attrs);
}

void Screen::sendTerminalId()
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

void Screen::clearToEndOfScreen()
{
    if (isAlternateScreen() && buffer_->cursor.position.row == 1 && buffer_->cursor.position.column == 1)
        buffer_->hyperlinks.clear();

    clearToEndOfLine();

    for_each(
        LIBTERMINAL_EXECUTION_COMMA(par)
        next(buffer_->currentLine),
        end(buffer_->lines),
        [&](ScreenBuffer::Line& line) {
            fill(begin(line), end(line), Cell{{}, buffer_->cursor.graphicsRendition});
        }
    );
}

void Screen::clearToBeginOfScreen()
{
    clearToBeginOfLine();

    for_each(
        LIBTERMINAL_EXECUTION_COMMA(par)
        begin(buffer_->lines),
        buffer_->currentLine,
        [&](ScreenBuffer::Line& line) {
            fill(begin(line), end(line), Cell{{}, buffer_->cursor.graphicsRendition});
        }
    );
}

void Screen::clearScreen()
{
    // Instead of *just* clearing the screen, and thus, losing potential important content,
    // we scroll up by RowCount number of lines, so move it all into history, so the user can scroll
    // up in case the content is still needed.
    buffer_->scrollUp(size().height);
}

void Screen::clearScrollbackBuffer()
{
    if (selector_)
        selector_.reset();

    buffer_->savedLines.clear();
}

void Screen::eraseCharacters(int _n)
{
    // Spec: https://vt100.net/docs/vt510-rm/ECH.html
    // It's not clear from the spec how to perform erase when inside margin and number of chars to be erased would go outside margins.
    // TODO: See what xterm does ;-)
    size_t const n = min(buffer_->size_.width - realCursorPosition().column + 1, _n == 0 ? 1 : _n);
    fill_n(buffer_->currentColumn, n, Cell{{}, buffer_->cursor.graphicsRendition});
}

void Screen::clearToEndOfLine()
{
    fill(
        buffer_->currentColumn,
        end(*buffer_->currentLine),
        Cell{{}, buffer_->cursor.graphicsRendition}
    );
}

void Screen::clearToBeginOfLine()
{
    fill(
        begin(*buffer_->currentLine),
        next(buffer_->currentColumn),
        Cell{{}, buffer_->cursor.graphicsRendition}
    );
}

void Screen::clearLine()
{
    fill(
        begin(*buffer_->currentLine),
        end(*buffer_->currentLine),
        Cell{{}, buffer_->cursor.graphicsRendition}
    );
}

void Screen::moveCursorToNextLine(int _n)
{
    buffer_->moveCursorTo({cursorPosition().row + _n, 1});
}

void Screen::moveCursorToPrevLine(int _n)
{
    auto const n = min(_n, cursorPosition().row - 1);
    buffer_->moveCursorTo({cursorPosition().row - n, 1});
}

void Screen::insertCharacters(int _n)
{
    if (isCursorInsideMargins())
        buffer_->insertChars(realCursorPosition().row, _n);
}

void Screen::insertLines(int _n)
{
    if (isCursorInsideMargins())
    {
        buffer_->scrollDown(
            _n,
            Margin{
                { buffer_->cursor.position.row, buffer_->margin_.vertical.to },
                buffer_->margin_.horizontal
            }
        );
    }
}

void Screen::insertColumns(int _n)
{
    if (isCursorInsideMargins())
        buffer_->insertColumns(_n);
}

void Screen::deleteLines(int _n)
{
    if (isCursorInsideMargins())
    {
        buffer_->scrollUp(
            _n,
            Margin{
                { buffer_->cursor.position.row, buffer_->margin_.vertical.to },
                buffer_->margin_.horizontal
            }
        );
    }
}

void Screen::deleteCharacters(int _n)
{
    if (isCursorInsideMargins() && _n != 0)
        buffer_->deleteChars(realCursorPosition().row, _n);
}

void Screen::deleteColumns(int _n)
{
    if (isCursorInsideMargins())
        for (cursor_pos_t lineNo = buffer_->margin_.vertical.from; lineNo <= buffer_->margin_.vertical.to; ++lineNo)
            buffer_->deleteChars(lineNo, _n);
}

void Screen::horizontalTabClear(HorizontalTabClear::Which _which)
{
    switch (_which)
    {
        case HorizontalTabClear::AllTabs:
            buffer_->clearAllTabs();
            break;
        case HorizontalTabClear::UnderCursor:
            buffer_->clearTabUnderCursor();
            break;
    }
}

void Screen::horizontalTabSet()
{
    buffer_->setTabUnderCursor();
}

void Screen::hyperlink(string const& _id, string const& _uri)
{
    if (_uri.empty())
        buffer_->currentHyperlink = nullptr;
    else if (_id.empty())
        buffer_->currentHyperlink = make_shared<HyperlinkInfo>(HyperlinkInfo{_id, _uri});
    else if (auto i = buffer_->hyperlinks.find(_id); i != buffer_->hyperlinks.end())
        buffer_->currentHyperlink = i->second;
    else
    {
        buffer_->currentHyperlink = make_shared<HyperlinkInfo>(HyperlinkInfo{_id, _uri});
        buffer_->hyperlinks[_id] = buffer_->currentHyperlink;
    }
    // TODO:
    // Care about eviction.
    // Move hyperlink store into ScreenBuffer, so it gets reset upon every switch into
    // alternate screen (not for main screen!)
}

void Screen::moveCursorUp(int _n)
{
    auto const n = min(
        _n,
        cursorPosition().row > buffer_->margin_.vertical.from
            ? cursorPosition().row - buffer_->margin_.vertical.from
            : cursorPosition().row - 1
    );

    buffer_->cursor.position.row -= n;
    buffer_->currentLine = prev(buffer_->currentLine, n);
    buffer_->setCurrentColumn(cursorPosition().column);
    buffer_->verifyState();
}

void Screen::moveCursorDown(int _n)
{
    auto const currentLineNumber = cursorPosition().row;
    auto const n = min(
        _n,
        currentLineNumber <= buffer_->margin_.vertical.to
            ? buffer_->margin_.vertical.to - currentLineNumber
            : size_.height - currentLineNumber
    );
    // auto const n =
    //     v.n > buffer_->margin_.vertical.to
    //         ? min(v.n, size_.height - cursorPosition().row)
    //         : min(v.n, buffer_->margin_.vertical.to - cursorPosition().row);

    buffer_->cursor.position.row += n;
    buffer_->currentLine = next(buffer_->currentLine, n);
    buffer_->setCurrentColumn(cursorPosition().column);
}

void Screen::moveCursorForward(int _n)
{
    buffer_->incrementCursorColumn(_n);
}

void Screen::moveCursorBackward(int _n)
{
    // even if you move to 80th of 80 columns, it'll first write a char and THEN flag wrap pending
    buffer_->wrapPending = false;

    // TODO: skip cells that in counting when iterating backwards over a wide cell (such as emoji)
    auto const n = min(_n, buffer_->cursor.position.column - 1);
    buffer_->setCurrentColumn(buffer_->cursor.position.column - n);
}

void Screen::moveCursorToColumn(int _column)
{
    buffer_->wrapPending = false;

    buffer_->setCurrentColumn(_column);
}

void Screen::moveCursorToBeginOfLine()
{
    buffer_->wrapPending = false;

    buffer_->setCurrentColumn(1);
}

void Screen::moveCursorToLine(int _row)
{
    moveCursorTo({_row, buffer_->cursor.position.column});
}

void Screen::moveCursorToNextTab()
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
            moveCursorForward(buffer_->tabs[i] - currentCursorColumn);
        else if (buffer_->realCursorPosition().column < buffer_->margin_.horizontal.to)
            moveCursorForward(buffer_->margin_.horizontal.to - currentCursorColumn);
        else
            moveCursorToNextLine(1);
    }
    else if (buffer_->tabWidth)
    {
        // default tab settings
        if (buffer_->realCursorPosition().column < buffer_->margin_.horizontal.to)
        {
            auto const n = min(
                buffer_->tabWidth - (buffer_->cursor.position.column - 1) % buffer_->tabWidth,
                size_.width - cursorPosition().column
            );
            moveCursorForward(n);
        }
        else
            moveCursorToNextLine(1);
    }
    else
    {
        // no tab stops configured
        if (buffer_->realCursorPosition().column < buffer_->margin_.horizontal.to)
            // then TAB moves to the end of the screen
            moveCursorToColumn(buffer_->margin_.horizontal.to);
        else
            // then TAB moves to next line left margin
            moveCursorToNextLine(1);
    }
}

void Screen::notify(string const& _title, string const& _content)
{
    cout << "Screen.NOTIFY: title: '" << _title << "', content: '" << _content << "'\n";
    eventListener_.notify(_title, _content);
}

void Screen::cursorBackwardTab(int _count)
{
    if (_count == 0)
        return;

    if (!buffer_->tabs.empty())
    {
        for (int k = 0; k < _count; ++k)
        {
            auto const i = std::find_if(rbegin(buffer_->tabs), rend(buffer_->tabs),
                                        [&](auto tabPos) -> bool {
                                            return tabPos <= cursorPosition().column - 1;
                                        });
            if (i != rend(buffer_->tabs))
            {
                // prev tab found -> move to prev tab
                moveCursorToColumn(*i);
            }
            else
            {
                moveCursorToColumn(buffer_->margin_.horizontal.from);
                break;
            }
        }
    }
    else if (buffer_->tabWidth)
    {
        // default tab settings
        if (buffer_->cursor.position.column <= buffer_->tabWidth)
            moveCursorToBeginOfLine();
        else
        {
            auto const m = buffer_->cursor.position.column % buffer_->tabWidth;
            auto const n = m
                         ? (_count - 1) * buffer_->tabWidth + m
                         : _count * buffer_->tabWidth + m;
            moveCursorBackward(n - 1);
        }
    }
    else
    {
        // no tab stops configured
        moveCursorToBeginOfLine();
    }
}

void Screen::index()
{
    if (realCursorPosition().row == buffer_->margin_.vertical.to)
        buffer_->scrollUp(1);
    else
        moveCursorTo({cursorPosition().row + 1, cursorPosition().column});
}

void Screen::reverseIndex()
{
    if (realCursorPosition().row == buffer_->margin_.vertical.from)
        buffer_->scrollDown(1);
    else
        moveCursorTo({cursorPosition().row - 1, cursorPosition().column});
}

void Screen::backIndex()
{
    if (realCursorPosition().column == buffer_->margin_.horizontal.from)
        ;// TODO: scrollRight(1);
    else
        moveCursorTo({cursorPosition().row, cursorPosition().column - 1});
}

void Screen::forwardIndex()
{
    if (realCursorPosition().column == buffer_->margin_.horizontal.to)
        ;// TODO: scrollLeft(1);
    else
        moveCursorTo({cursorPosition().row, cursorPosition().column + 1});
}

void Screen::setForegroundColor(Color const& _color)
{
    buffer_->cursor.graphicsRendition.foregroundColor = _color;
}

void Screen::setBackgroundColor(Color const& _color)
{
    buffer_->cursor.graphicsRendition.backgroundColor = _color;
}

void Screen::setUnderlineColor(Color const& _color)
{
    buffer_->cursor.graphicsRendition.underlineColor = _color;
}

void Screen::setCursorStyle(CursorDisplay _display, CursorShape _shape)
{
    eventListener_.setCursorStyle(_display, _shape);
}

void Screen::setGraphicsRendition(GraphicsRendition _rendition)
{
    // TODO: optimize this as there are only 3 cases
    // 1.) reset
    // 2.) set some bits |=
    // 3.) clear some bits &= ~
    switch (_rendition)
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

void Screen::setMark()
{
    buffer_->currentLine->marked = true;
}

void Screen::setDebugging(bool _enabled)
{
    if (_enabled == debugging())
        return;

    if (_enabled)
    {
        debugExecutor_ = make_unique<Debugger>(*this);
        commandExecutor_ = debugExecutor_.get();
    }
    else
    {
        commandExecutor_ = &directExecutor_;
        debugger()->flush();
        debugExecutor_.reset();
    }
}

void Screen::saveModes(std::vector<Mode> const& _modes)
{
    for (Mode const mode : _modes)
        if (!isAnsiMode(mode))
            savedModes_[mode].push_back(isModeEnabled(mode));
}

void Screen::restoreModes(std::vector<Mode> const& _modes)
{
    for (Mode const mode : _modes)
    {
        if (auto i = savedModes_.find(mode); i != savedModes_.end())
        {
            vector<bool>& saved = i->second;
            if (!saved.empty())
            {
                setMode(mode, saved.back());
                saved.pop_back();
            }
        }
    }
}

void Screen::setMode(Mode _mode, bool _enable)
{
    switch (_mode)
    {
        case Mode::Columns132:
        {
            if (_enable != isModeEnabled(Mode::Columns132))
            {
                // TODO: Well, should we also actually set column width to 132 or 80?
                clearScreen();
                setTopBottomMargin(1, size().height);    // DECSTBM
                setLeftRightMargin(1, size().width); // DECRLM
            }

            cursor_pos_t const columns = _enable ? 132 : 80;
            cursor_pos_t const rows = size().height;
            bool const unitInPixels = false;

            // Pre-resize in case the event callback right after is not actually resizing the window
            // (e.g. either by choice or because the window manager does not allow that, such as tiling WMs).
            resize(Size{columns, rows});

            eventListener_.resizeWindow(columns, rows, unitInPixels);

            setMode(Mode::LeftRightMargin, false);
            break;
        }
        case Mode::BatchedRendering:
            // Only perform batched rendering when NOT in debugging mode.
            if (!debugging())
            {
                if (_enable)
                    commandExecutor_ = &synchronizedExecutor_;
                else
                {
                    commandExecutor_ = &directExecutor_;
                    synchronizedExecutor_.flush();
                }
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
        case Mode::UsePrivateColorRegisters:
            commandBuilder_.setUsePrivateColorRegisters(_enable);
            break;
        default:
            break;
    }

    modes_.set(_mode, _enable);
    buffer_->setMode(_mode, _enable);
}

void Screen::requestMode(Mode _mode)
{
    enum class ModeResponse { // TODO: respect response 0, 3, 4.
        NotRecognized = 0,
        Set = 1,
        Reset = 2,
        PermanentlySet = 3,
        PermanentlyReset = 4
    };

    ModeResponse const modeResponse = isModeEnabled(_mode)
        ? ModeResponse::Set
        : ModeResponse::Reset;

    if (isAnsiMode(_mode))
        reply("\033[{};{}$y", to_code(_mode), static_cast<unsigned>(modeResponse));
    else
        reply("\033[?{};{}$y", to_code(_mode), static_cast<unsigned>(modeResponse));
}

void Screen::setTopBottomMargin(optional<int> _top, optional<int> _bottom)
{
	auto const bottom = _bottom.has_value()
		? min(_bottom.value(), size_.height)
		: size_.height;

	auto const top = _top.value_or(1);

	if (top < bottom)
    {
        buffer_->margin_.vertical.from = top;
        buffer_->margin_.vertical.to = bottom;
        buffer_->moveCursorTo({1, 1});
    }
}

void Screen::setLeftRightMargin(optional<int> _left, optional<int> _right)
{
    if (isModeEnabled(Mode::LeftRightMargin))
    {
		auto const right = _right.has_value()
			? min(_right.value(), size_.width)
			: size_.width;
		auto const left = _left.value_or(1);
		if (left + 1 < right)
        {
            buffer_->margin_.horizontal.from = left;
            buffer_->margin_.horizontal.to = right;
            buffer_->moveCursorTo({1, 1});
        }
    }
}

void Screen::screenAlignmentPattern()
{
    // sets the margins to the extremes of the page
    buffer_->margin_.vertical.from = 1;
    buffer_->margin_.vertical.to = size_.height;
    buffer_->margin_.horizontal.from = 1;
    buffer_->margin_.horizontal.to = size_.width;

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
                Cell{'E', buffer_->cursor.graphicsRendition}
            );
        }
    );
}

void Screen::sendMouseEvents(MouseProtocol _protocol, bool _enable)
{
    eventListener_.setMouseProtocol(_protocol, _enable);
}

void Screen::applicationKeypadMode(bool _enable)
{
    eventListener_.setApplicationkeypadMode(_enable);
}

void Screen::designateCharset(CharsetTable _table, CharsetId _charset)
{
    // TODO: unit test SCS and see if they also behave well with reset/softreset
    // Also, is the cursor shared between the two buffers?
    buffer_->cursor.charsets.select(_table, _charset);
}

void Screen::singleShiftSelect(CharsetTable _table)
{
    // TODO: unit test SS2, SS3
    buffer_->cursor.charsets.singleShift(_table);
}

void Screen::sixelImage(Size _size, vector<uint8_t> const& _data)
{
    auto const imageRef = imagePool_.create(_data, _size);
    auto const columnCount = int(ceil(float(imageRef->width()) / float(cellPixelSize_.width)));
    auto const rowCount = int(ceil(float(imageRef->height()) / float(cellPixelSize_.height)));
    auto const extent = Size{columnCount, rowCount};
    auto const sizelScrolling = isModeEnabled(Mode::SixelScrolling);
    auto const topLeft = sizelScrolling ? cursorPosition() : Coordinate{1, 1};
    auto const linesAvailable = 1 + size_.height - topLeft.row;
    auto const linesToBeRendered = min(extent.height, linesAvailable);
    auto const columnsToBeRendered = min(extent.width, size_.width - topLeft.column - 1);
    auto const gapColor = RGBAColor{0, 0, 0, 0}; // transarent

    auto const rasterizedImage = imagePool_.rasterize(
        imageRef,
        ImageAlignment::TopStart,
        ImageResize::NoResize,
        gapColor,
        extent,
        cellPixelSize_
    );

    if (linesToBeRendered)
    {
        crispy::for_each(
            LIBTERMINAL_EXECUTION_COMMA(par)
            Size{columnsToBeRendered, linesToBeRendered},
            [&](Coordinate const& offset) {
                at(topLeft + offset).setImage(
                    ImageFragment{rasterizedImage, offset},
                    currentBuffer().currentHyperlink
                );
            }
        );
        moveCursorTo(Coordinate{topLeft.row + linesToBeRendered - 1, topLeft.column});
    }

    // If there're lines to be rendered missing (because it didn't fit onto the screen just yet)
    // AND iff sixel sizelScrolling is enabled, then scroll as much as needed to render the remaining lines.
    if (linesToBeRendered != extent.height && sizelScrolling)
    {
        auto const remainingLineCount = extent.height - linesToBeRendered;
        for (auto const lineOffset : crispy::times(remainingLineCount))
        {
            linefeed();
            moveCursorForward(topLeft.column);
            crispy::for_each(
                LIBTERMINAL_EXECUTION_COMMA(par)
                crispy::times(columnsToBeRendered),
                [&](int columnOffset) {
                    at(Coordinate{size_.height, columnOffset + 1}).setImage(
                        ImageFragment{rasterizedImage, Coordinate{linesToBeRendered + lineOffset, columnOffset}},
                        currentBuffer().currentHyperlink
                    );
                }
            );
        }
    }

    // move ansi text cursor to position of the sixel cursor
    if (sixelCursorConformance_)
        moveCursorToColumn(topLeft.column + extent.width);
    else
    {
        moveCursorTo(Coordinate{topLeft.row + extent.height - 1, 1});
        linefeed();
    }
}

void Screen::setWindowTitle(std::string const& _title)
{
    windowTitle_ = _title;
    eventListener_.setWindowTitle(_title);
}

void Screen::saveWindowTitle()
{
    savedWindowTitles_.push(windowTitle_);
}

void Screen::restoreWindowTitle()
{
    if (!savedWindowTitles_.empty())
    {
        windowTitle_ = savedWindowTitles_.top();
        savedWindowTitles_.pop();
        eventListener_.setWindowTitle(windowTitle_);
    }
}

void Screen::requestDynamicColor(DynamicColorName _name)
{
    if (auto const color = eventListener_.requestDynamicColor(_name); color.has_value())
    {
        reply(
            "\033]{};{}\x07",
            setDynamicColorCommand(_name),
            setDynamicColorValue(color.value())
        );
    }
}

void Screen::requestPixelSize(RequestPixelSize::Area _area)
{
    switch (_area)
    {
        case RequestPixelSize::Area::WindowArea:
            [[fallthrough]]; // TODO
        case RequestPixelSize::Area::TextArea:
            // Result is CSI  4 ;  height ;  width t
            reply(
                "\033[4;{};{}t",
                cellPixelSize_.height * size_.height,
                cellPixelSize_.width * size_.width
            );
            break;
    }
}

void Screen::requestStatusString(RequestStatusString::Value _value)
{
    // xterm responds with DCS 1 $ r Pt ST for valid requests
    // or DCS 0 $ r Pt ST for invalid requests.
    auto const [status, response] = [&](RequestStatusString::Value _value) -> pair<bool, std::string> {
        switch (_value)
        {
            case RequestStatusString::Value::DECSCL:
            {
                auto level = 61;
                switch (terminalId_) {
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

                return {true, fmt::format("{};{}", level, c1t)};
            }
            default:
                return {false, ""};
        }
    }(_value);

    reply(
        "\033P{}$r{}\033\\",
        status ? 1 : 0,
        response,
        "\"p"
    );
}

void Screen::requestTabStops()
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
        for (int column = 2 * buffer_->tabWidth + 1; column <= size().width; column += buffer_->tabWidth)
            dcs << '/' << column;
    }
    dcs << '\x5c'; // ST

    reply(dcs.str());
}

void Screen::resetDynamicColor(DynamicColorName _name)
{
    eventListener_.resetDynamicColor(_name);
}

void Screen::setDynamicColor(DynamicColorName _name, RGBColor const& _color)
{
    eventListener_.setDynamicColor(_name, _color);
}

void Screen::dumpState()
{
    eventListener_.dumpState();
}

void Screen::smGraphics(XtSmGraphics::Item _item, XtSmGraphics::Action _action, XtSmGraphics::Value _value)
{
    using Item = XtSmGraphics::Item;
    using Action = XtSmGraphics::Action;

    switch (_item)
    {
        case Item::NumberOfColorRegisters:
            switch (_action)
            {
                case Action::Read:
                {
                    auto const value = imageColorPalette_->size();
                    reply("\033[?{};{};{}S", 1, 0, value);
                    break;
                }
                case Action::ReadLimit:
                {
                    auto const value = imageColorPalette_->maxSize();
                    reply("\033[?{};{};{}S", 1, 0, value);
                    break;
                }
                case Action::ResetToDefault:
                {
                    auto const value = 256; // TODO: read the configuration's default here
                    imageColorPalette_->setSize(value);
                    reply("\033[?{};{};{}S", 1, 0, value);
                    break;
                }
                case Action::SetToValue:
                {
                    visit(overloaded{
                        [&](int _number) {
                            imageColorPalette_->setSize(_number);
                            reply("\033[?{};{};{}S", 1, 0, _number);
                        },
                        [&](Size) {
                            reply("\033[?{};{};{}S", 1, 3, 0);
                        },
                        [&](monostate) {
                            reply("\033[?{};{};{}S", 1, 3, 0);
                        },
                    }, _value);
                    break;
                }
            }
            break;

        case Item::SixelGraphicsGeometry: // XXX Do we want/need to implement you?
        case Item::ReGISGraphicsGeometry: // Surely, we don't do ReGIS just yet. :-)
            break;
    }
}
// }}}

// {{{ DirectExecutor
void DirectExecutor::visit(AppendChar const& v) { screen_.writeText(v.ch); }
void DirectExecutor::visit(ApplicationKeypadMode const& v) { screen_.applicationKeypadMode(v.enable); }
void DirectExecutor::visit(BackIndex const&) { screen_.backIndex(); }
void DirectExecutor::visit(Backspace const&) { screen_.backspace(); }
void DirectExecutor::visit(Bell const&) { screen_.eventListener().bell(); }
void DirectExecutor::visit(ChangeIconTitle const& v) { (void) v; /*ignored*/ }
void DirectExecutor::visit(ChangeWindowTitle const& v) { screen_.setWindowTitle(v.title); }
void DirectExecutor::visit(ClearLine const&) { screen_.clearLine(); }
void DirectExecutor::visit(ClearScreen const&) { screen_.clearScreen(); }
void DirectExecutor::visit(ClearScrollbackBuffer const&) { screen_.clearScrollbackBuffer(); }
void DirectExecutor::visit(ClearToBeginOfLine const&) { screen_.clearToBeginOfLine(); }
void DirectExecutor::visit(ClearToBeginOfScreen const&) { screen_.clearToBeginOfScreen(); }
void DirectExecutor::visit(ClearToEndOfLine const&) { screen_.clearToEndOfLine(); }
void DirectExecutor::visit(ClearToEndOfScreen const&) { screen_.clearToEndOfScreen(); }
void DirectExecutor::visit(CopyToClipboard const& v) { screen_.eventListener().copyToClipboard(v.data); }
void DirectExecutor::visit(CursorBackwardTab const& v) { screen_.cursorBackwardTab(v.count); }
void DirectExecutor::visit(CursorNextLine const& v) { screen_.moveCursorToNextLine(v.n); }
void DirectExecutor::visit(CursorPreviousLine const& v) { screen_.moveCursorToPrevLine(v.n); }
void DirectExecutor::visit(DeleteCharacters const& v) { screen_.deleteCharacters(v.n); }
void DirectExecutor::visit(DeleteColumns const& v) { screen_.deleteColumns(v.n); }
void DirectExecutor::visit(DeleteLines const& v) { screen_.deleteLines(v.n); }
void DirectExecutor::visit(DesignateCharset const& v) { screen_.designateCharset(v.table, v.charset); }
void DirectExecutor::visit(DeviceStatusReport const&) { screen_.deviceStatusReport(); }
void DirectExecutor::visit(DumpState const&) { screen_.dumpState(); }
void DirectExecutor::visit(EraseCharacters const& v) { screen_.eraseCharacters(v.n); }
void DirectExecutor::visit(ForwardIndex const&) { screen_.forwardIndex(); }
void DirectExecutor::visit(FullReset const&) { screen_.resetHard(); }
void DirectExecutor::visit(HorizontalPositionAbsolute const& v) { screen_.moveCursorToColumn(v.n); }
void DirectExecutor::visit(HorizontalPositionRelative const& v) { screen_.moveCursorForward(v.n); /* HPR: We only care about column-mode (not pixel/inches) for now. */ }
void DirectExecutor::visit(HorizontalTabClear const& v) { screen_.horizontalTabClear(v.which); }
void DirectExecutor::visit(HorizontalTabSet const&) { screen_.horizontalTabSet(); }
void DirectExecutor::visit(Hyperlink const& v) { screen_.hyperlink(v.id, v.uri); }
void DirectExecutor::visit(Index const&) { screen_.index(); }
void DirectExecutor::visit(InsertCharacters const& v) { screen_.insertCharacters(v.n); }
void DirectExecutor::visit(InsertColumns const& v) { screen_.insertColumns(v.n); }
void DirectExecutor::visit(InsertLines const& v) { screen_.insertLines(v.n); }
void DirectExecutor::visit(Linefeed const&) { screen_.linefeed(); }
void DirectExecutor::visit(MoveCursorBackward const& v) { screen_.moveCursorBackward(v.n); }
void DirectExecutor::visit(MoveCursorDown const& v) { screen_.moveCursorDown(v.n); }
void DirectExecutor::visit(MoveCursorForward const& v) { screen_.moveCursorForward(v.n); }
void DirectExecutor::visit(MoveCursorTo const& v) { screen_.moveCursorTo(Coordinate{v.row, v.column}); }
void DirectExecutor::visit(MoveCursorToBeginOfLine const&) { screen_.moveCursorToBeginOfLine(); }
void DirectExecutor::visit(MoveCursorToColumn const& v) { screen_.moveCursorToColumn(v.column); }
void DirectExecutor::visit(MoveCursorToLine const& v) { screen_.moveCursorToLine(v.row); }
void DirectExecutor::visit(MoveCursorToNextTab const&) { screen_.moveCursorToNextTab(); }
void DirectExecutor::visit(MoveCursorUp const& v) { screen_.moveCursorUp(v.n); }
void DirectExecutor::visit(Notify const& v) { screen_.notify(v.title, v.content); }
void DirectExecutor::visit(ReportCursorPosition const&) { screen_.reportCursorPosition(); }
void DirectExecutor::visit(ReportExtendedCursorPosition const&) { screen_.reportExtendedCursorPosition(); }
void DirectExecutor::visit(RequestDynamicColor const& v) { screen_.requestDynamicColor(v.name); }
void DirectExecutor::visit(RequestMode const& v) { screen_.requestMode(v.mode); }
void DirectExecutor::visit(RequestPixelSize const& v) { screen_.requestPixelSize(v.area); }
void DirectExecutor::visit(RequestStatusString const& v) { screen_.requestStatusString(v.value); }
void DirectExecutor::visit(RequestTabStops const&) { screen_.requestTabStops(); }
void DirectExecutor::visit(ResetDynamicColor const& v) { screen_.resetDynamicColor(v.name); }
void DirectExecutor::visit(ResizeWindow const& v) { screen_.eventListener().resizeWindow(v.width, v.height, v.unit == ResizeWindow::Unit::Pixels); }
void DirectExecutor::visit(RestoreCursor const&) { screen_.restoreCursor(); }
void DirectExecutor::visit(RestoreWindowTitle const&) { screen_.restoreWindowTitle(); }
void DirectExecutor::visit(ReverseIndex const&) { screen_.reverseIndex(); }
void DirectExecutor::visit(SaveCursor const&) { screen_.saveCursor(); }
void DirectExecutor::visit(SaveWindowTitle const&) { screen_.saveWindowTitle(); }
void DirectExecutor::visit(ScreenAlignmentPattern const&) { screen_.screenAlignmentPattern(); }
void DirectExecutor::visit(ScrollDown const& v) { screen_.currentBuffer().scrollDown(v.n); }
void DirectExecutor::visit(ScrollUp const& v) { screen_.currentBuffer().scrollUp(v.n); }
void DirectExecutor::visit(SelectConformanceLevel const& v) { screen_.selectConformanceLevel(v.level /*TODO:, v.c1t*/); }
void DirectExecutor::visit(SendDeviceAttributes const&) { screen_.sendDeviceAttributes(); }
void DirectExecutor::visit(SendMouseEvents const& v) { screen_.sendMouseEvents(v.protocol, v.enable); }
void DirectExecutor::visit(SendTerminalId const&) { screen_.sendTerminalId(); }
void DirectExecutor::visit(SetBackgroundColor const& v) { screen_.setBackgroundColor(v.color); }
void DirectExecutor::visit(SetCursorStyle const& v) { screen_.setCursorStyle(v.display, v.shape); }
void DirectExecutor::visit(SetDynamicColor const& v) { screen_.setDynamicColor(v.name, v.color); }
void DirectExecutor::visit(SetForegroundColor const& v) { screen_.setForegroundColor(v.color); }
void DirectExecutor::visit(SetGraphicsRendition const& v) { screen_.setGraphicsRendition(v.rendition); }
void DirectExecutor::visit(SetLeftRightMargin const& v) { screen_.setLeftRightMargin(v.left, v.right); }
void DirectExecutor::visit(SetMark const&) { screen_.setMark(); }
void DirectExecutor::visit(SetMode const& v) { screen_.setMode(v.mode, v.enable); }
void DirectExecutor::visit(SetTopBottomMargin const& v) { screen_.setTopBottomMargin(v.top, v.bottom); }
void DirectExecutor::visit(SetUnderlineColor const& v) { screen_.setUnderlineColor(v.color); }
void DirectExecutor::visit(SingleShiftSelect const& v) { screen_.singleShiftSelect(v.table); }
void DirectExecutor::visit(SixelImage const& v) { screen_.sixelImage(v.size, v.rgba); }
void DirectExecutor::visit(SoftTerminalReset const&) { screen_.resetSoft(); }
void DirectExecutor::visit(InvalidCommand const& v) { if (logger_) logger_(InvalidOutputEvent{v.sequence.text(), "Unknown command"}); }
void DirectExecutor::visit(SaveMode const& v) { screen_.saveModes(v.modes); }
void DirectExecutor::visit(RestoreMode const& v) { screen_.restoreModes(v.modes); }
void DirectExecutor::visit(XtSmGraphics const& v) { screen_.smGraphics(v.item, v.action, v.value); }
// }}}

// {{{ SynchronizedExecutor
void SynchronizedExecutor::flush()
{
    for (Command const& cmd : queuedCommands_)
        screen_.write(cmd);

    queuedCommands_.clear();
}
/// }}}

} // namespace terminal
