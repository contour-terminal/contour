// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <vtbackend/core/RowMove.hpp>
#include <vtbackend/shell/StableIdSet.hpp>

#include <cstdint>
#include <set>
#include <span>

namespace vtbackend
{

/// Whether a user mark was placed on a line or taken off it.
///
/// Named rather than a bool: at the call site in Screen::noteUserMarkChange a bare `true` would say nothing
/// about which of the two it reports.
enum class UserMarkChange : uint8_t
{
    Removed = 0, ///< The line lost LineFlag::UserMark.
    Added,       ///< The line gained LineFlag::UserMark.
};

/// A user mark to place on, or take off, the logical line a row of the primary screen belongs to.
///
/// How a daemon mirror's user asks the host for one: a mirror's marks are the host's
/// (Terminal::Events::userMarkRequested), and the host performs the request on its own grid
/// (Terminal::placeUserMark). The row is named by its stable id, which means something only within the
/// grid's stable-id generation (Grid::stableIdGeneration): the generation travels with it, and a request that
/// outlived it is passed over rather than applied to whichever row the id names since.
struct UserMarkRequest
{
    int64_t stableId = 0;                          ///< A row of the logical line, by its stable id.
    uint64_t generation = 0;                       ///< The stable-id generation @ref stableId was taken in.
    UserMarkChange change = UserMarkChange::Added; ///< Whether to place the mark or take it off.
    bool operator==(UserMarkRequest const&) const = default;
};

/// The stable ids of the primary screen's lines that carry LineFlag::UserMark (vi `mm`).
///
/// What lets the scrollbar place a tick per user mark in O(marks) instead of walking the scrollback. A
/// CACHE, never the authority -- the flag on the line is -- kept current by being told of every change to the
/// flag: a mark placed or taken off, by vi `mm`, an erase of its row or a daemon host's rewrite of it
/// (record), and lines moving under their ids (follow). Nothing checks an id against its line on a read.
///
/// Ids mean something only within one Grid::stableIdGeneration(); after a reflow they name other rows. So
/// the index remembers the generation its ids belong to, ignores a change reported for any other, and is
/// rebuilt from one scan of the grid by whoever notices the mismatch. Ids that left the grid are pruned
/// against its addressable range, as FoldState prunes its heads: both hold their ids in a StableIdSet.
///
/// Not synchronised: the Terminal that owns it reads and writes it only under the terminal lock. The parser
/// thread reports a mark output erased or carried along, and the GUI thread one vi `mm` placed, and reads the
/// index for the scrollbar's ticks.
class UserMarkIndex
{
  public:
    /// Records that the logical line headed by @p stableId gained or lost its mark.
    ///
    /// Ignored when @p generation is not the one the index holds: the ids it holds are then already stale,
    /// and the rebuild that replaces them reads the flag this change left on the grid.
    /// @param stableId The head line's stable id.
    /// @param generation The Grid::stableIdGeneration() @p stableId was read in.
    /// @param change Whether the mark was placed or removed.
    void record(int64_t stableId, uint64_t generation, UserMarkChange change)
    {
        if (generation != _generation)
            return;

        if (change == UserMarkChange::Added)
            _ids.insert(stableId);
        else
            _ids.erase(stableId);
    }

    /// Whether the ids held were minted in @p generation, so no rebuild is due.
    /// @param generation The grid's current Grid::stableIdGeneration().
    /// @return Whether the ids held belong to @p generation.
    [[nodiscard]] bool isCurrentFor(uint64_t generation) const noexcept { return _generation == generation; }

    /// Replaces every id with @p ids -- the result of scanning the grid for LineFlag::UserMark.
    ///
    /// The revision moves only when the rescan found other ids than those held: a generation bump that left
    /// every marked line under its id gives a frontend nothing to redraw.
    /// @param ids The marked lines' stable ids, in any order.
    /// @param generation The Grid::stableIdGeneration() they were read in.
    void rebuild(std::span<int64_t const> ids, uint64_t generation)
    {
        auto const rescanned = std::set<int64_t>(ids.begin(), ids.end());
        _ids.forgetIf([&](int64_t id) { return !rescanned.contains(id); });
        for (auto const id: rescanned)
            _ids.insert(id);
        _generation = generation;
    }

    /// Forgets every id outside @p addressable: rows evicted from the scrollback above it, and rows a page
    /// that shrank under them cut off below it.
    /// @param addressable The grid's oldest still-addressable id (Grid::stableRangeFloor()) to the id of its
    ///        page's bottom row.
    void prune(StableIdSpan addressable)
    {
        _ids.prune(addressable.first);
        _ids.pruneAbove(addressable.last);
    }

    /// Keeps each mark with its line across a scroll that moved rows' contents under their stable ids: a
    /// mark whose line moved is held under its new id, one whose line was dropped is forgotten.
    ///
    /// The flag travels with the content, so without this the index goes on naming the place and never
    /// names where the marked line went.
    /// @param move What the scroll did.
    void follow(RowMove const& move) { _ids.follow(move); }

    /// The marked lines' stable ids, ascending -- top of the scrollback first.
    [[nodiscard]] std::set<int64_t> const& ids() const noexcept { return _ids.ids(); }

    /// Bumped by every change to ids(); what a cache of anything derived from them keys on.
    [[nodiscard]] uint64_t revision() const noexcept { return _ids.revision(); }

  private:
    StableIdSet _ids;
    uint64_t _generation = 0;
};

} // namespace vtbackend
