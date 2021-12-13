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
#include <terminal/Selector.h>
#include <terminal/Screen.h>
#include <terminal/Terminal.h>
#include <crispy/times.h>
#include <cassert>

using namespace std;

namespace terminal {

namespace // {{{ helper
{

tuple<vector<Selection::Range>, Coordinate const, Coordinate const> prepare(Selection const& _selection)
{
    vector<Selection::Range> result;

    auto const [from, to] = [&]() {
        if (_selection.from() <= _selection.to())
            return pair{_selection.from(), _selection.to()};
        else
            return pair{_selection.to(), _selection.from()};
    }();

    auto const numLines = to.line - from.line + 1;
    result.resize(numLines.as<size_t>());

    return {move(result), from, to};
}

// Constructs a top-left and bottom-right coordinate-pair from given input.
constexpr pair<Coordinate, Coordinate> orderedPoints(Coordinate a, Coordinate b) noexcept
{
    auto const topLeft = Coordinate{
        min(a.line, b.line),
        min(a.column, b.column)
    };
    auto const bottomRight = Coordinate{
        max(a.line, b.line),
        max(a.column, b.column)
    };
    return pair{topLeft, bottomRight};
}

} // }}}

// {{{ Selection
void Selection::extend(Coordinate _to)
{
    assert(state_ != State::Complete && "In order extend a selection, the selector must be active (started).");
    state_ = State::InProgress;
    to_ = _to;
}

void Selection::complete()
{
    if (state_ == State::InProgress)
        state_ = State::Complete;
}

Coordinate Selection::stretchedColumn(SelectionHelper const& _grid, Coordinate _coord) noexcept
{
    Coordinate stretched = _coord;
    if (auto const w = _grid.cellWidth(_coord); w > 1) // wide character
    {
        stretched.column += ColumnOffset::cast_from(w - 1);
        return stretched;
    }

    auto const pageWidth = _grid.pageSize().columns;
    while (*stretched.column + 1 < *pageWidth)
    {
        if (_grid.cellEmpty(stretched))
            stretched.column++;
        else
        {
            if (auto const w = _grid.cellWidth(stretched); w > 1)
                stretched.column += ColumnOffset::cast_from(w - 1);
            break;
        }
    }

    return stretched;
}

bool Selection::contains(Coordinate _coord) const noexcept
{
    return crispy::ascending(from_, _coord, to_)
        || crispy::ascending(to_, _coord, from_);
}

bool Selection::intersects(Rect _area) const noexcept
{
    // TODO: make me more efficient
    for (auto line = _area.top.as<LineOffset>(); line <= _area.bottom.as<LineOffset>(); ++line)
        for (auto col = _area.left.as<ColumnOffset>(); col <= _area.right.as<ColumnOffset>(); ++col)
            if (contains({line, col}))
                return true;

    return false;
}

std::vector<Selection::Range> Selection::ranges() const
{
    auto [result, from, to] = prepare(*this);
    auto const rightMargin = boxed_cast<ColumnOffset>(helper_.pageSize().columns - 1);

    switch (result.size())
    {
        case 1:
            result[0] = Range{from.line, from.column, min(to.column, rightMargin)};
            break;
        case 2:
            // Render first line partial from selected column to end.
            result[0] = Range{from.line, from.column, rightMargin};
            // Render last (second) line partial from beginning to last selected column.
            result[1] = Range{to.line, ColumnOffset(0), min(to.column, rightMargin)};
            break;
        default:
            // Render first line partial from selected column to end.
            result[0] = Range{from.line, from.column, rightMargin};

            // Render inner full.
            for (size_t n = 1; n < result.size(); ++n)
                result[n] = Range{from.line + LineOffset::cast_from(n), ColumnOffset(0), rightMargin};

            // Render last (second) line partial from beginning to last selected column.
            result[result.size() - 1] = Range{to.line, ColumnOffset(0), min(to.column, rightMargin)};
            break;
    }

    return result;
}
// }}}
// {{{ LinearSelection
LinearSelection::LinearSelection(SelectionHelper const& _helper, Coordinate _start):
    Selection(_helper, _start)
{
}
// }}}
// {{{ WordWiseSelection
WordWiseSelection::WordWiseSelection(SelectionHelper const& _helper, Coordinate _start):
    Selection(_helper, _start)
{
    from_ = extendSelectionBackward(from_);
    to_ = extendSelectionForward(to_);
}

Coordinate WordWiseSelection::extendSelectionBackward(Coordinate _pos) const noexcept
{
    auto last = _pos;
    auto current = last;
    for (;;) {
        auto const wrapIntoPreviousLine = *current.column == 0 && *current.line > 0 && helper_.wrappedLine(current.line);
        if (*current.column > 0)
            current.column--;
        else if (*current.line > 0 || wrapIntoPreviousLine)
        {
            current.line--;
            current.column = boxed_cast<ColumnOffset>(helper_.pageSize().columns) - 1;
        }
        else
            break;

        if (helper_.wordDelimited(current))
            break;
        last = current;
    }

    return last;
}

Coordinate WordWiseSelection::extendSelectionForward(Coordinate _pos) const noexcept
{
    auto last = _pos;
    auto current = last;
    for (;;) {
        if (*current.column == *helper_.pageSize().columns - 1 &&
            *current.line + 1 < *helper_.pageSize().lines &&
            helper_.wrappedLine(current.line))
        {
            current.line++;
            current.column = ColumnOffset(0);
            current = stretchedColumn(helper_, {current.line, current.column + 1});
        }

        if (*current.column + 1 < *helper_.pageSize().columns)
        {
            current = stretchedColumn(helper_, {current.line, current.column + 1});
        }
        else if (*current.line + 1 < *helper_.pageSize().lines)
        {
            current.line++;
            current.column = ColumnOffset(0);
        }
        else
            break;

        if (helper_.wordDelimited(current))
            break;
        last = current;
    }

    return stretchedColumn(helper_, last);
}

void WordWiseSelection::extend(Coordinate _to)
{
    if (_to >= from_) // extending to the right
    {
        from_ = extendSelectionBackward(from_);
        Selection::extend(extendSelectionForward(_to));
    }
    else // extending to the left
    {
        from_ = extendSelectionForward(from_);
        Selection::extend(extendSelectionBackward(_to));
    }
}
// }}}
// {{{ RectangularSelection
RectangularSelection::RectangularSelection(SelectionHelper const& _helper, Coordinate _start):
    Selection(_helper, _start)
{
}

bool RectangularSelection::contains(Coordinate _coord) const noexcept
{
    auto const [from, to] = orderedPoints(from_, to_);

    return crispy::ascending(from.line, _coord.line, to.line)
        && crispy::ascending(from.column, _coord.column, to.column);
}

bool RectangularSelection::intersects(Rect _area) const noexcept
{
    auto const [from, to] = orderedPoints(from_, to_);

    // selection is above area
    if (to.line < _area.top.as<LineOffset>())
        return false;

    // selection is below area
    if (from.line > _area.bottom.as<LineOffset>())
        return false;

    // selection starts at area-top
    if (from.line == _area.top.as<LineOffset>())
        return _area.left.as<ColumnOffset>() <= from.column &&
               from.column <= _area.right.as<ColumnOffset>();

    // selection ends at area-top
    if (to.line == _area.bottom.as<LineOffset>())
        return _area.left.as<ColumnOffset>() <= to.column &&
               to.column <= _area.right.as<ColumnOffset>();

    // innser
    return _area.top.as<LineOffset>() < from.line &&
           to.line < _area.bottom.as<LineOffset>();
}

vector<Selection::Range> RectangularSelection::ranges() const
{
    auto const [from, to] = orderedPoints(from_, to_);

    vector<Selection::Range> result;
    auto const numLines = to.line - from.line + 1;
    result.resize(numLines.as<size_t>());

    for (size_t i = 0; i < result.size(); ++i)
    {
        auto const line = from.line + LineOffset::cast_from(i);
        auto const left = from.column;
        auto const right= stretchedColumn(helper_, Coordinate{line, to.column}).column;
        result[i] = Range{line, left, right};
    }

    return result;
}
// }}}
// {{{ FullLineSelection
FullLineSelection::FullLineSelection(SelectionHelper const& _helper, Coordinate _start):
    Selection(_helper, _start)
{
    from_.column = ColumnOffset(0);
    to_.column = boxed_cast<ColumnOffset>(helper_.pageSize().columns - 1);
}

void FullLineSelection::extend(Coordinate _to)
{
    if (_to >= from_)
    {
        from_.column = ColumnOffset(0);
        _to.column = boxed_cast<ColumnOffset>(helper_.pageSize().columns - 1);
        while (helper_.wrappedLine(_to.line + 1))
            ++_to.line;
    }
    else
    {
        from_.column = boxed_cast<ColumnOffset>(helper_.pageSize().columns - 1);
        _to.column = ColumnOffset(0);
        while (helper_.wrappedLine(_to.line))
            --_to.line;
    }

    Selection::extend(_to);
}
// }}}

} // namespace terminal
