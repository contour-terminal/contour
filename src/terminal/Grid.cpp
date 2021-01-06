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

#include <algorithm>
#include <iostream>
#include <optional>
#include <utility>

using std::string;
using std::optional;
using std::min;
using std::rotate;
using std::for_each;
using std::generate_n;
using crispy::Comparison;

#if defined(LIBTERMINAL_EXECUTION_PAR)
#include <execution>
#define LIBTERMINAL_EXECUTION_COMMA(par) (std::execution:: par),
#else
#define LIBTERMINAL_EXECUTION_COMMA(par) /*!*/
#endif

namespace terminal {

namespace // {{{ helper
{
    bool isspace(Cell const& _cell)
    {
        return _cell.codepointCount() == 1
            && _cell.codepoint(0) == 0x20;
    }
}
// }}}
// {{{ Cell impl
string Cell::toUtf8() const
{
    return unicode::to_utf8(codepoints_.data(), codepointCount_);
}
// }}}
// {{{ Line impl
Line::Line(int _numCols, LineBuffer const& _init)
    : buffer_{_init}
{
    buffer_.resize(static_cast<int>(_numCols));
}

void Line::resize(int _size)
{
    if (_size >= 0)
        buffer_.resize(static_cast<int>(_size));
}

Line::LineBuffer Line::reflow(int _column)
{
    LineBuffer wrappedColumns;

    switch (crispy::strongCompare(_column, size()))
    {
        case Comparison::Equal:
            break;
        case Comparison::Greater:
            buffer_.resize(_column);
            break;
        case Comparison::Less:
        {
            // TODO: properly handle wide cells
            // - when cutting in the middle of a wide char, the wide char gets wrapped and an empty
            //   cell needs to be injected to match the expected column width.

            auto const oldSize = size();

            auto const i = _column - 1; //- buffer_[_column].width();
            auto a = next(buffer_.begin(), i);
            wrappedColumns.insert(wrappedColumns.end(), a, buffer_.end());
            buffer_.erase(a, buffer_.end());

            setWrapped(!wrappedColumns.empty());

            // truncate empty (non-reserved) cells
            while (!wrappedColumns.empty())
            {
                Cell const& lastColumn = wrappedColumns.back();
                if (lastColumn.codepointCount() != 0 && !isspace(lastColumn))
                    break;
                if (wrappedColumns.size() >= 2 && wrappedColumns[wrappedColumns.size() - 2].width() == 2)
                    break;
                wrappedColumns.pop_back();
            }

            if (wrappedColumns.size())
            {
                std::cout << fmt::format("reflow(): from {} to {}, wrap {}", oldSize, _column, wrappedColumns.size());
                for (auto const& c : wrappedColumns)
                    std::cout << fmt::format(" {}", c.toUtf8());
                std::cout << '\n';
                // std::cout << fmt::format("Line.reflow({}->{}): (cutoff: {} -> {}: {})\n",
                //                          oldSize, _column, ca, cb, cd);
            }
            break;
        }
    }

    return wrappedColumns;
}
// }}}
// {{{ Grid impl
Grid::Grid(Size _screenSize, bool _reflowOnResize, optional<int> _maxHistoryLineCount) :
    screenSize_{ _screenSize },
    reflowOnResize_{ _reflowOnResize },
    maxHistoryLineCount_{ _maxHistoryLineCount },
    lines_(
        static_cast<size_t>(_screenSize.height),
        Line(static_cast<size_t>(_screenSize.width), Cell{})
    )
{
}

void Grid::setMaxHistoryLineCount(optional<int> _maxHistoryLineCount)
{
    maxHistoryLineCount_ = _maxHistoryLineCount;
    clampHistory();
}

Coordinate Grid::resize(Size _newSize, Coordinate _currentCursorPos, bool _wrapPending)
{
    auto const growLines = [this](int _newHeight) -> Coordinate {
        // Grow line count by splicing available lines from history back into buffer, if available,
        // or create new ones until screenSize_.height == _newHeight.

        auto const extendCount = _newHeight - screenSize_.height;
        auto const rowsToTakeFromSavedLines = min(extendCount, historyLineCount());
        auto const fillLineCount = extendCount - rowsToTakeFromSavedLines;

        assert(rowsToTakeFromSavedLines >= 0);
        assert(fillLineCount >= 0);

        generate_n(
            back_inserter(lines_),
            fillLineCount,
            [=]() { return Line(screenSize_.width, Cell{}); }
        );

        screenSize_.height = _newHeight;

        return Coordinate{rowsToTakeFromSavedLines, 0};
    };

    auto const shrinkLines = [this](int _newHeight, Coordinate _cursor) -> Coordinate {
        // Shrink existing line count to _newSize.height
        // by splicing the number of lines to be shrinked by into savedLines bottom.

        if (_cursor.row == screenSize_.height)
        {
            auto const shrinkedLinesCount = screenSize_.height - _newHeight;
            screenSize_.height = _newHeight;
            clampHistory();
            return Coordinate{shrinkedLinesCount, 0};
        }
        else
        {
            // Hard-cut below cursor by the number of lines to shrink.
            lines_.resize(historyLineCount() + _newHeight);
            screenSize_.height = _newHeight;
            return Coordinate{0, 0};
        }
    };

    auto const growColumns = [this, _wrapPending](int _newColumnCount, Coordinate _cursor) -> Coordinate {
        for (Line& line : lines_)
            if (static_cast<int>(line.size()) < _newColumnCount)
                line.resize(_newColumnCount);
        screenSize_.width = _newColumnCount;
        return _cursor + Coordinate{0, _wrapPending ? 1 : 0};
    };

    auto const shrinkColumns = [this](int _newColumnCount, Coordinate _cursor) -> Coordinate {
        screenSize_.width = _newColumnCount;
        return _cursor + Coordinate{0, min(_cursor.column, _newColumnCount)};
    };

    Coordinate cursorPosition = _currentCursorPos;

    // grow/shrink columns
    switch (crispy::strongCompare(_newSize.width, screenSize_.width))
    {
        case Comparison::Greater:
            cursorPosition = growColumns(_newSize.width, cursorPosition);
            break;
        case Comparison::Less:
            cursorPosition = shrinkColumns(_newSize.width, cursorPosition);
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

void Grid::clearHistory()
{
    if (historyLineCount())
        lines_.erase(begin(lines_), next(begin(lines_), historyLineCount()));
}

void Grid::clampHistory()
{
    if (maxHistoryLineCount_.has_value())
    {
        auto const actual = historyLineCount();
        auto const expected = maxHistoryLineCount_.value();
        auto const garbage = actual > expected ? actual - expected : 0;
        lines_.erase(begin(lines_), next(begin(lines_), garbage));
    }
}

void Grid::scrollUp(int _n, GraphicsAttributes const& _defaultAttributes, Margin const& _margin)
{
    if (_margin.horizontal != Margin::Range{1, screenSize_.width})
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
    else if (_margin.vertical == Margin::Range{1, screenSize_.height})
    {
        if (auto const n = min(_n, screenSize_.height); n > 0)
        {
            generate_n(
                back_inserter(lines_),
                n,
                [&]() { return Line(screenSize_.width, Cell{{}, _defaultAttributes}); }
            );
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

void Grid::scrollDown(int v_n, GraphicsAttributes const& _defaultAttributes, Margin const& _margin)
{
    auto const marginHeight = _margin.vertical.length();
    auto const n = min(v_n, marginHeight);

    if (_margin.horizontal != Margin::Range{1, screenSize_.width})
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
    else if (_margin.vertical == Margin::Range{1, screenSize_.height})
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
// }}}

}
