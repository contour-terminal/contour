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
#include <terminal/Grid.h>

#include <crispy/Comparison.h>
#include <crispy/indexed.h>
#include <crispy/range.h>

#include <unicode/convert.h>

#include <algorithm>
#include <iostream>
#include <optional>
#include <sstream>
#include <tuple>
#include <utility>

using crispy::Comparison;
using crispy::Size;

using std::back_inserter;
using std::fill_n;
using std::for_each;
using std::front_inserter;
using std::generate_n;
using std::min;
using std::move;
using std::next;
using std::nullopt;
using std::optional;
using std::prev;
using std::reverse;
using std::rotate;
using std::string;
using std::tuple;

#if defined(LIBTERMINAL_EXECUTION_PAR)
#include <execution>
#define LIBTERMINAL_EXECUTION_COMMA(par) (std::execution:: par),
#else
#define LIBTERMINAL_EXECUTION_COMMA(par) /*!*/
#endif

namespace terminal {

namespace // {{{ helper
{
    bool is_blank(Cell const& _cell) noexcept
    {
        return
#if defined(LIBTERMINAL_IMAGES)
            !_cell.imageFragment() &&
#endif
            _cell.codepointCount() == 0;
    }

    template <typename... Args>
    void logf([[maybe_unused]] Args&&... _args)
    {
#if 0
        std::cout << fmt::format(std::forward<Args>(_args)...) << '\n';
#endif
    }

    /**
     * Appends logical line by splitting into fixed-width lines.
     *
     * @param _targetLines
     * @param _newColumnCount
     * @param _logicalLineBuffer
     * @param _baseFlags
     * @param _initialNoWrap
     */
    void addNewWrappedLines(Lines& _targetLines,
                            size_t _newColumnCount,
                            Line::Buffer&& _logicalLineBuffer, // TODO: don't move, do (c)ref instead
                            Line::Flags _baseFlags,
                            bool _initialNoWrap)
    {
        // TODO: avoid unnecessary copies via erase() by incrementally updating (from, to)
        int i = 0;

        while (_logicalLineBuffer.size() >= _newColumnCount)
        {
            auto from = begin(_logicalLineBuffer);
            auto to = next(begin(_logicalLineBuffer), static_cast<int>(_newColumnCount));
            auto const wrappedFlag = i == 0 && _initialNoWrap ? Line::Flags::None : Line::Flags::Wrapped;
            _targetLines.emplace_back(Line(from, to, _baseFlags | wrappedFlag));
            logf(" - add line: '{}' ({})", _targetLines.back().toUtf8(), _targetLines.back().flags());
            _logicalLineBuffer.erase(from, to);
            ++i;
        }

        if (_logicalLineBuffer.size() > 0)
        {
            auto const wrappedFlag = i == 0 && _initialNoWrap ? Line::Flags::None : Line::Flags::Wrapped;
            _targetLines.emplace_back(Line(_newColumnCount, move(_logicalLineBuffer), _baseFlags | wrappedFlag));
            logf(" - add line: '{}' ({})", _targetLines.back().toUtf8(), _targetLines.back().flags());
        }
    }
}
// }}}
// {{{ Cell impl
string Cell::toUtf8() const
{
    if (!codepoints_.empty())
        return unicode::convert_to<char>(codepoints());
    else
        return " ";
}
// }}}
// {{{ Line impl
Line::Line(Buffer&& _init, Flags _flags) :
    buffer_{ move(_init) },
    flags_{ static_cast<unsigned>(_flags) }
{
}

Line::Line(iterator const& _begin, iterator const& _end, Flags _flags) :
    buffer_(_begin, _end),
    flags_{ static_cast<unsigned>(_flags) }
{
}

Line::Line(size_t _numCols, Buffer&& _init, Flags _flags) :
    buffer_{move(_init)},
    flags_{ static_cast<unsigned>(_flags) }
{
    buffer_.resize(_numCols);
}

Line::Line(size_t _numCols, std::string_view const& _s, Flags _flags) :
    Line(_numCols, Cell{}, _flags)
{
    for (auto const && [i, ch] : crispy::indexed(_s))
        buffer_.at(i).setCharacter(static_cast<char32_t>(ch));
}

string Line::toUtf8() const
{
    std::stringstream sstr;
    for (Cell const& cell : crispy::range(begin(), next(begin(), static_cast<long int>(size()))))
    {
        if (cell.codepointCount() == 0)
        {
            sstr << ' ';
        }
        else
        {
            for (char32_t codepoint : cell.codepoints())
                sstr << unicode::convert_to<char>(codepoint);
        }
    }
    return sstr.str();
}

string Line::toUtf8Trimmed() const
{
    string output = toUtf8();
    while (!output.empty() && isspace(output.back()))
        output.pop_back();
    return output;
}

void Line::prepend(Buffer const& _cells)
{
    buffer_.insert(buffer_.begin(), _cells.begin(), _cells.end());
}

void Line::append(Buffer const& _cells)
{
    buffer_.insert(buffer_.end(), _cells.begin(), _cells.end());
}

void Line::append(size_t _count, Cell const& _initial)
{
    fill_n(back_inserter(buffer_), _count, _initial);
}

crispy::range<Line::const_iterator> Line::trim_blank_right() const
{
    auto i = buffer_.cbegin();
    auto e = buffer_.cend();

    while (i != e && is_blank(*prev(e)))
        e = prev(e);

    return crispy::range(i, e);
}

Line::Buffer Line::shift_left(unsigned _count, Cell const& _fill)
{
    auto const actualShiftCount = min(static_cast<size_t>(_count), size());
    auto const from = std::begin(buffer_);
    auto const to = std::next(std::begin(buffer_), static_cast<long int>(actualShiftCount));

    auto out = remove(from, to);
    append(actualShiftCount, _fill);

    // trim trailing blanks
    while (!out.empty() && is_blank(out.back()))
        out.pop_back();

    return out;
}

Line::Buffer Line::remove(iterator const& _from, iterator const& _to)
{
    auto removedColumns = Buffer(_from, _to);
    buffer_.erase(_from, _to);
    return removedColumns;
}

void Line::setText(std::string_view _u8string)
{
    for (auto const [i, ch] : crispy::indexed(unicode::convert_to<char32_t>(_u8string)))
        buffer_.at(i).setCharacter(ch);
}

void Line::resize(size_t _size)
{
    buffer_.resize(_size);
}

bool Line::blank() const noexcept
{
    return std::all_of(cbegin(), cend(), is_blank);
}

Line::Buffer Line::reflow(size_t _newColumnCount)
{
    switch (crispy::strongCompare(_newColumnCount, size()))
    {
        case Comparison::Equal:
            break;
        case Comparison::Greater:
            buffer_.resize(static_cast<size_t>(_newColumnCount));
            break;
        case Comparison::Less:
        {
            // TODO: properly handle wide character cells
            // - when cutting in the middle of a wide char, the wide char gets wrapped and an empty
            //   cell needs to be injected to match the expected column width.

            if (wrappable())
            {
                auto const [reflowStart, reflowEnd] = [this, _newColumnCount]()
                {
                    auto const reflowStart = next(buffer_.begin(), static_cast<long int>(_newColumnCount /* - buffer_[_newColumnCount].width()*/));
                    auto reflowEnd = buffer_.end();

                    while (reflowEnd != reflowStart && is_blank(*prev(reflowEnd)))
                        reflowEnd = prev(reflowEnd);

                    return tuple{reflowStart, reflowEnd};
                }();

                auto removedColumns = Buffer(reflowStart, reflowEnd);
                buffer_.erase(reflowStart, buffer_.end());
                assert(size() == _newColumnCount);
                return removedColumns;
            }
            else
            {
                auto const reflowStart = next(buffer_.cbegin(), static_cast<long int>(_newColumnCount));
                buffer_.erase(reflowStart, buffer_.end());
                assert(size() == _newColumnCount);
                return {};
            }
        }
    }
    return {};
}
// }}}
// {{{ Grid impl
Grid::Grid(Size _screenSize, bool _reflowOnResize, optional<int> _maxHistoryLineCount) :
    screenSize_{ _screenSize },
    reflowOnResize_{ _reflowOnResize },
    maxHistoryLineCount_{ _maxHistoryLineCount },
    lines_(
        static_cast<size_t>(_screenSize.height),
        Line(
            static_cast<size_t>(_screenSize.width),
            Cell{},
            _reflowOnResize ? Line::Flags::Wrappable : Line::Flags::None
        )
    )
{
}

void Grid::setMaxHistoryLineCount(optional<size_t> _maxHistoryLineCount)
{
    maxHistoryLineCount_ = _maxHistoryLineCount;
    clampHistory();
}

int Grid::computeRelativeLogicalLineNumberFromBottom(unsigned _n) const noexcept
{
    int logicalLineCount = 0;
    int outputRelativePhysicalLine = static_cast<int>(screenSize_.height);

    auto i = lines_.rbegin();
    while (i != lines_.rend())
    {
        if (!i->wrapped())
            logicalLineCount++;
        outputRelativePhysicalLine--;
        ++i;
        if (logicalLineCount == _n)
            break;
    }

    // XXX If the top-most logical line is reached, we still need to traverse upwards until the
    // beginning of the top-most logical line (the one that does not have the wrapped-flag set).
    while (i != lines_.rend() && i->wrapped())
    {
        printf("further upwards: l %d, p %d\n", logicalLineCount, outputRelativePhysicalLine);
        outputRelativePhysicalLine--;
        ++i;
    }

    return outputRelativePhysicalLine;
}

Coordinate Grid::resize(Size _newSize, Coordinate _currentCursorPos, bool _wrapPending)
{
    auto const growLines = [this](unsigned _newHeight) -> Coordinate
    {
        assert(_newHeight > screenSize_.height);
        // Grow line count by splicing available lines from history back into buffer, if available,
        // or create new ones until screenSize_.height == _newHeight.

        auto const extendCount = static_cast<unsigned>(_newHeight - screenSize_.height);
        auto const rowsToTakeFromSavedLines = min(extendCount, static_cast<unsigned>(historyLineCount()));
        auto const fillLineCount = extendCount - rowsToTakeFromSavedLines;
        auto const wrappableFlag = lines_.back().wrappableFlag();

        generate_n(
            back_inserter(lines_),
            fillLineCount,
            [this, wrappableFlag]() { return Line(static_cast<size_t>(screenSize_.width), Cell{}, wrappableFlag); }
        );

        screenSize_.height = _newHeight;

        return Coordinate{static_cast<int>(rowsToTakeFromSavedLines), 0};
    };

    auto const shrinkLines = [this](unsigned _newHeight, Coordinate _cursor) -> Coordinate
    {
        // Shrink existing line count to _newSize.height
        // by splicing the number of lines to be shrinked by into savedLines bottom.

        if (_cursor.row == screenSize_.height)
        {
            auto const shrinkedLinesCount = screenSize_.height - _newHeight;
            screenSize_.height = _newHeight;
            clampHistory();
            return Coordinate{static_cast<int>(shrinkedLinesCount), 0};
        }
        else
        {
            // Hard-cut below cursor by the number of lines to shrink.
            lines_.resize(historyLineCount() + static_cast<size_t>(_newHeight));
            screenSize_.height = _newHeight;
            return Coordinate{0, 0};
        }
    };

    auto const growColumns = [this, _wrapPending](size_t _newColumnCount, Coordinate _cursor) -> Coordinate
    {
        if (!reflowOnResize_)
        {
            for (Line& line : lines_)
                if (line.size() < static_cast<size_t>(_newColumnCount))
                    line.resize(static_cast<size_t>(_newColumnCount));
            screenSize_.width = static_cast<int>(_newColumnCount);
            return _cursor + Coordinate{0, _wrapPending ? 1 : 0};
        }
        else
        {
            // Grow columns by inverse shrink,
            // i.e. the lines are traversed in reverse order.

            assert(_newColumnCount > static_cast<unsigned>(screenSize_.width));
            auto const extendCount = _newColumnCount - static_cast<unsigned>(screenSize_.width);

            logf("Growing by {} cols", extendCount);

            Lines grownLines;
            Line::Buffer logicalLineBuffer; // Temporary state, representing wrapped columns from the line "below".
            Line::Flags logicalLineFlags = Line::Flags::None;

            [[maybe_unused]] auto i = 1;
            for (Line& line : lines_)
            {
                logf("{:>2}: line: '{}' (wrapped: '{}') {}",
                     i++,
                     line.toUtf8(),
                     Line(Line::Buffer(logicalLineBuffer), line.flags()).toUtf8(),
                     line.wrapped() ? "WRAPPED" : "");
                assert(line.size() >= static_cast<unsigned>(screenSize_.width));

                if (line.wrapped())
                {
                    crispy::copy(line.trim_blank_right(), back_inserter(logicalLineBuffer));
                    logf(" - join: '{}'", Line(Line::Buffer(logicalLineBuffer), line.flags()).toUtf8());
                }
                else // line is not wrapped
                {
                    if (!logicalLineBuffer.empty())
                    {
                        addNewWrappedLines(grownLines, _newColumnCount, move(logicalLineBuffer), logicalLineFlags, true);
                        logicalLineBuffer.clear();
                    }

                    crispy::copy(line, back_inserter(logicalLineBuffer));
                    logicalLineFlags = line.wrappableFlag() | line.markedFlag();

                    logf(" - start new logical line: '{}'", line.toUtf8());
                }
            }

            if (!logicalLineBuffer.empty())
            {
                addNewWrappedLines(grownLines, _newColumnCount, move(logicalLineBuffer), logicalLineFlags, true);
                logicalLineBuffer.clear();
            }

            lines_ = move(grownLines);
            screenSize_.width = static_cast<int>(_newColumnCount);

            auto cy = size_t{0};
            auto const historyLineCountDelta = (static_cast<int>(lines_.size()) - static_cast<int>(screenSize_.height));
            if (historyLineCountDelta < 0)
            {
                cy = historyLineCount();
                appendNewLines(static_cast<size_t>(-historyLineCountDelta), lines_.back()->back().attributes());
            }

            return _cursor + Coordinate{static_cast<int>(cy), _wrapPending ? 1 : 0};
        }
    };

    auto const shrinkColumns = [this](size_t _newColumnCount, Coordinate _cursor) -> Coordinate
    {
        if (!reflowOnResize_)
        {
            screenSize_.width = static_cast<int>(_newColumnCount);
            crispy::for_each(lines_, [=](Line& line) {
                if (line.size() < _newColumnCount)
                    line.resize(_newColumnCount);
            });
            return _cursor + Coordinate{0u, static_cast<int>(min(static_cast<size_t>(_cursor.column), _newColumnCount))};
        }
        else
        {
            // {{{ Shrinking progress
            // -----------------------------------------------------------------------
            //  (one-by-one)        | (from-5-to-2)
            // -----------------------------------------------------------------------
            // "ABCDE"              | "ABCDE"
            // "abcde"              | "xy   "
            // ->                   | "abcde"
            // "ABCD"               | ->
            // "E   "   Wrapped     | "AB"                  push "AB", wrap "CDE"
            // "abcd"               | "CD"      Wrapped     push "CD", wrap "E"
            // "e   "   Wrapped     | "E"       Wrapped     push "E",  inc line
            // ->                   | "xy"      no-wrapped  push "xy", inc line
            // "ABC"                | "ab"      no-wrapped  push "ab", wrap "cde"
            // "DE "    Wrapped     | "cd"      Wrapped     push "cd", wrap "e"
            // "abc"                | "e "      Wrapped     push "e",  inc line
            // "de "    Wrapped
            // ->
            // "AB"
            // "DE"     Wrapped
            // "E "     Wrapped
            // "ab"
            // "cd"     Wrapped
            // "e "     Wrapped
            // }}}

            Lines shrinkedLines;
            Line::Buffer wrappedColumns;
            Line::Flags previousFlags = lines_.front().inheritableFlags();

            int i = 0;
            for (Line& line : lines_)
            {
                logf("shrink line {}: \"{}\" wrapped: \"{}\"",
                    i,
                    line.toUtf8(),
                    Line(Line::Buffer(wrappedColumns), previousFlags).toUtf8()
                );
                // do we have previous columns carried?
                if (!wrappedColumns.empty())
                {
                    if (line.wrapped() && line.inheritableFlags() == previousFlags)
                    {
                        assert(previousFlags == line.inheritableFlags());
                        // Prepend previously wrapped columns into current line.
                        line.prepend(wrappedColumns);
                    }
                    else
                    {
                        // Insert NEW line(s) between previous and this line with previously wrapped columns.
                        addNewWrappedLines(shrinkedLines, _newColumnCount, move(wrappedColumns), previousFlags, false);
                        previousFlags = line.inheritableFlags();
                    }
                }
                else
                {
                    previousFlags = line.inheritableFlags();
                }

                wrappedColumns = line.reflow(_newColumnCount);

                auto const wrappedLine = Line(Line::Buffer(wrappedColumns), Line::Flags::None);
                logf(" - ADD LINE: '{}' ({}) wrapped: \"{}\"", line.toUtf8(), line.flags(),
                    Line(Line::Buffer(wrappedColumns), Line::Flags::None).toUtf8());

                shrinkedLines.emplace_back(move(line));
                assert(shrinkedLines.back().size() >= _newColumnCount);
                i++;
            }
            addNewWrappedLines(shrinkedLines, _newColumnCount, move(wrappedColumns), previousFlags, false);

            lines_ = move(shrinkedLines);
            screenSize_.width = static_cast<int>(_newColumnCount);

            return _cursor; // TODO
        }
    };

    Coordinate cursorPosition = _currentCursorPos;

    // grow/shrink columns
    switch (crispy::strongCompare(_newSize.width, screenSize_.width))
    {
        case Comparison::Greater:
            cursorPosition = growColumns(static_cast<size_t>(_newSize.width), cursorPosition);
            break;
        case Comparison::Less:
            cursorPosition = shrinkColumns(static_cast<size_t>(_newSize.width), cursorPosition);
            break;
        case Comparison::Equal:
            break;
    }

    // grow/shrink lines
    switch (crispy::strongCompare(_newSize.height, screenSize_.height))
    {
        case Comparison::Greater:
            cursorPosition += growLines(_newSize.height);
            break;
        case Comparison::Less:
            cursorPosition += shrinkLines(_newSize.height, cursorPosition);
            break;
        case Comparison::Equal:
            break;
    }

    return cursorPosition;
}

void Grid::appendNewLines(size_t _count, GraphicsAttributes _attr)
{
    auto const wrappableFlag = lines_.back().wrappableFlag();

    if (historyLineCount() == maxHistoryLineCount().value_or(-1))
    {
        // We've reached to history line count limit already.
        // Rotate lines that would fall off down to the bottom again in a clean state.
        // We do save quite some overhead due to avoiding unnecessary memory allocations.
        for (size_t i = 0; i < _count; ++i)
        {
            Line line = move(lines_.front());
            lines_.pop_front();
            line.reset(_attr);
            lines_.emplace_back(move(line));
        }
        return;
    }

    if (auto const n = min(_count, static_cast<size_t>(screenSize_.height)); n > 0)
    {
        generate_n(
            back_inserter(lines_),
            n,
            [&]() { return Line(static_cast<size_t>(screenSize_.width), Cell{{}, _attr}, wrappableFlag); }
        );
        clampHistory();
    }
}

void Grid::clearHistory()
{
    if (historyLineCount())
        lines_.erase(begin(lines_), next(begin(lines_), static_cast<long int>(historyLineCount())));
}

void Grid::clampHistory()
{
    if (!maxHistoryLineCount_.has_value())
        return;

    auto const actual = historyLineCount();
    auto const maxHistoryLines = maxHistoryLineCount_.value();
    if (actual < maxHistoryLines)
        return;

    auto const diff = actual - maxHistoryLines;

    // any line that moves into history is using the default Wrappable flag.
    for (auto& line : lines(historyLineCount() - diff, historyLineCount()))
    {
        auto const wrappable = true;
        // std::cout << fmt::format(
        //     "clampHistory: wrappable={}: \"{}\"\n",
        //     wrappable ? "true" : "false",
        //     line.toUtf8()
        // );
        line.setFlag(Line::Flags::Wrappable, wrappable);
    }

    lines_.erase(begin(lines_), next(begin(lines_), static_cast<long int>(diff)));
}

void Grid::scrollUp(unsigned _n, GraphicsAttributes const& _defaultAttributes, Margin const& _margin)
{
    if (_margin.horizontal != Margin::Range{1, static_cast<unsigned>(screenSize_.width)})
    {
        // a full "inside" scroll-up
        auto const marginHeight = _margin.vertical.length();
        auto const n = min(_n, marginHeight);

        if (n < marginHeight)
        {
            auto targetLine = next(begin(mainPage()), _margin.vertical.from - 1);     // target line
            auto sourceLine = next(begin(mainPage()), _margin.vertical.from - 1 + n); // source line
            auto const bottomLine = next(begin(mainPage()), _margin.vertical.to);     // bottom margin's end-line iterator

            for (; sourceLine != bottomLine; ++sourceLine, ++targetLine)
            {
                copy_n(
                    next(begin(*sourceLine), _margin.horizontal.from - 1),
                    _margin.horizontal.length(),
                    next(begin(*targetLine), _margin.horizontal.from - 1)
                );
            }
        }

        // clear bottom n lines in margin.
        auto const topLine = next(begin(mainPage()), _margin.vertical.to - n);
        auto const bottomLine = next(begin(mainPage()), _margin.vertical.to);     // bottom margin's end-line iterator
#if 1
        for (Line& line : crispy::range(topLine, bottomLine))
        {
            fill_n(
                next(begin(line), _margin.horizontal.from - 1),
                _margin.horizontal.length(),
                Cell{{}, _defaultAttributes}
            );
        }
#else
        for_each(
            topLine,
            bottomLine,
            [&](Line& line) {
                fill_n(
                    next(begin(line), _margin.horizontal.from - 1),
                    _margin.horizontal.length(),
                    Cell{{}, _defaultAttributes}
                );
            }
        );
#endif
    }
    else if (_margin.vertical == Margin::Range{1, static_cast<unsigned>(screenSize_.height)})
    {
        if (auto const n = min(_n, static_cast<unsigned>(screenSize_.height)); n > 0)
        {
            appendNewLines(n, _defaultAttributes);
        }
    }
    else
    {
        // scroll up only inside vertical margin with full horizontal extend
        auto const marginHeight = _margin.vertical.length();
        auto const n = min(_n, marginHeight);
        if (n < marginHeight)
        {
            rotate(
                next(begin(mainPage()), _margin.vertical.from - 1),
                next(begin(mainPage()), _margin.vertical.from - 1 + n),
                next(begin(mainPage()), _margin.vertical.to)
            );
        }

        for_each(
            LIBTERMINAL_EXECUTION_COMMA(par)
            next(begin(mainPage()), _margin.vertical.to - n),
            next(begin(mainPage()), _margin.vertical.to),
            [&](Line& line) {
                fill(begin(line), end(line), Cell{{}, _defaultAttributes});
            }
        );
    }
}

void Grid::scrollDown(unsigned v_n, GraphicsAttributes const& _defaultAttributes, Margin const& _margin)
{
    auto const marginHeight = _margin.vertical.length();
    auto const n = min(v_n, marginHeight);

    if (_margin.horizontal != Margin::Range{1, static_cast<unsigned>(screenSize_.width)})
    {
        // full "inside" scroll-down
        if (n < marginHeight)
        {
            auto sourceLine = next(begin(mainPage()), _margin.vertical.to - n - 1);
            auto targetLine = next(begin(mainPage()), _margin.vertical.to - 1);
            auto const sourceEndLine = next(begin(mainPage()), _margin.vertical.from - 1);

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
                next(begin(mainPage()), _margin.vertical.from - 1),
                next(begin(mainPage()), _margin.vertical.from - 1 + n),
                [&](Line& line) {
                    fill_n(
                        next(begin(line), _margin.horizontal.from - 1),
                        _margin.horizontal.length(),
                        Cell{{}, _defaultAttributes}
                    );
                }
            );
        }
        else
        {
            // clear everything in margin
            for_each(
                next(begin(mainPage()), _margin.vertical.from - 1),
                next(begin(mainPage()), _margin.vertical.to),
                [&](Line& line) {
                    fill_n(
                        next(begin(line), _margin.horizontal.from - 1),
                        _margin.horizontal.length(),
                        Cell{{}, _defaultAttributes}
                    );
                }
            );
        }
    }
    else if (_margin.vertical == Margin::Range{1, static_cast<unsigned>(screenSize_.height)})
    {
        rotate(
            begin(mainPage()),
            next(begin(mainPage()), marginHeight - n),
            end(mainPage())
        );

        for_each(
            begin(mainPage()),
            next(begin(mainPage()), n),
            [&](Line& line) {
                fill(
                    begin(line),
                    end(line),
                    Cell{{}, _defaultAttributes}
                );
            }
        );
    }
    else
    {
        // scroll down only inside vertical margin with full horizontal extend
        rotate(
            next(begin(mainPage()), _margin.vertical.from - 1),
            next(begin(mainPage()), _margin.vertical.to - n),
            next(begin(mainPage()), _margin.vertical.to)
        );

        for_each(
            next(begin(mainPage()), _margin.vertical.from - 1),
            next(begin(mainPage()), _margin.vertical.from - 1 + n),
            [&](Line& line) {
                fill(
                    begin(line),
                    end(line),
                    Cell{{}, _defaultAttributes}
                );
            }
        );
    }
}

string Grid::renderTextLineAbsolute(unsigned row) const
{
    string line;
    line.reserve(static_cast<size_t>(screenSize_.width));
    for (int col = 1; col <= screenSize_.width; ++col)
        if (auto const& cell = at({static_cast<int>(row - historyLineCount()) + 1, col}); cell.codepointCount())
            line += cell.toUtf8();
        else
            line += " "; // fill character

    return line;
}

string Grid::renderTextLine(int row) const
{
    string line;
    line.reserve(static_cast<size_t>(screenSize_.width));
    for (int col = 1; col <= screenSize_.width; ++col)
        if (auto const& cell = at({row, col}); cell.codepointCount())
            line += cell.toUtf8();
        else
            line += " "; // fill character

    return line;
}

string Grid::renderAllText() const
{
    string text;
    text.reserve((historyLineCount() + static_cast<size_t>(screenSize_.height)) * static_cast<size_t>(screenSize_.width + 1));

    for (unsigned lineNr = 0; lineNr < historyLineCount() + static_cast<unsigned>(screenSize_.height); ++lineNr)
    {
        text += renderTextLineAbsolute(lineNr);
        text += '\n';
    }

    return text;
}

string Grid::renderText() const
{
    string text;
    text.reserve(
        static_cast<uint16_t>(screenSize_.height) *
        static_cast<uint16_t>(screenSize_.width + 1)
    );

    for (int lineNr = 1; lineNr <= screenSize_.height; ++lineNr)
    {
        text += renderTextLine(lineNr);
        text += '\n';
    }

    return text;
}
// }}}

}
