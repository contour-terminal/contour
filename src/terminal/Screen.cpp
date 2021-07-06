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
#include <terminal/VTType.h>
#include <terminal/logging.h>

#include <crispy/Comparison.h>
#include <crispy/algorithm.h>
#include <crispy/escape.h>
#include <crispy/debuglog.h>
#include <crispy/size.h>
#include <crispy/times.h>
#include <crispy/utils.h>

#include <unicode/emoji_segmenter.h>
#include <unicode/word_segmenter.h>
#include <unicode/grapheme_segmenter.h>
#include <unicode/convert.h>
#include <unicode/utf8.h>

#include <algorithm>
#include <iostream>
#include <iterator>
#include <sstream>
#include <string_view>
#include <tuple>
#include <variant>

#include <assert.h>

#if defined(LIBTERMINAL_EXECUTION_PAR)
#include <execution>
#define LIBTERMINAL_EXECUTION_COMMA(par) (std::execution:: par),
#else
#define LIBTERMINAL_EXECUTION_COMMA(par) /*!*/
#endif

using namespace crispy;
using namespace std::string_view_literals;

using std::accumulate;
using std::clamp;
using std::array;
using std::cerr;
using std::distance;
using std::endl;
using std::function;
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
    std::string vtSequenceParameterString(GraphicsAttributes const& _sgr)
    {
        std::string output;

        auto const sgrSep = [&]() { if (!output.empty()) output += ';'; };
        auto const sgrAdd = [&](unsigned _value) { sgrSep(); output += std::to_string(_value); };
        auto const sgrAddStr = [&](string_view _value) { sgrSep(); output += _value; };
        auto const sgrAddSub = [&](unsigned _value) { output += std::to_string(_value); };

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
            auto const& rgb = getRGBColor(_sgr.foregroundColor);
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
        auto constexpr masks = array{
            pair{CellFlags::Bold, "1"sv},
            pair{CellFlags::Faint, "2"sv},
            pair{CellFlags::Italic, "3"sv},
            pair{CellFlags::Underline, "4"sv},
            pair{CellFlags::Blinking, "5"sv},
            pair{CellFlags::Inverse, "7"sv},
            pair{CellFlags::Hidden, "8"sv},
            pair{CellFlags::CrossedOut, "9"sv},
            pair{CellFlags::DoublyUnderlined, "4:2"sv},
            pair{CellFlags::CurlyUnderlined, "4:3"sv},
            pair{CellFlags::DottedUnderline, "4:4"sv},
            pair{CellFlags::DashedUnderline, "4:5"sv},
            pair{CellFlags::Framed, "51"sv},
            // TODO(impl or completely remove): pair{CellFlags::Encircled, ""sv},
            pair{CellFlags::Overline, "53"sv},
        };

        for (auto const& mask: masks)
            if (_sgr.styles & mask.first)
                sgrAddStr(mask.second);

        return output;
    }

    class VTWriter {
      public:
        // TODO: compare with old sgr value set instead to be more generic in reusing stuff
        using Writer = std::function<void(char const*, size_t)>;

        explicit VTWriter(Writer writer) : writer_{std::move(writer)} {}
        explicit VTWriter(std::ostream& output) : VTWriter{[&](auto d, auto n) { output.write(d, n); }} {}
        explicit VTWriter(std::vector<char>& output) : VTWriter{[&](auto d, auto n) { output.insert(output.end(), d, d + n); }} {}

        void setCursorKeysMode(KeyMode _mode) noexcept { cursorKeysMode_ = _mode; }
        bool normalCursorKeys() const noexcept { return cursorKeysMode_ == KeyMode::Normal; }
        bool applicationCursorKeys() const noexcept { return !normalCursorKeys(); }

        void write(char32_t v)
        {
            char buf[4];
            auto enc = unicode::encoder<char>{};
            auto count = distance(buf, enc(v, buf));
            write(string_view(buf, count));
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
                currentForegroundColor_ = DefaultColor();
                currentBackgroundColor_ = DefaultColor();
                currentUnderlineColor_ = DefaultColor();
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

        void sgr_rewind()
        {
            swap(lastSGR_, sgr_);
            sgr_.clear();
        }

        void sgr_add(GraphicsRendition m)
        {
            sgr_add(static_cast<unsigned>(m));
        }

        void setForegroundColor(Color const& _color)
        {
            //if (true) // _color != currentForegroundColor_)
            if (_color != currentForegroundColor_)
            {
                currentForegroundColor_ = _color;
                if (isIndexedColor(_color))
                {
                    auto const colorValue = getIndexedColor(_color);
                    if (static_cast<unsigned>(colorValue) < 8)
                        sgr_add(30 + static_cast<unsigned>(colorValue));
                    else
                    {
                        sgr_add(38);
                        sgr_add(5);
                        sgr_add(static_cast<unsigned>(colorValue));
                    }
                }
                else if (isDefaultColor(_color))
                    sgr_add(39);
                else if (isBrightColor(_color))
                    sgr_add(90 + static_cast<unsigned>(getBrightColor(_color)));
                else if (isRGBColor(_color))
                {
                    auto const rgb = getRGBColor(_color);
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
                if (isIndexedColor(_color))
                {
                    auto const colorValue = getIndexedColor(_color);
                    if (static_cast<unsigned>(colorValue) < 8)
                        sgr_add(40 + static_cast<unsigned>(colorValue));
                    else
                    {
                        sgr_add(48);
                        sgr_add(5);
                        sgr_add(static_cast<unsigned>(colorValue));
                    }
                }
                else if (isDefaultColor(_color))
                    sgr_add(49);
                else if (isBrightColor(_color))
                    sgr_add(100 + static_cast<unsigned>(getBrightColor(_color)));
                else if (isRGBColor(_color))
                {
                    auto const& rgb = getRGBColor(_color);
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
        std::vector<unsigned> lastSGR_;
        std::vector<unsigned> sgr_;
        std::stringstream sstr;
        Color currentForegroundColor_ = DefaultColor();
        Color currentUnderlineColor_ = DefaultColor();
        Color currentBackgroundColor_ = DefaultColor();
        KeyMode cursorKeysMode_ = KeyMode::Normal;
    };

    constexpr bool GridTextReflowEnabled = true;

    array<Grid, 2> emptyGrids(PageSize _size, optional<LineCount> _maxHistoryLineCount)
    {
        return array<Grid, 2>{
            Grid(_size, GridTextReflowEnabled, _maxHistoryLineCount),
            Grid(_size, false, LineCount(0))
        };
    }
}
// }}}

Screen::Screen(PageSize _size,
               ScreenEvents& _eventListener,
               bool _logRaw,
               bool _logTrace,
               optional<LineCount> _maxHistoryLineCount,
               ImageSize _maxImageSize,
               int _maxImageColorRegisters,
               bool _sixelCursorConformance,
               ColorPalette _colorPalette
) :
    eventListener_{ _eventListener },
    logRaw_{ _logRaw },
    logTrace_{ _logTrace },
    modes_{},
    savedModes_{},
    defaultColorPalette_{ _colorPalette },
    colorPalette_{ _colorPalette },
    maxImageColorRegisters_{ _maxImageColorRegisters },
    maxImageSize_{ _maxImageSize },
    maxImageSizeLimit_{ _maxImageSize },
    imageColorPalette_(make_shared<SixelColorPalette>(maxImageColorRegisters_, maxImageColorRegisters_)),
    imagePool_{
        [this](Image const* _image) { eventListener_.discardImage(*_image); },
        1
    },
    sequencer_{
        *this,
        _maxImageSize,
        RGBAColor{}, // TODO
        imageColorPalette_
    },
    parser_{ ref(sequencer_) },
    size_{ _size },
    sixelCursorConformance_{ _sixelCursorConformance },
    grids_{ emptyGrids(size(), _maxHistoryLineCount) },
    activeGrid_{ &primaryGrid() }
{
    resetHard();
}

unsigned Screen::numericCapability(capabilities::Code _cap) const
{
    using namespace capabilities::literals;

    switch (_cap)
    {
        case "li"_tcap: return unbox<unsigned>(size_.lines);
        case "co"_tcap: return unbox<unsigned>(size_.columns);
        case "it"_tcap: return tabWidth_;
        default:
            return StaticDatabase::numericCapability(_cap);
    }
}

void Screen::setMaxHistoryLineCount(optional<LineCount> _maxHistoryLineCount)
{
    primaryGrid().setMaxHistoryLineCount(_maxHistoryLineCount);
}

void Screen::resizeColumns(ColumnCount _newColumnCount, bool _clear)
{
    // DECCOLM / DECSCPP
    if (_clear)
    {
        // Sets the left, right, top and bottom scrolling margins to their default positions.
        setTopBottomMargin(1, *size_.lines);    // DECSTBM
        setLeftRightMargin(1, *size_.columns);  // DECRLM

        // Erases all data in page memory
        clearScreen();
    }

    // resets vertical split screen mode (DECLRMM) to unavailable
    setMode(DECMode::LeftRightMargin, false); // DECSLRM

    // Pre-resize in case the event callback right after is not actually resizing the window
    // (e.g. either by choice or because the window manager does not allow that, such as tiling WMs).
    auto const newSize = PageSize{size_.lines, _newColumnCount};
    resize(newSize);

    eventListener_.resizeWindow(newSize);
}

void Screen::resize(PageSize _newSize)
{
    cursor_.position = grid().resize(_newSize, cursor_.position, wrapPending_);
    (void) backgroundGrid().resize(_newSize, cursor_.position, false);

    // update wrap-pending
    if (_newSize.columns > size_.columns)
        wrapPending_ = 0;
    else if (cursor_.position.column == unbox<int>(size_.columns) && _newSize.columns < size_.columns)
        // Shrink existing columns to _newSize.columns.
        // Nothing should be done, I think, as we preserve prior (now exceeding) content.
        if (!wrapPending_)
            wrapPending_ = 1;

    // Reset margin to their default.
    margin_ = Margin{
        Margin::Range{1, unbox<int>(_newSize.lines)},
        Margin::Range{1, unbox<int>(_newSize.columns)}
    };

    size_ = _newSize;

    cursor_.position = clampCoordinate(cursor_.position);
    updateCursorIterators();

    // update last-cursor position & iterators
    lastCursorPosition_ = clampCoordinate(lastCursorPosition_);
    lastColumn_ = columnIteratorAt(
        begin(*next(begin(grid().mainPage()), lastCursorPosition_.row - 1)), // last line
        lastCursorPosition_.column
    );

    // truncating tabs
    while (!tabs_.empty() && tabs_.back() > boxed_cast<ColumnPosition>(_newSize.columns))
        tabs_.pop_back();

    // TODO: find out what to do with DECOM mode. Reset it to?
}

void Screen::verifyState() const
{
#if !defined(NDEBUG)
    auto const lrmm = isModeEnabled(DECMode::LeftRightMargin);
    if (wrapPending_ &&
            ((lrmm && (cursor_.position.column + wrapPending_ - 1) != margin_.horizontal.to)
            || (!lrmm && (cursor_.position.column + wrapPending_ - 1) != unbox<int>(size_.columns))))
    {
        fail(fmt::format(
            "Wrap is pending but cursor's column ({}) is not at right side of margin ({}) or screen ({}).",
            cursor_.position.column, margin_.horizontal.to, size_.columns
        ));
    }

    if (unbox<size_t>(size_.lines) != grid().mainPage().size())
        fail(fmt::format("Line count mismatch. Actual line count {} but should be {}.", grid().mainPage().size(), size_.lines));

    // verify cursor positions
    [[maybe_unused]] auto const clampedCursorPos = clampToScreen(cursor_.position);
    if (cursor_.position != clampedCursorPos)
        fail(fmt::format("Cursor {} does not match clamp to screen {}.", cursor_, clampedCursorPos));
    // FIXME: the above triggers on tmux vertical screen split (cursor.column off-by-one)

    // verify iterators
    [[maybe_unused]] auto const line = next(begin(grid().mainPage()), cursor_.position.row - 1);
    [[maybe_unused]] auto const col = columnIteratorAt(cursor_.position.column);

    if (line != currentLine_)
        fail(fmt::format("Calculated current line does not match."));
    else if (col != currentColumn_)
        fail(fmt::format("Calculated current column does not match."));

    if (wrapPending_ && (cursor_.position.column + wrapPending_ - 1) != unbox<int>(size_.columns) && cursor_.position.column != margin_.horizontal.to)
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
    if (!_size)
        return;
#if defined(LIBTERMINAL_LOG_RAW)
    if (crispy::logging_sink::for_debug().enabled())
        debuglog(ScreenRawOutputTag).write("raw: \"{}\"", escape(_data, _data + _size));
#endif

    parser_.parseFragment(string_view(_data, _size));
    eventListener_.screenUpdated();
}

void Screen::write(std::u32string_view const& _text)
{
    parser_.parseFragment(_text);
    eventListener_.screenUpdated();
}

void Screen::writeText(char32_t _char)
{
#if defined(LIBTERMINAL_LOG_TRACE)
    debuglog(VTParserTraceTag).write("text: {}", unicode::convert_to<char>(_char));
#endif
    bool const consecutiveTextWrite = sequencer_.instructionCounter() == 1;

    if (wrapPending_ && cursor_.autoWrap)
    {
        linefeed(margin_.horizontal.from);
        if (isModeEnabled(DECMode::TextReflow))
            currentLine_->setWrapped(true);
    }

    char32_t const ch =
        _char < 127 ? cursor_.charsets.map(static_cast<char>(_char))
                    : _char == 0x7F ? ' ' : _char;

    char32_t const lastChar =
        consecutiveTextWrite && !lastColumn_->empty()
            ? lastColumn_->codepoint(lastColumn_->codepointCount() - 1)
            : char32_t{0};

    bool const insertToPrev =
        lastChar && unicode::grapheme_segmenter::nonbreakable(lastChar, ch);

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
    cell.setAttributes(cursor_.graphicsRendition);
#if defined(LIBTERMINAL_HYPERLINKS)
    cell.setHyperlink(currentHyperlink_);
#endif

    lastColumn_ = currentColumn_;
    lastCursorPosition_ = cursor_.position;

    bool const cursorInsideMargin = isModeEnabled(DECMode::LeftRightMargin) && isCursorInsideMargins();
    auto const cellsAvailable = cursorInsideMargin
        ? margin_.horizontal.to - cursor_.position.column
        : unbox<int>(size_.columns) - cursor_.position.column;

    auto const n = min(cell.width(), cellsAvailable);

    if (n == cell.width())
    {
        assert(n > 0);
        cursor_.position.column += n;
        currentColumn_++;
        for (int i = 1; i < n; ++i)
#if defined(LIBTERMINAL_HYPERLINKS)
            (currentColumn_++)->reset(cursor_.graphicsRendition, currentHyperlink_);
#else
            (currentColumn_++)->reset(cursor_.graphicsRendition);
#endif
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
#if defined(LIBTERMINAL_HYPERLINKS)
            (currentColumn_++)->reset(cursor_.graphicsRendition, currentHyperlink_);
#else
            (currentColumn_++)->reset(cursor_.graphicsRendition);
#endif
    }
    else if (cursor_.autoWrap)
    {
        wrapPending_ = 1;
    }
}

std::string Screen::screenshot(function<string(int)> const& _postLine) const
{
    auto result = std::stringstream{};
    auto writer = VTWriter(result);

    for (int const absoluteRow : crispy::times(1, *grid().historyLineCount() + *size_.lines))
    {
        auto const row = absoluteRow - unbox<int>(grid().historyLineCount());
        for (int const col: crispy::times(1, *size_.columns))
        {
            Cell const& cell = at({row, col});

            if (cell.attributes().styles & CellFlags::Bold)
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

        if (_postLine)
            writer.write(_postLine(row));

        writer.write('\r');
        writer.write('\n');
    }

    return result.str();
}

optional<int> Screen::findMarkerBackward(int _currentCursorLine) const
{
    // XXX _currentCursorLine is an absolute history line coordinate
    if (_currentCursorLine < 0 || isAlternateScreen())
        return nullopt;

    _currentCursorLine = min(
        _currentCursorLine,
        unbox<int>(historyLineCount()) + unbox<int>(size_.lines)
    );

    for (int i = _currentCursorLine - 1; i >= 0; --i)
        if (grid().absoluteLineAt(i).marked())
            return {i};

    return nullopt;
}

optional<int> Screen::findMarkerForward(int _currentCursorLine) const
{
    if (_currentCursorLine < 0 || !isPrimaryScreen())
        return nullopt;

    auto const end = unbox<int>(historyLineCount()) +
                     unbox<int>(grid().screenSize().lines);

    for (int i = _currentCursorLine + 1; i < end; ++i)
        if (grid().absoluteLineAt(i).marked())
            return {i};

    return nullopt;
}

// {{{ tabs related
void Screen::clearAllTabs()
{
    tabs_.clear();
}

void Screen::clearTabUnderCursor()
{
    // populate tabs vector in case of default tabWidth is used (until now).
    if (tabs_.empty() && tabWidth_ != 0)
        for (int column = tabWidth_; column <= unbox<int>(size_.columns); column += tabWidth_)
            tabs_.emplace_back(column);

    // erase the specific tab underneath
    for (auto i = begin(tabs_); i != end(tabs_); ++i)
    {
        if (*i == ColumnPosition(realCursorPosition().column))
        {
            tabs_.erase(i);
            break;
        }
    }
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
    restoreCursor(savedCursor_);

    setMode(DECMode::AutoWrap, savedCursor_.autoWrap);
    setMode(DECMode::Origin, savedCursor_.originMode);
}

void Screen::restoreCursor(Cursor const& _savedCursor)
{
    wrapPending_ = 0;
    cursor_ = _savedCursor;
    updateCursorIterators();
}

void Screen::resetSoft()
{
    // https://vt100.net/docs/vt510-rm/DECSTR.html
    setMode(DECMode::BatchedRendering, false);
    setMode(DECMode::TextReflow, GridTextReflowEnabled);
    setGraphicsRendition(GraphicsRendition::Reset); // SGR
    savedCursor_.position = Coordinate{1, 1}; // DECSC (Save cursor state)
    setMode(DECMode::VisibleCursor, true); // DECTCEM (Text cursor enable)
    setMode(DECMode::Origin, false); // DECOM
    setMode(AnsiMode::KeyboardAction, false); // KAM
    setMode(DECMode::AutoWrap, false); // DECAWM
    setMode(AnsiMode::Insert, false); // IRM
    setMode(DECMode::UseApplicationCursorKeys, false); // DECCKM (Cursor keys)
    setTopBottomMargin(1, unbox<int>(size_.lines)); // DECSTBM
    setLeftRightMargin(1, unbox<int>(size_.columns)); // DECRLM

#if defined(LIBTERMINAL_HYPERLINKS)
    currentHyperlink_ = {};
#endif
    colorPalette_ = defaultColorPalette_;

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
    setMode(DECMode::AutoWrap, true);
    setMode(DECMode::TextReflow, true);

    clearAllTabs();

    grids_ = emptyGrids(size(), primaryGrid().maxHistoryLineCount());
    activeGrid_ = &primaryGrid();
    moveCursorTo(Coordinate{1, 1});

    lastColumn_ = currentColumn_;
    lastCursorPosition_ = cursor_.position;

    margin_ = Margin{
        Margin::Range{1, unbox<int>(size_.lines)},
        Margin::Range{1, unbox<int>(size_.columns)}
    };

#if defined(LIBTERMINAL_HYPERLINKS)
    currentHyperlink_ = {};
#endif
    colorPalette_ = defaultColorPalette_;

    eventListener_.hardReset();
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
                activeGrid_ = &primaryGrid();
                break;
            case ScreenType::Alternate:
                if (isModeEnabled(DECMode::MouseAlternateScroll))
                    eventListener_.setMouseWheelMode(InputGenerator::MouseWheelMode::ApplicationCursorKeys);
                else
                    eventListener_.setMouseWheelMode(InputGenerator::MouseWheelMode::NormalCursorKeys);
                activeGrid_ = &alternateGrid();
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
        realCursorPosition().row == unbox<int>(size_.lines))
    {
        scrollUp(LineCount(1));
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

void Screen::scrollUp(LineCount _n, Margin const& _margin)
{
    grid().scrollUp(_n, cursor().graphicsRendition, _margin);
    updateCursorIterators();
}

void Screen::scrollDown(LineCount _n, Margin const& _margin)
{
    grid().scrollDown(_n, cursor().graphicsRendition, _margin);
    updateCursorIterators();
}

void Screen::setCurrentColumn(ColumnPosition _n)
{
    auto const col = cursor_.originMode
        ? ColumnPosition::cast_from(margin_.horizontal.from + *_n - 1)
        : _n;
    auto const clampedCol = min(col, boxed_cast<ColumnPosition>(size_.columns));
    cursor_.position.column = *clampedCol;
    updateColumnIterator();
}

string Screen::renderText() const
{
    return grid().renderText();
}

string Screen::renderTextLine(int row) const
{
    return grid().renderTextLine(row);
}

string Screen::renderHistoryTextLine(int _lineNumberIntoHistory) const
{
    assert(1 <= _lineNumberIntoHistory && _lineNumberIntoHistory <= unbox<int>(historyLineCount()));
    string line;
    line.reserve(*size_.columns);

    for (Cell const& cell : grid().lineAt(1 - _lineNumberIntoHistory))
        if (cell.codepointCount())
            line += cell.toUtf8();
        else
            line += ' '; // fill character

    return line;
}
// }}}

// {{{ ops
void Screen::linefeed()
{
    if (isModeEnabled(AnsiMode::AutomaticNewLine))
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
        DeviceAttributes::CaptureScreenBuffer |
        DeviceAttributes::Columns132 |
        //TODO: DeviceAttributes::NationalReplacementCharacterSets |
        DeviceAttributes::RectangularEditing |
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
#if defined(LIBTERMINAL_HYPERLINKS)
    if (isAlternateScreen() && cursor_.position.row == 1 && cursor_.position.column == 1)
        hyperlinks_.clear();
#endif

    clearToEndOfLine();

    for_each(
        LIBTERMINAL_EXECUTION_COMMA(par)
        next(currentLine_),
        end(grid().mainPage()),
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
        begin(grid().mainPage()),
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
    scrollUp(size_.lines);
}

void Screen::clearScrollbackBuffer()
{
    primaryGrid().clearHistory();
    alternateGrid().clearHistory();
    eventListener_.scrollbackBufferCleared();
}

void Screen::eraseCharacters(ColumnCount _n)
{
    // Spec: https://vt100.net/docs/vt510-rm/ECH.html
    // It's not clear from the spec how to perform erase when inside margin and number of chars to be erased would go outside margins.
    // TODO: See what xterm does ;-)
    size_t const n = min(
        unbox<int>(size_.columns) - realCursorPosition().column + 1,
        *_n == 0 ? 1 : unbox<int>(_n));
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

void Screen::moveCursorToNextLine(LineCount _n)
{
    moveCursorTo({cursorPosition().row + unbox<int>(_n), 1});
}

void Screen::moveCursorToPrevLine(LineCount _n)
{
    auto const n = min(unbox<int>(_n), cursorPosition().row - 1);
    moveCursorTo({cursorPosition().row - n, 1});
}

void Screen::insertCharacters(ColumnCount _n)
{
    if (isCursorInsideMargins())
        insertChars(realCursorPosition().row, _n);
}

/// Inserts @p _n characters at given line @p _lineNo.
void Screen::insertChars(int _lineNo, ColumnCount _n)
{
    auto const n = min(
        unbox<int>(_n),
        margin_.horizontal.to - cursorPosition().column + 1
    );

    auto && line = grid().lineAt(_lineNo);
    auto column0 = next(begin(line), realCursorPosition().column - 1);
    auto column1 = next(begin(line), margin_.horizontal.to - n);
    auto column2 = next(begin(line), margin_.horizontal.to);

    rotate(
        column0,
        column1,
        column2
    );

    if (&line == &*currentLine_)
        updateColumnIterator();

    fill_n(
        columnIteratorAt(begin(line), cursor_.position.column),
        n,
        Cell{L' ', cursor_.graphicsRendition}
    );
}

void Screen::insertLines(LineCount _n)
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

void Screen::insertColumns(ColumnCount _n)
{
    if (isCursorInsideMargins())
        for (int lineNo = margin_.vertical.from; lineNo <= margin_.vertical.to; ++lineNo)
            insertChars(lineNo, _n);
}

void Screen::copyArea(int _top, int _left, int _bottom, int _right, int _page,
                      int _targetTop, int _targetLeft, int _targetPage
)
{
    (void) _page;
    (void) _targetPage;

    // The space at https://vt100.net/docs/vt510-rm/DECCRA.html states:
    // "If Pbs is greater than Pts, // or Pls is greater than Prs, the terminal ignores DECCRA."
    //
    // However, the first part "Pbs is greater than Pts" does not make sense.
    if (_bottom < _top || _right < _left)
        return;

    if (_top == _targetTop && _left == _targetLeft)
        // Copy to its own location => no-op.
        return;

    auto const [x0, xInc, xEnd] = [&]() {
        if (_targetLeft > _left) // moving right
            return std::tuple{_right - _left, -1, -1};
        else
            return std::tuple{0, +1, _right - _left + 1};
    }();

    auto const [y0, yInc, yEnd] = [&]() {
        if (_targetTop > _top) // moving down
            return std::tuple{_bottom - _top, -1, -1};
        else
            return std::tuple{0, +1, _bottom - _top + 1};
    }();

    for (auto y = y0; y != yEnd; y += yInc)
    {
        for (auto x = x0; x != xEnd; x += xInc)
        {
            Cell const& sourceCell = at({_top + y, _left + x});
            Cell& targetCell = at({_targetTop + y, _targetLeft + x});
            targetCell = sourceCell;
        }
    }

    updateCursorIterators();
}

void Screen::eraseArea(int _top, int _left, int _bottom, int _right)
{
    assert(_right <= unbox<int>(size_.columns));
    assert(_bottom <= unbox<int>(size_.lines));

    if (_top > _bottom || _left > _right)
        return;

    for (int y = _top; y <= _bottom; ++y)
    {
        Line& line = grid().lineAt(y);
        auto column = next(begin(line), _left - 1);
        for (int x = _left; x <= _right; ++x)
        {
            Cell& cell = *column;
            cell.reset();
            cell.setCharacter(0x20);
            ++column;
        }
    }
}

void Screen::fillArea(char32_t _ch, int _top, int _left, int _bottom, int _right)
{
    // "Pch can be any value from 32 to 126 or from 160 to 255."
    if (!(32 <= _ch && _ch <= 126) && !(160 <= _ch && _ch <= 255))
        return;

    for (int y = _top; y <= _bottom; ++y)
    {
        Line& line = grid().lineAt(y);
        auto column = next(begin(line), _left - 1);
        for (int x = _left; x <= _right; ++x)
        {
            Cell& cell = *column;
            cell.reset(cursor().graphicsRendition);
            cell.setCharacter(_ch);
            ++column;
        }
    }
}

void Screen::deleteLines(LineCount _n)
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

void Screen::deleteCharacters(ColumnCount _n)
{
    if (isCursorInsideMargins() && *_n != 0)
        deleteChars(realCursorPosition().row, _n);
}

void Screen::deleteChars(int _lineNo, ColumnCount _n)
{
    auto line = next(begin(grid().mainPage()), _lineNo - 1);
    auto column = next(begin(*line), realCursorPosition().column - 1);
    auto rightMargin = next(begin(*line), margin_.horizontal.to);
    auto const n = min(unbox<int>(_n), static_cast<int>(distance(column, rightMargin)));

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
void Screen::deleteColumns(ColumnCount _n)
{
    if (isCursorInsideMargins())
        for (int lineNo = margin_.vertical.from; lineNo <= margin_.vertical.to; ++lineNo)
            deleteChars(lineNo, _n);
}

void Screen::horizontalTabClear(HorizontalTabClear _which)
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

void Screen::setCurrentWorkingDirectory(string const& _url)
{
    currentWorkingDirectory_ = _url;
}

void Screen::hyperlink(string const& _id, string const& _uri)
{
#if defined(LIBTERMINAL_HYPERLINKS)
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
#endif
}

void Screen::moveCursorUp(LineCount _n)
{
    auto const n = min(
        unbox<int>(_n),
        cursorPosition().row > margin_.vertical.from
            ? cursorPosition().row - margin_.vertical.from
            : cursorPosition().row - 1
    );

    cursor_.position.row -= n;
    currentLine_ = prev(currentLine_, n);
    setCurrentColumn(ColumnPosition::cast_from(cursorPosition().column));
}

void Screen::moveCursorDown(LineCount _n)
{
    auto const currentLineNumber = cursorPosition().row;
    auto const n = min(
        unbox<int>(_n),
        currentLineNumber <= margin_.vertical.to
            ? margin_.vertical.to - currentLineNumber
            : unbox<int>(size_.lines) - currentLineNumber
    );
    // auto const n =
    //     v.n > margin_.vertical.to
    //         ? min(v.n, size_.lines - cursorPosition().row)
    //         : min(v.n, margin_.vertical.to - cursorPosition().row);

    cursor_.position.row += n;
    currentLine_ = next(currentLine_, n);
    setCurrentColumn(ColumnPosition(cursorPosition().column));
}

void Screen::moveCursorForward(ColumnCount _n)
{
    auto const n = min(unbox<int>(_n), margin_.horizontal.length() - cursor_.position.column);
    cursor_.position.column += n;
    updateColumnIterator();
}

void Screen::moveCursorBackward(ColumnCount _n)
{
    // even if you move to 80th of 80 columns, it'll first write a char and THEN flag wrap pending
    wrapPending_ = 0;

    // TODO: skip cells that in counting when iterating backwards over a wide cell (such as emoji)
    auto const n = min(unbox<int>(_n), static_cast<int>(cursor_.position.column - 1));
    setCurrentColumn(ColumnPosition(cursor_.position.column - n));
}

void Screen::moveCursorToColumn(ColumnPosition _column)
{
    wrapPending_ = 0;
    setCurrentColumn(_column);
}

void Screen::moveCursorToBeginOfLine()
{
    wrapPending_ = 0;
    setCurrentColumn(ColumnPosition(1));
}

void Screen::moveCursorToLine(LinePosition _row)
{
    moveCursorTo(Coordinate{unbox<int>(_row), cursor_.position.column});
}

void Screen::moveCursorToNextTab()
{
    // TODO: I guess something must remember when a \t was added, for proper move-back?
    // TODO: respect HTS/TBC

    if (!tabs_.empty())
    {
        // advance to the next tab
        size_t i = 0;
        while (i < tabs_.size() && realCursorPosition().column >= *tabs_[i])
            ++i;

        auto const currentCursorColumn = cursorPosition().column;

        if (i < tabs_.size())
            moveCursorForward(ColumnCount(*tabs_[i] - currentCursorColumn));
        else if (realCursorPosition().column < margin_.horizontal.to)
            moveCursorForward(ColumnCount(margin_.horizontal.to - currentCursorColumn));
        else
            moveCursorToNextLine(LineCount(1));
    }
    else if (tabWidth_)
    {
        // default tab settings
        if (realCursorPosition().column < margin_.horizontal.to)
        {
            auto const n = min(
                ColumnCount(tabWidth_ - (cursor_.position.column - 1) % tabWidth_),
                size_.columns - ColumnCount(cursorPosition().column)
            );
            moveCursorForward(n);
        }
        else
            moveCursorToNextLine(LineCount(1));
    }
    else
    {
        // no tab stops configured
        if (realCursorPosition().column < margin_.horizontal.to)
            // then TAB moves to the end of the screen
            moveCursorToColumn(ColumnPosition(margin_.horizontal.to));
        else
            // then TAB moves to next line left margin
            moveCursorToNextLine(LineCount(1));
    }
}

void Screen::notify(string const& _title, string const& _content)
{
    std::cout << "Screen.NOTIFY: title: '" << _title << "', content: '" << _content << "'\n";
    eventListener_.notify(_title, _content);
}

void Screen::captureBuffer(int _lineCount, bool _logicalLines)
{
    // TODO: Unit test case! (for ensuring line numbering and limits are working as expected)

    auto capturedBuffer = std::string();
    auto writer = VTWriter([&](auto buf, auto len) { capturedBuffer += string_view(buf, len); });

    // TODO: when capturing _lineCount < screenSize.lines, start at the lowest non-empty line.
    auto const relativeStartLine = _logicalLines ? grid().computeRelativeLineNumberFromBottom(_lineCount)
                                                 : unbox<int>(size_.lines) - _lineCount + 1;
    auto const startLine = clamp(
        1 - unbox<int>(historyLineCount()),
        relativeStartLine,
        unbox<int>(size_.lines));

    // dumpState();

    auto const lineCount = unbox<int>(size_.lines) - startLine + 1;

    auto const trimSpaceRight = [](string& value)
    {
        while (!value.empty() && value.back() == ' ')
            value.pop_back();
    };

    for (int const row : crispy::times(startLine, lineCount))
    {
        auto const& lineBuffer = grid().lineAt(row);

        if (_logicalLines && lineBuffer.wrapped() && !capturedBuffer.empty())
            capturedBuffer.pop_back();

        if (!lineBuffer.blank())
        {
            for (int const col : crispy::times(1, unbox<int>(size_.columns)))
            {
                Cell const& cell = at({row, col});
                if (!cell.codepointCount())
                    writer.write(U' ');
                else
                    for (char32_t const ch : cell.codepoints())
                        writer.write(ch);
            }
            trimSpaceRight(capturedBuffer);
        }

        writer.write('\n');
    }

    while (crispy::endsWith(string_view(capturedBuffer), "\n\n"sv)) // TODO: unit test
        capturedBuffer.pop_back();

    auto constexpr PageSize = size_t{4096};
    for (size_t i = 0; i < capturedBuffer.size(); i += PageSize)
    {
        auto const start = capturedBuffer.data() + i;
        auto const count = min(PageSize, capturedBuffer.size() - i);
        reply("\033]314;{}\033\\", string_view(start, count));
    }

    reply("\033]314;\033\\"); // mark the end
}

void Screen::cursorForwardTab(TabStopCount _count)
{
    for (int i = 0; i < unbox<int>(_count); ++i)
        moveCursorToNextTab();
}

void Screen::cursorBackwardTab(TabStopCount _count)
{
    if (!_count)
        return;

    if (!tabs_.empty())
    {
        for (unsigned k = 0; k < unbox<unsigned>(_count); ++k)
        {
            auto const i = std::find_if(rbegin(tabs_), rend(tabs_),
                                        [&](ColumnPosition tabPos) -> bool {
                                            return *tabPos <= cursorPosition().column - 1;
                                        });
            if (i != rend(tabs_))
            {
                // prev tab found -> move to prev tab
                moveCursorToColumn(*i);
            }
            else
            {
                moveCursorToColumn(ColumnPosition(margin_.horizontal.from));
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
                         ? (*_count - 1) * tabWidth_ + m
                         : *_count * tabWidth_ + m;
            moveCursorBackward(ColumnCount(n - 1));
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
        scrollUp(LineCount(1));
    else
        moveCursorTo({cursorPosition().row + 1, cursorPosition().column});
}

void Screen::reverseIndex()
{
    if (realCursorPosition().row == margin_.vertical.from)
        scrollDown(LineCount(1));
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
    cursorDisplay_ = _display;
    cursorShape_ = _shape;

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
            cursor_.graphicsRendition.styles |= CellFlags::Bold;
            break;
        case GraphicsRendition::Faint:
            cursor_.graphicsRendition.styles |= CellFlags::Faint;
            break;
        case GraphicsRendition::Italic:
            cursor_.graphicsRendition.styles |= CellFlags::Italic;
            break;
        case GraphicsRendition::Underline:
            cursor_.graphicsRendition.styles |= CellFlags::Underline;
            break;
        case GraphicsRendition::Blinking:
            cursor_.graphicsRendition.styles |= CellFlags::Blinking;
            break;
        case GraphicsRendition::Inverse:
            cursor_.graphicsRendition.styles |= CellFlags::Inverse;
            break;
        case GraphicsRendition::Hidden:
            cursor_.graphicsRendition.styles |= CellFlags::Hidden;
            break;
        case GraphicsRendition::CrossedOut:
            cursor_.graphicsRendition.styles |= CellFlags::CrossedOut;
            break;
        case GraphicsRendition::DoublyUnderlined:
            cursor_.graphicsRendition.styles |= CellFlags::DoublyUnderlined;
            break;
        case GraphicsRendition::CurlyUnderlined:
            cursor_.graphicsRendition.styles |= CellFlags::CurlyUnderlined;
            break;
        case GraphicsRendition::DottedUnderline:
            cursor_.graphicsRendition.styles |= CellFlags::DottedUnderline;
            break;
        case GraphicsRendition::DashedUnderline:
            cursor_.graphicsRendition.styles |= CellFlags::DashedUnderline;
            break;
        case GraphicsRendition::Framed:
            cursor_.graphicsRendition.styles |= CellFlags::Framed;
            break;
        case GraphicsRendition::Overline:
            cursor_.graphicsRendition.styles |= CellFlags::Overline;
            break;
        case GraphicsRendition::Normal:
            cursor_.graphicsRendition.styles &= ~(CellFlags::Bold | CellFlags::Faint);
            break;
        case GraphicsRendition::NoItalic:
            cursor_.graphicsRendition.styles &= ~CellFlags::Italic;
            break;
        case GraphicsRendition::NoUnderline:
            cursor_.graphicsRendition.styles &= ~CellFlags::Underline;
            break;
        case GraphicsRendition::NoBlinking:
            cursor_.graphicsRendition.styles &= ~CellFlags::Blinking;
            break;
        case GraphicsRendition::NoInverse:
            cursor_.graphicsRendition.styles &= ~CellFlags::Inverse;
            break;
        case GraphicsRendition::NoHidden:
            cursor_.graphicsRendition.styles &= ~CellFlags::Hidden;
            break;
        case GraphicsRendition::NoCrossedOut:
            cursor_.graphicsRendition.styles &= ~CellFlags::CrossedOut;
            break;
        case GraphicsRendition::NoFramed:
            cursor_.graphicsRendition.styles &= ~CellFlags::Framed;
            break;
        case GraphicsRendition::NoOverline:
            cursor_.graphicsRendition.styles &= ~CellFlags::Overline;
            break;
    }
}

void Screen::setMark()
{
    currentLine_->setMarked(true);
}

void Screen::saveModes(std::vector<DECMode> const& _modes)
{
    modes_.save(_modes);
}

void Screen::restoreModes(std::vector<DECMode> const& _modes)
{
    modes_.restore(_modes);
}

void Screen::setMode(AnsiMode _mode, bool _enable)
{
    if (!isValidAnsiMode(static_cast<int>(_mode)))
        return;

    modes_.set(_mode, _enable);
}

void Screen::setMode(DECMode _mode, bool _enable)
{
    if (!isValidDECMode(static_cast<int>(_mode)))
        return;

    switch (_mode)
    {
        case DECMode::AutoWrap:
            cursor_.autoWrap = _enable;
            break;
        case DECMode::LeftRightMargin:
            // Resetting DECLRMM also resets the horizontal margins back to screen size.
            if (!_enable)
                margin_.horizontal = {1, unbox<int>(size_.columns)};
            break;
        case DECMode::Origin:
            cursor_.originMode = _enable;
            break;
        case DECMode::Columns132:
            if (!_enable || isModeEnabled(DECMode::AllowColumns80to132))
            {
                auto const clear = _enable != isModeEnabled(_mode);

                // sets the number of columns on the page to 80 or 132 and selects the
                // corresponding 80- or 132-column font
                auto const columns = ColumnCount(_enable ? 132 : 80);

                resizeColumns(columns, clear);
            }
            break;
        case DECMode::BatchedRendering:
            // Only perform batched rendering when NOT in debugging mode.
            // TODO: also, do I still need this here?
            break;
        case DECMode::TextReflow:
            if (isPrimaryScreen())
            {
                if (_enable)
                {
                    // enabling reflow enables every line in the main page area
                    for (Line& line : primaryGrid().mainPage())
                        line.setFlag(Line::Flags::Wrappable, _enable);
                }
                else
                {
                    // disabling reflow only affects currently line and below
                    auto const numHistLines = historyLineCount().value;
                    auto const numScreenLines = size_.lines.value;
                    auto const startLineOffset = realCursorPosition().row - 1;
                    auto const startLine = LinePosition(numHistLines + startLineOffset);
                    auto const endLine = LinePosition(numHistLines + numScreenLines);
                    // auto const startLine = LinePosition(*historyLineCount() + realCursorPosition().row - 1);
                    // auto const endLine = LinePosition(*historyLineCount() + *size_.lines);
                    assert(primaryGrid().lines(startLine, endLine).begin() == currentLine_);
                    for (Line& line : primaryGrid().lines(startLine, endLine))
                        line.setFlag(Line::Flags::Wrappable, _enable);
                }
            }
            break;
        case DECMode::DebugLogging:
            // Since this mode (Xterm extension) does not support finer graind control,
            // we'll be just globally enable/disable all debug logging.
            crispy::logging_sink::for_debug().enable(_enable);
            for (auto& tag: crispy::debugtag::store())
                tag.enabled = _enable;
            break;
        case DECMode::UseAlternateScreen:
            if (_enable)
                setBuffer(ScreenType::Alternate);
            else
                setBuffer(ScreenType::Main);
            break;
        case DECMode::UseApplicationCursorKeys:
            eventListener_.useApplicationCursorKeys(_enable);
            if (isAlternateScreen())
            {
                if (_enable)
                    eventListener_.setMouseWheelMode(InputGenerator::MouseWheelMode::ApplicationCursorKeys);
                else
                    eventListener_.setMouseWheelMode(InputGenerator::MouseWheelMode::NormalCursorKeys);
            }
            break;
        case DECMode::BracketedPaste:
            eventListener_.setBracketedPaste(_enable);
            break;
        case DECMode::MouseSGR:
            if (_enable)
                eventListener_.setMouseTransport(MouseTransport::SGR);
            else
                eventListener_.setMouseTransport(MouseTransport::Default);
            break;
        case DECMode::MouseExtended:
            eventListener_.setMouseTransport(MouseTransport::Extended);
            break;
        case DECMode::MouseURXVT:
            eventListener_.setMouseTransport(MouseTransport::URXVT);
            break;
        case DECMode::MouseAlternateScroll:
            if (_enable)
                eventListener_.setMouseWheelMode(InputGenerator::MouseWheelMode::ApplicationCursorKeys);
            else
                eventListener_.setMouseWheelMode(InputGenerator::MouseWheelMode::NormalCursorKeys);
            break;
        case DECMode::FocusTracking:
            eventListener_.setGenerateFocusEvents(_enable);
            break;
        case DECMode::UsePrivateColorRegisters:
            sequencer_.setUsePrivateColorRegisters(_enable);
            break;
        case DECMode::VisibleCursor:
            cursor_.visible = _enable;
            eventListener_.setCursorVisibility(_enable);
            break;
        case DECMode::MouseProtocolX10:
            sendMouseEvents(MouseProtocol::X10, _enable);
            break;
        case DECMode::MouseProtocolNormalTracking:
            sendMouseEvents(MouseProtocol::NormalTracking, _enable);
            break;
        case DECMode::MouseProtocolHighlightTracking:
            sendMouseEvents(MouseProtocol::HighlightTracking, _enable);
            break;
        case DECMode::MouseProtocolButtonTracking:
            sendMouseEvents(MouseProtocol::ButtonTracking, _enable);
            break;
        case DECMode::MouseProtocolAnyEventTracking:
            sendMouseEvents(MouseProtocol::AnyEventTracking, _enable);
            break;
        case DECMode::SaveCursor:
            if (_enable)
                saveCursor();
            else
                restoreCursor();
            break;
        case DECMode::ExtendedAltScreen:
            if (_enable)
            {
                savedPrimaryCursor_ = cursor();
                setMode(DECMode::UseAlternateScreen, true);
                clearScreen();
            }
            else
            {
                setMode(DECMode::UseAlternateScreen, false);
                restoreCursor(savedPrimaryCursor_);
            }
            break;
        default:
            break;
    }

    modes_.set(_mode, _enable);
}

enum class ModeResponse { // TODO: respect response 0, 3, 4.
    NotRecognized = 0,
    Set = 1,
    Reset = 2,
    PermanentlySet = 3,
    PermanentlyReset = 4
};

void Screen::requestAnsiMode(int _mode)
{
    ModeResponse const modeResponse =
        isValidAnsiMode(_mode)
            ? isModeEnabled(static_cast<AnsiMode>(_mode))
                ? ModeResponse::Set
                : ModeResponse::Reset
            : ModeResponse::NotRecognized;

    auto const code = toAnsiModeNum(static_cast<AnsiMode>(_mode));

    reply("\033[{};{}$y", code, static_cast<unsigned>(modeResponse));
}

void Screen::requestDECMode(int _mode)
{
    ModeResponse const modeResponse =
        isValidDECMode(_mode)
            ? isModeEnabled(static_cast<DECMode>(_mode))
                ? ModeResponse::Set
                : ModeResponse::Reset
            : ModeResponse::NotRecognized;

    auto const code = toDECModeNum(static_cast<DECMode>(_mode));

    reply("\033[?{};{}$y", code, static_cast<unsigned>(modeResponse));
}

void Screen::setTopBottomMargin(optional<int> _top, optional<int> _bottom)
{
	auto const bottom = _bottom.has_value()
		? min(_bottom.value(), unbox<int>(size_.lines))
		: unbox<int>(size_.lines);

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
    if (isModeEnabled(DECMode::LeftRightMargin))
    {
		auto const right = _right.has_value()
			? min(_right.value(), unbox<int>(size_.columns))
			: unbox<int>(size_.columns);
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
    margin_.vertical.to = *size_.lines;
    margin_.horizontal.from = 1;
    margin_.horizontal.to = *size_.columns;

    // and moves the cursor to the home position
    moveCursorTo({1, 1});

    // fills the complete screen area with a test pattern
    crispy::for_each(
        LIBTERMINAL_EXECUTION_COMMA(par)
        grid().mainPage(),
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

void Screen::sixelImage(ImageSize _pixelSize, Image::Data&& _data)
{
    auto const columnCount = ColumnCount(unsigned(ceilf(float(*_pixelSize.width) / float(*cellPixelSize_.width))));
    auto const lineCount = LineCount(unsigned(ceilf(float(*_pixelSize.height) / float(*cellPixelSize_.height))));
    auto const extent = GridSize{lineCount, columnCount};
    auto const sixelScrolling = isModeEnabled(DECMode::SixelScrolling);
    auto const topLeft = sixelScrolling ? cursorPosition() : Coordinate{1, 1};

    auto const alignmentPolicy = ImageAlignment::TopStart;
    auto const resizePolicy = ImageResize::NoResize;

    auto const imageOffset = Coordinate{0, 0};
    auto const imageSize = _pixelSize;

    if (auto const imageRef = uploadImage(ImageFormat::RGBA, _pixelSize, move(_data)); imageRef)
        renderImage(imageRef, topLeft, extent,
                    imageOffset, imageSize,
                    alignmentPolicy, resizePolicy,
                    sixelScrolling);

    if (!sixelCursorConformance_)
        linefeed(topLeft.column);
}

std::shared_ptr<Image const> Screen::uploadImage(ImageFormat _format, ImageSize _imageSize, Image::Data&& _pixmap)
{
    return imagePool_.create(_format, _imageSize, move(_pixmap));
}

void Screen::renderImage(std::shared_ptr<Image const> const& _imageRef,
                         Coordinate _topLeft, GridSize _gridSize,
                         Coordinate _imageOffset, ImageSize _imageSize,
                         ImageAlignment _alignmentPolicy,
                         ImageResize _resizePolicy,
                         bool _autoScroll)
{
    // TODO: make use of _imageOffset and _imageSize
    (void) _imageOffset;
    (void) _imageSize;

#if !defined(LIBTERMINAL_IMAGES)
    (void) _imageRef;
    (void) _alignmentPolicy;
    (void) _autoScroll;
#else
    auto const linesAvailable = LineCount(1 + *size_.lines - _topLeft.row);
    auto const linesToBeRendered = min(_gridSize.lines, linesAvailable);
    auto const columnsToBeRendered = ColumnCount(min(*_gridSize.columns, *size_.columns - _topLeft.column - 1));
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

    if (*linesToBeRendered)
    {
        crispy::for_each(
            LIBTERMINAL_EXECUTION_COMMA(par)
            GridSize{linesToBeRendered, columnsToBeRendered},
            [&](GridSize::Offset offset) {
                auto const cellCoord = Coordinate{
                    _topLeft.row + *offset.line,
                    _topLeft.column + *offset.column};
                Cell& cell = at(cellCoord);
                cell.setImage(
                    ImageFragment{
                        rasterizedImage,
                        Coordinate(*offset.line, *offset.column)
                    }
                );
#if defined(LIBTERMINAL_HYPERLINKS)
                cell.setHyperlink(currentHyperlink_);
#endif
            }
        );
        moveCursorTo(Coordinate{_topLeft.row + unbox<int>(linesToBeRendered) - 1, _topLeft.column});
    }

    // If there're lines to be rendered missing (because it didn't fit onto the screen just yet)
    // AND iff sixel sixelScrolling  is enabled, then scroll as much as needed to render the remaining lines.
    if (linesToBeRendered != _gridSize.lines && _autoScroll)
    {
        auto const remainingLineCount = _gridSize.lines - linesToBeRendered;
        for (auto const lineOffset : crispy::times(*remainingLineCount))
        {
            linefeed();
            moveCursorForward(ColumnCount(_topLeft.column));
            crispy::for_each(
                LIBTERMINAL_EXECUTION_COMMA(par)
                crispy::times(unbox<int>(columnsToBeRendered)),
                [&](int columnOffset) {
                    Cell& cell = at(Coordinate{unbox<int>(size_.lines), columnOffset + 1});
                    cell.setImage(ImageFragment{
                        rasterizedImage,
                        Coordinate{unbox<int>(linesToBeRendered) + int(lineOffset), columnOffset}
                    });
#if defined(LIBTERMINAL_HYPERLINKS)
                    cell.setHyperlink(currentHyperlink_);
#endif
                }
            );
        }
    }

    // move ansi text cursor to position of the sixel cursor
    moveCursorToColumn(ColumnPosition(_topLeft.column + unbox<int>(_gridSize.columns)));
#endif
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
    auto const color = [&]() -> optional<RGBColor>
    {
        switch (_name)
        {
            case DynamicColorName::DefaultForegroundColor:
                return colorPalette_.defaultForeground;
            case DynamicColorName::DefaultBackgroundColor:
                return colorPalette_.defaultBackground;
            case DynamicColorName::TextCursorColor:
                return colorPalette_.cursor;
            case DynamicColorName::MouseForegroundColor:
                return colorPalette_.mouseForeground;
            case DynamicColorName::MouseBackgroundColor:
                return colorPalette_.mouseBackground;
            case DynamicColorName::HighlightForegroundColor:
                if (colorPalette_.selectionForeground.has_value())
                    return colorPalette_.selectionForeground.value();
                else
                    return nullopt;
            case DynamicColorName::HighlightBackgroundColor:
                if (colorPalette_.selectionBackground.has_value())
                    return colorPalette_.selectionBackground.value();
                else
                    return nullopt;
        }
        return nullopt; // should never happen
    }();

    if (color.has_value())
    {
        reply(
            "\033]{};{}\033\\",
            setDynamicColorCommand(_name),
            setDynamicColorValue(color.value())
        );
    }
}

void Screen::requestPixelSize(RequestPixelSize _area)
{
    switch (_area)
    {
        case RequestPixelSize::WindowArea:
            [[fallthrough]]; // TODO
        case RequestPixelSize::TextArea:
            // Result is CSI  4 ;  height ;  width t
            reply(
                "\033[4;{};{}t",
                *cellPixelSize_.height * *size_.lines,
                *cellPixelSize_.width * *size_.columns
            );
            break;
        case RequestPixelSize::CellArea:
            // Result is CSI  6 ;  height ;  width t
            reply(
                "\033[6;{};{}t",
                cellPixelSize_.height,
                cellPixelSize_.width
            );
            break;
    }
}

void Screen::requestCharacterSize(RequestPixelSize _area) // TODO: rename RequestPixelSize to RequestArea?
{
    switch (_area)
    {
        case RequestPixelSize::TextArea:
            reply("\033[8;{};{}t", size_.lines, size_.columns);
            break;
        case RequestPixelSize::WindowArea:
            reply("\033[9;{};{}t", size_.lines, size_.columns);
            break;
        case RequestPixelSize::CellArea:
            assert(!"Screen.requestCharacterSize: Doesn't make sense, and cannot be called, therefore, fortytwo.");
            break;
    }
}

void Screen::requestStatusString(RequestStatusString _value)
{
    // xterm responds with DCS 1 $ r Pt ST for valid requests
    // or DCS 0 $ r Pt ST for invalid requests.
    auto const response = [&](RequestStatusString _value) -> optional<string> {
        switch (_value)
        {
            case RequestStatusString::DECSCL:
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

                return fmt::format("{};{}\"p", level, c1t);
            }
            case RequestStatusString::DECSCUSR: // Set cursor style (DECSCUSR), VT520
            {
                int const blinkingOrSteady = cursorDisplay_ == CursorDisplay::Steady  ? 1 : 0;
                int const shape = [&]() {
                    switch (cursorShape_)
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
                if (*size_.lines >= 24)
                    return fmt::format("{}t", size_.lines);
#if defined(LIBTERMINAL_LOG_RAW)
                debuglog(ScreenRawOutputTag).write("Requesting device status for {} not with line count < 24 is undefined.");
#endif
                return nullopt;
            case RequestStatusString::DECSTBM:
                return fmt::format("{};{}r", margin_.vertical.from, margin_.vertical.to);
            case RequestStatusString::DECSLRM:
                return fmt::format("{};{}s", margin_.horizontal.from, margin_.horizontal.to);
            case RequestStatusString::DECSCPP:
                // EXTENSION: Usually DECSCPP only knows about 80 and 132, but we take any.
                return fmt::format("{}|$", size_.columns);
            case RequestStatusString::DECSNLS:
                return fmt::format("{}*|", size_.lines);
            case RequestStatusString::SGR:
                return fmt::format("0;{}m", vtSequenceParameterString(cursor_.graphicsRendition));
            case RequestStatusString::DECSCA: // TODO
#if defined(LIBTERMINAL_LOG_RAW)
                debuglog(ScreenRawOutputTag).write("Requesting device status for {} not implemented yet.", _value);
#endif
                break;
        }
        return nullopt;
    }(_value);

    reply(
        "\033P{}$r{}\033\\",
        response.has_value() ? 1 : 0,
        response.value_or(""),
        "\"p"
    );
}

void Screen::requestTabStops()
{
    // Response: `DCS 2 $ u Pt ST`
    ostringstream dcs;
    dcs << "\033P2$u"sv; // DCS
    if (!tabs_.empty())
    {
        for (size_t const i : times(tabs_.size()))
        {
            if (i)
                dcs << '/';
            dcs << *tabs_[i];
        }
    }
    else if (tabWidth_ != 0)
    {
        dcs << 1;
        for (int column = tabWidth_ + 1; column <= *size_.columns; column += tabWidth_)
            dcs << '/' << column;
    }
    dcs << "\033\\"sv; // ST

    reply(dcs.str());
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
}

void Screen::requestCapability(std::string_view _name)
{
    if (!respondToTCapQuery_)
    {
#if defined(LIBTERMINAL_LOG_RAW)
        debuglog(ScreenRawOutputTag).write("Requesting terminal capability {} ignored. Experimental tcap feature disabled.", _name);
#endif
        return;
    }

    if (booleanCapability(_name))
        reply("\033P1+r{}\033\\", toHexString(_name));
    else if (auto const value = numericCapability(_name); value >= 0)
    {
        auto hexValue = fmt::format("{:X}", value);
        if (hexValue.size() % 2)
            hexValue.insert(hexValue.begin(), '0');
        reply("\033P1+r{}={}\033\\", toHexString(_name), hexValue);
    }
    else if (auto const value = stringCapability(_name); !value.empty())
        reply("\033P1+r{}={}\033\\", toHexString(_name), asHex(value));
    else
        reply("\033P0+r\033\\");
}

void Screen::requestCapability(capabilities::Code _code)
{
    if (!respondToTCapQuery_)
    {
#if defined(LIBTERMINAL_LOG_RAW)
        debuglog(ScreenRawOutputTag).write("Requesting terminal capability {} ignored. Experimental tcap feature disabled.", _code);
#endif
        return;
    }

#if defined(LIBTERMINAL_LOG_RAW)
    debuglog(ScreenRawOutputTag).write("Requesting terminal capability: {}", _code);
#endif
    if (booleanCapability(_code))
        reply("\033P1+r{}\033\\", _code.hex());
    else if (auto const value = numericCapability(_code); value >= 0)
    {
        auto hexValue = fmt::format("{:X}", value);
        if (hexValue.size() % 2)
            hexValue.insert(hexValue.begin(), '0');
        reply("\033P1+r{}={}\033\\", _code.hex(), hexValue);
    }
    else if (auto const value = stringCapability(_code); !value.empty())
        reply("\033P1+r{}={}\033\\", _code.hex(), asHex(value));
    else
        reply("\033P0+r\033\\");
}

void Screen::resetDynamicColor(DynamicColorName _name)
{
    switch (_name)
    {
        case DynamicColorName::DefaultForegroundColor:
            colorPalette_.defaultForeground = defaultColorPalette_.defaultForeground;
            break;
        case DynamicColorName::DefaultBackgroundColor:
            colorPalette_.defaultBackground = defaultColorPalette_.defaultBackground;
            break;
        case DynamicColorName::TextCursorColor:
            colorPalette_.cursor = defaultColorPalette_.cursor;
            break;
        case DynamicColorName::MouseForegroundColor:
            colorPalette_.mouseForeground = defaultColorPalette_.mouseForeground;
            break;
        case DynamicColorName::MouseBackgroundColor:
            colorPalette_.mouseBackground = defaultColorPalette_.mouseBackground;
            break;
        case DynamicColorName::HighlightForegroundColor:
            colorPalette_.selectionForeground = defaultColorPalette_.selectionForeground;
            break;
        case DynamicColorName::HighlightBackgroundColor:
            colorPalette_.selectionBackground = defaultColorPalette_.selectionBackground;
            break;
    }
}

void Screen::setDynamicColor(DynamicColorName _name, RGBColor const& _value)
{
    switch (_name)
    {
        case DynamicColorName::DefaultForegroundColor:
            colorPalette_.defaultForeground = _value;
            break;
        case DynamicColorName::DefaultBackgroundColor:
            colorPalette_.defaultBackground = _value;
            break;
        case DynamicColorName::TextCursorColor:
            colorPalette_.cursor = _value;
            break;
        case DynamicColorName::MouseForegroundColor:
            colorPalette_.mouseForeground = _value;
            break;
        case DynamicColorName::MouseBackgroundColor:
            colorPalette_.mouseBackground = _value;
            break;
        case DynamicColorName::HighlightForegroundColor:
            colorPalette_.selectionForeground = _value;
            break;
        case DynamicColorName::HighlightBackgroundColor:
            colorPalette_.selectionBackground = _value;
            break;
    }
}

void Screen::dumpState()
{
    eventListener_.dumpState();
}

void Screen::dumpState(std::string const& _message) const
{
    auto const hline = [&]() {
        for_each(crispy::times(*size_.columns), [](auto) { cerr << '='; });
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
    cerr << screenshot([this](int _lineNo) -> string {
        //auto const absoluteLine = grid().toAbsoluteLine(_lineNo);
        return fmt::format("| {:>4}: {}", _lineNo, grid().lineAt(_lineNo).flags());
    });
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

    constexpr auto NumberOfColorRegistersItem = 1;
    constexpr auto SixelItem = 2;

    constexpr auto Success = 0;
    constexpr auto Failure = 3;

    switch (_item)
    {
        case Item::NumberOfColorRegisters:
            switch (_action)
            {
                case Action::Read:
                {
                    auto const value = imageColorPalette_->size();
                    reply("\033[?{};{};{}S", NumberOfColorRegistersItem, Success, value);
                    break;
                }
                case Action::ReadLimit:
                {
                    auto const value = imageColorPalette_->maxSize();
                    reply("\033[?{};{};{}S", NumberOfColorRegistersItem, Success, value);
                    break;
                }
                case Action::ResetToDefault:
                {
                    auto const value = 256; // TODO: read the configuration's default here
                    imageColorPalette_->setSize(value);
                    reply("\033[?{};{};{}S", NumberOfColorRegistersItem, Success, value);
                    break;
                }
                case Action::SetToValue:
                {
                    visit(overloaded{
                        [&](int _number) {
                            imageColorPalette_->setSize(_number);
                            reply("\033[?{};{};{}S", NumberOfColorRegistersItem, Success, _number);
                        },
                        [&](ImageSize) {
                            reply("\033[?{};{};{}S", NumberOfColorRegistersItem, Failure, 0);
                        },
                        [&](monostate) {
                            reply("\033[?{};{};{}S", NumberOfColorRegistersItem, Failure, 0);
                        },
                    }, _value);
                    break;
                }
            }
            break;

        case Item::SixelGraphicsGeometry:
            switch (_action)
            {
                case Action::Read:
                    reply("\033[?{};{};{};{}S", SixelItem, Success, maxImageSize_.width, maxImageSize_.height);
                    break;
                case Action::ReadLimit:
                    reply("\033[?{};{};{};{}S", SixelItem, Success, maxImageSizeLimit_.width, maxImageSizeLimit_.height);
                    break;
                case Action::ResetToDefault:
                    // The limit is the default at the same time.
                    maxImageSize_ = maxImageSizeLimit_;
                    break;
                case Action::SetToValue:
                    if (holds_alternative<ImageSize>(_value))
                    {
                        auto size = get<ImageSize>(_value);
                        size.width = min(size.width, maxImageSize_.width);
                        size.height = min(size.height, maxImageSize_.height);
                        maxImageSize_ = size;
                        // No reply.
                    }
                    break;
            }
            break;

        case Item::ReGISGraphicsGeometry: // Surely, we don't do ReGIS just yet. :-)
            break;
    }
}
// }}}

} // namespace terminal
