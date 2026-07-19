// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <vtbackend/primitives.h>

#include <libunicode/bidi.h>

#include <cstdint>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

namespace vtbackend
{

/// The lowest codepoint whose Bidi_Class is not Left_To_Right.
///
/// Everything below U+0590 -- ASCII, Latin-1, Latin Extended, Greek, Cyrillic, and the punctuation
/// and symbols between them -- resolves to a left-to-right level in a left-to-right paragraph, so a
/// line holding only such characters needs no reordering at all. That covers essentially all real
/// terminal output, which is why the scan below is worth doing before anything else.
inline constexpr char32_t FirstBidiRelevantCodepoint = 0x0590;

/// Whether a line could possibly need reordering.
///
/// A cheap linear scan that decides the pure-left-to-right fast path. False is a definitive "no
/// reordering needed"; true only means "run the algorithm and find out".
[[nodiscard]] constexpr bool mayContainBidi(std::u32string_view text) noexcept
{
    for (auto const codepoint: text)
        if (codepoint >= FirstBidiRelevantCodepoint)
            return true;
    return false;
}

/// One line of a page, as the bidi layout sees it.
struct BidiLineInput
{
    /// The line's codepoints in logical order, one per column.
    std::u32string_view text;

    /// Whether this line continues the paragraph begun on the line above, rather than starting a
    /// new one. This is Line::wrapped(): a soft wrap continues a paragraph, a hard newline ends it.
    bool continuesParagraph = false;
};

/// Visual layout of one line.
struct BidiLineLayout
{
    /// The base direction of the paragraph this line belongs to.
    unicode::Bidi_Direction paragraphDirection = unicode::Bidi_Direction::Left_To_Right;

    /// Resolved embedding level per column. Empty when @ref identity holds.
    std::vector<uint8_t> levels;

    /// For each visual column, the logical column drawn there. Empty when @ref identity holds.
    std::vector<ColumnOffset> visualToLogical;

    /// For each logical column, the visual column it is drawn at. Empty when @ref identity holds.
    std::vector<ColumnOffset> logicalToVisual;

    /// Whether the line needs no permutation at all, so that visual column == logical column.
    ///
    /// Kept as an explicit flag rather than inferred from the tables so the common case costs
    /// nothing: when this is true the tables are not even allocated.
    bool identity = true;

    /// The logical column drawn at @p column, which is @p column itself on the fast path.
    [[nodiscard]] ColumnOffset logicalColumnAt(ColumnOffset column) const noexcept
    {
        if (identity || unbox<size_t>(column) >= visualToLogical.size())
            return column;
        return visualToLogical[unbox<size_t>(column)];
    }

    /// The visual column that @p column is drawn at, which is @p column itself on the fast path.
    [[nodiscard]] ColumnOffset visualColumnAt(ColumnOffset column) const noexcept
    {
        if (identity || unbox<size_t>(column) >= logicalToVisual.size())
            return column;
        return logicalToVisual[unbox<size_t>(column)];
    }

    /// The resolved embedding level at logical column @p column; 0 on the fast path.
    [[nodiscard]] uint8_t levelAt(ColumnOffset column) const noexcept
    {
        if (identity || unbox<size_t>(column) >= levels.size())
            return 0;
        return levels[unbox<size_t>(column)];
    }
};

/// Visual layout of a contiguous run of lines.
struct BidiPageLayout
{
    std::vector<BidiLineLayout> lines;

    /// Layout of the line at @p index, or a neutral left-to-right layout when out of range.
    [[nodiscard]] BidiLineLayout const& lineAt(size_t index) const noexcept
    {
        static auto const identityLayout = BidiLineLayout {};
        return index < lines.size() ? lines[index] : identityLayout;
    }
};

/// Computes the visual layout of a contiguous run of lines.
///
/// Paragraphs, not rows, are the unit UAX#9 works on: a soft-wrapped line continues the paragraph
/// above it, and a character's direction can depend on text on another row. The lines are therefore
/// grouped into paragraphs by @ref BidiLineInput::continuesParagraph, resolved one paragraph at a
/// time, and the levels sliced back out per line.
///
/// It follows that the caller must pass a range already expanded to paragraph boundaries -- back to
/// the first line that does not continue a paragraph, and forward through any continuations -- or
/// the reordering will be computed from a truncated paragraph and change whenever the viewport
/// scrolls. See @ref expandToParagraphBounds.
///
/// @param lines             the lines to lay out, in top-to-bottom order.
/// @param paragraphDirection the base direction to impose on every paragraph; nullopt autodetects
///                          each paragraph separately from its first strong character (P2/P3).
/// @return one layout per input line, in the same order.
[[nodiscard]] BidiPageLayout computeBidiPageLayout(std::span<BidiLineInput const> lines,
                                                   std::optional<unicode::Bidi_Direction> paragraphDirection);

} // namespace vtbackend
