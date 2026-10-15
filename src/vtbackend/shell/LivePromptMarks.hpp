// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <vtbackend/core/Primitives.hpp>
#include <vtbackend/core/RowMove.hpp>
#include <vtbackend/shell/PromptRegion.hpp>

#include <algorithm>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace vtbackend
{

/// A logical line, named so that a scroll keeps the name and a reflow or a reset voids it: by the stable id
/// of its head, within one stable-id generation (Grid::stableLineIdOf(), Grid::stableIdGeneration()).
struct StableLine
{
    int64_t id = 0;          ///< The stable id of the line's head.
    uint64_t generation = 0; ///< The stable-id generation @ref id is one of.

    bool operator==(StableLine const&) const = default;
};

/// Where the input that begins on a line of the prompt ends.
enum class InputEnd : uint8_t
{
    AtOutputStart = 0, ///< Where the output starts (OSC 133;C): the prompt's end was OSC 133;B.
    AtLineEnd,         ///< At the end of its line: the prompt's end was OSC 133;I.
};

/// What the shell said about the lines of the prompt in progress that their rows have no room for: where a
/// right prompt begins on a line (OSC 133;P;k=r), and whether the input that begins on a line ends with it
/// (OSC 133;I).
///
/// Kept beside the grid rather than on its rows, for only command-line recovery reads it, and only for the
/// prompt in progress: no row pays a byte for it. A new prompt forgets all of it.
class LivePromptMarks
{
  public:
    /// How many lines' marks are kept at most: as many as the live-prompt scan climbs (MaxPromptScanLines),
    /// for a mark further up than that belongs to no prompt it can find. A line marked beyond that pushes the
    /// line marked first out.
    static constexpr size_t MaxLines = MaxPromptScanLines;

    /// Records that a right prompt begins at the logical column @p column of @p line, in place of any marked
    /// there before.
    /// @param line The line.
    /// @param column Where on it the right prompt begins, counted across the whole logical line.
    void markRightPrompt(StableLine line, ColumnOffset column);

    /// @param line A line.
    /// @return Where a right prompt begins on @p line, if one was marked there.
    [[nodiscard]] std::optional<ColumnOffset> rightPromptOn(StableLine line) const noexcept;

    /// Records where the input that begins on @p line ends, in place of what was said before.
    /// @param line The line the prompt's end was marked on.
    /// @param end Where its input ends.
    void markInputEnd(StableLine line, InputEnd end);

    /// @param line A line.
    /// @return Where the input that begins on @p line ends; AtOutputStart unless OSC 133;I said otherwise.
    [[nodiscard]] InputEnd inputEndOn(StableLine line) const noexcept;

    /// Gives what was marked on @p from the name @p to: a resize names every line anew, even one whose rows
    /// it leaves as they were.
    /// @param from The line's name before.
    /// @param to Its name now.
    /// @param moveColumn Where a column of the line lies now, given where it lay.
    template <std::invocable<ColumnOffset> MoveColumn>
    void rename(StableLine from, StableLine to, MoveColumn moveColumn)
    {
        auto const known = std::ranges::find(_lines, from, &LineMarks::line);
        if (known == _lines.end())
            return;
        known->line = to;
        if (known->rightPrompt)
            known->rightPrompt = moveColumn(*known->rightPrompt);
    }

    /// Moves each line's marks to the id that names its head after a scroll that moved rows' contents under
    /// their stable ids, and forgets those of a line the scroll dropped: a scroll inside margins at the
    /// prompt moves the prompt while its old ids go on naming the places it left.
    /// @param move What the scroll did.
    void follow(RowMove const& move);

    /// Forgets every mark: a new prompt starts.
    void clear() noexcept { _lines.clear(); }

  private:
    /// The marks of one line.
    struct LineMarks
    {
        StableLine line;
        std::optional<ColumnOffset> rightPrompt {};
        InputEnd inputEnd = InputEnd::AtOutputStart;
    };

    /// @return The marks of @p line, if it has any.
    [[nodiscard]] LineMarks const* find(StableLine line) const noexcept;

    /// @return The marks of @p line, made for it if it has none.
    [[nodiscard]] LineMarks& marksOf(StableLine line);

    std::vector<LineMarks> _lines; ///< In the order they were first marked.
};

} // namespace vtbackend
