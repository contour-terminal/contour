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
#include <vtbackend/Screen.h>
#include <vtbackend/Selector.h>
#include <vtbackend/Terminal.h>

#include <crispy/times.h>

#include <cassert>

using namespace std;

namespace terminal
{

namespace // {{{ helper
{

    tuple<vector<Selection::Range>, cell_location const, cell_location const> prepare(
        Selection const& selection)
    {
        vector<Selection::Range> result;

        auto const [from, to] = [&]() {
            if (selection.from() <= selection.to())
                return pair { selection.from(), selection.to() };
            else
                return pair { selection.to(), selection.from() };
        }();

        auto const numLines = to.line - from.line + 1;
        result.resize(numLines.as<size_t>());

        return { std::move(result), from, to };
    }
} // namespace
// }}}

// {{{ Selection
bool Selection::extend(cell_location to)
{
    assert(_state != State::Complete
           && "In order extend a selection, the selector must be active (started).");
    _state = State::InProgress;
    _to = to;
    _onSelectionUpdated();
    return true;
}

void Selection::complete()
{
    if (_state == State::InProgress)
        _state = State::Complete;
}

cell_location Selection::stretchedColumn(SelectionHelper const& gridHelper, cell_location coord) noexcept
{
    cell_location stretched = coord;
    if (auto const w = gridHelper.cellWidth(coord); w > 1) // wide character
    {
        stretched.column += column_offset::cast_from(w - 1);
        return stretched;
    }

    return stretched;
}

bool Selection::contains(cell_location coord) const noexcept
{
    return crispy::ascending(_from, coord, _to) || crispy::ascending(_to, coord, _from);
}

bool Selection::containsLine(line_offset line) const noexcept
{
    return crispy::ascending(_from.line, line, _to.line) || crispy::ascending(_to.line, line, _from.line);
}

bool Selection::intersects(rect area) const noexcept
{
    // TODO: make me more efficient
    for (auto line = area.top.as<line_offset>(); line <= area.bottom.as<line_offset>(); ++line)
        for (auto col = area.left.as<column_offset>(); col <= area.right.as<column_offset>(); ++col)
            if (contains({ line, col }))
                return true;

    return false;
}

std::vector<Selection::Range> Selection::ranges() const
{
    auto [result, from, to] = prepare(*this);

    auto const rightMargin = boxed_cast<column_offset>(_helper.pageSize().columns - 1);

    switch (result.size())
    {
        case 1: result[0] = Range { from.line, from.column, min(to.column, rightMargin) }; break;
        case 2:
            // Render first line partial from selected column to end.
            result[0] = Range { from.line, from.column, rightMargin };
            // Render last (second) line partial from beginning to last selected column.
            result[1] = Range { to.line, column_offset(0), min(to.column, rightMargin) };
            break;
        default:
            // Render first line partial from selected column to end.
            result[0] = Range { from.line, from.column, rightMargin };

            // Render inner full.
            for (size_t n = 1; n < result.size(); ++n)
                result[n] = Range { from.line + line_offset::cast_from(n), column_offset(0), rightMargin };

            // Render last (second) line partial from beginning to last selected column.
            result[result.size() - 1] = Range { to.line, column_offset(0), min(to.column, rightMargin) };
            break;
    }

    return result;
}
// }}}
// {{{ LinearSelection
LinearSelection::LinearSelection(SelectionHelper const& helper,
                                 cell_location start,
                                 OnSelectionUpdated onSelectionUpdated):
    Selection(helper, vi_mode::Visual, start, std::move(onSelectionUpdated))
{
}
// }}}
// {{{ WordWiseSelection
WordWiseSelection::WordWiseSelection(SelectionHelper const& helper,
                                     cell_location start,
                                     OnSelectionUpdated onSelectionUpdated):
    Selection(helper, vi_mode::Visual, start, std::move(onSelectionUpdated))
{
    _from = extendSelectionBackward(_from);
    extend(extendSelectionForward(_to));
}

cell_location WordWiseSelection::extendSelectionBackward(cell_location pos) const noexcept
{
    auto last = pos;
    auto current = last;
    for (;;)
    {
        auto const wrapIntoPreviousLine =
            *current.column == 0 && *current.line > 0 && _helper.wrappedLine(current.line);
        if (*current.column > 0)
            current.column--;
        else if (*current.line > 0 || wrapIntoPreviousLine)
        {
            current.line--;
            current.column = boxed_cast<column_offset>(_helper.pageSize().columns) - 1;
        }
        else
            break;

        if (_helper.wordDelimited(current))
            break;
        last = current;
    }

    return last;
}

cell_location WordWiseSelection::extendSelectionForward(cell_location pos) const noexcept
{
    auto last = pos;
    auto current = last;
    for (;;)
    {
        if (*current.column == *_helper.pageSize().columns - 1
            && *current.line + 1 < *_helper.pageSize().lines && _helper.wrappedLine(current.line))
        {
            current.line++;
            current.column = column_offset(0);
            current = stretchedColumn(_helper, { current.line, current.column + 1 });
        }

        if (*current.column + 1 < *_helper.pageSize().columns)
        {
            current = stretchedColumn(_helper, { current.line, current.column + 1 });
        }
        else if (*current.line + 1 < *_helper.pageSize().lines)
        {
            current.line++;
            current.column = column_offset(0);
        }
        else
            break;

        if (_helper.wordDelimited(current))
            break;
        last = current;
    }

    return stretchedColumn(_helper, last);
}

bool WordWiseSelection::extend(cell_location to)
{
    if (to >= _from) // extending to the right
    {
        _from = extendSelectionBackward(_from);
        return Selection::extend(extendSelectionForward(to));
    }
    else // extending to the left
    {
        _from = extendSelectionForward(_from);
        return Selection::extend(extendSelectionBackward(to));
    }
}
// }}}
// {{{ RectangularSelection
RectangularSelection::RectangularSelection(SelectionHelper const& helper,
                                           cell_location start,
                                           OnSelectionUpdated onSelectionUpdated):
    Selection(helper, vi_mode::VisualBlock, start, std::move(onSelectionUpdated))
{
}

bool RectangularSelection::contains(cell_location coord) const noexcept
{
    auto const [from, to] = orderedPoints(_from, _to);

    return crispy::ascending(from.line, coord.line, to.line)
           && crispy::ascending(from.column, coord.column, to.column);
}

bool RectangularSelection::intersects(rect area) const noexcept
{
    auto const [from, to] = orderedPoints(_from, _to);

    // selection is above area
    if (to.line < area.top.as<line_offset>())
        return false;

    // selection is below area
    if (from.line > area.bottom.as<line_offset>())
        return false;

    // selection starts at area-top
    if (from.line == area.top.as<line_offset>())
        return area.left.as<column_offset>() <= from.column && from.column <= area.right.as<column_offset>();

    // selection ends at area-top
    if (to.line == area.bottom.as<line_offset>())
        return area.left.as<column_offset>() <= to.column && to.column <= area.right.as<column_offset>();

    // innser
    return area.top.as<line_offset>() < from.line && to.line < area.bottom.as<line_offset>();
}

vector<Selection::Range> RectangularSelection::ranges() const
{
    auto const [from, to] = orderedPoints(_from, _to);

    vector<Selection::Range> result;
    auto const numLines = to.line - from.line + 1;
    result.resize(numLines.as<size_t>());

    for (size_t i = 0; i < result.size(); ++i)
    {
        auto const line = from.line + line_offset::cast_from(i);
        auto const left = from.column;
        auto const right = stretchedColumn(_helper, cell_location { line, to.column }).column;
        result[i] = Range { line, left, right };
    }

    return result;
}
// }}}
// {{{ FullLineSelection
FullLineSelection::FullLineSelection(SelectionHelper const& helper,
                                     cell_location start,
                                     OnSelectionUpdated onSelectionUpdated):
    Selection(helper, vi_mode::VisualLine, start, std::move(onSelectionUpdated))
{
    _from.column = column_offset(0);
    extend(cell_location { _to.line, boxed_cast<column_offset>(_helper.pageSize().columns - 1) });
}

bool FullLineSelection::extend(cell_location to)
{
    if (to.line >= _from.line)
    {
        _from.column = column_offset(0);
        to.column = boxed_cast<column_offset>(_helper.pageSize().columns - 1);
        while (_helper.wrappedLine(to.line + 1))
            ++to.line;
    }
    else
    {
        if (to.line < _from.line)
            while (_helper.wrappedLine(_from.line + 1))
                ++_from.line;
        _from.column = boxed_cast<column_offset>(_helper.pageSize().columns - 1);
        to.column = column_offset(0);
        while (_helper.wrappedLine(to.line))
            --to.line;
    }

    return Selection::extend(to);
}
// }}}

} // namespace terminal
