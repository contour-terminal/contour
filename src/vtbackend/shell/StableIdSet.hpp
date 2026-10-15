// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <vtbackend/core/RowMove.hpp>

#include <algorithm>
#include <concepts>
#include <cstdint>
#include <set>
#include <vector>

namespace vtbackend
{

/// An ordered set of stable ids (Grid::stableLineIdOf()) that counts its own changes.
///
/// What a holder of lines by stable id keeps them in -- the collapsed folds (FoldState), the user-marked
/// lines (UserMarkIndex) -- so the bookkeeping every such holder needs is written once: an eviction drops a
/// prefix (prune), a scroll that moves contents under their ids carries each id along (follow).
///
/// Every mutator bumps revision() exactly when it changed the set, so a cache keyed on the revision is
/// rebuilt when there is something new to derive, and only then.
class StableIdSet
{
  public:
    /// Whether @p id is held.
    [[nodiscard]] bool contains(int64_t id) const noexcept { return _ids.contains(id); }

    /// Adds @p id, if it is not held yet.
    void insert(int64_t id)
    {
        if (_ids.insert(id).second)
            ++_revision;
    }

    /// Removes @p id, if it is held.
    void erase(int64_t id)
    {
        if (_ids.erase(id) != 0)
            ++_revision;
    }

    /// Forgets every id below @p floorStableId: rows evicted from the scrollback.
    ///
    /// Without it the set grows for the life of the session, and an id a later generation reuses would come
    /// back held. One erase of a prefix, the set being ordered.
    /// @param floorStableId The oldest still-addressable id (Grid::stableRangeFloor()).
    void prune(int64_t floorStableId)
    {
        auto const end = _ids.lower_bound(floorStableId);
        if (end == _ids.begin())
            return;

        _ids.erase(_ids.begin(), end);
        ++_revision;
    }

    /// Forgets every id above @p lastStableId: rows a page that shrank under them cut off below its bottom,
    /// whose ids the rows scrolled in next come to name.
    ///
    /// One erase of a suffix, the set being ordered.
    /// @param lastStableId The id of the page's bottom row.
    void pruneAbove(int64_t lastStableId)
    {
        auto const begin = _ids.upper_bound(lastStableId);
        if (begin == _ids.end())
            return;

        _ids.erase(begin, _ids.end());
        ++_revision;
    }

    /// Keeps each id with its line across a scroll that moved rows' contents under their stable ids: an id
    /// whose line moved is held under the id it moved to, one whose line was dropped is forgotten.
    ///
    /// The holder means the line, not the place: without this the id goes on naming whatever scrolled into
    /// the place, and the line itself is held under none.
    /// @param move What the scroll did.
    void follow(RowMove const& move)
    {
        auto const begin = _ids.lower_bound(move.changed.first);
        auto const end = _ids.upper_bound(move.changed.last);
        if (begin == end)
            return;

        auto const affected = std::vector<int64_t>(begin, end);
        _ids.erase(begin, end);
        for (auto const id: affected)
            if (auto const destination = move.destinationOf(id))
                _ids.insert(*destination);

        // Only a move by zero rows leaves every id where it was.
        if (std::ranges::any_of(affected, [&](int64_t id) { return move.destinationOf(id) != id; }))
            ++_revision;
    }

    /// Forgets every id @p isStale names.
    /// @param isStale Whether an id is to go.
    template <std::predicate<int64_t> Predicate>
    void forgetIf(Predicate const& isStale)
    {
        if (std::erase_if(_ids, isStale) != 0)
            ++_revision;
    }

    /// Forgets every id.
    void clear() noexcept
    {
        if (_ids.empty())
            return;

        _ids.clear();
        ++_revision;
    }

    /// The ids held, ascending -- top of the scrollback first.
    [[nodiscard]] std::set<int64_t> const& ids() const noexcept { return _ids; }

    /// Bumped by every change to ids(); what a cache of anything derived from them keys on.
    [[nodiscard]] uint64_t revision() const noexcept { return _revision; }

  private:
    // Ordered, so prune() is one prefix erase, follow() one run, and ids() comes out top-down.
    std::set<int64_t> _ids;
    uint64_t _revision = 0;
};

} // namespace vtbackend
