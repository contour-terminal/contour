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

#include <terminal/InputGenerator.h>

#include <terminal/Logger.h>
#include <terminal/Size.h>
#include <terminal/VTType.h>

#include <crispy/algorithm.h>
#include <crispy/escape.h>
#include <crispy/times.h>
#include <crispy/utils.h>

#include <unicode/emoji_segmenter.h>
#include <unicode/word_segmenter.h>
#include <unicode/grapheme_segmenter.h>
#include <unicode/utf8.h>

#include <algorithm>
#include <iostream>
#include <iterator>
#include <sstream>
#include <variant>

#include <assert.h>

#if defined(LIBTERMINAL_EXECUTION_PAR)
#include <execution>
#define LIBTERMINAL_EXECUTION_COMMA(par) (std::execution:: par),
#else
#define LIBTERMINAL_EXECUTION_COMMA(par) /*!*/
#endif

using namespace crispy;

using std::accumulate;
using std::cerr;
using std::endl;
using std::get;
using std::holds_alternative;
using std::make_shared;
using std::max;
using std::min;
using std::monostate;
using std::next;
using std::nullopt;
using std::optional;
using std::ostringstream;
using std::pair;
using std::ref;
using std::string;
using std::string_view;
using std::vector;

namespace terminal {

namespace // {{{ helper
{
    class VTWriter {
      public:
        using Writer = std::function<void(char const*, size_t)>;

        explicit VTWriter(Writer writer) : writer_{std::move(writer)} {}
        explicit VTWriter(std::ostream& output) : VTWriter{[&](auto d, auto n) { output.write(d, n); }} {}
        explicit VTWriter(std::vector<char>& output) : VTWriter{[&](auto d, auto n) { output.insert(output.end(), d, d + n); }} {}

        void setCursorKeysMode(KeyMode _mode) noexcept { cursorKeysMode_ = _mode; }
        bool normalCursorKeys() const noexcept { return cursorKeysMode_ == KeyMode::Normal; }
        bool applicationCursorKeys() const noexcept { return !normalCursorKeys(); }

        void write(char32_t v)
        {
            write(unicode::to_utf8(v));
        }

        void write(std::string_view const& _s)
        {
            flush();
            writer_(_s.data(), _s.size());
        }

        template <typename... Args>
        void write(std::string_view const& _s, Args&&... _args)
        {
            write(fmt::format(_s, std::forward<Args>(_args)...));
        }

        void flush()
        {
            if (!sgr_.empty())
            {
                auto const f = flush(sgr_);
                sgr_.clear();
                writer_(f.data(), f.size());
            }
        }

        string flush(vector<unsigned> const& _sgr)
        {
            if (_sgr.empty())
                return "";

            auto const params =
                _sgr.size() != 1 || _sgr[0] != 0
                    ? accumulate(begin(_sgr), end(_sgr), string{},
                                 [](auto a, auto b) {
                                     return a.empty() ? fmt::format("{}", b) : fmt::format("{};{}", a, b);
                                 })
                    : string();

            return fmt::format("\033[{}m", params);
        }

        void sgr_add(unsigned n)
        {
            if (n == 0)
            {
                sgr_.clear();
                sgr_.push_back(n);
                currentForegroundColor_ = DefaultColor{};
                currentBackgroundColor_ = DefaultColor{};
                currentUnderlineColor_ = DefaultColor{};
            }
            else
            {
                if (sgr_.empty() || sgr_.back() != n)
                    sgr_.push_back(n);

                if (sgr_.size() == 16)
                {
                    write(flush(sgr_));
                    sgr_.clear();
                }
            }
        }

        void sgr_add(GraphicsRendition m)
        {
            sgr_add(static_cast<unsigned>(m));
        }

        void setForegroundColor(Color const& _color)
        {
            if (true) // _color != currentForegroundColor_)
            {
                currentForegroundColor_ = _color;
                if (holds_alternative<IndexedColor>(_color))
                {
                    auto const colorValue = get<IndexedColor>(_color);
                    if (static_cast<unsigned>(colorValue) < 8)
                        sgr_add(30 + static_cast<unsigned>(colorValue));
                    else
                    {
                        sgr_add(38);
                        sgr_add(5);
                        sgr_add(static_cast<unsigned>(colorValue));
                    }
                }
                else if (holds_alternative<DefaultColor>(_color))
                    sgr_add(39);
                else if (holds_alternative<BrightColor>(_color))
                    sgr_add(90 + static_cast<unsigned>(get<BrightColor>(_color)));
                else if (holds_alternative<RGBColor>(_color))
                {
                    auto const& rgb = get<RGBColor>(_color);
                    sgr_add(38);
                    sgr_add(2);
                    sgr_add(static_cast<unsigned>(rgb.red));
                    sgr_add(static_cast<unsigned>(rgb.green));
                    sgr_add(static_cast<unsigned>(rgb.blue));
                }
            }
        }

        void setBackgroundColor(Color const& _color)
        {
            if (true)//_color != currentBackgroundColor_)
            {
                currentBackgroundColor_ = _color;
                if (holds_alternative<IndexedColor>(_color))
                {
                    auto const colorValue = get<IndexedColor>(_color);
                    if (static_cast<unsigned>(colorValue) < 8)
                        sgr_add(40 + static_cast<unsigned>(colorValue));
                    else
                    {
                        sgr_add(48);
                        sgr_add(5);
                        sgr_add(static_cast<unsigned>(colorValue));
                    }
                }
                else if (holds_alternative<DefaultColor>(_color))
                    sgr_add(49);
                else if (holds_alternative<BrightColor>(_color))
                    sgr_add(100 + static_cast<unsigned>(get<BrightColor>(_color)));
                else if (holds_alternative<RGBColor>(_color))
                {
                    auto const& rgb = get<RGBColor>(_color);
                    sgr_add(48);
                    sgr_add(2);
                    sgr_add(static_cast<unsigned>(rgb.red));
                    sgr_add(static_cast<unsigned>(rgb.green));
                    sgr_add(static_cast<unsigned>(rgb.blue));
                }
            }
        }

      private:
        Writer writer_;
        std::vector<unsigned> sgr_;
        std::stringstream sstr;
        Color currentForegroundColor_ = DefaultColor{};
        Color currentUnderlineColor_ = DefaultColor{};
        Color currentBackgroundColor_ = DefaultColor{};
        KeyMode cursorKeysMode_ = KeyMode::Normal;
    };
}
// }}}

std::string Cell::toUtf8() const
{
    return unicode::to_utf8(codepoints_.data(), codepointCount_);
}

std::array<Lines, 2> emptyBuffers(Size _size)
{
    return std::array<Lines, 2>{
        Lines(
            static_cast<size_t>(_size.height),
            Line(static_cast<size_t>(_size.width), Cell{})
        ),
        Lines(
            static_cast<size_t>(_size.height),
            Line(static_cast<size_t>(_size.width), Cell{})
        )
    };
}

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
    sequencer_{
        *this,
        _logger,
        _maxImageSize,
        RGBAColor{}, // TODO
        imageColorPalette_
    },
    parser_{ ref(sequencer_) },
    size_{ _size },
    maxHistoryLineCount_{ _maxHistoryLineCount },
    sixelCursorConformance_{ _sixelCursorConformance }
{
    resetHard();
}

void Screen::setMaxHistoryLineCount(optional<size_t> _maxHistoryLineCount)
{
    maxHistoryLineCount_ = _maxHistoryLineCount;

    maxHistoryLineCount_ = _maxHistoryLineCount;
    clampSavedLines();

    // Alternate buffer does not have a history usually (and for now we keep it that way).
}

void Screen::clampSavedLines()
{
    clampSavedLines(savedLines_);
}

void Screen::clampSavedLines(Lines& _savedLines) const
{
    if (maxHistoryLineCount_.has_value())
        while (_savedLines.size() > maxHistoryLineCount_.value())
            _savedLines.pop_front();
}

void Screen::resizeColumns(int _newColumnCount, bool _clear)
{
    // DECCOLM / DECSCPP
    if (_clear)
    {
        // Sets the left, right, top and bottom scrolling margins to their default positions.
        setTopBottomMargin(1, size().height);   // DECSTBM
        setLeftRightMargin(1, size().width);    // DECRLM

        // Erases all data in page memory
        clearScreen();
    }

    // resets vertical split screen mode (DECLRMM) to unavailable
    setMode(Mode::LeftRightMargin, false); // DECSLRM

    // Pre-resize in case the event callback right after is not actually resizing the window
    // (e.g. either by choice or because the window manager does not allow that, such as tiling WMs).
    auto const newSize = Size{_newColumnCount, size().height};
    resize(newSize);

    bool constexpr unitInPixels = false;
    eventListener_.resizeWindow(newSize.width, newSize.height, unitInPixels);
}

void Screen::resize(Size const& _newSize)
{
    // TODO: only resize current screen buffer, and then make sure we resize the other upon actual switch

    auto dummyLines = Lines{};
    resizeBuffer(_newSize, alternateBuffer(), dummyLines);

    cursor_.position = resizeBuffer(_newSize, primaryBuffer(), savedLines_);

    // update wrap-pending
    if (_newSize.width > size_.width)
        wrapPending_ = 0;
    else if (cursor_.position.column == size_.width && _newSize.width < size_.width)
        // Shrink existing columns to _newSize.width.
        // Nothing should be done, I think, as we preserve prior (now exceeding) content.
        if (!wrapPending_)
            wrapPending_ = 1;

    // Reset margin to their default.
    margin_ = Margin{
        Margin::Range{1, _newSize.height},
        Margin::Range{1, _newSize.width}
    };

    size_ = _newSize;

    cursor_.position = clampCoordinate(cursor_.position);
    updateCursorIterators();
    clampSavedLines();

    // update last-cursor position & iterators
    lastCursorPosition_ = clampCoordinate(lastCursorPosition_);
    lastColumn_ = columnIteratorAt(
        begin(*next(begin(lines()), lastCursorPosition_.row - 1)), // last line
        lastCursorPosition_.column
    );

    // truncating tabs
    while (!tabs_.empty() && tabs_.back() > _newSize.width)
        tabs_.pop_back();

    // TODO: find out what to do with DECOM mode. Reset it to?
}

Coordinate Screen::resizeBuffer(Size const& _newSize,
                                Lines& _lines,
                                Lines& _savedLines) const
{
    auto newPosition = cursor_.position;

    if (_newSize.height > size_.height)
    {
        // Grow line count by splicing available lines from history back into buffer, if available,
        // or create new ones until size_.height == _newSize.height.
        auto const extendCount = _newSize.height - size_.height;

        auto const rowsToTakeFromSavedLines = min(extendCount, static_cast<int>(std::size(_savedLines)));

        for_each(
            crispy::times(rowsToTakeFromSavedLines),
            [&](auto) {
                _savedLines.back().resize(_newSize.width);
                _lines.emplace_front(std::move(_savedLines.back()));
                _savedLines.pop_back();
            }
        );

        newPosition.row += rowsToTakeFromSavedLines;

        auto const fillLineCount = extendCount - rowsToTakeFromSavedLines;
        generate_n(
            back_inserter(_lines),
            fillLineCount,
            [=]() { return Line{static_cast<size_t>(_newSize.width), Cell{}}; });
    }
    else if (_newSize.height < size_.height)
    {
        // Shrink existing line count to _newSize.height
        // by splicing the number of lines to be shrinked by into savedLines bottom.
        if (cursor_.position.row == size_.height)
        {
            auto const n = size_.height - _newSize.height;
            crispy::for_each(
                crispy::times(n),
                [&](auto) {
                    _lines.front().resize(_newSize.width);
                    _savedLines.emplace_back(std::move(_lines.front()));
                    _lines.pop_front();
                }
            );
            clampSavedLines(_savedLines);
        }
        else
            // Hard-cut below cursor by the number of lines to shrink.
            _lines.resize(_newSize.height);

        assert(_lines.size() == static_cast<size_t>(_newSize.height));
    }

    if (_newSize.width > size_.width)
    {
        // Grow existing columns to _newSize.width.
        std::for_each(
            begin(_lines),
            end(_lines),
            [=](auto& line) { line.resize(_newSize.width); }
        );
        newPosition.column += wrapPending_;
    }

    return newPosition;
}

void Screen::verifyState() const
{
#if !defined(NDEBUG)
    auto const lrmm = isModeEnabled(Mode::LeftRightMargin);
    if (wrapPending_ &&
            ((lrmm && (cursor_.position.column + wrapPending_ - 1) != margin_.horizontal.to)
            || (!lrmm && (cursor_.position.column + wrapPending_ - 1) != size_.width)))
    {
        fail(fmt::format(
            "Wrap is pending but cursor's column ({}) is not at right side of margin ({}) or screen ({}).",
            cursor_.position.column, margin_.horizontal.to, size_.width
        ));
    }

    if (static_cast<size_t>(size_.height) != lines().size())
        fail(fmt::format("Line count mismatch. Actual line count {} but should be {}.", lines().size(), size_.height));

    // verify cursor positions
    [[maybe_unused]] auto const clampedCursorPos = clampToScreen(cursor_.position);
    if (cursor_.position != clampedCursorPos)
        fail(fmt::format("Cursor {} does not match clamp to screen {}.", cursor_, clampedCursorPos));
    // FIXME: the above triggers on tmux vertical screen split (cursor.column off-by-one)

    // verify iterators
    [[maybe_unused]] auto const line = next(begin(lines()), cursor_.position.row - 1);
    [[maybe_unused]] auto const col = columnIteratorAt(cursor_.position.column);

    if (line != currentLine_)
        fail(fmt::format("Calculated current line does not match."));
    else if (col != currentColumn_)
        fail(fmt::format("Calculated current column does not match."));

    if (wrapPending_ && (cursor_.position.column + wrapPending_ - 1) != size_.width && cursor_.position.column != margin_.horizontal.to)
        fail(fmt::format("wrapPending flag set when cursor is not in last column."));
#endif
}

void Screen::fail(std::string const& _message) const
{
    dumpState(_message);
    assert(false);
}

void Screen::write(char const * _data, size_t _size)
{
#if defined(LIBTERMINAL_LOG_RAW)
    if (logRaw_ && logger_)
        logger_(RawOutputEvent{ escape(_data, _data + _size) });
#endif

    parser_.parseFragment(_data, _size);

    eventListener_.screenUpdated();
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
    bool const consecutiveTextWrite = sequencer_.instructionCounter() == 1;

    if (wrapPending_ && cursor_.autoWrap)
        linefeed(margin_.horizontal.from);

    auto const ch =
        _char < 127 ? cursor_.charsets.map(static_cast<char>(_char))
                    : _char == 0x7F ? ' ' : _char;

    bool const insertToPrev =
        consecutiveTextWrite
        && !lastColumn_->empty()
        && unicode::grapheme_segmenter::nonbreakable(lastColumn_->codepoint(lastColumn_->codepointCount() - 1), ch);

    if (!insertToPrev)
        writeCharToCurrentAndAdvance(ch);
    else
    {
        auto const extendedWidth = lastColumn_->appendCharacter(ch);

        if (extendedWidth > 0)
            clearAndAdvance(extendedWidth);
    }

    sequencer_.resetInstructionCounter();
}

void Screen::writeCharToCurrentAndAdvance(char32_t _character)
{
    Cell& cell = *currentColumn_;
    cell.setCharacter(_character);
    cell.attributes() = cursor_.graphicsRendition;
    cell.setHyperlink(currentHyperlink_);

    lastColumn_ = currentColumn_;
    lastCursorPosition_ = cursor_.position;

    bool const cursorInsideMargin = isModeEnabled(Mode::LeftRightMargin) && isCursorInsideMargins();
    auto const cellsAvailable = cursorInsideMargin ? margin_.horizontal.to - cursor_.position.column
                                                   : size_.width - cursor_.position.column;

    auto const n = min(cell.width(), cellsAvailable);

    if (n == cell.width())
    {
        assert(n > 0);
        cursor_.position.column += n;
        currentColumn_++;
        for (int i = 1; i < n; ++i)
            (currentColumn_++)->reset(cursor_.graphicsRendition, currentHyperlink_);
    }
    else if (cursor_.autoWrap)
        wrapPending_ = 1;
}

void Screen::clearAndAdvance(int _offset)
{
    if (_offset == 0)
        return;

    auto const availableColumnCount = margin_.horizontal.length() - cursor_.position.column;
    auto const n = min(_offset, availableColumnCount);

    if (n == _offset)
    {
        assert(n > 0);
        cursor_.position.column += n;
        for (auto i = 0; i < n; ++i)
            (currentColumn_++)->reset(cursor_.graphicsRendition, currentHyperlink_);
    }
    else if (cursor_.autoWrap)
    {
        wrapPending_ = 1;
    }
}

string Screen::renderHistoryTextLine(int _lineNumberIntoHistory) const
{
    assert(1 <= _lineNumberIntoHistory && _lineNumberIntoHistory <= historyLineCount());
    string line;
    line.reserve(size_.width);
    auto const lineIter = next(savedLines_.rbegin(), _lineNumberIntoHistory - 1);
    for (Cell const& cell : *lineIter)
        if (cell.codepointCount())
            line += cell.toUtf8();
        else
            line += " "; // fill character

    return line;
}

std::string Screen::screenshot() const
{
    auto result = std::stringstream{};
    auto writer = VTWriter(result);

    for (int const row : crispy::times(1, size_.height))
    {
        for (int const col : crispy::times(1, size_.width))
        {
            Cell const& cell = at({row, col});

            if (cell.attributes().styles & CharacterStyleMask::Bold)
                writer.sgr_add(GraphicsRendition::Bold);
            else
                writer.sgr_add(GraphicsRendition::Normal);

            // TODO: other styles (such as underline, ...)?

            writer.setForegroundColor(cell.attributes().foregroundColor);
            writer.setBackgroundColor(cell.attributes().backgroundColor);

            if (!cell.codepointCount())
                writer.write(U' ');
            else
                for (char32_t const ch : cell.codepoints())
                    writer.write(ch);
        }
        writer.sgr_add(GraphicsRendition::Reset);
        writer.write('\r');
        writer.write('\n');
    }

    return result.str();
}

optional<int> Screen::findMarkerBackward(int _currentCursorLine) const
{
    if (_currentCursorLine < 0 || !isPrimaryScreen())
        return nullopt;

    _currentCursorLine = min(_currentCursorLine, historyLineCount() + size_.height);

    // main lines
    for (int i = _currentCursorLine - historyLineCount() - 1; i >= 0; --i)
        if (lines().at(i).marked)
            return {historyLineCount() + i};

    // saved lines
    for (int i = min(_currentCursorLine, historyLineCount()) - 1; i >= 0; --i)
        if (savedLines_.at(i).marked)
            return {i};

    return nullopt;
}

optional<int> Screen::findMarkerForward(int _currentCursorLine) const
{
    if (_currentCursorLine < 0 || !isPrimaryScreen())
        return nullopt;

    for (int i = _currentCursorLine + 1; i < historyLineCount(); ++i)
        if (savedLines_.at(i).marked)
            return {i};

    for (int i = _currentCursorLine < historyLineCount()
            ? 0 : _currentCursorLine - historyLineCount() + 1; i < size_.height; ++i)
        if (lines().at(i).marked)
            return {historyLineCount() + i};

    return nullopt;
}

// {{{ tabs related
void Screen::clearAllTabs()
{
    tabs_.clear();
    tabWidth_ = 0;
}

void Screen::clearTabUnderCursor()
{
    // populate tabs vector in case of default tabWidth is used (until now).
    if (tabs_.empty() && tabWidth_ != 0)
        for (int column = tabWidth_; column <= size().width; column += tabWidth_)
            tabs_.emplace_back(column);

    // erase the specific tab underneath
    if (auto i = find(begin(tabs_), end(tabs_), realCursorPosition().column); i != end(tabs_))
        tabs_.erase(i);
}

void Screen::setTabUnderCursor()
{
    tabs_.emplace_back(realCursorPosition().column);
    sort(begin(tabs_), end(tabs_));
}
// }}}

// {{{ others
void Screen::saveCursor()
{
    // https://vt100.net/docs/vt510-rm/DECSC.html
    savedCursor_ = cursor_;
}

void Screen::restoreCursor()
{
    // https://vt100.net/docs/vt510-rm/DECRC.html
    wrapPending_ = 0;
    cursor_ = savedCursor_;
    updateCursorIterators();

    setMode(Mode::AutoWrap, savedCursor_.autoWrap);
    setMode(Mode::Origin, savedCursor_.originMode);
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

    currentHyperlink_ = {};

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
    setBuffer(ScreenType::Main);

    modes_ = Modes{};
    setMode(Mode::AutoWrap, true);

    lines_ = emptyBuffers(size_);
    activeBuffer_ = &primaryBuffer();
    moveCursorTo(Coordinate{1, 1});

    lastColumn_ = currentColumn_;
    lastCursorPosition_ = cursor_.position;

    margin_ = Margin{
        Margin::Range{1, size_.height},
        Margin::Range{1, size_.width}
    };

    currentHyperlink_ = {};
}

void Screen::moveCursorTo(Coordinate to)
{
    wrapPending_ = 0;
    cursor_.position = clampToScreen(toRealCoordinate(to));
    updateCursorIterators();
}

void Screen::setBuffer(ScreenType _type)
{
    if (bufferType() != _type)
    {
        switch (_type)
        {
            case ScreenType::Main:
                eventListener_.setMouseWheelMode(InputGenerator::MouseWheelMode::Default);
                activeBuffer_ = &primaryBuffer();
                break;
            case ScreenType::Alternate:
                if (isModeEnabled(Mode::MouseAlternateScroll))
                    eventListener_.setMouseWheelMode(InputGenerator::MouseWheelMode::ApplicationCursorKeys);
                else
                    eventListener_.setMouseWheelMode(InputGenerator::MouseWheelMode::NormalCursorKeys);
                activeBuffer_ = &alternateBuffer();
                break;
        }
        screenType_ = _type;

        eventListener_.bufferChanged(_type);
    }
}

void Screen::linefeed(int _newColumn)
{
    wrapPending_ = 0;

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
        cursor_.position.row++;
        cursor_.position.column = _newColumn;
        currentLine_++;
        updateColumnIterator();
    }
}

void Screen::scrollUp(int v_n, Margin const& margin)
{
    if (margin.horizontal != Margin::Range{1, size_.width})
    {
        // a full "inside" scroll-up
        auto const marginHeight = margin.vertical.length();
        auto const n = min(v_n, marginHeight);

        if (n < marginHeight)
        {
            auto targetLine = next(begin(lines()), margin.vertical.from - 1);     // target line
            auto sourceLine = next(begin(lines()), margin.vertical.from - 1 + n); // source line
            auto const bottomLine = next(begin(lines()), margin.vertical.to);     // bottom margin's end-line iterator

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
        auto const topLine = next(begin(lines()), margin.vertical.to - n);
        auto const bottomLine = next(begin(lines()), margin.vertical.to);     // bottom margin's end-line iterator
#if 1
        for (Line& line : crispy::range(topLine, bottomLine))
        {
            fill_n(
                next(begin(line), margin.horizontal.from - 1),
                margin.horizontal.length(),
                Cell{{}, cursor_.graphicsRendition}
            );
        }
#else
        for_each(
            topLine,
            bottomLine,
            [&](Line& line) {
                fill_n(
                    next(begin(line), margin.horizontal.from - 1),
                    margin.horizontal.length(),
                    Cell{{}, cursor_.graphicsRendition}
                );
            }
        );
#endif
    }
    else if (margin.vertical == Margin::Range{1, size_.height})
    {
        // full-screen scroll-up
        auto const n = min(v_n, size_.height);

        if (n > 0)
        {
            if (isPrimaryScreen())
            {
                crispy::for_each(
                    crispy::times(n),
                    [&](auto) {
                        savedLines_.emplace_back(std::move(lines().front()));
                        lines().pop_front();
                    }
                );
                clampSavedLines();
            }
            else
            {
                crispy::for_each(
                    crispy::times(n),
                    [&](auto) { lines().pop_front(); }
                );
            }

            generate_n(
                back_inserter(lines()),
                n,
                [this]() { return Line{static_cast<size_t>(size_.width), Cell{{}, cursor_.graphicsRendition}}; }
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
                next(begin(lines()), margin.vertical.from - 1),
                next(begin(lines()), margin.vertical.from - 1 + n),
                next(begin(lines()), margin.vertical.to)
            );
        }

        for_each(
            LIBTERMINAL_EXECUTION_COMMA(par)
            next(begin(lines()), margin.vertical.to - n),
            next(begin(lines()), margin.vertical.to),
            [&](Line& line) {
                fill(begin(line), end(line), Cell{{}, cursor_.graphicsRendition});
            }
        );
    }

    updateCursorIterators();
}

void Screen::scrollDown(int v_n, Margin const& _margin)
{
    auto const marginHeight = _margin.vertical.length();
    auto const n = min(v_n, marginHeight);

    if (_margin.horizontal != Margin::Range{1, size_.width})
    {
        // full "inside" scroll-down
        if (n < marginHeight)
        {
            auto sourceLine = next(begin(lines()), _margin.vertical.to - n - 1);
            auto targetLine = next(begin(lines()), _margin.vertical.to - 1);
            auto const sourceEndLine = next(begin(lines()), _margin.vertical.from - 1);

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
                next(begin(lines()), _margin.vertical.from - 1),
                next(begin(lines()), _margin.vertical.from - 1 + n),
                [_margin, this](Line& line) {
                    fill_n(
                        next(begin(line), _margin.horizontal.from - 1),
                        _margin.horizontal.length(),
                        Cell{{}, cursor_.graphicsRendition}
                    );
                }
            );
        }
        else
        {
            // clear everything in margin
            for_each(
                next(begin(lines()), _margin.vertical.from - 1),
                next(begin(lines()), _margin.vertical.to),
                [_margin, this](Line& line) {
                    fill_n(
                        next(begin(line), _margin.horizontal.from - 1),
                        _margin.horizontal.length(),
                        Cell{{}, cursor_.graphicsRendition}
                    );
                }
            );
        }
    }
    else if (_margin.vertical == Margin::Range{1, size_.height})
    {
        rotate(
            begin(lines()),
            next(begin(lines()), marginHeight - n),
            end(lines())
        );

        for_each(
            begin(lines()),
            next(begin(lines()), n),
            [this](Line& line) {
                fill(
                    begin(line),
                    end(line),
                    Cell{{}, cursor_.graphicsRendition}
                );
            }
        );
    }
    else
    {
        // scroll down only inside vertical margin with full horizontal extend
        rotate(
            next(begin(lines()), _margin.vertical.from - 1),
            next(begin(lines()), _margin.vertical.to - n),
            next(begin(lines()), _margin.vertical.to)
        );

        for_each(
            next(begin(lines()), _margin.vertical.from - 1),
            next(begin(lines()), _margin.vertical.from - 1 + n),
            [this](Line& line) {
                fill(
                    begin(line),
                    end(line),
                    Cell{{}, cursor_.graphicsRendition}
                );
            }
        );
    }

    updateCursorIterators();
}

void Screen::setCurrentColumn(int _n)
{
    auto const col = cursor_.originMode ? margin_.horizontal.from + _n - 1 : _n;
    auto const clampedCol = min(col, size_.width);
    cursor_.position.column = clampedCol;
    updateColumnIterator();
}

string Screen::renderTextLine(int row) const
{
    string line;
    line.reserve(size_.width);
    for (int col = 1; col <= size_.width; ++col)
        if (auto const& cell = at({row, col}); cell.codepointCount())
            line += cell.toUtf8();
        else
            line += " "; // fill character

    return line;
}

string Screen::renderText() const
{
    string text;
    text.reserve(size_.height * (size_.width + 1));

    for (int row = 1; row <= size_.height; ++row)
    {
        text += renderTextLine(row);
        text += '\n';
    }

    return text;
}
// }}}

// {{{ ops
void Screen::linefeed()
{
    if (isModeEnabled(Mode::AutomaticNewLine))
        linefeed(margin_.horizontal.from);
    else
        linefeed(realCursorPosition().column);
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
    if (isAlternateScreen() && cursor_.position.row == 1 && cursor_.position.column == 1)
        hyperlinks_.clear();

    clearToEndOfLine();

    for_each(
        LIBTERMINAL_EXECUTION_COMMA(par)
        next(currentLine_),
        end(lines()),
        [&](Line& line) {
            fill(begin(line), end(line), Cell{{}, cursor_.graphicsRendition});
        }
    );
}

void Screen::clearToBeginOfScreen()
{
    clearToBeginOfLine();

    for_each(
        LIBTERMINAL_EXECUTION_COMMA(par)
        begin(lines()),
        currentLine_,
        [&](Line& line) {
            fill(begin(line), end(line), Cell{{}, cursor_.graphicsRendition});
        }
    );
}

void Screen::clearScreen()
{
    // Instead of *just* clearing the screen, and thus, losing potential important content,
    // we scroll up by RowCount number of lines, so move it all into history, so the user can scroll
    // up in case the content is still needed.
    scrollUp(size().height);
}

void Screen::clearScrollbackBuffer()
{
    savedLines_.clear();
    eventListener_.scrollbackBufferCleared();
}

void Screen::eraseCharacters(int _n)
{
    // Spec: https://vt100.net/docs/vt510-rm/ECH.html
    // It's not clear from the spec how to perform erase when inside margin and number of chars to be erased would go outside margins.
    // TODO: See what xterm does ;-)
    size_t const n = min(size_.width - realCursorPosition().column + 1, _n == 0 ? 1 : _n);
    fill_n(currentColumn_, n, Cell{{}, cursor_.graphicsRendition});
}

void Screen::clearToEndOfLine()
{
    fill(
        currentColumn_,
        end(*currentLine_),
        Cell{{}, cursor_.graphicsRendition}
    );
}

void Screen::clearToBeginOfLine()
{
    fill(
        begin(*currentLine_),
        next(currentColumn_),
        Cell{{}, cursor_.graphicsRendition}
    );
}

void Screen::clearLine()
{
    fill(
        begin(*currentLine_),
        end(*currentLine_),
        Cell{{}, cursor_.graphicsRendition}
    );
}

void Screen::moveCursorToNextLine(int _n)
{
    moveCursorTo({cursorPosition().row + _n, 1});
}

void Screen::moveCursorToPrevLine(int _n)
{
    auto const n = min(_n, cursorPosition().row - 1);
    moveCursorTo({cursorPosition().row - n, 1});
}

void Screen::insertCharacters(int _n)
{
    if (isCursorInsideMargins())
        insertChars(realCursorPosition().row, _n);
}

/// Inserts @p _n characters at given line @p _lineNo.
void Screen::insertChars(int _lineNo, int _n)
{
    auto const n = min(_n, margin_.horizontal.to - cursorPosition().column + 1);

    auto line = next(begin(lines()), _lineNo - 1);
    auto column0 = next(begin(*line), realCursorPosition().column - 1);
    auto column1 = next(begin(*line), margin_.horizontal.to - n);
    auto column2 = next(begin(*line), margin_.horizontal.to);

    rotate(
        column0,
        column1,
        column2
    );

    if (line == currentLine_)
        updateColumnIterator();

    fill_n(
        columnIteratorAt(begin(*line), cursor_.position.column),
        n,
        Cell{L' ', cursor_.graphicsRendition}
    );
}

void Screen::insertLines(int _n)
{
    if (isCursorInsideMargins())
    {
        scrollDown(
            _n,
            Margin{
                { cursor_.position.row, margin_.vertical.to },
                margin_.horizontal
            }
        );
    }
}

void Screen::insertColumns(int _n)
{
    if (isCursorInsideMargins())
        for (int lineNo = margin_.vertical.from; lineNo <= margin_.vertical.to; ++lineNo)
            insertChars(lineNo, _n);
}

void Screen::deleteLines(int _n)
{
    if (isCursorInsideMargins())
    {
        scrollUp(
            _n,
            Margin{
                { cursor_.position.row, margin_.vertical.to },
                margin_.horizontal
            }
        );
    }
}

void Screen::deleteCharacters(int _n)
{
    if (isCursorInsideMargins() && _n != 0)
        deleteChars(realCursorPosition().row, _n);
}

void Screen::deleteChars(int _lineNo, int _n)
{
    auto line = next(begin(lines()), _lineNo - 1);
    auto column = next(begin(*line), realCursorPosition().column - 1);
    auto rightMargin = next(begin(*line), margin_.horizontal.to);
    auto const n = min(_n, static_cast<int>(distance(column, rightMargin)));

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
        Cell{L' ', cursor_.graphicsRendition}
    );
}
void Screen::deleteColumns(int _n)
{
    if (isCursorInsideMargins())
        for (int lineNo = margin_.vertical.from; lineNo <= margin_.vertical.to; ++lineNo)
            deleteChars(lineNo, _n);
}

void Screen::horizontalTabClear(HorizontalTabClear::Which _which)
{
    switch (_which)
    {
        case HorizontalTabClear::AllTabs:
            clearAllTabs();
            break;
        case HorizontalTabClear::UnderCursor:
            clearTabUnderCursor();
            break;
    }
}

void Screen::horizontalTabSet()
{
    setTabUnderCursor();
}

void Screen::hyperlink(string const& _id, string const& _uri)
{
    if (_uri.empty())
        currentHyperlink_ = nullptr;
    else if (_id.empty())
        currentHyperlink_ = make_shared<HyperlinkInfo>(HyperlinkInfo{_id, _uri});
    else if (auto i = hyperlinks_.find(_id); i != hyperlinks_.end())
        currentHyperlink_ = i->second;
    else
    {
        currentHyperlink_ = make_shared<HyperlinkInfo>(HyperlinkInfo{_id, _uri});
        hyperlinks_[_id] = currentHyperlink_;
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
        cursorPosition().row > margin_.vertical.from
            ? cursorPosition().row - margin_.vertical.from
            : cursorPosition().row - 1
    );

    cursor_.position.row -= n;
    currentLine_ = prev(currentLine_, n);
    setCurrentColumn(cursorPosition().column);
}

void Screen::moveCursorDown(int _n)
{
    auto const currentLineNumber = cursorPosition().row;
    auto const n = min(
        _n,
        currentLineNumber <= margin_.vertical.to
            ? margin_.vertical.to - currentLineNumber
            : size_.height - currentLineNumber
    );
    // auto const n =
    //     v.n > margin_.vertical.to
    //         ? min(v.n, size_.height - cursorPosition().row)
    //         : min(v.n, margin_.vertical.to - cursorPosition().row);

    cursor_.position.row += n;
    currentLine_ = next(currentLine_, n);
    setCurrentColumn(cursorPosition().column);
}

void Screen::moveCursorForward(int _n)
{
    auto const n = min(_n,  margin_.horizontal.length() - cursor_.position.column);
    cursor_.position.column += n;
    updateColumnIterator();
}

void Screen::moveCursorBackward(int _n)
{
    // even if you move to 80th of 80 columns, it'll first write a char and THEN flag wrap pending
    wrapPending_ = 0;

    // TODO: skip cells that in counting when iterating backwards over a wide cell (such as emoji)
    auto const n = min(_n, cursor_.position.column - 1);
    setCurrentColumn(cursor_.position.column - n);
}

void Screen::moveCursorToColumn(int _column)
{
    wrapPending_ = 0;
    setCurrentColumn(_column);
}

void Screen::moveCursorToBeginOfLine()
{
    wrapPending_ = 0;
    setCurrentColumn(1);
}

void Screen::moveCursorToLine(int _row)
{
    moveCursorTo({_row, cursor_.position.column});
}

void Screen::moveCursorToNextTab()
{
    // TODO: I guess something must remember when a \t was added, for proper move-back?
    // TODO: respect HTS/TBC

    if (!tabs_.empty())
    {
        // advance to the next tab
        size_t i = 0;
        while (i < tabs_.size() && realCursorPosition().column >= tabs_[i])
            ++i;

        auto const currentCursorColumn = cursorPosition().column;

        if (i < tabs_.size())
            moveCursorForward(tabs_[i] - currentCursorColumn);
        else if (realCursorPosition().column < margin_.horizontal.to)
            moveCursorForward(margin_.horizontal.to - currentCursorColumn);
        else
            moveCursorToNextLine(1);
    }
    else if (tabWidth_)
    {
        // default tab settings
        if (realCursorPosition().column < margin_.horizontal.to)
        {
            auto const n = min(
                tabWidth_ - (cursor_.position.column - 1) % tabWidth_,
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
        if (realCursorPosition().column < margin_.horizontal.to)
            // then TAB moves to the end of the screen
            moveCursorToColumn(margin_.horizontal.to);
        else
            // then TAB moves to next line left margin
            moveCursorToNextLine(1);
    }
}

void Screen::notify(string const& _title, string const& _content)
{
    std::cout << "Screen.NOTIFY: title: '" << _title << "', content: '" << _content << "'\n";
    eventListener_.notify(_title, _content);
}

void Screen::cursorForwardTab(int _count)
{
    printf("CHT: %d\n", _count);
    for (int i = 0; i < _count; ++i)
        moveCursorToNextTab();
}

void Screen::cursorBackwardTab(int _count)
{
    if (_count == 0)
        return;

    if (!tabs_.empty())
    {
        for (int k = 0; k < _count; ++k)
        {
            auto const i = std::find_if(rbegin(tabs_), rend(tabs_),
                                        [&](auto tabPos) -> bool {
                                            return tabPos <= cursorPosition().column - 1;
                                        });
            if (i != rend(tabs_))
            {
                // prev tab found -> move to prev tab
                moveCursorToColumn(*i);
            }
            else
            {
                moveCursorToColumn(margin_.horizontal.from);
                break;
            }
        }
    }
    else if (tabWidth_)
    {
        // default tab settings
        if (cursor_.position.column <= tabWidth_)
            moveCursorToBeginOfLine();
        else
        {
            auto const m = cursor_.position.column % tabWidth_;
            auto const n = m
                         ? (_count - 1) * tabWidth_ + m
                         : _count * tabWidth_ + m;
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
    if (realCursorPosition().row == margin_.vertical.to)
        scrollUp(1);
    else
        moveCursorTo({cursorPosition().row + 1, cursorPosition().column});
}

void Screen::reverseIndex()
{
    if (realCursorPosition().row == margin_.vertical.from)
        scrollDown(1);
    else
        moveCursorTo({cursorPosition().row - 1, cursorPosition().column});
}

void Screen::backIndex()
{
    if (realCursorPosition().column == margin_.horizontal.from)
        ;// TODO: scrollRight(1);
    else
        moveCursorTo({cursorPosition().row, cursorPosition().column - 1});
}

void Screen::forwardIndex()
{
    if (realCursorPosition().column == margin_.horizontal.to)
        ;// TODO: scrollLeft(1);
    else
        moveCursorTo({cursorPosition().row, cursorPosition().column + 1});
}

void Screen::setForegroundColor(Color const& _color)
{
    cursor_.graphicsRendition.foregroundColor = _color;
}

void Screen::setBackgroundColor(Color const& _color)
{
    cursor_.graphicsRendition.backgroundColor = _color;
}

void Screen::setUnderlineColor(Color const& _color)
{
    cursor_.graphicsRendition.underlineColor = _color;
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
            cursor_.graphicsRendition = {};
            break;
        case GraphicsRendition::Bold:
            cursor_.graphicsRendition.styles |= CharacterStyleMask::Bold;
            break;
        case GraphicsRendition::Faint:
            cursor_.graphicsRendition.styles |= CharacterStyleMask::Faint;
            break;
        case GraphicsRendition::Italic:
            cursor_.graphicsRendition.styles |= CharacterStyleMask::Italic;
            break;
        case GraphicsRendition::Underline:
            cursor_.graphicsRendition.styles |= CharacterStyleMask::Underline;
            break;
        case GraphicsRendition::Blinking:
            cursor_.graphicsRendition.styles |= CharacterStyleMask::Blinking;
            break;
        case GraphicsRendition::Inverse:
            cursor_.graphicsRendition.styles |= CharacterStyleMask::Inverse;
            break;
        case GraphicsRendition::Hidden:
            cursor_.graphicsRendition.styles |= CharacterStyleMask::Hidden;
            break;
        case GraphicsRendition::CrossedOut:
            cursor_.graphicsRendition.styles |= CharacterStyleMask::CrossedOut;
            break;
        case GraphicsRendition::DoublyUnderlined:
            cursor_.graphicsRendition.styles |= CharacterStyleMask::DoublyUnderlined;
            break;
        case GraphicsRendition::CurlyUnderlined:
            cursor_.graphicsRendition.styles |= CharacterStyleMask::CurlyUnderlined;
            break;
        case GraphicsRendition::DottedUnderline:
            cursor_.graphicsRendition.styles |= CharacterStyleMask::DottedUnderline;
            break;
        case GraphicsRendition::DashedUnderline:
            cursor_.graphicsRendition.styles |= CharacterStyleMask::DashedUnderline;
            break;
        case GraphicsRendition::Framed:
            cursor_.graphicsRendition.styles |= CharacterStyleMask::Framed;
            break;
        case GraphicsRendition::Overline:
            cursor_.graphicsRendition.styles |= CharacterStyleMask::Overline;
            break;
        case GraphicsRendition::Normal:
            cursor_.graphicsRendition.styles &= ~(CharacterStyleMask::Bold | CharacterStyleMask::Faint);
            break;
        case GraphicsRendition::NoItalic:
            cursor_.graphicsRendition.styles &= ~CharacterStyleMask::Italic;
            break;
        case GraphicsRendition::NoUnderline:
            cursor_.graphicsRendition.styles &= ~CharacterStyleMask::Underline;
            break;
        case GraphicsRendition::NoBlinking:
            cursor_.graphicsRendition.styles &= ~CharacterStyleMask::Blinking;
            break;
        case GraphicsRendition::NoInverse:
            cursor_.graphicsRendition.styles &= ~CharacterStyleMask::Inverse;
            break;
        case GraphicsRendition::NoHidden:
            cursor_.graphicsRendition.styles &= ~CharacterStyleMask::Hidden;
            break;
        case GraphicsRendition::NoCrossedOut:
            cursor_.graphicsRendition.styles &= ~CharacterStyleMask::CrossedOut;
            break;
        case GraphicsRendition::NoFramed:
            cursor_.graphicsRendition.styles &= ~CharacterStyleMask::Framed;
            break;
        case GraphicsRendition::NoOverline:
            cursor_.graphicsRendition.styles &= ~CharacterStyleMask::Overline;
            break;
    }
}

void Screen::setMark()
{
    currentLine_->marked = true;
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
        case Mode::AutoWrap:
            cursor_.autoWrap = _enable;
            break;
        case Mode::LeftRightMargin:
            // Resetting DECLRMM also resets the horizontal margins back to screen size.
            if (!_enable)
                margin_.horizontal = {1, size_.width};
            break;
        case Mode::Origin:
            cursor_.originMode = _enable;
            break;
        case Mode::Columns132:
            if (!_enable || isModeEnabled(Mode::AllowColumns80to132))
            {
                auto const clear = _enable != isModeEnabled(_mode);

                // sets the number of columns on the page to 80 or 132 and selects the
                // corresponding 80- or 132-column font
                auto const columns = _enable ? 132 : 80;

                resizeColumns(columns, clear);
            }
            break;
        case Mode::BatchedRendering:
            // Only perform batched rendering when NOT in debugging mode.
            // TODO: also, do I still need this here?
            break;
        case Mode::UseAlternateScreen:
            if (_enable)
                setBuffer(ScreenType::Alternate);
            else
                setBuffer(ScreenType::Main);
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
            if (_enable)
                eventListener_.setMouseTransport(MouseTransport::SGR);
            else
                eventListener_.setMouseTransport(MouseTransport::Default);
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
            sequencer_.setUsePrivateColorRegisters(_enable);
            break;
        case Mode::VisibleCursor:
            cursor_.visible = _enable;
            eventListener_.setCursorVisibility(_enable);
            break;
        case Mode::MouseProtocolX10:
            sendMouseEvents(MouseProtocol::X10, _enable);
            break;
        case Mode::MouseProtocolNormalTracking:
            sendMouseEvents(MouseProtocol::NormalTracking, _enable);
            break;
        case Mode::MouseProtocolHighlightTracking:
            sendMouseEvents(MouseProtocol::HighlightTracking, _enable);
            break;
        case Mode::MouseProtocolButtonTracking:
            sendMouseEvents(MouseProtocol::ButtonTracking, _enable);
            break;
        case Mode::MouseProtocolAnyEventTracking:
            sendMouseEvents(MouseProtocol::AnyEventTracking, _enable);
            break;
        case Mode::SaveCursor:
            if (_enable)
                saveCursor();
            else
                restoreCursor();
            break;
        case Mode::ExtendedAltScreen:
            if (_enable)
            {
                saveCursor();
                setMode(Mode::UseAlternateScreen, true);
                clearScreen();
            }
            else
            {
                setMode(Mode::UseAlternateScreen, false);
                restoreCursor();
            }
            break;
        default:
            break;
    }

    modes_.set(_mode, _enable);
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
        margin_.vertical.from = top;
        margin_.vertical.to = bottom;
        moveCursorTo({1, 1});
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
            margin_.horizontal.from = left;
            margin_.horizontal.to = right;
            moveCursorTo({1, 1});
        }
    }
}

void Screen::screenAlignmentPattern()
{
    // sets the margins to the extremes of the page
    margin_.vertical.from = 1;
    margin_.vertical.to = size_.height;
    margin_.horizontal.from = 1;
    margin_.horizontal.to = size_.width;

    // and moves the cursor to the home position
    moveCursorTo({1, 1});

    // fills the complete screen area with a test pattern
    for_each(
        LIBTERMINAL_EXECUTION_COMMA(par)
        begin(lines()),
        end(lines()),
        [&](Line& line) {
            fill(
                LIBTERMINAL_EXECUTION_COMMA(par)
                begin(line),
                end(line),
                Cell{'E', cursor_.graphicsRendition}
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
    cursor_.charsets.select(_table, _charset);
}

void Screen::singleShiftSelect(CharsetTable _table)
{
    // TODO: unit test SS2, SS3
    cursor_.charsets.singleShift(_table);
}

void Screen::sixelImage(Size _pixelSize, Image::Data&& _data)
{
    auto const columnCount = int(ceilf(float(_pixelSize.width) / float(cellPixelSize_.width)));
    auto const rowCount = int(ceilf(float(_pixelSize.height) / float(cellPixelSize_.height)));
    auto const extent = Size{columnCount, rowCount};
    auto const sixelScrolling = isModeEnabled(Mode::SixelScrolling);
    auto const topLeft = sixelScrolling ? cursorPosition() : Coordinate{1, 1};

    auto const alignmentPolicy = ImageAlignment::TopStart;
    auto const resizePolicy = ImageResize::NoResize;

    auto const imageOffset = Coordinate{0, 0};

    if (auto const imageRef = uploadImage(ImageFormat::RGBA, _pixelSize, move(_data)); imageRef)
        renderImage(imageRef, topLeft, extent,
                    imageOffset, _pixelSize,
                    alignmentPolicy, resizePolicy,
                    sixelScrolling);

    if (!sixelCursorConformance_)
        linefeed(topLeft.column);
}

std::shared_ptr<Image const> Screen::uploadImage(ImageFormat _format, Size _imageSize, Image::Data&& _pixmap)
{
    return imagePool_.create(_format, _imageSize, move(_pixmap));
}

void Screen::renderImage(std::shared_ptr<Image const> const& _imageRef,
                         Coordinate _topLeft, Size _gridSize,
                         Coordinate _imageOffset, Size _imageSize,
                         ImageAlignment _alignmentPolicy,
                         ImageResize _resizePolicy,
                         bool _autoScroll)
{
    // TODO: make use of THIS function in sixelImage, too!

    auto const linesAvailable = 1 + size_.height - _topLeft.row;
    auto const linesToBeRendered = min(_gridSize.height, linesAvailable);
    auto const columnsToBeRendered = min(_gridSize.width, size_.width - _topLeft.column - 1);
    auto const gapColor = RGBAColor{}; // TODO: cursor_.graphicsRendition.backgroundColor;

    // TODO: make use of _imageOffset and _imageSize
    auto const rasterizedImage = imagePool_.rasterize(
        _imageRef,
        _alignmentPolicy,
        _resizePolicy,
        gapColor,
        _gridSize,
        cellPixelSize_
    );

    if (linesToBeRendered)
    {
        crispy::for_each(
            LIBTERMINAL_EXECUTION_COMMA(par)
            Size{columnsToBeRendered, linesToBeRendered},
            [&](Coordinate const& offset) {
                at(_topLeft + offset).setImage(
                    ImageFragment{rasterizedImage, offset},
                    currentHyperlink_
                );
            }
        );
        moveCursorTo(Coordinate{_topLeft.row + linesToBeRendered - 1, _topLeft.column});
    }

    // If there're lines to be rendered missing (because it didn't fit onto the screen just yet)
    // AND iff sixel sixelScrolling  is enabled, then scroll as much as needed to render the remaining lines.
    if (linesToBeRendered != _gridSize.height && _autoScroll)
    {
        auto const remainingLineCount = _gridSize.height - linesToBeRendered;
        for (auto const lineOffset : crispy::times(remainingLineCount))
        {
            linefeed();
            moveCursorForward(_topLeft.column);
            crispy::for_each(
                LIBTERMINAL_EXECUTION_COMMA(par)
                crispy::times(columnsToBeRendered),
                [&](int columnOffset) {
                    at(Coordinate{size_.height, columnOffset + 1}).setImage(
                        ImageFragment{rasterizedImage, Coordinate{linesToBeRendered + lineOffset, columnOffset}},
                        currentHyperlink_
                    );
                }
            );
        }
    }

    // move ansi text cursor to position of the sixel cursor
    moveCursorToColumn(_topLeft.column + _gridSize.width);
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
    auto const [status, response] = [&](RequestStatusString::Value _value) -> pair<bool, string> {
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
    if (!tabs_.empty())
    {
        for (size_t const i : times(tabs_.size()))
        {
            if (i)
                dcs << '/';
            dcs << tabs_[i];
        }
    }
    else if (tabWidth_ != 0)
    {
        dcs << tabWidth_ + 1;
        for (int column = 2 * tabWidth_ + 1; column <= size().width; column += tabWidth_)
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

void Screen::dumpState(std::string const& _message) const
{
    auto const hline = [&]() {
        for_each(crispy::times(size_.width), [](auto) { cerr << '='; });
        cerr << endl;
    };

    hline();
    cerr << "\033[1;37;41m" << _message << "\033[m" << endl;
    hline();

    cerr << fmt::format("Rendered screen at the time of failure: {}\n", size_);
    cerr << fmt::format("cursor position      : {}\n", cursor_);
    if (cursor_.originMode)
        cerr << fmt::format("real cursor position : {})\n", toRealCoordinate(cursor_.position));
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

} // namespace terminal
