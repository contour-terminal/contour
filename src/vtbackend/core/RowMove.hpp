// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <algorithm>
#include <cstdint>
#include <optional>

namespace vtbackend
{

/// A run of stable ids (Grid::stableLineIdOf()), both ends included; empty when @ref first exceeds @ref last.
struct StableIdSpan
{
    int64_t first {}; ///< The lowest id.
    int64_t last {};  ///< The highest id.

    /// @return Whether @p id lies in the span.
    [[nodiscard]] constexpr bool contains(int64_t id) const noexcept { return first <= id && id <= last; }

    /// @return The smallest span that holds both this one and @p other.
    [[nodiscard]] constexpr StableIdSpan joinedWith(StableIdSpan other) const noexcept
    {
        return StableIdSpan { .first = std::min(first, other.first), .last = std::max(last, other.last) };
    }

    bool operator==(StableIdSpan const&) const = default;
};

/// What a scroll that leaves the stable ids in place did to the rows' contents.
///
/// A scroll of the whole page carries every row's stable id along with its content, unless it is a reverse
/// one over a history, which must stay where it is. That one, and one inside margins, do not: they move the
/// contents between rows while each stable id keeps naming its place, and a reverse scroll drops the
/// region's bottom rows, whose ids later come to name new ones. Whoever holds stable ids --
/// a command block's head, a collapsed fold -- learns from this where what its id named went.
struct RowMove
{
    /// Every id that names other content afterwards. Ids outside it keep theirs.
    StableIdSpan changed;
    /// The ids, within @ref changed, whose content moved rather than went; empty when all of it went.
    StableIdSpan moved;
    /// How far, in stable ids, the content that moved went.
    int64_t by {};

    /// @param id A stable id, as it named content before the move.
    /// @return The id that names that content afterwards: @p id itself where the move left it alone, nullopt
    ///         where it dropped it.
    [[nodiscard]] constexpr std::optional<int64_t> destinationOf(int64_t id) const noexcept
    {
        if (!changed.contains(id))
            return id;
        if (!moved.contains(id))
            return std::nullopt;
        return id + by;
    }

    bool operator==(RowMove const&) const = default;
};

} // namespace vtbackend
