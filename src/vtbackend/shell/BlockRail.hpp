// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// Which part of a command block's rail a row draws, and which mark: pure decisions over the facts
/// Terminal::fillGutter gathers per drawn row, so the rail is table-testable without a terminal.

#include <vtbackend/core/BlockRailKinds.hpp>
#include <vtbackend/core/Color.hpp>
#include <vtbackend/core/CommandBlockId.hpp>
#include <vtbackend/core/CommandBlockOutcome.hpp>
#include <vtbackend/core/UserMarkPresence.hpp>
#include <vtbackend/shell/CommandBlock.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>

namespace vtbackend
{

struct ColorPalette;

/// How much attention a block has: none, the pointer's, or the keyboard's.
enum class BlockAttention : uint8_t
{
    Rest = 0, ///< Neither hovered nor focused.
    Hovered,  ///< The pointer is over its rows or the strip beside them.
    Focused,  ///< The keyboard focused it.
};

/// The facts about one drawn row that decide its rail segment and mark.
struct RailRowFacts
{
    /// The block the row belongs to; nullopt for a row of no block.
    std::optional<CommandBlockId> block {};
    /// The block of the row directly above in the fold projection -- looked up one row beyond the drawn rows
    /// at the page's top edge; nullopt where there is no such row or it belongs to no block.
    std::optional<CommandBlockId> above {};
    /// The block of the row directly below, likewise beyond the page's bottom edge.
    std::optional<CommandBlockId> below {};
    CommandBlockState state = CommandBlockState::Prompting;     ///< Where the row's block is in its cycle.
    CommandBlockOutcome outcome = CommandBlockOutcome::Success; ///< How the row's block reads.
    UserMarkPresence mark = UserMarkPresence::Absent;           ///< Whether the user marked the row.
    BlockAttention attention = BlockAttention::Rest;            ///< Whether its block is hovered or focused.

    bool operator==(RailRowFacts const&) const = default;
};

/// The rail segment a row with @p facts draws.
///
/// A block's first row in the projection is its Head, its last its Tail, those between Body, and a block with
/// a single row Single -- on screen or not, so a block continuing above the page draws Body on the top row.
/// The live prompt (a Prompting block) and rows of no block draw none.
/// @param facts What is true of the row.
/// @return Its segment.
[[nodiscard]] constexpr RailSegment railSegmentOf(RailRowFacts const& facts) noexcept
{
    if (!facts.block || facts.state == CommandBlockState::Prompting)
        return RailSegment::None;
    auto const startsHere = facts.above != facts.block;
    auto const endsHere = facts.below != facts.block;
    if (startsHere && endsHere)
        return RailSegment::Single;
    if (startsHere)
        return RailSegment::Head;
    if (endsHere)
        return RailSegment::Tail;
    return RailSegment::Body;
}

/// One outcome's marks on its block's head row: at rest, and while hovered or focused.
struct RailMarkRow
{
    CommandBlockOutcome outcome = CommandBlockOutcome::Success; ///< The outcome the row is for.
    RailMark atRest = RailMark::None;                           ///< The mark at rest.
    RailMark attended = RailMark::None;                         ///< The mark while hovered or focused.
};

/// The head marks, one row per CommandBlockOutcome in its declared order: a success says nothing until
/// attended to, a failure and a running block always say what they are.
inline constexpr auto BlockRailMarkRows = std::array {
    RailMarkRow {
        .outcome = CommandBlockOutcome::Success, .atRest = RailMark::None, .attended = RailMark::Check },
    RailMarkRow {
        .outcome = CommandBlockOutcome::Failure, .atRest = RailMark::Cross, .attended = RailMark::Cross },
    RailMarkRow {
        .outcome = CommandBlockOutcome::Running, .atRest = RailMark::Running, .attended = RailMark::Running },
};
static_assert(BlockRailMarkRows.size() == CommandBlockOutcomeCount);

/// The mark a row with @p facts carries, drawing @p segment.
///
/// The head (or only) row takes its outcome's mark from BlockRailMarkRows; a user-marked row that heads
/// nothing takes the user mark.
/// @param facts What is true of the row.
/// @param segment Its segment (railSegmentOf()).
/// @return The mark, or RailMark::None.
[[nodiscard]] constexpr RailMark railMarkOf(RailRowFacts const& facts, RailSegment segment) noexcept
{
    if (segment == RailSegment::Head || segment == RailSegment::Single)
    {
        auto const& row = BlockRailMarkRows[static_cast<std::size_t>(facts.outcome)];
        return facts.attention == BlockAttention::Rest ? row.atRest : row.attended;
    }
    return facts.mark == UserMarkPresence::Present ? RailMark::UserMark : RailMark::None;
}

/// The colour a block's rail is drawn in: its outcome's status colour at rest, the hover colour while hovered
/// or focused.
/// @param palette The colour scheme.
/// @param outcome How the block reads.
/// @param attention Whether it is hovered or focused.
/// @return The rail's colour.
[[nodiscard]] RGBColor railColor(ColorPalette const& palette,
                                 CommandBlockOutcome outcome,
                                 BlockAttention attention) noexcept;

} // namespace vtbackend
