// SPDX-License-Identifier: Apache-2.0
#include <vtbackend/Screen.h>
#include <vtbackend/Selector.h>
#include <vtbackend/Terminal.h>

#include <crispy/times.h>

#include <cassert>

using namespace std;

namespace vtbackend
{

namespace // {{{ helper
{

    tuple<vector<Selection::Range>, CellLocation const, CellLocation const> prepare(
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
bool Selection::extend(CellLocation to)
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

CellLocation Selection::stretchedColumn(SelectionHelper const& gridHelper, CellLocation coord) noexcept
{
    CellLocation stretched = coord;
    if (auto const w = gridHelper.cellWidth(coord); w > 1) // wide character
    {
        stretched.column += ColumnOffset::cast_from(w - 1);
        return stretched;
    }

    return stretched;
}

bool Selection::contains(CellLocation coord) const noexcept
{
    return crispy::ascending(_from, coord, _to) || crispy::ascending(_to, coord, _from);
}

bool Selection::containsLine(LineOffset line) const noexcept
{
    return crispy::ascending(_from.line, line, _to.line) || crispy::ascending(_to.line, line, _from.line);
}

bool Selection::intersects(Rect area) const noexcept
{
    // TODO: make me more efficient
    for (auto line = area.top.as<LineOffset>(); line <= area.bottom.as<LineOffset>(); ++line)
        for (auto col = area.left.as<ColumnOffset>(); col <= area.right.as<ColumnOffset>(); ++col)
            if (contains({ .line = line, .column = col }))
                return true;

    return false;
}

std::vector<Selection::Range> Selection::ranges() const
{
    auto [result, from, to] = prepare(*this);

    auto const rightMargin = boxed_cast<ColumnOffset>(_helper.pageSize().columns - 1);

    switch (result.size())
    {
        case 1: result[0] = Range { from.line, from.column, min(to.column, rightMargin) }; break;
        case 2:
            // Render first line partial from selected column to end.
            result[0] = Range { from.line, from.column, rightMargin };
            // Render last (second) line partial from beginning to last selected column.
            result[1] = Range { to.line, ColumnOffset(0), min(to.column, rightMargin) };
            break;
        default:
            // Render first line partial from selected column to end.
            result[0] = Range { from.line, from.column, rightMargin };

            // Render inner full.
            for (size_t n = 1; n < result.size(); ++n)
                result[n] = Range { from.line + LineOffset::cast_from(n), ColumnOffset(0), rightMargin };

            // Render last (second) line partial from beginning to last selected column.
            result[result.size() - 1] = Range { to.line, ColumnOffset(0), min(to.column, rightMargin) };
            break;
    }

    return result;
}
// }}}
// {{{ LinearSelection
LinearSelection::LinearSelection(SelectionHelper const& helper,
                                 CellLocation start,
                                 OnSelectionUpdated onSelectionUpdated):
    Selection(helper, ViMode::Visual, start, std::move(onSelectionUpdated))
{
}
// }}}
// {{{ WordWiseSelection
WordWiseSelection::WordWiseSelection(SelectionHelper const& helper,
                                     CellLocation start,
                                     OnSelectionUpdated onSelectionUpdated):
    Selection(helper, ViMode::Visual, start, std::move(onSelectionUpdated))
{
    _from = extendSelectionBackward(_from);
    extend(extendSelectionForward(_to));
}

CellLocation WordWiseSelection::extendSelectionBackward(CellLocation pos) const noexcept
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
            current.column = boxed_cast<ColumnOffset>(_helper.pageSize().columns) - 1;
        }
        else
            break;

        if (_helper.wordDelimited(current))
            break;
        last = current;
    }

    return last;
}

CellLocation WordWiseSelection::extendSelectionForward(CellLocation pos) const noexcept
{
    auto last = pos;
    auto current = last;
    for (;;)
    {
        if (*current.column == *_helper.pageSize().columns - 1
            && *current.line + 1 < *_helper.pageSize().lines && _helper.wrappedLine(current.line))
        {
            current.line++;
            current.column = ColumnOffset(0);
            current = stretchedColumn(_helper, { .line = current.line, .column = current.column + 1 });
        }

        if (*current.column + 1 < *_helper.pageSize().columns)
        {
            current = stretchedColumn(_helper, { .line = current.line, .column = current.column + 1 });
        }
        else if (*current.line + 1 < *_helper.pageSize().lines)
        {
            current.line++;
            current.column = ColumnOffset(0);
        }
        else
            break;

        if (_helper.wordDelimited(current))
            break;
        last = current;
    }

    return stretchedColumn(_helper, last);
}

bool WordWiseSelection::extend(CellLocation to)
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
                                           CellLocation start,
                                           OnSelectionUpdated onSelectionUpdated):
    Selection(helper, ViMode::VisualBlock, start, std::move(onSelectionUpdated))
{
}

bool RectangularSelection::contains(CellLocation coord) const noexcept
{
    auto const [from, to] = orderedPoints(_from, _to);

    return crispy::ascending(from.line, coord.line, to.line)
           && crispy::ascending(from.column, coord.column, to.column);
}

bool RectangularSelection::intersects(Rect area) const noexcept
{
    auto const [from, to] = orderedPoints(_from, _to);

    // selection is above area
    if (to.line < area.top.as<LineOffset>())
        return false;

    // selection is below area
    if (from.line > area.bottom.as<LineOffset>())
        return false;

    // selection starts at area-top
    if (from.line == area.top.as<LineOffset>())
        return area.left.as<ColumnOffset>() <= from.column && from.column <= area.right.as<ColumnOffset>();

    // selection ends at area-top
    if (to.line == area.bottom.as<LineOffset>())
        return area.left.as<ColumnOffset>() <= to.column && to.column <= area.right.as<ColumnOffset>();

    // innser
    return area.top.as<LineOffset>() < from.line && to.line < area.bottom.as<LineOffset>();
}

vector<Selection::Range> RectangularSelection::ranges() const
{
    auto const [from, to] = orderedPoints(_from, _to);

    vector<Selection::Range> result;
    auto const numLines = to.line - from.line + 1;
    result.resize(numLines.as<size_t>());

    for (size_t i = 0; i < result.size(); ++i)
    {
        auto const line = from.line + LineOffset::cast_from(i);
        auto const left = from.column;
        auto const right =
            stretchedColumn(_helper, CellLocation { .line = line, .column = to.column }).column;
        result[i] = Range { line, left, right };
    }

    return result;
}
// }}}
// {{{ FullLineSelection
FullLineSelection::FullLineSelection(SelectionHelper const& helper,
                                     CellLocation start,
                                     OnSelectionUpdated onSelectionUpdated):
    Selection(helper, ViMode::VisualLine, start, std::move(onSelectionUpdated))
{
    _from.column = ColumnOffset(0);
    extend(CellLocation { .line = _to.line,
                          .column = boxed_cast<ColumnOffset>(_helper.pageSize().columns - 1) });
}

bool FullLineSelection::extend(CellLocation to)
{
    if (to.line >= _from.line)
    {
        _from.column = ColumnOffset(0);
        to.column = boxed_cast<ColumnOffset>(_helper.pageSize().columns - 1);
        while (_helper.wrappedLine(to.line + 1))
            ++to.line;
    }
    else
    {
        if (to.line < _from.line)
            while (_helper.wrappedLine(_from.line + 1))
                ++_from.line;
        _from.column = boxed_cast<ColumnOffset>(_helper.pageSize().columns - 1);
        to.column = ColumnOffset(0);
        while (_helper.wrappedLine(to.line))
            --to.line;
    }

    return Selection::extend(to);
}
// }}}

} // namespace vtbackend
