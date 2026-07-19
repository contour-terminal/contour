// SPDX-License-Identifier: Apache-2.0
#include <vtbackend/Bidi.h>

#include <libunicode/codepoint_properties.h>

#include <algorithm>
#include <ranges>
#include <numeric>

using unicode::Bidi_Class;
using unicode::Bidi_Direction;

namespace vtbackend
{

namespace
{
    /// Whether rule L1 resets this character's level when it trails a line.
    [[nodiscard]] bool isL1Resettable(char32_t codepoint) noexcept
    {
        switch (unicode::codepoint_properties::get(codepoint).bidi_class)
        {
            case Bidi_Class::White_Space:
            case Bidi_Class::Left_To_Right_Isolate:
            case Bidi_Class::Right_To_Left_Isolate:
            case Bidi_Class::First_Strong_Isolate:
            case Bidi_Class::Pop_Directional_Isolate: return true;
            default: return false;
        }
    }

    /// Applies rule L1's fourth clause -- a trailing run of whitespace takes the paragraph level --
    /// to one display row.
    ///
    /// bidi_resolve() already applied L1 to the paragraph, but "the line" in L1 means a DISPLAY
    /// line, and a soft-wrapped paragraph occupies several. Without this, trailing blanks on an
    /// intermediate row would keep the level of the text before them and a right-to-left row would
    /// render its padding on the wrong side.
    void applyLineEndWhitespaceReset(std::u32string_view text,
                                     uint8_t paragraphLevel,
                                     std::span<uint8_t> levels) noexcept
    {
        for (auto i = text.size(); i-- > 0;)
        {
            if (levels[i] == unicode::bidi_removed_level)
                continue;
            if (!isL1Resettable(text[i]))
                return;
            levels[i] = paragraphLevel;
        }
    }

    /// Builds the two permutation tables for one row from its resolved levels.
    ///
    /// Characters removed by X9 have no level and take no part in reordering, so they are held in
    /// place: a terminal cell always exists, even where the algorithm says nothing is displayed.
    void buildPermutation(BidiLineLayout& layout, std::span<uint8_t const> levels)
    {
        auto const columns = levels.size();

        auto displayed = std::vector<size_t> {};
        auto displayedLevels = std::vector<uint8_t> {};
        displayed.reserve(columns);
        displayedLevels.reserve(columns);
        for (size_t i = 0; i < columns; ++i)
        {
            if (levels[i] == unicode::bidi_removed_level)
                continue;
            displayed.push_back(i);
            displayedLevels.push_back(levels[i]);
        }

        auto visualToLogical = std::vector<ColumnOffset> {};
        visualToLogical.reserve(columns);
        auto const reordered = unicode::bidi_reorder_visual(displayedLevels);
        for (auto const visual: reordered)
            visualToLogical.push_back(ColumnOffset::cast_from(displayed[visual]));

        // Anything the algorithm dropped keeps its own column, so that every column maps somewhere
        // and the two tables stay total.
        auto placed = std::vector<bool>(columns, false);
        for (auto const column: visualToLogical)
            placed[unbox<size_t>(column)] = true;
        for (size_t i = 0; i < columns; ++i)
            if (!placed[i])
                visualToLogical.push_back(ColumnOffset::cast_from(i));

        auto logicalToVisual = std::vector<ColumnOffset>(columns);
        for (size_t visual = 0; visual < visualToLogical.size(); ++visual)
            logicalToVisual[unbox<size_t>(visualToLogical[visual])] = ColumnOffset::cast_from(visual);

        layout.identity = std::ranges::all_of(std::views::iota(size_t { 0 }, columns), [&](size_t i) {
            return unbox<size_t>(visualToLogical[i]) == i;
        });

        if (layout.identity)
            return;

        layout.visualToLogical = std::move(visualToLogical);
        layout.logicalToVisual = std::move(logicalToVisual);
        layout.levels.assign(levels.begin(), levels.end());
    }

    /// Lays out one paragraph, which may span several rows, and writes a layout per row.
    void layOutParagraph(std::span<BidiLineInput const> paragraph,
                         std::optional<Bidi_Direction> paragraphDirection,
                         std::span<BidiLineLayout> out)
    {
        // Fast path. In a left-to-right paragraph every codepoint below U+0590 resolves to level 0:
        // the strong ones are L, and the weak and neutral ones take the embedding direction from
        // W7/N2. So the layout is the identity and nothing needs computing. A right-to-left
        // paragraph is excluded because there even pure ASCII gets a non-zero level.
        auto const forcedRightToLeft = paragraphDirection == Bidi_Direction::Right_To_Left;
        auto const anyBidi = std::ranges::any_of(
            paragraph, [](BidiLineInput const& line) { return mayContainBidi(line.text); });
        if (!anyBidi && !forcedRightToLeft)
        {
            for (auto& layout: out)
            {
                layout.paragraphDirection = Bidi_Direction::Left_To_Right;
                layout.identity = true;
            }
            return;
        }

        auto text = std::u32string {};
        for (auto const& line: paragraph)
            text += line.text;

        auto const resolved = unicode::bidi_resolve(text, paragraphDirection);
        auto const paragraphLevel =
            static_cast<uint8_t>(resolved.base_direction == Bidi_Direction::Right_To_Left ? 1 : 0);

        auto offset = size_t { 0 };
        for (size_t row = 0; row < paragraph.size(); ++row)
        {
            auto const length = paragraph[row].text.size();
            auto rowLevels =
                std::vector<uint8_t>(resolved.levels.begin() + static_cast<ptrdiff_t>(offset),
                                     resolved.levels.begin() + static_cast<ptrdiff_t>(offset + length));

            applyLineEndWhitespaceReset(paragraph[row].text, paragraphLevel, rowLevels);

            out[row].paragraphDirection = resolved.base_direction;
            buildPermutation(out[row], rowLevels);

            offset += length;
        }
    }

} // namespace

BidiPageLayout computeBidiPageLayout(std::span<BidiLineInput const> lines,
                                     std::optional<Bidi_Direction> paragraphDirection)
{
    auto result = BidiPageLayout {};
    result.lines.resize(lines.size());

    // Group the rows into paragraphs. A row that does not continue the one above it starts a new
    // paragraph; the first row always does, whatever its flag says, because there is nothing above
    // it in this range to continue.
    auto begin = size_t { 0 };
    while (begin < lines.size())
    {
        auto end = begin + 1;
        while (end < lines.size() && lines[end].continuesParagraph)
            ++end;

        layOutParagraph(lines.subspan(begin, end - begin),
                        paragraphDirection,
                        std::span(result.lines).subspan(begin, end - begin));
        begin = end;
    }

    return result;
}

} // namespace vtbackend
