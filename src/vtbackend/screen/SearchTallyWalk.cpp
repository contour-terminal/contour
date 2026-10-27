// SPDX-License-Identifier: Apache-2.0
#include <vtbackend/screen/SearchTallyWalk.hpp>

#include <vtbackend/grid/Grid.hpp>

#include <algorithm>
#include <utility>

namespace vtbackend
{

SearchTallyWalk::SearchTallyWalk(std::shared_ptr<SearchQuery const> query,
                                 Grid const& grid,
                                 CellLocation position,
                                 size_t limit):
    _query { std::move(query) }, _position { position }, _limit { limit }
{
    restart(grid);
}

void SearchTallyWalk::restart(Grid const& grid)
{
    _idGeneration = grid.stableIdGeneration();
    _nextRow = grid.stableLineIdOf(-boxed_cast<LineOffset>(grid.historyLineCount()));
    _lastRow = grid.stableLineIdOf(boxed_cast<LineOffset>(grid.pageSize().lines) - 1);
    _stablePosition = StableCell { .row = grid.stableLineIdOf(_position.line), .column = _position.column };
    _tally = SearchMatchTally {};
    _nearest.reset();
    _nearestOrdinal = 0;
    _last.reset();
    _progress = _limit == 0 ? WalkProgress::Finished : WalkProgress::Running;
}

std::expected<WalkProgress, SearchPatternError> SearchTallyWalk::step(Grid const& grid, LineCount budget)
{
    _rowsLastStep = LineCount(0);
    if (grid.stableIdGeneration() != _idGeneration)
        restart(grid);
    if (_progress == WalkProgress::Finished)
        return _progress;

    // A regex is fed about one segment per step, whatever the caller allows: its cost grows faster than its
    // subject, and the step runs under the terminal's lock.
    budget = _query->boundedRows(budget, grid.pageSize().columns);

    // The segment just read: the next one is found from it without walking back to its line's head, so a
    // step looks a head up once, where it begins, however long the line.
    auto previous = std::optional<SearchSegment> {};
    while (_progress == WalkProgress::Running && (_rowsLastStep < budget || _rowsLastStep == LineCount(0)))
    {
        // Rows evicted before the walk reached them are gone, and so are their matches.
        _nextRow = std::max(_nextRow, grid.stableRangeFloor());
        auto const row = grid.lineOffsetOf(_nextRow);
        if (_nextRow > _lastRow || !row)
        {
            _progress = WalkProgress::Finished;
            break;
        }

        // Between steps the history's top may have passed this line's head, which moves its cuts: the
        // segment holding the next row may then begin above it (rows read by the step before, filtered
        // below), and a regex match across the old cut, which neither cut reads whole, is not counted.
        auto const segment = previous ? *nextSearchSegment(grid, *previous, _query->segmentCells())
                                      : segmentFromHead(grid, *row);
        previous = segment;
        auto const matches = _query->matchesIn(grid, segment);
        if (!matches)
            return std::unexpected { matches.error() };

        for (auto const& match: *matches)
        {
            auto const cell =
                StableCell { .row = grid.stableLineIdOf(match.first.line), .column = match.first.column };
            // A segment's rows above the next row were read by the step before (its cuts move with the
            // history's top), and its rows below the last one were appended since the walk began: neither is
            // this walk's.
            if (cell.row < _nextRow || cell.row > _lastRow)
                continue;
            if (_tally.total == _limit)
            {
                // A match exists beyond the limit, so the count is a floor rather than a total.
                _tally.exactness = TallyExactness::Capped;
                _progress = WalkProgress::Finished;
                return _progress;
            }
            ++_tally.total;
            if (cell == _stablePosition)
                _tally.ordinal = _tally.total;
            if (cell <= _stablePosition)
            {
                _nearest = cell;
                _nearestOrdinal = _tally.total;
            }
            _last = cell;
        }

        _rowsLastStep += boxed_cast<LineCount>(segment.last - *row) + LineCount(1);
        _nextRow = grid.stableLineIdOf(segment.last) + 1;
        if (_nextRow > _lastRow)
            _progress = WalkProgress::Finished;
    }
    return _progress;
}

SearchSegment SearchTallyWalk::segmentFromHead(Grid const& grid, LineOffset row)
{
    ++_headLookups;
    return searchSegmentAt(grid, row, _query->segmentCells());
}

std::optional<CellLocation> SearchTallyWalk::locate(Grid const& grid, std::optional<StableCell> cell) noexcept
{
    if (!cell)
        return std::nullopt;
    auto const row = grid.lineOffsetOf(cell->row);
    if (!row)
        return std::nullopt;
    return CellLocation { .line = *row, .column = cell->column };
}

std::optional<CellLocation> SearchTallyWalk::nearestAtOrBefore(Grid const& grid) const
{
    if (grid.stableIdGeneration() != _idGeneration)
        return std::nullopt;
    return locate(grid, _nearest);
}

std::optional<CellLocation> SearchTallyWalk::positionIn(Grid const& grid) const
{
    if (grid.stableIdGeneration() != _idGeneration)
        return std::nullopt;
    return locate(grid, _stablePosition);
}

std::optional<CellLocation> SearchTallyWalk::lastMatch(Grid const& grid) const
{
    if (grid.stableIdGeneration() != _idGeneration)
        return std::nullopt;
    return locate(grid, _last);
}

} // namespace vtbackend
