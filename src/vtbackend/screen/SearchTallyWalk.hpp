// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <vtbackend/core/Primitives.hpp>
#include <vtbackend/core/Search.hpp>
#include <vtbackend/screen/SearchQuery.hpp>

#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <optional>

namespace vtbackend
{

class Grid;

/// How many rows one step of a search walk reads at least, of whole segments: at any width, 500 rows of
/// cells.
inline constexpr auto SearchWalkStepLines = LineCount(500);

/// Whether a search walk has more to read.
enum class WalkProgress : uint8_t
{
    Running = 0, //!< Rows remain; step() again.
    Finished,    //!< The walk passed its last row, or hit its limit; the tally is final.
};

/// Counts a query's matches over a grid a bounded number of rows at a time, so that the caller can release
/// the terminal's lock and yield to the event loop between steps.
///
/// Survives what the grid does between steps: it addresses rows by stable id, so output that scrolls rows
/// away moves nothing it has read or has yet to read; rows evicted before it reached them are skipped; rows
/// appended after it began are the next walk's; and a reflow, which destroys row identity, restarts it from
/// the top.
class SearchTallyWalk
{
  public:
    /// Begins a walk over @p grid, as it is now.
    /// @param query    The compiled pattern; not null.
    /// @param grid     The grid to walk.
    /// @param position Reported back as SearchMatchTally::ordinal when a match starts there.
    /// @param limit    Stop after this many matches and report TallyExactness::Capped.
    SearchTallyWalk(std::shared_ptr<SearchQuery const> query,
                    Grid const& grid,
                    CellLocation position,
                    size_t limit = DefaultSearchMatchLimit);

    /// Reads whole segments, from where the last step stopped, until at least @p budget rows are read or the
    /// walk's last row is passed. A segment is never split across steps.
    /// @param grid   The grid the walk began on, as it is now.
    /// @param budget The rows to read at least, unless the walk ends first.
    /// @return Whether rows remain; SearchPatternError::TooComplex where the regex engine gave up.
    [[nodiscard]] std::expected<WalkProgress, SearchPatternError> step(
        Grid const& grid, LineCount budget = SearchWalkStepLines);

    /// @return The matches counted so far; @c ordinal is set once the walk has passed a match at the
    /// position.
    [[nodiscard]] SearchMatchTally tally() const noexcept { return _tally; }

    /// @return The last match the walk found at or before the position, where it still is in @p grid.
    [[nodiscard]] std::optional<CellLocation> nearestAtOrBefore(Grid const& grid) const;

    /// @return The 1-based ordinal of the match nearestAtOrBefore() names; 0 while there is none.
    [[nodiscard]] size_t nearestAtOrBeforeOrdinal() const noexcept { return _nearestOrdinal; }

    /// @return The last match the walk found, where it still is in @p grid.
    [[nodiscard]] std::optional<CellLocation> lastMatch(Grid const& grid) const;

    /// @return The position it was begun at, where that cell is in @p grid now: output that scrolls the grid
    ///         moves it with its row; nothing once its row is evicted or row identity is lost.
    [[nodiscard]] std::optional<CellLocation> positionIn(Grid const& grid) const;

    /// @return How many segments the walk has found by walking back to their line's head: at most one per
    ///         step, the rest found from the segment before them. A cost, read by the tests.
    [[nodiscard]] size_t headLookups() const noexcept { return _headLookups; }

    /// @return How many rows the last step read.
    [[nodiscard]] LineCount rowsLastStep() const noexcept { return _rowsLastStep; }

    /// @return Whether the walk is done.
    [[nodiscard]] WalkProgress progress() const noexcept { return _progress; }

    /// @return The query being counted.
    [[nodiscard]] std::shared_ptr<SearchQuery const> const& query() const noexcept { return _query; }

    /// Starts over from @p grid's top, with an empty tally: the rows read so far no longer exist as such, or
    /// the walk moves to another grid (the terminal's other page). Keeps the query, the position and the
    /// limit.
    void restart(Grid const& grid);

  private:
    /// A cell named by its row's stable id, so it stays put while rows scroll.
    struct StableCell
    {
        int64_t row = 0;
        ColumnOffset column {};
        auto operator<=>(StableCell const&) const = default;
    };

    /// The segment holding @p row, found by walking back to its line's head: counted. @see headLookups
    [[nodiscard]] SearchSegment segmentFromHead(Grid const& grid, LineOffset row);

    [[nodiscard]] static std::optional<CellLocation> locate(Grid const& grid,
                                                            std::optional<StableCell> cell) noexcept;

    std::shared_ptr<SearchQuery const> _query;
    CellLocation _position;
    size_t _limit;
    uint64_t _idGeneration = 0;    ///< The grid's stableIdGeneration() the ids below belong to.
    int64_t _nextRow = 0;          ///< The stable id of the next row to read.
    int64_t _lastRow = 0;          ///< The stable id of the walk's last row: the bottom one at its start.
    StableCell _stablePosition {}; ///< @ref _position by stable id.
    SearchMatchTally _tally {};    ///< So far.
    std::optional<StableCell> _nearest {}; ///< The last match at or before the position.
    size_t _nearestOrdinal = 0;            ///< The ordinal of @ref _nearest.
    std::optional<StableCell> _last {};    ///< The last match.
    LineCount _rowsLastStep {};
    size_t _headLookups = 0; ///< @see headLookups
    WalkProgress _progress = WalkProgress::Running;
};

} // namespace vtbackend
