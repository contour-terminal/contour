// SPDX-License-Identifier: Apache-2.0
#include <vtbackend/links/ScanText.hpp>

#include <vtbackend/core/Utf8Prefix.hpp>
#include <vtbackend/grid/Grid.hpp>
#include <vtbackend/grid/Line.hpp>
#include <vtbackend/grid/LineSoA.hpp>

#include <core/Assert.hpp>

#include <libunicode/codepoint_properties.h>
#include <libunicode/convert.h>

#include <algorithm>
#include <array>
#include <iterator>
#include <ranges>
#include <utility>

namespace vtbackend
{

namespace
{
    /// Which East Asian width a blanking rule asks for.
    enum class WidthRequirement : uint8_t
    {
        Any = 0,         ///< Any width.
        WideOrFullwidth, ///< East_Asian_Width W or F only.
    };

    /// One row of §4.1's table: a General_Category that reads to a link as a word break.
    struct BlankingRule
    {
        unicode::General_Category category;
        WidthRequirement width;
    };

    constexpr auto BlankingRules = std::array {
        BlankingRule { unicode::General_Category::Space_Separator, WidthRequirement::Any },
        BlankingRule { unicode::General_Category::Line_Separator, WidthRequirement::Any },
        BlankingRule { unicode::General_Category::Paragraph_Separator, WidthRequirement::Any },
        BlankingRule { unicode::General_Category::Open_Punctuation, WidthRequirement::WideOrFullwidth },
        BlankingRule { unicode::General_Category::Close_Punctuation, WidthRequirement::WideOrFullwidth },
        BlankingRule { unicode::General_Category::Other_Punctuation, WidthRequirement::WideOrFullwidth },
        BlankingRule { unicode::General_Category::Dash_Punctuation, WidthRequirement::WideOrFullwidth },
        BlankingRule { unicode::General_Category::Connector_Punctuation, WidthRequirement::WideOrFullwidth },
        BlankingRule { unicode::General_Category::Initial_Punctuation, WidthRequirement::WideOrFullwidth },
        BlankingRule { unicode::General_Category::Final_Punctuation, WidthRequirement::WideOrFullwidth },
    };

    /// Whether a cell whose first codepoint is @p codepoint reads as a word break to a link.
    [[nodiscard]] bool blanksAsWordBreak(char32_t codepoint) noexcept
    {
        if (codepoint < 0x80)
            return false; // ASCII is matched as printed
        auto const properties = unicode::codepoint_properties::get(codepoint);
        auto const wide = properties.east_asian_width == unicode::East_Asian_Width::Wide
                          || properties.east_asian_width == unicode::East_Asian_Width::Fullwidth;
        return std::ranges::any_of(BlankingRules, [&](BlankingRule const& rule) {
            return rule.category == properties.general_category
                   && (rule.width == WidthRequirement::Any || wide);
        });
    }

    void appendUtf8(std::string& out, char32_t codepoint)
    {
        unicode::convert_to<char>(std::u32string_view(&codepoint, 1), std::back_inserter(out));
    }
} // namespace

ScanRow makeScanRow(Line const& line, LineOffset offset, LineContinuation continuation)
{
    auto row = ScanRow {
        .text = {}, .original = {}, .cellByteStart = {}, .line = offset, .continuation = continuation
    };
    auto const columns = unbox<std::size_t>(line.size());
    row.cellByteStart.reserve(columns);
    // A byte a cell is the whole row in ASCII, so a typical row allocates once rather than doubling its way
    // up.
    row.text.reserve(columns);

    if (line.isBlank())
    {
        row.text.assign(columns, ' ');
        for (auto const column: std::views::iota(std::size_t { 0 }, columns))
            row.cellByteStart.push_back(static_cast<uint32_t>(column));
        return row;
    }

    auto const& storage = line.storage();
    auto blanked = std::vector<std::pair<std::size_t, std::size_t>> {};
    auto leaderStart = uint32_t { 0 };
    auto continuationCells = 0;
    for (auto const column: std::views::iota(std::size_t { 0 }, columns))
    {
        if (continuationCells > 0)
        {
            // A wide character's further column: no text of its own, it maps to its leader.
            --continuationCells;
            row.cellByteStart.push_back(leaderStart);
            continue;
        }
        auto const begin = row.text.size();
        row.cellByteStart.push_back(static_cast<uint32_t>(begin));
        if (storage.clusterSize[column] == 0)
        {
            // Empty, wrap padding, or the lower part of a tall text-sizing block.
            row.text += ' ';
            continue;
        }
        forEachCodepoint(storage, column, [&](char32_t codepoint) { appendUtf8(row.text, codepoint); });
        // An OSC 8 link takes precedence (§6.4): its cells are no text a detected link may be found in.
        if (storage.hyperlinks[column] != HyperlinkId {} || blanksAsWordBreak(storage.codepoints[column]))
            blanked.emplace_back(begin, row.text.size());
        leaderStart = static_cast<uint32_t>(begin);
        continuationCells = std::max(1, static_cast<int>(storage.widths[column])) - 1;
    }

    if (!blanked.empty())
    {
        row.original = row.text;
        for (auto const& [begin, end]: blanked)
            std::fill(row.text.begin() + static_cast<std::ptrdiff_t>(begin),
                      row.text.begin() + static_cast<std::ptrdiff_t>(end),
                      ' ');
    }
    return row;
}

ScanRow makePlainScanRow(std::string_view text, LineOffset offset, LineContinuation continuation)
{
    auto row = ScanRow { .text = std::string { text },
                         .original = {},
                         .cellByteStart = {},
                         .line = offset,
                         .continuation = continuation };
    for (auto const byte: std::views::iota(std::size_t { 0 }, text.size()))
        if (!isUtf8ContinuationByte(text[byte]))
            row.cellByteStart.push_back(static_cast<uint32_t>(byte));

    // The grid scan's word breaks (§4.1), by the same table: a settings page preview matches what the
    // terminal would.
    for (auto const cell: std::views::iota(std::size_t { 0 }, row.cellByteStart.size()))
    {
        auto const begin = std::size_t { row.cellByteStart[cell] };
        auto const end =
            cell + 1 < row.cellByteStart.size() ? std::size_t { row.cellByteStart[cell + 1] } : text.size();
        if (static_cast<unsigned char>(text[begin]) < 0x80)
            continue; // ASCII is matched as printed
        auto const codepoints = unicode::convert_to<char32_t>(text.substr(begin, end - begin));
        if (codepoints.empty() || !blanksAsWordBreak(codepoints.front()))
            continue;
        if (row.original.empty())
            row.original = row.text;
        std::fill(row.text.begin() + static_cast<std::ptrdiff_t>(begin),
                  row.text.begin() + static_cast<std::ptrdiff_t>(end),
                  ' ');
    }
    return row;
}

ScanText::ScanText(std::span<ScanRow const> rows, ScanClip clip): _clip { clip }
{
    Require(!rows.empty());
    auto const anyBlanked =
        std::ranges::any_of(rows, [](ScanRow const& row) { return !row.original.empty(); });
    for (auto const& row: rows)
    {
        auto const base = static_cast<uint32_t>(_text.size());
        _text += row.text;
        if (anyBlanked)
            _original += row.original.empty() ? row.text : row.original;
        for (auto const start: row.cellByteStart)
            _cellByteStart.push_back(base + start);
        _rowCellEnds.push_back(_cellByteStart.size());
        _rowLines.push_back(row.line);
    }
}

std::string_view ScanText::original(std::size_t begin, std::size_t end) const noexcept
{
    auto const& source = _original.empty() ? _text : _original;
    return std::string_view { source }.substr(begin, end - begin);
}

std::size_t ScanText::columnsOf(std::size_t row) const noexcept
{
    return _rowCellEnds[row] - (row == 0 ? 0 : _rowCellEnds[row - 1]);
}

std::size_t ScanText::firstCellAt(std::size_t byte) const noexcept
{
    return static_cast<std::size_t>(std::ranges::lower_bound(_cellByteStart, byte) - _cellByteStart.begin());
}

std::size_t ScanText::lastCellBefore(std::size_t byteEnd) const noexcept
{
    auto const end =
        static_cast<std::size_t>(std::ranges::lower_bound(_cellByteStart, byteEnd) - _cellByteStart.begin());
    return end == 0 ? 0 : end - 1;
}

ScanPosition ScanText::positionOf(std::size_t cell) const noexcept
{
    auto const row =
        static_cast<std::size_t>(std::ranges::upper_bound(_rowCellEnds, cell) - _rowCellEnds.begin());
    auto const clamped = std::min(row, _rowCellEnds.size() - 1);
    auto const rowStart = clamped == 0 ? std::size_t { 0 } : _rowCellEnds[clamped - 1];
    return ScanPosition { .row = clamped,
                          .column =
                              ColumnOffset::cast_from(std::min(cell, _rowCellEnds[clamped] - 1) - rowStart) };
}

std::size_t ScanText::cellIndexOf(ScanPosition position) const noexcept
{
    return (position.row == 0 ? 0 : _rowCellEnds[position.row - 1]) + unbox<std::size_t>(position.column);
}

std::vector<ScanText> joinLogicalLines(std::span<ScanRow const> rows,
                                       LineContinuation continuesBelow,
                                       std::size_t maxCells)
{
    auto result = std::vector<ScanText> {};
    if (rows.empty())
        return result;

    auto first = std::size_t { 0 };
    auto cells = std::size_t { 0 };
    auto front = rows.front().continuation == LineContinuation::Yes ? ScanEdge::Clipped : ScanEdge::Whole;
    auto const flush = [&](std::size_t end, ScanEdge back) {
        result.emplace_back(rows.subspan(first, end - first), ScanClip { .front = front, .back = back });
    };

    for (auto const index: std::views::iota(std::size_t { 1 }, rows.size()))
    {
        auto const& row = rows[index];
        auto const continues =
            row.continuation == LineContinuation::Yes && row.line == rows[index - 1].line + LineOffset(1);
        cells += rows[index - 1].cellByteStart.size();
        if (!continues)
        {
            flush(index, ScanEdge::Whole);
            front = row.continuation == LineContinuation::Yes ? ScanEdge::Clipped : ScanEdge::Whole;
        }
        else if (cells + row.cellByteStart.size() > maxCells)
        {
            flush(index, ScanEdge::Clipped);
            front = ScanEdge::Clipped;
        }
        else
            continue;
        first = index;
        cells = 0;
    }
    flush(rows.size(), continuesBelow == LineContinuation::Yes ? ScanEdge::Clipped : ScanEdge::Whole);
    return result;
}

ScanExtent scanExtentOf(Grid const& grid, LineOffset first, LineOffset last, std::size_t maxCells)
{
    auto const bottom = boxed_cast<LineOffset>(grid.pageSize().lines) - LineOffset(1);
    auto const top = -boxed_cast<LineOffset>(grid.historyLineCount());
    auto const columns = std::max<std::size_t>(unbox<std::size_t>(grid.pageSize().columns), 1);
    auto const reach = LineOffset::cast_from(std::max<std::size_t>(maxCells / columns, 1));

    auto const stretchFirst = first;
    while (first > top && first > stretchFirst - reach && grid.isLineWrapped(first))
        --first;
    auto const stretchLast = last;
    while (last < bottom && last < stretchLast + reach && grid.isLineWrapped(last + LineOffset(1)))
        ++last;

    return ScanExtent { .first = first,
                        .last = last,
                        .continuesBelow = last < bottom && grid.isLineWrapped(last + LineOffset(1))
                                              ? LineContinuation::Yes
                                              : LineContinuation::No };
}

std::vector<ScanRow> scanRowsOf(Grid const& grid, LineOffset first, LineOffset last)
{
    auto rows = std::vector<ScanRow> {};
    rows.reserve(unbox<std::size_t>(last - first) + 1);
    for (auto const row: std::views::iota(unbox(first), unbox(last) + 1))
    {
        auto const line = LineOffset(row);
        rows.push_back(makeScanRow(grid.lineAt(line),
                                   line,
                                   grid.isLineWrapped(line) ? LineContinuation::Yes : LineContinuation::No));
    }
    return rows;
}

} // namespace vtbackend
