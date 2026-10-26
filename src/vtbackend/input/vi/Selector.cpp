// SPDX-License-Identifier: Apache-2.0
#include <vtbackend/input/vi/Selector.hpp>

#include <vtbackend/grid/WordWalk.hpp>
#include <vtbackend/screen/Screen.hpp>
#include <vtbackend/screen/Terminal.hpp>

#include <core/Times.hpp>

#include <algorithm>
#include <cassert>
#include <cstdlib>
#include <ranges>

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

    /// A selection's cells as a word walk sees them, through the helper that answers for the grid.
    class SelectionWordCells
    {
      public:
        explicit SelectionWordCells(SelectionHelper const& helper): _helper { &helper } {}

        [[nodiscard]] PageSize pageSize() const noexcept { return _helper->pageSize(); }
        [[nodiscard]] bool continuesRowAbove(LineOffset line) const noexcept
        {
            return _helper->wrappedLine(line);
        }
        [[nodiscard]] bool isWordDelimiter(CellLocation position) const
        {
            return _helper->wordDelimited(position);
        }

      private:
        SelectionHelper const* _helper;
    };
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
    return core::ascending(_from, coord, _to) || core::ascending(_to, coord, _from);
}

bool Selection::containsLine(LineOffset line) const noexcept
{
    return core::ascending(_from.line, line, _to.line) || core::ascending(_to.line, line, _from.line);
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

vector<Selection::Range> Selection::ranges() const
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

ColumnCount Selection::rowWidth() const noexcept
{
    return _helper.pageSize().columns;
}

SelectionExtent Selection::extent() const
{
    auto const lines = LineCount::cast_from(std::abs(unbox(_to.line - _from.line)) + 1);
    if (unbox<size_t>(lines) * unbox<size_t>(rowWidth()) >= SelectionCharacterCountLimit)
        return SelectionExtent { .lines = lines, .characters = nullopt };

    auto characters = size_t { 0 };
    for (Range const& range: ranges())
    {
        auto const at = [&](int column) {
            return CellLocation { .line = range.line, .column = ColumnOffset(column) };
        };
        auto const columns = std::views::iota(unbox(range.fromColumn), unbox(range.toColumn) + 1);

        // As a copy does, the row ends at its last non-blank cell: the padding after it is not text.
        auto const blanks =
            std::ranges::distance(columns | std::views::reverse | std::views::take_while([&](int column) {
                                      return _helper.cellEmpty(at(column));
                                  }));

        // A wide character is one character over the cells it covers.
        auto covered = 0;
        for (auto const column: columns | std::views::take(std::ranges::ssize(columns) - blanks))
        {
            if (covered > 0)
            {
                --covered;
                continue;
            }
            ++characters;
            covered = std::max(1, _helper.cellWidth(at(column))) - 1;
        }
    }
    return SelectionExtent { .lines = lines, .characters = characters };
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
    return wordBegin(SelectionWordCells(_helper), pos);
}

CellLocation WordWiseSelection::extendSelectionForward(CellLocation pos) const noexcept
{
    return wordEnd(SelectionWordCells(_helper), pos);
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

    return core::ascending(from.line, coord.line, to.line)
           && core::ascending(from.column, coord.column, to.column);
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

ColumnCount RectangularSelection::rowWidth() const noexcept
{
    return ColumnCount::cast_from(std::abs(unbox(_to.column - _from.column)) + 1);
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
