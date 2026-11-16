// SPDX-License-Identifier: Apache-2.0
#include <core/tui/Buffer.hpp>

#include <core/FNV.hpp>
#include <core/tui/FrameDiff.hpp>
#include <core/tui/TerminalOutput.hpp>
#include <core/tui/Unicode.hpp>

#include <libunicode/utf8_grapheme_segmenter.h>

#include <algorithm>
#include <cstdint>
#include <format>
#include <ranges>
#include <utility>

namespace core::tui
{

Buffer::Buffer(int rows, int cols): _rows(rows), _cols(cols)
{
    if (_rows > 0 && _cols > 0)
    {
        _cells.resize(static_cast<size_t>(_rows) * static_cast<size_t>(_cols));
        clear();
    }
}

void Buffer::resize(int rows, int cols)
{
    if (rows == _rows && cols == _cols)
        return;

    if (rows <= 0 || cols <= 0)
    {
        _cells.clear();
        _rows = 0;
        _cols = 0;
        _cursor = { .x = 0, .y = 0 };
        return;
    }

    std::vector<Cell> newCells(static_cast<size_t>(rows) * static_cast<size_t>(cols));

    // Initialize all cells to spaces
    for (auto& cell: newCells)
        cell.reset();

    // Copy existing content
    int const copyRows = std::min(_rows, rows);
    int const copyCols = std::min(_cols, cols);

    for (auto const r: std::views::iota(0, copyRows))
    {
        for (auto const c: std::views::iota(0, copyCols))
        {
            auto const oldIdx =
                (static_cast<size_t>(r) * static_cast<size_t>(_cols)) + static_cast<size_t>(c);
            auto const newIdx = (static_cast<size_t>(r) * static_cast<size_t>(cols)) + static_cast<size_t>(c);
            newCells[newIdx] = _cells[oldIdx];
        }
    }

    _cells = std::move(newCells);
    _rows = rows;
    _cols = cols;
    clearImages();
    clearHyperlinks();
    clearMarks();

    // Clamp cursor to new bounds
    _cursor.x = std::min(_cursor.x, _cols - 1);
    _cursor.y = std::min(_cursor.y, _rows - 1);
    _cursor.x = std::max(_cursor.x, 0);
    _cursor.y = std::max(_cursor.y, 0);
}

Cell* Buffer::cellAt(Point position) noexcept
{
    if (!inBounds(position.y, position.x))
        return nullptr;
    return &_cells[index(position.y, position.x)];
}

Cell const* Buffer::cellAt(Point position) const noexcept
{
    if (!inBounds(position.y, position.x))
        return nullptr;
    return &_cells[index(position.y, position.x)];
}

void Buffer::clear(Style const& style)
{
    for (auto& cell: _cells)
        cell.reset(style);
    clearImages();
    clearHyperlinks();
    clearMarks();
}

void Buffer::clearRect(Rect area, Style const& style)
{
    // Clipped to the buffer: an area partly outside is cleared where it is inside, and one wholly
    // outside clears nothing.
    auto const clipped = area.intersect(Rect { .x = 0, .y = 0, .width = _cols, .height = _rows });
    for (auto const row: std::views::iota(clipped.y, clipped.bottom()))
        for (auto const col: std::views::iota(clipped.x, clipped.right()))
            _cells[index(row, col)].reset(style);
}

int Buffer::putString(int row, int col, std::string_view text, Style const& style, int maxWidth)
{
    if (row < 0 || row >= _rows || col < 0)
        return 0;

    // Effective right boundary: the lesser of the buffer width and the caller's column budget.
    // Clamp the budget to a non-negative span starting at col, guarding against overflow when
    // maxWidth is the default sentinel (std::numeric_limits<int>::max()).
    int const budgetLimit = (maxWidth >= _cols - col) ? _cols : col + maxWidth;
    int const colLimit = std::min(_cols, budgetLimit);

    int currentCol = col;

    // Use libunicode to segment into grapheme clusters
    auto segmenter = unicode::utf8_grapheme_segmenter(text);

    // The look-ahead iterator drives the loop, so the cluster's end is `next`'s start without a
    // second copy of the iterator, and a `continue` below cannot skip the step.
    auto next = segmenter.begin();
    while (next != segmenter.end())
    {
        auto it = next;
        ++next;

        if (currentCol >= colLimit)
            break;

        auto const& cluster = *it;

        // Skip C0 control characters (codepoint < 0x20) to prevent terminal corruption.
        if (!cluster.empty() && static_cast<char32_t>(cluster[0]) < 0x20)
            continue;

        // Calculate display width for the grapheme cluster
        int const clusterWidth = graphemeClusterWidth(cluster);

        // Check if it fits within the effective boundary (buffer width and caller budget).
        // A wide cluster that would straddle the boundary is dropped, not partially written,
        // so its continuation cells never spill past colLimit.
        if (currentCol + clusterWidth > colLimit)
            break;

        // Get the cluster as a string_view into the original text
        // it._clusterStart points to the start of the current cluster in the source
        char const* clusterStart = it._clusterStart;
        char const* clusterEnd = (next != segmenter.end()) ? next._clusterStart : (text.data() + text.size());
        std::string_view const clusterView(clusterStart, static_cast<size_t>(clusterEnd - clusterStart));

        // Write the grapheme to the first cell
        Cell& cell = _cells[index(row, currentCol)];
        cell.grapheme = std::string(clusterView);
        cell.style = style;
        cell.width = static_cast<uint8_t>(clusterWidth);

        // Mark continuation cells for wide characters
        for (auto const i: std::views::iota(1, std::max(1, std::min(clusterWidth, _cols - currentCol))))
        {
            Cell& contCell = _cells[index(row, currentCol + i)];
            contCell.resetContinuation();
            contCell.style = style;
        }

        currentCol += clusterWidth;
    }

    return currentCol - col;
}

void Buffer::fill(Rect area, char ch, Style const& style)
{
    // Clipped to the buffer, as clearRect() is.
    auto const clipped = area.intersect(Rect { .x = 0, .y = 0, .width = _cols, .height = _rows });
    std::string const grapheme(1, ch);

    for (auto const row: std::views::iota(clipped.y, clipped.bottom()))
    {
        for (auto const col: std::views::iota(clipped.x, clipped.right()))
        {
            Cell& cell = _cells[index(row, col)];
            cell.grapheme = grapheme;
            cell.style = style;
            cell.width = 1;
        }
    }
}

void Buffer::setCursor(int row, int col) noexcept
{
    _cursor.x = col;
    _cursor.y = row;
}

void Buffer::addImage(Rect cellArea, std::string encodedSixel)
{
    auto const hash = std::hash<std::string> {}(encodedSixel);
    _images.push_back(ImageRegion {
        .cellArea = cellArea,
        .encodedSixel = std::move(encodedSixel),
        .contentHash = hash,
    });
}

std::span<ImageRegion const> Buffer::images() const noexcept
{
    return _images;
}

void Buffer::clearImages() noexcept
{
    _images.clear();
}

void Buffer::addHyperlink(Rect cellArea, std::string uri)
{
    if (uri.empty() || cellArea.empty())
        return;

    // The OSC 8 `id` is derived from the URI so it is identical for every region pointing at the
    // same target and stable across frames — a redraw must not mint a new link identity. FNV
    // rather than std::hash because the value goes on the wire and must not vary between runs or
    // standard-library versions. Hashed as bytes so any byte >= 0x80 does not sign-extend.
    auto const hash = core::FNV<std::uint8_t, std::uint32_t> {}(
        reinterpret_cast<std::uint8_t const*>(uri.data()), uri.size());

    _hyperlinks.push_back(HyperlinkRegion {
        .cellArea = cellArea,
        .uri = std::move(uri),
        // Rendered here rather than at every link open: a region is registered once per frame but
        // opened once per row it spans, and a multi-row link would re-format it per row. The hex
        // digits are the whole id: endo prefixed them with its own name, and core::tui is the
        // shared library every consumer's hyperlinks go through.
        .linkId = std::format("{:x}", hash),
    });
}

std::span<HyperlinkRegion const> Buffer::hyperlinks() const noexcept
{
    return _hyperlinks;
}

void Buffer::clearHyperlinks() noexcept
{
    _hyperlinks.clear();
}

void Buffer::addMark(int row, int column, std::string sequence)
{
    _marks.push_back(RowMark { .row = row, .column = column, .sequence = std::move(sequence) });
}

std::span<RowMark const> Buffer::marks() const noexcept
{
    return _marks;
}

void Buffer::clearMarks() noexcept
{
    _marks.clear();
}

void Buffer::writeTo(TerminalOutput& out) const
{
    // A full repaint, row by row from where the cursor is: the inline flush's diff with nothing to
    // diff against. Hyperlink regions are honoured here too, not just in Screen's flushes: the
    // payload lives at this layer, so a component that registers a link and renders through
    // writeTo() would otherwise lose it silently, with nothing to hint why.
    writeFrameDiff(out,
                   *this,
                   *this,
                   DiffOptions { .addressing = DiffAddressing::Relative,
                                 .repaint = DiffRepaint::All,
                                 .hyperlinks = DiffHyperlinks::Emit });

    // Position terminal cursor at the buffer's logical cursor location.
    if (_cursorVisible)
    {
        // After the loop, terminal cursor is at row (_rows - 1), end of content.
        auto const rowsUp = _rows - 1 - _cursor.y;
        if (rowsUp > 0)
            out.moveUp(rowsUp);
        out.carriageReturn();
        if (_cursor.x > 0)
            out.moveRight(_cursor.x);
        out.showCursor();
    }
}

} // namespace core::tui
