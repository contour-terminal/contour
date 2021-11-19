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
#include <crispy/size.h>
#include <crispy/times.h>
#include <crispy/utils.h>

#include <unicode/emoji_segmenter.h>
#include <unicode/word_segmenter.h>
#include <unicode/grapheme_segmenter.h>
#include <unicode/convert.h>
#include <unicode/utf8.h>

#include <range/v3/view/iota.hpp>

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
using std::max;
using std::min;
using std::monostate;
using std::next;
using std::nullopt;
using std::optional;
using std::ostringstream;
using std::pair;
using std::ref;
using std::rotate;
using std::prev;
using std::string;
using std::string_view;
using std::tuple;
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

        void write(char32_t v)
        {
            flush();
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
            if (sgr_.empty())
                return;

            auto const f = flush(sgr_);
            if (sgr_ != lastSGR_)
                writer_(f.data(), f.size());
            sgr_rewind();
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
                    flush();
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

        void setForegroundColor(Color _color)
        {
            // if (_color == currentForegroundColor_)
            //     return;

            currentForegroundColor_ = _color;
            switch (_color.type())
            {
                case ColorType::Default:
                    sgr_add(39);
                    break;
                case ColorType::Indexed:
                    if (static_cast<unsigned>(_color.index()) < 8)
                        sgr_add(30 + static_cast<unsigned>(_color.index()));
                    else
                    {
                        sgr_add(38);
                        sgr_add(5);
                        sgr_add(static_cast<unsigned>(_color.index()));
                    }
                    break;
                case ColorType::Bright:
                    sgr_add(90 + static_cast<unsigned>(getBrightColor(_color)));
                    break;
                case ColorType::RGB:
                    sgr_add(38);
                    sgr_add(2);
                    sgr_add(static_cast<unsigned>(_color.rgb().red));
                    sgr_add(static_cast<unsigned>(_color.rgb().green));
                    sgr_add(static_cast<unsigned>(_color.rgb().blue));
                case ColorType::Undefined:
                    break;
            }
        }

        void setBackgroundColor(Color _color)
        {
            // if (_color == currentBackgroundColor_)
            //     return;

            currentBackgroundColor_ = _color;
            switch (_color.type())
            {
                case ColorType::Default:
                    sgr_add(49);
                    break;
                case ColorType::Indexed:
                    if (static_cast<unsigned>(_color.index()) < 8)
                        sgr_add(40 + static_cast<unsigned>(_color.index()));
                    else
                    {
                        sgr_add(48);
                        sgr_add(5);
                        sgr_add(static_cast<unsigned>(_color.index()));
                    }
                    break;
                case ColorType::Bright:
                    sgr_add(100 + static_cast<unsigned>(getBrightColor(_color)));
                    break;
                case ColorType::RGB:
                    sgr_add(48);
                    sgr_add(2);
                    sgr_add(static_cast<unsigned>(_color.rgb().red));
                    sgr_add(static_cast<unsigned>(_color.rgb().green));
                    sgr_add(static_cast<unsigned>(_color.rgb().blue));
                case ColorType::Undefined:
                    break;
            }
        }

      private:
        Writer writer_;
        std::vector<unsigned> sgr_;
        std::stringstream sstr;
        std::vector<unsigned> lastSGR_;
        Color currentForegroundColor_ = DefaultColor();
        Color currentUnderlineColor_ = DefaultColor();
        Color currentBackgroundColor_ = DefaultColor();
    };

    array<Grid<Cell>, 2> emptyGrids(PageSize _size, bool _reflowOnResize, LineCount _maxHistoryLineCount)
    {
        return array<Grid<Cell>, 2>{
            Grid<Cell>(_size, _reflowOnResize, _maxHistoryLineCount),
            Grid<Cell>(_size, false, LineCount(0))
        };
    }
}
// }}}

template <typename EventListener>
Screen<EventListener>::Screen(PageSize _size,
                              EventListener& _eventListener,
                              bool _logRaw,
                              bool _logTrace,
                              LineCount _maxHistoryLineCount,
                              ImageSize _maxImageSize,
                              int _maxImageColorRegisters,
                              bool _sixelCursorConformance,
                              ColorPalette _colorPalette,
                              bool _allowReflowOnResize
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
        [this](Image const* _image) { eventListener_.discardImage(*_image); }
    },
    imageFragments_{
        80lu * 25lu * 8lu, // TODO: make this resource limit configurable
    },
    sequencer_{
        *this,
        _maxImageSize,
        colorPalette_.defaultBackground,
        imageColorPalette_
    },
    parser_{ ref(sequencer_) },
    pageSize_{ _size },
    sixelCursorConformance_{ _sixelCursorConformance },
    margin_{
        Margin::Vertical{{}, pageSize_.lines.as<LineOffset>() - LineOffset(1)},
        Margin::Horizontal{{}, pageSize_.columns.as<ColumnOffset>() - ColumnOffset(1)}
    },
    allowReflowOnResize_{ _allowReflowOnResize },
    grids_{ emptyGrids(pageSize_, _allowReflowOnResize, _maxHistoryLineCount) },
    activeGrid_{ &primaryGrid() },
    cursor_{},
    lastCursorPosition_{},
    hyperlinks_{ HyperlinkCache{1024} },
    respondToTCapQuery_{false}
{
#if 0
    resetHard();
#else
    setMode(DECMode::AutoWrap, true);
    setMode(DECMode::TextReflow, true);
#endif
}

template <typename T>
unsigned Screen<T>::numericCapability(capabilities::Code _cap) const
{
    using namespace capabilities::literals;

    switch (_cap)
    {
        case "li"_tcap: return pageSize_.lines.as<unsigned>();
        case "co"_tcap: return pageSize_.columns.as<unsigned>();
        case "it"_tcap: return tabWidth_.as<unsigned>();
        default:
            return StaticDatabase::numericCapability(_cap);
    }
}

template <typename T>
void Screen<T>::setMaxHistoryLineCount(LineCount _maxHistoryLineCount)
{
    primaryGrid().setMaxHistoryLineCount(_maxHistoryLineCount);
}

template <typename T>
void Screen<T>::resizeColumns(ColumnCount _newColumnCount, bool _clear)
{
    // DECCOLM / DECSCPP
    if (_clear)
    {
        // Sets the left, right, top and bottom scrolling margins to their default positions.
        setTopBottomMargin({}, pageSize_.lines.as<LineOffset>() - LineOffset(1));    // DECSTBM
        setLeftRightMargin({}, pageSize_.columns.as<ColumnOffset>() - ColumnOffset(1));  // DECRLM

        // Erases all data in page memory
        clearScreen();
    }

    // resets vertical split screen mode (DECLRMM) to unavailable
    setMode(DECMode::LeftRightMargin, false); // DECSLRM

    // Pre-resize in case the event callback right after is not actually resizing the window
    // (e.g. either by choice or because the window manager does not allow that, such as tiling WMs).
    auto const newSize = PageSize{pageSize_.lines, _newColumnCount};
    resize(newSize);

    eventListener_.resizeWindow(newSize);
}

template <typename T>
void Screen<T>::resize(PageSize _newSize)
{
    // NOTE: This will only resize the currently active buffer.
    // Any other buffer will be resized when it is switched to.

    auto const oldCursorPos = cursor_.position;

    cursor_.position = activeGrid_->resize(_newSize, oldCursorPos, wrapPending_);
    pageSize_ = _newSize;

    // Reset margin to their default.
    margin_ = Margin{
        Margin::Vertical{{}, _newSize.lines.as<LineOffset>() - 1},
        Margin::Horizontal{{}, _newSize.columns.as<ColumnOffset>() - 1}
    };

    applyPageSizeToCurrentBuffer();
}

template <typename T>
void Screen<T>::applyPageSizeToCurrentBuffer()
{
    // Ensure correct screen buffer size for the buffer we've just switched to.
    cursor_.position = activeGrid_->resize(pageSize_, cursor_.position, wrapPending_);
    cursor_.position = clampCoordinate(cursor_.position);

    // update last-cursor position & iterators
    lastCursorPosition_ = cursor_.position;
    lastCursorPosition_ = clampCoordinate(lastCursorPosition_);

    // truncating tabs
    while (!tabs_.empty() && tabs_.back() >= pageSize_.columns.as<ColumnOffset>())
        tabs_.pop_back();

    // TODO: find out what to do with DECOM mode. Reset it to?
#if 0
    dumpState("after resize", std::cout);
    fmt::print("applyPageSizeToCurrentBuffer: cursor pos before: {} after: {}\n", oldCursorPos, cursor_.position);
#endif

    verifyState();
}

template <typename T>
void Screen<T>::verifyState() const
{
#if !defined(NDEBUG)
    Expects(activeGrid_->pageSize() == pageSize_);
    Expects(*cursor_.position.column < *pageSize_.columns);
    Expects(*cursor_.position.line < *pageSize_.lines);
    Expects(tabs_.empty() || tabs_.back() < pageSize_.columns.as<ColumnOffset>());

    if (*pageSize_.lines != static_cast<int>(grid().mainPage().size()))
        fail(fmt::format("Line count mismatch. Actual line count {} but should be {}.",
                    grid().mainPage().size(), pageSize_.lines));

    // verify cursor positions
    [[maybe_unused]] auto const clampedCursorPos = clampToScreen(cursor_.position);
    if (cursor_.position != clampedCursorPos)
        fail(fmt::format("Cursor {} does not match clamp to screen {}.", cursor_, clampedCursorPos));
    // FIXME: the above triggers on tmux vertical screen split (cursor.column off-by-one)
#endif
}

template <typename T>
void Screen<T>::fail(std::string const& _message) const
{
    dumpState(_message, std::cerr);
    abort();
}

template <typename T>
void Screen<T>::write(std::string_view _data)
{
    if (_data.empty())
        return;

#if defined(LIBTERMINAL_LOG_RAW)
    if (ScreenRawOutputLog)
        LOGSTORE(ScreenRawOutputLog)("Received bytes: \"{}\"", escape(_data));
#endif

    parser_.parseFragment(_data);

    if (modes_.enabled(DECMode::BatchedRendering))
        return;

    eventListener_.screenUpdated();
}

template <typename T>
void Screen<T>::write(std::u32string_view _data)
{
#if defined(LIBTERMINAL_LOG_RAW)
    if (ScreenRawOutputLog)
        LOGSTORE(ScreenRawOutputLog)("Received bytes: \"{}\"", escape(unicode::convert_to<char>(_data)));
#endif

    parser_.parseFragment(_data);

    if (modes_.enabled(DECMode::BatchedRendering))
        return;

    eventListener_.screenUpdated();
}

template <typename T>
void Screen<T>::writeText(string_view _chars)
{
    //#define LIBTERMINAL_BULK_TEXT_OPTIMIZATION 1

    #if defined(LIBTERMINAL_BULK_TEXT_OPTIMIZATION)
    if (margin_ == pageSize_)
    {
        #if defined(LIBTERMINAL_LOG_TRACE)
        if (VTParserTraceLog)
            LOGSTORE(VTParserTraceLog)("text: \"{}\"", _chars);
        #endif

        // XXX
        // Case B) AutoWrap disabled
        //   1. write (charsLeft - 1) chars, then last char of range
        // Case A) AutoWrap enabled
        //   1. write charsCount chars
        //   2. reset remaining columns in last line with cursor.SGR
        //   3. scroll up accordingly
        //   4. udpate cursor position
        //   5. if line is wrappable, then update consecutive line flags with Wrapped

        // TODO: make sure handle the grapheme cluster case?
        // auto const lastChar = sequencer_.precedingGraphicCharacter();
        // auto const isAsciiBreakable = lastChar < 128 && _chars.front() < 128; // NB: This is an optimization for US-ASCII text versus grapheme cluster segmentation.

        auto constexpr ASCII_Width = 1;
        if (isModeEnabled(DECMode::AutoWrap))
        {
            // Case A)
            if (wrapPending_)
                linefeed();

            auto const marginColumnCount = margin_.horizontal.length();
            auto const writeCharsToLine =
                [this, ASCII_Width, marginColumnCount]
                (string_view text,
                 LineOffset lineOffset,
                 ColumnOffset columnOffset) noexcept
                -> size_t
                {
                    //fmt::print("writeCharsToLine({}:{}): \"{}\"\n", lineOffset, columnOffset, text);
                    auto const columnsAvailable = marginColumnCount - *columnOffset;
                    auto const cutoff = std::min(columnsAvailable.as<size_t>(), text.size());
                    auto const charsToWrite = text.substr(0, cutoff);
                    Line<Cell>& line = grid().lineAt(lineOffset);
                    line.fill(columnOffset, cursor_.graphicsRendition, charsToWrite);
                    return cutoff;
                };

            if (*cursor_.position.column + static_cast<int>(_chars.size()) < *marginColumnCount)
            {
                // fill line partially
                writeCharsToLine(_chars, cursor_.position.line, cursor_.position.column);
                cursor_.position.column += ColumnOffset::cast_from(_chars.size());
            }
            else if ((cursor_.position.column + static_cast<int>(_chars.size())).as<ColumnCount>() == marginColumnCount)
            {
                // fill line up to the right margin
                writeCharsToLine(_chars, cursor_.position.line, cursor_.position.column);
                cursor_.position.column = boxed_cast<ColumnOffset>(marginColumnCount - 1);
                wrapPending_ = true;
            }
            else
            {
                // fill more than one line

                // TODO: Ensure Wrappable|Wrapped line flag is set accordingly.
                auto const n = writeCharsToLine(_chars, cursor_.position.line, cursor_.position.column);
                _chars.remove_prefix(n);

                bool const lineWrappable = currentLine().wrappable();
                linefeed(margin_.horizontal.from);
                currentLine().setFlag(LineFlags::Wrappable | LineFlags::Wrapped, lineWrappable);

                if (!_chars.empty())
                {
                    // middle lines
                    while (_chars.size() > marginColumnCount.as<size_t>())
                    {
                        writeCharsToLine(_chars, cursor_.position.line, ColumnOffset(0));
                        _chars.remove_prefix(marginColumnCount.as<size_t>());
                        linefeed(margin_.horizontal.from);
                        currentLine().setFlag(LineFlags::Wrappable | LineFlags::Wrapped, lineWrappable);
                    }

                    // tail line
                    writeCharsToLine(_chars, cursor_.position.line, ColumnOffset(0));
                }

                if (_chars.size() == marginColumnCount.as<size_t>())
                {
                    wrapPending_ = true;
                    cursor_.position.column = marginColumnCount.as<ColumnOffset>() - 1;
                }
                else
                {
                    cursor_.position.column = ColumnOffset::cast_from(_chars.size());
                    // reset remaining columns in last line with cursor.SGR
                    Line<Cell>& line = grid().lineAt(cursor_.position.line);
                    line.fill(cursor_.position.column, cursor_.graphicsRendition, {});
                }
            }
        }
        else
        {
            // Case B - AutoWrap disabled
            auto const topLineColumnsAvailable =
                pageSize_.columns - cursor_.position.column.as<ColumnCount>();
            char const* s = _chars.data();
            auto const n = min(_chars.size(), topLineColumnsAvailable.as<size_t>());
            auto const* e = s + n;
            auto t = &useCurrentCell();
            for (; s != e; s++, t++)
                t->write(cursor_.graphicsRendition, static_cast<char32_t>(*s), ASCII_Width, cursor_.hyperlink);
            if (s + 1 != e)
                (t - 1)->setCharacter(_chars.back(), 1);
            cursor_.position.column =
                min(cursor_.position.column + ColumnOffset::cast_from(n),
                    pageSize_.columns.as<ColumnOffset>() - 1);
        }

        // TODO: Call this but with range range of point.
        // markCellDirty(oldCursorPos, newCursorPos);
        // XXX: But even if we keep it but enable the setReportDamage(bool),
        //      then this should still be cheap as it's only invoked when something
        //      is actually selected.
        // eventListener_.markRegionDirty(
        //     cursor_.position.line,
        //     cursor_.position.column
        // );

        sequencer_.resetInstructionCounter();
        return;
    }
    #endif

    for (char const ch: _chars)
        writeText(static_cast<char32_t>(ch));
}

template <typename T>
void Screen<T>::writeText(char32_t _char)
{
    #if defined(LIBTERMINAL_LOG_TRACE)
    if (VTParserTraceLog)
        LOGSTORE(VTParserTraceLog)("text: \"{}\"", unicode::convert_to<char>(_char));
    #endif

    if (wrapPending_ && cursor_.autoWrap) // && !isModeEnabled(DECMode::TextReflow))
    {
        bool const lineWrappable = currentLine().wrappable();
        linefeed(margin_.horizontal.from);
        if (lineWrappable)
            currentLine().setFlag(LineFlags::Wrappable | LineFlags::Wrapped, true);
    }

    char32_t const codepoint = cursor_.charsets.map(_char);

    auto const lastChar = sequencer_.precedingGraphicCharacter();
    auto const isAsciiBreakable = lastChar < 128 && codepoint < 128; // NB: This is an optimization for US-ASCII text versus grapheme cluster segmentation.

    if (!lastChar || isAsciiBreakable || unicode::grapheme_segmenter::breakable(lastChar, codepoint))
    {
        writeCharToCurrentAndAdvance(codepoint);
    }
    else
    {
        auto const extendedWidth = usePreviousCell().appendCharacter(codepoint);
        if (extendedWidth > 0)
            clearAndAdvance(extendedWidth);
        eventListener_.markCellDirty(lastCursorPosition_);
    }

    sequencer_.resetInstructionCounter();
}

template <typename T>
void Screen<T>::writeCharToCurrentAndAdvance(char32_t _character) noexcept
{
    Line<Cell>& line = grid().lineAt(cursor_.position.line);
    Cell& cell = line.useCellAt(cursor_.position.column);

    #if defined(LINE_AVOID_CELL_RESET)
    bool const consecutiveTextWrite = sequencer_.instructionCounter() == 1;
    if (!consecutiveTextWrite)
        cell.reset();
    #endif

    cell.write(cursor_.graphicsRendition,
               _character, unicode::width(_character),
               cursor_.hyperlink);

    lastCursorPosition_ = cursor_.position;

#if 1
    clearAndAdvance(cell.width());
#else
    bool const cursorInsideMargin = isModeEnabled(DECMode::LeftRightMargin) && isCursorInsideMargins();
    auto const cellsAvailable = cursorInsideMargin
        ? *(margin_.horizontal.to - cursor_.position.column) - 1
        : *pageSize_.columns - *cursor_.position.column - 1;

    auto const n = min(cell.width(), cellsAvailable);

    if (n == cell.width())
    {
        assert(n > 0);
        cursor_.position.column++;
        for (int i = 1; i < n; ++i)
        {
            currentCell().reset(cursor_.graphicsRendition, cursor_.hyperlink);
            cursor_.position.column++;
        }
    }
    else if (cursor_.autoWrap)
        wrapPending_ = true;
#endif

    // TODO: maybe move selector API up? So we can make this call conditional,
    //       and only call it when something is selected?
    //       Alternatively we could add a boolean to make this callback
    //       conditional, something like: setReportDamage(bool);
    //       The latter is probably the easiest.
    eventListener_.markCellDirty(cursor_.position);
}

template <typename T>
void Screen<T>::clearAndAdvance(int _offset) noexcept
{
    if (_offset == 0)
        return;

    bool const cursorInsideMargin = isModeEnabled(DECMode::LeftRightMargin) && isCursorInsideMargins();
    auto const cellsAvailable = cursorInsideMargin
        ? *(margin_.horizontal.to - cursor_.position.column) - 1
        : *pageSize_.columns - *cursor_.position.column - 1;
    auto const n = min(_offset, cellsAvailable);

    if (n == _offset)
    {
        cursor_.position.column++;
        for (int i = 1; i < n; ++i)
        {
            useCurrentCell().reset(cursor_.graphicsRendition, cursor_.hyperlink);
            cursor_.position.column++;
        }
    }
    else if (cursor_.autoWrap)
    {
        wrapPending_ = true;
    }
}

template <typename T>
std::string Screen<T>::screenshot(function<string(LineOffset)> const& _postLine) const
{
    auto result = std::stringstream{};
    auto writer = VTWriter(result);

    for (int const line: ranges::views::iota(-unbox<int>(historyLineCount()), *pageSize_.lines))
    {
        for (int const col: ranges::views::iota(0, *pageSize_.columns))
        {
            Cell const& cell = at(LineOffset(line), ColumnOffset(col));

            if (cell.styles() & CellFlags::Bold)
                writer.sgr_add(GraphicsRendition::Bold);
            else
                writer.sgr_add(GraphicsRendition::Normal);

            // TODO: other styles (such as underline, ...)?

            writer.setForegroundColor(cell.foregroundColor());
            writer.setBackgroundColor(cell.backgroundColor());

            if (!cell.codepointCount())
                writer.write(U' ');
            else
                writer.write(cell.toUtf8());
        }
        writer.sgr_add(GraphicsRendition::Reset);

        if (_postLine)
            writer.write(_postLine(LineOffset(line)));

        writer.write('\r');
        writer.write('\n');
    }

    return result.str();
}

template <typename T>
optional<LineOffset> Screen<T>::findMarkerUpwards(LineOffset _startLine) const
{
    // XXX _startLine is an absolute history line coordinate
    if (isAlternateScreen())
        return nullopt;
    if (*_startLine <= -*historyLineCount())
        return nullopt;

    _startLine = min(_startLine, boxed_cast<LineOffset>(pageSize_.lines - 1));

    for (LineOffset i = _startLine - 1; i >= -boxed_cast<LineOffset>(historyLineCount()); --i)
        if (grid().lineAt(i).marked())
            return {i};

    return nullopt;
}

template <typename T>
optional<LineOffset> Screen<T>::findMarkerDownwards(LineOffset _lineOffset) const
{
    if (!isPrimaryScreen())
        return nullopt;

    auto const top =
        std::clamp(_lineOffset, -boxed_cast<LineOffset>(historyLineCount()),
                                +boxed_cast<LineOffset>(pageSize_.lines) - 1);

    auto const bottom = LineOffset(0);

    for (LineOffset i = top + 1; i <= bottom; ++i)
        if (grid().lineAt(i).marked())
            return {i};

    return nullopt;
}

// {{{ tabs related
template <typename T>
void Screen<T>::clearAllTabs()
{
    tabs_.clear();
}

template <typename T>
void Screen<T>::clearTabUnderCursor()
{
    // populate tabs vector in case of default tabWidth is used (until now).
    if (tabs_.empty() && *tabWidth_ != 0)
        for (auto column = tabWidth_.as<ColumnOffset>(); column < pageSize_.columns.as<ColumnOffset>(); column += tabWidth_.as<ColumnOffset>())
            tabs_.emplace_back(column - 1);

    // erase the specific tab underneath
    for (auto i = begin(tabs_); i != end(tabs_); ++i)
    {
        if (*i == realCursorPosition().column)
        {
            tabs_.erase(i);
            break;
        }
    }
}

template <typename T>
void Screen<T>::setTabUnderCursor()
{
    tabs_.emplace_back(realCursorPosition().column);
    sort(begin(tabs_), end(tabs_));
}
// }}}

// {{{ others
template <typename T>
void Screen<T>::saveCursor()
{
    // https://vt100.net/docs/vt510-rm/DECSC.html
    savedCursor_ = cursor_;
}

template <typename T>
void Screen<T>::restoreCursor()
{
    // https://vt100.net/docs/vt510-rm/DECRC.html
    restoreCursor(savedCursor_);

    setMode(DECMode::AutoWrap, savedCursor_.autoWrap);
    setMode(DECMode::Origin, savedCursor_.originMode);
}

template <typename T>
void Screen<T>::restoreCursor(Cursor const& _savedCursor)
{
    wrapPending_ = false;
    cursor_ = _savedCursor;
    cursor_.position = clampCoordinate(_savedCursor.position);
    verifyState();
}

template <typename T>
void Screen<T>::resetSoft()
{
    // https://vt100.net/docs/vt510-rm/DECSTR.html
    setMode(DECMode::BatchedRendering, false);
    setMode(DECMode::TextReflow, allowReflowOnResize_);
    setGraphicsRendition(GraphicsRendition::Reset); // SGR
    savedCursor_.position = {}; // DECSC (Save cursor state)
    setMode(DECMode::VisibleCursor, true); // DECTCEM (Text cursor enable)
    setMode(DECMode::Origin, false); // DECOM
    setMode(AnsiMode::KeyboardAction, false); // KAM
    setMode(DECMode::AutoWrap, false); // DECAWM
    setMode(AnsiMode::Insert, false); // IRM
    setMode(DECMode::UseApplicationCursorKeys, false); // DECCKM (Cursor keys)
    setTopBottomMargin({}, pageSize_.lines.as<LineOffset>() - LineOffset(1)); // DECSTBM
    setLeftRightMargin({}, pageSize_.columns.as<ColumnOffset>() - ColumnOffset(1)); // DECRLM

    cursor_.hyperlink = {};
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

template <typename T>
void Screen<T>::resetHard()
{
    setBuffer(ScreenType::Main);

    modes_ = Modes{};
    setMode(DECMode::AutoWrap, true);
    setMode(DECMode::TextReflow, allowReflowOnResize_);

    clearAllTabs();

    for (auto& grid: grids_)
        grid.reset();

    cursor_ = {};

    lastCursorPosition_ = cursor_.position;

    margin_ = Margin{
        Margin::Vertical{{}, pageSize_.lines.as<LineOffset>() - 1},
        Margin::Horizontal{{}, pageSize_.columns.as<ColumnOffset>() - 1}
    };

    colorPalette_ = defaultColorPalette_;

    verifyState();

    eventListener_.hardReset();
}

template <typename T>
void Screen<T>::moveCursorTo(LineOffset _line, ColumnOffset _column)
{
    auto const [line, column] = [&]()
    {
        if (!cursor_.originMode)
            return pair{_line, _column};
        else
            return pair{_line + margin_.vertical.from, _column + margin_.horizontal.from};
    }();

    wrapPending_ = false;
    cursor_.position.line = clampedLine(line);
    cursor_.position.column = clampedColumn(column);
}

template <typename T>
void Screen<T>::setBuffer(ScreenType _type)
{
    if (bufferType() == _type)
        return;

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

    // Reset wrapPending-flag when switching buffer.
    wrapPending_ = false;

    // Reset last-cursor position.
    lastCursorPosition_ = cursor_.position;

    // Ensure correct screen buffer size for the buffer we've just switched to.
    applyPageSizeToCurrentBuffer();

    eventListener_.bufferChanged(_type);
}

template <typename T>
void Screen<T>::linefeed(ColumnOffset _newColumn)
{
    wrapPending_ = false;
    cursor_.position.column = _newColumn;

    if (*realCursorPosition().line == *margin_.vertical.to)
    {
        // TODO(perf) if we know that we text is following this LF
        // (i.e. parser state will be ground state),
        // then invoke scrollUpUninitialized instead
        // and make sure the subsequent text write will
        // possibly also reset remaining grid cells in that line
        // if the incoming text did not write to the full line
        scrollUp(LineCount(1), {}, margin_);
    }
    else
    {
        // using moveCursorTo() would embrace code reusage,
        // but due to the fact that it's fully recalculating iterators,
        // it may be faster to just incrementally update them.
        // moveCursorTo({logicalCursorPosition().line + 1, margin_.horizontal.from});
        cursor_.position.line++;
    }
}

template <typename T>
void Screen<T>::scrollUp(LineCount n, GraphicsAttributes sgr, Margin margin)
{
    auto const scrollCount = grid().scrollUp(n, sgr, margin);
    eventListener_.onBufferScrolled(scrollCount);
}

template <typename T>
void Screen<T>::scrollUp(LineCount _n, Margin _margin)
{
    scrollUp(_n, cursor().graphicsRendition, _margin);
}

template <typename T>
void Screen<T>::scrollDown(LineCount _n, Margin _margin)
{
    grid().scrollDown(_n, cursor().graphicsRendition, _margin);
}

template <typename T>
void Screen<T>::setCurrentColumn(ColumnOffset _n)
{
    auto const col = cursor_.originMode
        ? margin_.horizontal.from + _n
        : _n;
    auto const clampedCol = min(col, boxed_cast<ColumnOffset>(pageSize_.columns) - 1);
    wrapPending_ = false;
    cursor_.position.column = clampedCol;
}

template <typename T>
string Screen<T>::renderMainPageText() const
{
    return grid().renderMainPageText();
}
// }}}

// {{{ ops
template <typename T>
void Screen<T>::linefeed()
{
    if (isModeEnabled(AnsiMode::AutomaticNewLine))
        linefeed(margin_.horizontal.from);
    else
        linefeed(realCursorPosition().column);
}

template <typename T>
void Screen<T>::backspace()
{
    if (cursor_.position.column.value)
        cursor_.position.column--;
}

template <typename T>
void Screen<T>::deviceStatusReport()
{
    reply("\033[0n");
}

template <typename T>
void Screen<T>::reportCursorPosition()
{
    reply("\033[{};{}R", logicalCursorPosition().line + 1, logicalCursorPosition().column + 1);
}

template <typename T>
void Screen<T>::reportExtendedCursorPosition()
{
    auto const pageNum = 1;
    reply("\033[{};{};{}R", logicalCursorPosition().line + 1, logicalCursorPosition().column + 1, pageNum);
}

template <typename T>
void Screen<T>::selectConformanceLevel(VTType _level)
{
    // Don't enforce the selected conformance level, just remember it.
    terminalId_ = _level;
}

template <typename T>
void Screen<T>::sendDeviceAttributes()
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

template <typename T>
void Screen<T>::sendTerminalId()
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

template <typename T>
void Screen<T>::clearToEndOfScreen()
{
    clearToEndOfLine();

    for (auto const lineOffset: ranges::views::iota(cursor_.position.line.as<int>() + 1, pageSize_.lines.as<int>()))
    {
        Line<Cell>& line = grid().lineAt(LineOffset::cast_from(lineOffset));
        line.reset(grid().defaultLineFlags(), cursor_.graphicsRendition);
    }
}

template <typename T>
void Screen<T>::clearToBeginOfScreen()
{
    clearToBeginOfLine();

    for (auto const lineOffset: ranges::views::iota(0, *cursor_.position.line))
    {
        Line<Cell>& line = grid().lineAt(LineOffset::cast_from(lineOffset));
        line.reset(grid().defaultLineFlags(), cursor_.graphicsRendition);
    }
}

template <typename T>
void Screen<T>::clearScreen()
{
    // Instead of *just* clearing the screen, and thus, losing potential important content,
    // we scroll up by RowCount number of lines, so move it all into history, so the user can scroll
    // up in case the content is still needed.
    scrollUp(pageSize_.lines);
}

template <typename T>
void Screen<T>::clearScrollbackBuffer()
{
    primaryGrid().clearHistory();
    alternateGrid().clearHistory();
    eventListener_.scrollbackBufferCleared();
}

template <typename T>
void Screen<T>::eraseCharacters(ColumnCount _n)
{
    // Spec: https://vt100.net/docs/vt510-rm/ECH.html
    // It's not clear from the spec how to perform erase when inside margin and number of chars to be erased would go outside margins.
    // TODO: See what xterm does ;-)

    // erase characters from current colum to the right
    auto const columnsAvailable = pageSize_.columns - boxed_cast<ColumnCount>(realCursorPosition().column);
    auto const n = unbox<long>(clamp(_n, ColumnCount(1), columnsAvailable));

    auto& line = grid().lineAt(cursor_.position.line);
    for (int i = 0; i < n; ++i)
        line.useCellAt(cursor_.position.column + i).reset(cursor_.graphicsRendition);
}

template <typename T>
void Screen<T>::clearToEndOfLine()
{
    Cell* i = &at(cursor_.position);
    Cell* e = i + (pageSize_.columns.as<int>() - cursor_.position.column.as<int>());
    while (i != e)
    {
        i->reset(cursor_.graphicsRendition);
        ++i;
    }

    auto const line = cursor_.position.line;
    auto const left = cursor_.position.column;
    auto const right = boxed_cast<ColumnOffset>(pageSize_.columns - 1);
    auto const area = Rect{Top(*line), Left(*left), Bottom(*line), Right(*right)};
    eventListener_.markRegionDirty(area);
}

template <typename T>
void Screen<T>::clearToBeginOfLine()
{
    Cell* i = &at(cursor_.position.line, ColumnOffset(0));
    Cell* e = i + cursor_.position.column.as<int>() + 1;
    while (i != e)
    {
        i->reset(cursor_.graphicsRendition);
        ++i;
    }
}

template <typename T>
void Screen<T>::clearLine()
{
    Cell* i = &at(cursor_.position.line, ColumnOffset(0));
    Cell* e = i + pageSize_.columns.as<int>();
    while (i != e)
    {
        i->reset(cursor_.graphicsRendition);
        ++i;
    }

    auto const line = cursor_.position.line;
    auto const left = ColumnOffset(0);
    auto const right = boxed_cast<ColumnOffset>(pageSize_.columns - 1);
    auto const area = Rect{Top(*line), Left(*left), Bottom(*line), Right(*right)};
    eventListener_.markRegionDirty(area);
}

template <typename T>
void Screen<T>::moveCursorToNextLine(LineCount _n)
{
    moveCursorTo(logicalCursorPosition().line + _n.as<LineOffset>(), ColumnOffset(0));
}

template <typename T>
void Screen<T>::moveCursorToPrevLine(LineCount _n)
{
    auto const n = min(_n.as<LineOffset>(), logicalCursorPosition().line);
    moveCursorTo(logicalCursorPosition().line - n, ColumnOffset(0));
}

template <typename T>
void Screen<T>::insertCharacters(ColumnCount _n)
{
    if (isCursorInsideMargins())
        insertChars(realCursorPosition().line, _n);
}

/// Inserts @p _n characters at given line @p _lineNo.
template <typename T>
void Screen<T>::insertChars(LineOffset _lineNo, ColumnCount _n)
{
    auto const n = min(
        *_n,
        *margin_.horizontal.to - *logicalCursorPosition().column + 1
    );

    auto column0 = &at(_lineNo, realCursorPosition().column);
    auto column1 = &at(_lineNo, margin_.horizontal.to - ColumnOffset::cast_from(n - 1));
    auto column2 = &at(_lineNo, margin_.horizontal.to + ColumnOffset(1));

    rotate(
        column0,
        column1,
        column2
    );

    for (Cell& cell: grid().lineAt(_lineNo).useRange(cursor_.position.column.as<ColumnOffset>(),
                                                     ColumnCount::cast_from(n)))
    {
        cell.write(cursor_.graphicsRendition, L' ', 1);
    }

    grid().lineAt(_lineNo).markUsedFirst(_n);
}

template <typename T>
void Screen<T>::insertLines(LineCount _n)
{
    if (isCursorInsideMargins())
    {
        scrollDown(
            _n,
            Margin{
                Margin::Vertical{cursor_.position.line, margin_.vertical.to},
                margin_.horizontal
            }
        );
    }
}

template <typename T>
void Screen<T>::insertColumns(ColumnCount _n)
{
    if (isCursorInsideMargins())
        for (auto lineNo = margin_.vertical.from; lineNo <= margin_.vertical.to; ++lineNo)
            insertChars(lineNo, _n);
}

template <typename T>
void Screen<T>::copyArea(Rect _sourceArea, int _page,
                         Coordinate _targetTopLeft, int _targetPage)
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
            return std::tuple{*_sourceArea.right - *_sourceArea.left, -1, -1};
        else
            return std::tuple{0, +1, *_sourceArea.right - *_sourceArea.left + 1};
    }();

    auto const [y0, yInc, yEnd] = [&]() {
        if (*_targetTopLeft.line > *_sourceArea.top) // moving down
            return std::tuple{*_sourceArea.bottom - *_sourceArea.top, -1, -1};
        else
            return std::tuple{0, +1, *_sourceArea.bottom - *_sourceArea.top + 1};
    }();

    for (auto y = y0; y != yEnd; y += yInc)
    {
        for (auto x = x0; x != xEnd; x += xInc)
        {
            Cell const& sourceCell = at(LineOffset::cast_from(*_sourceArea.top + y), ColumnOffset::cast_from(_sourceArea.left + x));
            Cell& targetCell = at(LineOffset::cast_from(_targetTopLeft.line + y), ColumnOffset::cast_from(_targetTopLeft.column + x));
            targetCell = sourceCell;
        }
    }
}

template <typename T>
void Screen<T>::eraseArea(int _top, int _left, int _bottom, int _right)
{
    assert(_right <= unbox<int>(pageSize_.columns));
    assert(_bottom <= unbox<int>(pageSize_.lines));

    if (_top > _bottom || _left > _right)
        return;

    for (int y = _top; y <= _bottom; ++y)
    {
        for (Cell& cell: grid().lineAt(LineOffset::cast_from(y)).useRange(ColumnOffset(_left), ColumnCount(_right - _left + 1)))
        {
            cell.write(cursor_.graphicsRendition, L' ', 1);
        }
    }
}

template <typename T>
void Screen<T>::fillArea(char32_t _ch, int _top, int _left, int _bottom, int _right)
{
    // "Pch can be any value from 32 to 126 or from 160 to 255."
    if (!(32 <= _ch && _ch <= 126) && !(160 <= _ch && _ch <= 255))
        return;

    auto const w = unicode::width(_ch);
    for (int y = _top; y <= _bottom; ++y)
    {
        for (Cell& cell: grid().lineAt(LineOffset::cast_from(y))
                               .useRange(ColumnOffset::cast_from(_left),
                                         ColumnCount::cast_from(_right - _left + 1)))
        {
            cell.write(cursor().graphicsRendition, _ch, w);
        }
    }
}

template <typename T>
void Screen<T>::deleteLines(LineCount _n)
{
    if (isCursorInsideMargins())
    {
        scrollUp(
            _n,
            Margin{
                Margin::Vertical{cursor_.position.line, margin_.vertical.to},
                margin_.horizontal
            }
        );
    }
}

template <typename T>
void Screen<T>::deleteCharacters(ColumnCount _n)
{
    if (isCursorInsideMargins() && *_n != 0)
        deleteChars(realCursorPosition().line, realCursorPosition().column, _n);
}

template <typename T>
void Screen<T>::deleteChars(LineOffset _line, ColumnOffset _column, ColumnCount _n)
{
    auto& line = grid().lineAt(_line);
    auto lineBuffer = line.cells();

    Cell* left = const_cast<Cell*>(lineBuffer.data() + _column.as<size_t>());
    Cell* right = const_cast<Cell*>(lineBuffer.data() + *margin_.horizontal.to + 1);
    long const n = min(_n.as<long>(), static_cast<long>(distance(left, right)));
    Cell* mid = left + n;

    rotate(left, mid, right);

    for (Cell& cell: gsl::make_span(right - n, right))
    {
        cell.write(cursor_.graphicsRendition, L' ', 1);
    }

    line.markUsedFirst(ColumnCount::cast_from(*margin_.horizontal.to + 1));
}

template <typename T>
void Screen<T>::deleteColumns(ColumnCount _n)
{
    if (isCursorInsideMargins())
        for (auto lineNo = margin_.vertical.from; lineNo <= margin_.vertical.to; ++lineNo)
            deleteChars(lineNo, realCursorPosition().column, _n);
}

template <typename T>
void Screen<T>::horizontalTabClear(HorizontalTabClear _which)
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

template <typename T>
void Screen<T>::horizontalTabSet()
{
    setTabUnderCursor();
}

template <typename T>
void Screen<T>::setCurrentWorkingDirectory(string const& _url)
{
    currentWorkingDirectory_ = _url;
}

template <typename T>
void Screen<T>::hyperlink(string _id, string _uri)
{
    if (_uri.empty())
        cursor_.hyperlink = {};
    else if (!_id.empty())
        cursor_.hyperlink = hyperlinks_.hyperlinkIdByUserId(_id);
    else
    {
        cursor_.hyperlink = hyperlinks_.nextHyperlinkId++;
        hyperlinks_.cache.emplace(
            cursor_.hyperlink,
            make_shared<HyperlinkInfo>(HyperlinkInfo{move(_id), move(_uri)})
        );
    }
    // TODO:
    // Care about eviction.
    // Move hyperlink store into ScreenBuffer, so it gets reset upon every switch into
    // alternate screen (not for main screen!)
}

template <typename T>
void Screen<T>::moveCursorUp(LineCount _n)
{
    wrapPending_ = 0;
    auto const n = min(
        _n.as<LineOffset>(),
        logicalCursorPosition().line > margin_.vertical.from
            ? logicalCursorPosition().line - margin_.vertical.from
            : logicalCursorPosition().line
    );

    cursor_.position.line -= n;
}

template <typename T>
void Screen<T>::moveCursorDown(LineCount _n)
{
    wrapPending_ = 0;
    auto const currentLineNumber = logicalCursorPosition().line;
    auto const n = min(
        _n.as<LineOffset>(),
        currentLineNumber <= margin_.vertical.to
            ? margin_.vertical.to - currentLineNumber
            : (pageSize_.lines.as<LineOffset>() - 1) - currentLineNumber
    );

    cursor_.position.line += n;
}

template <typename T>
void Screen<T>::moveCursorForward(ColumnCount _n)
{
    wrapPending_ = 0;
    cursor_.position.column = min(cursor_.position.column + _n.as<ColumnOffset>(),
                                  margin_.horizontal.to);
}

template <typename T>
void Screen<T>::moveCursorBackward(ColumnCount _n)
{
    // even if you move to 80th of 80 columns, it'll first write a char and THEN flag wrap pending
    wrapPending_ = false;

    // TODO: skip cells that in counting when iterating backwards over a wide cell (such as emoji)
    auto const n = min(_n.as<ColumnOffset>(), cursor_.position.column);
    setCurrentColumn(cursor_.position.column - n);
}

template <typename T>
void Screen<T>::moveCursorToColumn(ColumnOffset _column)
{
    setCurrentColumn(_column);
}

template <typename T>
void Screen<T>::moveCursorToBeginOfLine()
{
    setCurrentColumn(ColumnOffset(0));
}

template <typename T>
void Screen<T>::moveCursorToLine(LineOffset _row)
{
    moveCursorTo(_row, cursor_.position.column);
}

template <typename T>
void Screen<T>::moveCursorToNextTab()
{
    // TODO: I guess something must remember when a \t was added, for proper move-back?
    // TODO: respect HTS/TBC

    if (!tabs_.empty())
    {
        // advance to the next tab
        size_t i = 0;
        while (i < tabs_.size() && realCursorPosition().column >= tabs_[i])
            ++i;

        auto const currentCursorColumn = logicalCursorPosition().column;

        if (i < tabs_.size())
            moveCursorForward(boxed_cast<ColumnCount>(tabs_[i] - currentCursorColumn));
        else if (realCursorPosition().column < margin_.horizontal.to)
            moveCursorForward(boxed_cast<ColumnCount>(margin_.horizontal.to - currentCursorColumn));
        else
            moveCursorToNextLine(LineCount(1));
    }
    else if (tabWidth_.value)
    {
        // default tab settings
        if (realCursorPosition().column < margin_.horizontal.to)
        {
            auto const n = min(
                (tabWidth_ - cursor_.position.column.as<ColumnCount>() % tabWidth_),
                pageSize_.columns - boxed_cast<ColumnCount>(logicalCursorPosition().column)
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
            moveCursorToColumn(margin_.horizontal.to);
        else
            // then TAB moves to next line left margin
            moveCursorToNextLine(LineCount(1));
    }
}

template <typename T>
void Screen<T>::notify(string const& _title, string const& _content)
{
    std::cout << "Screen.NOTIFY: title: '" << _title << "', content: '" << _content << "'\n";
    eventListener_.notify(_title, _content);
}

template <typename T>
void Screen<T>::captureBuffer(int _lineCount, bool _logicalLines)
{
    // TODO: Unit test case! (for ensuring line numbering and limits are working as expected)

    auto capturedBuffer = std::string();
    auto writer = VTWriter([&](auto buf, auto len) { capturedBuffer += string_view(buf, len); });

    // TODO: when capturing _lineCount < screenSize.lines, start at the lowest non-empty line.
    auto const relativeStartLine = _logicalLines
        ? grid().computeLogicalLineNumberFromBottom(LineCount::cast_from(_lineCount))
        : unbox<int>(pageSize_.lines) - _lineCount;
    auto const startLine = clamp(
        relativeStartLine,
        -unbox<int>(historyLineCount()),
        unbox<int>(pageSize_.lines));

    // dumpState();

    auto const trimSpaceRight = [](string& value)
    {
        while (!value.empty() && value.back() == ' ')
            value.pop_back();
    };

    for (LineOffset line = LineOffset(startLine); line < pageSize_.lines.as<LineOffset>(); ++line)
    {
        if (_logicalLines && grid().lineAt(line).wrapped() && !capturedBuffer.empty())
            capturedBuffer.pop_back();

        if (grid().isLineBlank(line))
            continue;

        for (ColumnOffset col = ColumnOffset{0}; col < pageSize_.columns.as<ColumnOffset>(); ++col)
        {
            Cell const& cell = at({line, col});
            if (!cell.codepointCount())
                writer.write(U' ');
            else
            {
                writer.write(cell.codepoint(0));
                for (size_t i = 1; i < cell.codepointCount(); ++i)
                    writer.write(cell.codepoint(i));
            }
        }
        trimSpaceRight(capturedBuffer);

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

template <typename T>
void Screen<T>::cursorForwardTab(TabStopCount _count)
{
    for (int i = 0; i < unbox<int>(_count); ++i)
        moveCursorToNextTab();
}

template <typename T>
void Screen<T>::cursorBackwardTab(TabStopCount _count)
{
    if (!_count)
        return;

    if (!tabs_.empty())
    {
        for (unsigned k = 0; k < unbox<unsigned>(_count); ++k)
        {
            auto const i = std::find_if(rbegin(tabs_), rend(tabs_),
                                        [&](ColumnOffset tabPos) -> bool {
                                            return tabPos < logicalCursorPosition().column;
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
    else if (tabWidth_.value)
    {
        // default tab settings
        if (*cursor_.position.column < *tabWidth_)
            moveCursorToBeginOfLine();
        else
        {
            auto const m = (*cursor_.position.column + 1) % *tabWidth_;
            auto const n = m
                         ? (*_count - 1) * *tabWidth_ + m
                         : *_count * *tabWidth_ + m;
            moveCursorBackward(ColumnCount(n - 1));
        }
    }
    else
    {
        // no tab stops configured
        moveCursorToBeginOfLine();
    }
}

template <typename T>
void Screen<T>::index()
{
    if (*realCursorPosition().line == *margin_.vertical.to)
        scrollUp(LineCount(1));
    else
        moveCursorDown(LineCount(1));
}

template <typename T>
void Screen<T>::reverseIndex()
{
    if (unbox<int>(realCursorPosition().line) == unbox<int>(margin_.vertical.from))
        scrollDown(LineCount(1));
    else
        moveCursorUp(LineCount(1));
}

template <typename T>
void Screen<T>::backIndex()
{
    if (realCursorPosition().column == margin_.horizontal.from)
        ;// TODO: scrollRight(1);
    else
        moveCursorForward(ColumnCount(1));
}

template <typename T>
void Screen<T>::forwardIndex()
{
    if (*realCursorPosition().column + 1 == *margin_.horizontal.to)
        ;// TODO: scrollLeft(1);
    else
        moveCursorDown(LineCount(1));
}

template <typename T>
void Screen<T>::setForegroundColor(Color _color)
{
    cursor_.graphicsRendition.foregroundColor = _color;
}

template <typename T>
void Screen<T>::setBackgroundColor(Color _color)
{
    cursor_.graphicsRendition.backgroundColor = _color;
}

template <typename T>
void Screen<T>::setUnderlineColor(Color _color)
{
    cursor_.graphicsRendition.underlineColor = _color;
}

template <typename T>
void Screen<T>::setCursorStyle(CursorDisplay _display, CursorShape _shape)
{
    cursorDisplay_ = _display;
    cursorShape_ = _shape;

    eventListener_.setCursorStyle(_display, _shape);
}

template <typename T>
void Screen<T>::setGraphicsRendition(GraphicsRendition _rendition)
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
            cursor_.graphicsRendition.styles &= ~(CellFlags::Underline |
                                                  CellFlags::DoublyUnderlined |
                                                  CellFlags::CurlyUnderlined |
                                                  CellFlags::DottedUnderline |
                                                  CellFlags::DashedUnderline);
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

template <typename T>
void Screen<T>::setMark()
{
    currentLine().setMarked(true);
}

template <typename T>
void Screen<T>::saveModes(std::vector<DECMode> const& _modes)
{
    modes_.save(_modes);
}

template <typename T>
void Screen<T>::restoreModes(std::vector<DECMode> const& _modes)
{
    modes_.restore(_modes);
}

template <typename T>
void Screen<T>::setMode(AnsiMode _mode, bool _enable)
{
    if (!isValidAnsiMode(static_cast<int>(_mode)))
        return;

    modes_.set(_mode, _enable);
}

template <typename T>
void Screen<T>::setMode(DECMode _mode, bool _enable)
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
                margin_.horizontal = Margin::Horizontal{ColumnOffset(0), boxed_cast<ColumnOffset>(pageSize_.columns - 1)};
            break;
        case DECMode::Origin:
            cursor_.originMode = _enable;
            break;
        case DECMode::Columns132:
            if (!isModeEnabled(DECMode::AllowColumns80to132))
                break;
            if (_enable != isModeEnabled(DECMode::Columns132))
            {
                auto const clear = _enable != isModeEnabled(_mode);

                // sets the number of columns on the page to 80 or 132 and selects the
                // corresponding 80- or 132-column font
                auto const columns = ColumnCount(_enable ? 132 : 80);

                resizeColumns(columns, clear);
            }
            break;
        case DECMode::BatchedRendering:
            if (modes_.enabled(DECMode::BatchedRendering) != _enable)
                eventListener_.synchronizedOutput(_enable);
            break;
        case DECMode::TextReflow:
            if (allowReflowOnResize_ && isPrimaryScreen())
            {
                // Enabling reflow enables every line in the main page area.
                // Disabling reflow only affects currently line and below.
                auto const startLine = _enable ? LineOffset(0)
                                               : realCursorPosition().line;
                for (auto line = startLine; line < pageSize_.lines.as<LineOffset>(); ++line)
                    grid().lineAt(line).setWrappable(_enable);
            }
            break;
        case DECMode::DebugLogging:
            // Since this mode (Xterm extension) does not support finer graind control,
            // we'll be just globally enable/disable all debug logging.
            for (auto& category: logstore::get())
                category.get().enable(_enable);
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

template <typename T>
void Screen<T>::requestAnsiMode(int _mode)
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

template <typename T>
void Screen<T>::requestDECMode(int _mode)
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

template <typename T>
void Screen<T>::setTopBottomMargin(optional<LineOffset> _top, optional<LineOffset> _bottom)
{
	auto const bottom = _bottom.has_value()
		? min(_bottom.value(), pageSize_.lines.as<LineOffset>() - 1)
		: pageSize_.lines.as<LineOffset>() - 1;

	auto const top = _top.value_or(LineOffset(0));

	if (top < bottom)
    {
        margin_.vertical.from = top;
        margin_.vertical.to = bottom;
        moveCursorTo({}, {});
    }
}

template <typename T>
void Screen<T>::setLeftRightMargin(optional<ColumnOffset> _left, optional<ColumnOffset> _right)
{
    if (isModeEnabled(DECMode::LeftRightMargin))
    {
		auto const right = _right.has_value()
			? min(_right.value(), pageSize_.columns.as<ColumnOffset>() - ColumnOffset(1))
			: pageSize_.columns.as<ColumnOffset>() - ColumnOffset(1);
		auto const left = _left.value_or(ColumnOffset(0));
		if (left < right)
        {
            margin_.horizontal.from = left;
            margin_.horizontal.to = right;
            moveCursorTo({}, {});
        }
    }
}

template <typename T>
void Screen<T>::screenAlignmentPattern()
{
    // sets the margins to the extremes of the page
    margin_.vertical.from = LineOffset(0);
    margin_.vertical.to = pageSize_.lines.as<LineOffset>() - LineOffset(1);
    margin_.horizontal.from = ColumnOffset(0);
    margin_.horizontal.to = pageSize_.columns.as<ColumnOffset>() - ColumnOffset(1);

    // and moves the cursor to the home position
    moveCursorTo({}, {});

    // fills the complete screen area with a test pattern
    for (auto& line: grid().mainPage())
    {
        line.reset(grid().defaultLineFlags(),
                   GraphicsAttributes{}, U'E', 1);
    }
}

template <typename T>
void Screen<T>::sendMouseEvents(MouseProtocol _protocol, bool _enable)
{
    eventListener_.setMouseProtocol(_protocol, _enable);
}

template <typename T>
void Screen<T>::applicationKeypadMode(bool _enable)
{
    eventListener_.setApplicationkeypadMode(_enable);
}

template <typename T>
void Screen<T>::designateCharset(CharsetTable _table, CharsetId _charset)
{
    // TODO: unit test SCS and see if they also behave well with reset/softreset
    // Also, is the cursor shared between the two buffers?
    cursor_.charsets.select(_table, _charset);
}

template <typename T>
void Screen<T>::singleShiftSelect(CharsetTable _table)
{
    // TODO: unit test SS2, SS3
    cursor_.charsets.singleShift(_table);
}

template <typename T>
void Screen<T>::sixelImage(ImageSize _pixelSize, Image::Data&& _data)
{
    auto const columnCount = ColumnCount::cast_from(ceilf(float(*_pixelSize.width) / float(*cellPixelSize_.width)));
    auto const lineCount = LineCount::cast_from(ceilf(float(*_pixelSize.height) / float(*cellPixelSize_.height)));
    auto const extent = GridSize{lineCount, columnCount};
    auto const autoScrollAtBottomMargin = isModeEnabled(DECMode::SixelScrolling); // If DECSDM is enabled, scrolling is meant to be disabled.
    auto const topLeft = autoScrollAtBottomMargin ? logicalCursorPosition() : Coordinate{};

    auto const alignmentPolicy = ImageAlignment::TopStart;
    auto const resizePolicy = ImageResize::NoResize;

    auto const imageOffset = Coordinate{};
    auto const imageSize = _pixelSize;

    Image const& imageRef = uploadImage(ImageFormat::RGBA, _pixelSize, move(_data));
    renderImage(imageRef.id(), topLeft, extent,
                imageOffset, imageSize,
                alignmentPolicy, resizePolicy,
                autoScrollAtBottomMargin);

    if (!sixelCursorConformance_)
        linefeed(topLeft.column);
}

template <typename T>
Image const& Screen<T>::uploadImage(ImageFormat _format, ImageSize _imageSize, Image::Data&& _pixmap)
{
    return imagePool_.create(_format, _imageSize, move(_pixmap));
}

template <typename T>
void Screen<T>::renderImage(ImageId _imageId,
                            Coordinate _topLeft, GridSize _gridSize,
                            Coordinate _imageOffset, ImageSize _imageSize,
                            ImageAlignment _alignmentPolicy,
                            ImageResize _resizePolicy,
                            bool _autoScroll)
{
    // TODO: make use of _imageOffset and _imageSize
    (void) _imageOffset;
    (void) _imageSize;

    auto const linesAvailable = LineCount(1) + pageSize_.lines - _topLeft.line.as<LineCount>();
    auto const linesToBeRendered = min(_gridSize.lines, linesAvailable);
    auto const columnsAvailable = *pageSize_.columns - *_topLeft.column + 1;
    auto const columnsToBeRendered = ColumnCount(min(columnsAvailable, *_gridSize.columns));
    auto const gapColor = RGBAColor{}; // TODO: cursor_.graphicsRendition.backgroundColor;

    // TODO: make use of _imageOffset and _imageSize
    auto const rasterizedImage = imagePool_.rasterize(
        _imageId,
        _alignmentPolicy,
        _resizePolicy,
        gapColor,
        _gridSize,
        cellPixelSize_
    );

    if (*linesToBeRendered)
    {
        crispy::for_each(
            GridSize{linesToBeRendered, columnsToBeRendered},
            [&](GridSize::Offset offset) {
                auto const cellCoord = Coordinate{
                    _topLeft.line + offset.line,
                    _topLeft.column + offset.column};
                Cell& cell = at(cellCoord.line, cellCoord.column);
                cell.setImage(nextImageFragmentId_++);
                imageFragments_.emplace(
                    cell.imageFragment(),
                    ImageFragment{
                        rasterizedImage,
                        Coordinate{offset.line, offset.column}
                    }
                );
                cell.setHyperlink(cursor_.hyperlink);
            }
        );
        moveCursorTo(_topLeft.line + unbox<int>(linesToBeRendered) - 1, _topLeft.column);
    }

    // If there're lines to be rendered missing (because it didn't fit onto the screen just yet)
    // AND iff sixel !sixelScrolling  is enabled, then scroll as much as needed to render the remaining lines.
    if (linesToBeRendered != _gridSize.lines && _autoScroll)
    {
        auto const remainingLineCount = _gridSize.lines - linesToBeRendered;
        for (auto const lineOffset : crispy::times(*remainingLineCount))
        {
            linefeed();
            moveCursorForward(_topLeft.column.as<ColumnCount>());
            crispy::for_each(
                crispy::times(columnsToBeRendered.as<ColumnOffset>()),
                [&](ColumnOffset columnOffset) {
                    Cell& cell = at(Coordinate{pageSize_.lines.as<LineOffset>(),
                                               _topLeft.column + columnOffset});
                    cell.setImage(nextImageFragmentId_++);
                    imageFragments_.emplace(
                        cell.imageFragment(),
                        ImageFragment{
                            rasterizedImage,
                            Coordinate{
                                boxed_cast<LineOffset>(linesToBeRendered) + lineOffset,
                                columnOffset
                            }
                        }
                    );
                    cell.setHyperlink(cursor_.hyperlink);
                }
            );
        }
    }
    // move ansi text cursor to position of the sixel cursor
    moveCursorToColumn(_topLeft.column + _gridSize.columns.as<ColumnOffset>());
}

template <typename T>
void Screen<T>::setWindowTitle(std::string const& _title)
{
    windowTitle_ = _title;
    eventListener_.setWindowTitle(_title);
}

template <typename T>
void Screen<T>::saveWindowTitle()
{
    savedWindowTitles_.push(windowTitle_);
}

template <typename T>
void Screen<T>::restoreWindowTitle()
{
    if (!savedWindowTitles_.empty())
    {
        windowTitle_ = savedWindowTitles_.top();
        savedWindowTitles_.pop();
        eventListener_.setWindowTitle(windowTitle_);
    }
}

template <typename T>
void Screen<T>::requestDynamicColor(DynamicColorName _name)
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
                if (holds_alternative<CellForegroundColor>(colorPalette_.cursor.color))
                    return colorPalette_.defaultForeground;
                else if (holds_alternative<CellBackgroundColor>(colorPalette_.cursor.color))
                    return colorPalette_.defaultBackground;
                else
                    return get<RGBColor>(colorPalette_.cursor.color);
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

template <typename T>
void Screen<T>::requestPixelSize(RequestPixelSize _area)
{
    switch (_area)
    {
        case RequestPixelSize::WindowArea:
            [[fallthrough]]; // TODO
        case RequestPixelSize::TextArea:
            // Result is CSI  4 ;  height ;  width t
            reply(
                "\033[4;{};{}t",
                *cellPixelSize_.height * *pageSize_.lines,
                *cellPixelSize_.width * *pageSize_.columns
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

template <typename T>
void Screen<T>::requestCharacterSize(RequestPixelSize _area) // TODO: rename RequestPixelSize to RequestArea?
{
    switch (_area)
    {
        case RequestPixelSize::TextArea:
            reply("\033[8;{};{}t", pageSize_.lines, pageSize_.columns);
            break;
        case RequestPixelSize::WindowArea:
            reply("\033[9;{};{}t", pageSize_.lines, pageSize_.columns);
            break;
        case RequestPixelSize::CellArea:
            assert(!"Screen.requestCharacterSize: Doesn't make sense, and cannot be called, therefore, fortytwo.");
            break;
    }
}

template <typename T>
void Screen<T>::requestStatusString(RequestStatusString _value)
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
                if (*pageSize_.lines >= 24)
                    return fmt::format("{}t", pageSize_.lines);
#if defined(LIBTERMINAL_LOG_RAW)
                LOGSTORE(ScreenRawOutputLog)("Requesting device status for {} not with line count < 24 is undefined.");
#endif
                return nullopt;
            case RequestStatusString::DECSTBM:
                return fmt::format("{};{}r", 1 + *margin_.vertical.from, *margin_.vertical.to);
            case RequestStatusString::DECSLRM:
                return fmt::format("{};{}s", 1 + *margin_.horizontal.from, *margin_.horizontal.to);
            case RequestStatusString::DECSCPP:
                // EXTENSION: Usually DECSCPP only knows about 80 and 132, but we take any.
                return fmt::format("{}|$", pageSize_.columns);
            case RequestStatusString::DECSNLS:
                return fmt::format("{}*|", pageSize_.lines);
            case RequestStatusString::SGR:
                return fmt::format("0;{}m", vtSequenceParameterString(cursor_.graphicsRendition));
            case RequestStatusString::DECSCA: // TODO
#if defined(LIBTERMINAL_LOG_RAW)
                LOGSTORE(ScreenRawOutputLog)("Requesting device status for {} not implemented yet.", _value);
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

template <typename T>
void Screen<T>::requestTabStops()
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
            dcs << *tabs_[i] + 1;
        }
    }
    else if (*tabWidth_ != 0)
    {
        dcs << 1;
        for (auto column = *tabWidth_ + 1; column <= *pageSize_.columns; column += *tabWidth_)
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

template <typename T>
void Screen<T>::requestCapability(std::string_view _name)
{
    if (!respondToTCapQuery_)
    {
#if defined(LIBTERMINAL_LOG_RAW)
        LOGSTORE(ScreenRawOutputLog)("Requesting terminal capability {} ignored. Experimental tcap feature disabled.", _name);
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

template <typename T>
void Screen<T>::requestCapability(capabilities::Code _code)
{
    if (!respondToTCapQuery_)
    {
#if defined(LIBTERMINAL_LOG_RAW)
        if (ScreenRawOutputLog)
            LOGSTORE(ScreenRawOutputLog)("Requesting terminal capability {} ignored. Experimental tcap feature disabled.", _code);
#endif
        return;
    }

#if defined(LIBTERMINAL_LOG_RAW)
    if (ScreenRawOutputLog)
        LOGSTORE(ScreenRawOutputLog)("Requesting terminal capability: {}", _code);
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

template <typename T>
void Screen<T>::resetDynamicColor(DynamicColorName _name)
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

template <typename T>
void Screen<T>::setDynamicColor(DynamicColorName _name, RGBColor _value)
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
            colorPalette_.cursor.color = _value;
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

template <typename T>
void Screen<T>::dumpState()
{
    eventListener_.dumpState();
}

template <typename T>
void Screen<T>::dumpState(std::string const& _message, std::ostream& _os) const
{
    auto const hline = [&]() {
        for_each(crispy::times(*pageSize_.columns), [&](auto) { _os << '='; });
        _os << endl;
    };

    auto const gridInfoLine = [&](Grid<Cell> const& grid) {
        return fmt::format(
            "main page lines: scrollback cur {} max {}, main page lines {}, used lines {}, zero index {}\n",
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
    _os << fmt::format("main page size       : {}\n", pageSize_);
    _os << fmt::format("history line count   : {} (max {})\n", historyLineCount(), maxHistoryLineCount());
    _os << fmt::format("cursor position      : {}\n", cursor_);
    if (cursor_.originMode)
        _os << fmt::format("real cursor position : {})\n", toRealCoordinate(cursor_.position));
    _os << fmt::format("vertical margins     : {}\n", margin_.vertical);
    _os << fmt::format("horizontal margins   : {}\n", margin_.horizontal);
    _os << gridInfoLine(grid());

    hline();
    _os << screenshot([this](LineOffset _lineNo) -> string {
        //auto const absoluteLine = grid().toAbsoluteLine(_lineNo);
        return fmt::format("| {:>4}: {}", _lineNo.value, grid().lineAt(_lineNo).flags());
    });
    hline();

    // TODO: print more useful debug information
    // - screen size
    // - left/right margin
    // - top/down margin
    // - cursor position
    // - autoWrap
    // - ... other output related modes
}

template <typename T>
void Screen<T>::smGraphics(XtSmGraphics::Item _item, XtSmGraphics::Action _action, XtSmGraphics::Value _value)
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
                    auto const value = maxImageColorRegisters_;
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
                        size.width = min(size.width, maxImageSizeLimit_.width);
                        size.height = min(size.height, maxImageSizeLimit_.height);
                        maxImageSize_ = size;
                        reply("\033[?{};{};{};{}S", SixelItem, Success, size.width, size.height);
                    }
                    else
                        reply("\033[?{};{};{}S", SixelItem, Failure, 0);
                    break;
            }
            break;

        case Item::ReGISGraphicsGeometry: // Surely, we don't do ReGIS just yet. :-)
            break;
    }
}
// }}}

} // namespace terminal

template class terminal::Screen<terminal::MockTerm>;

#include <terminal/Terminal.h>
template class terminal::Screen<terminal::Terminal>;
//template class terminal::Screen<terminal::MockScreenEvents>;
