// SPDX-License-Identifier: Apache-2.0
#include <core/tui/Canvas.hpp>

#include <core/tui/Theme.hpp>
#include <core/tui/Unicode.hpp>

#include <libunicode/utf8_grapheme_segmenter.h>

#include <algorithm>
#include <ranges>
#include <utility>

namespace core::tui
{

Canvas::Canvas(Buffer& buffer, Rect area, Theme const& theme): _buffer(buffer), _area(area), _theme(theme)
{
}

void Canvas::put(int row, int col, std::string_view grapheme, Style const& style)
{
    if (!inBounds(row, col))
        return;

    int const bufRow = toBufferRow(row);
    int const bufCol = toBufferCol(col);

    if (Cell* cell = _buffer.cellAt({ .x = bufCol, .y = bufRow }))
    {
        cell->grapheme = std::string(grapheme);
        cell->style = style;
        cell->width = 1;
    }
}

int Canvas::putString(int row, int col, std::string_view text, Style const& style)
{
    if (row < 0 || row >= _area.height || col < 0)
        return 0;

    // Calculate available width from col to right edge
    int const availableWidth = _area.width - col;
    if (availableWidth <= 0)
        return 0;

    int const bufRow = toBufferRow(row);
    int const bufCol = toBufferCol(col);

    // Constrain the write to this canvas' available width. Buffer::putString stops before any
    // grapheme cluster (including its continuation cells) would exceed the budget, so a wide
    // cluster at the right edge cannot spill into cells outside this canvas' area.
    int const written = _buffer.putString(bufRow, bufCol, text, style, availableWidth);

    // Clamp to available width (defensive; Buffer already honours the budget).
    return std::min(written, availableWidth);
}

void Canvas::fill(Rect area, char ch, Style const& style)
{
    Rect const clipped = clipToLocal(area);
    if (clipped.empty())
        return;

    Rect const bufRect = toBufferRect(clipped);
    _buffer.fill(bufRect, ch, style);
}

void Canvas::clear(Style const& style)
{
    _buffer.clearRect(_area, style);
}

void Canvas::drawBox(Rect area, BorderStyle border, Style const& style)
{
    drawBox(area, border, style, "", TitleAlign::Left);
}

void Canvas::drawBox(
    Rect area, BorderStyle border, Style const& style, std::string_view title, TitleAlign align)
{
    Rect const clipped = clipToLocal(area);
    if (clipped.width < 2 || clipped.height < 2)
        return;

    auto const chars = BorderChars::fromStyle(border);

    int const top = clipped.y;
    int const bottom = clipped.bottom() - 1;
    int const left = clipped.x;
    int const right = clipped.right() - 1;

    // Draw corners
    put(top, left, chars.topLeft, style);
    put(top, right, chars.topRight, style);
    put(bottom, left, chars.bottomLeft, style);
    put(bottom, right, chars.bottomRight, style);

    // Draw horizontal lines
    for (auto const col: std::views::iota(left + 1, std::max(left + 1, right)))
    {
        put(top, col, chars.horizontal, style);
        put(bottom, col, chars.horizontal, style);
    }

    // Draw vertical lines
    for (auto const row: std::views::iota(top + 1, std::max(top + 1, bottom)))
    {
        put(row, left, chars.vertical, style);
        put(row, right, chars.vertical, style);
    }

    // Draw title if provided
    if (!title.empty() && clipped.width > 4)
    {
        int const maxTitleWidth = clipped.width - 4; // Leave space for corners and padding
        std::string_view const displayTitle = title.substr(0, static_cast<size_t>(maxTitleWidth));

        auto titleCol = 0;
        switch (align)
        {
            case TitleAlign::Left: titleCol = left + 2; break;
            case TitleAlign::Center:
                titleCol = left + ((clipped.width - static_cast<int>(displayTitle.size())) / 2);
                break;
            case TitleAlign::Right: titleCol = right - static_cast<int>(displayTitle.size()) - 1; break;
        }

        putString(top, titleCol, displayTitle, style);
    }
}

void Canvas::drawHLine(int row, int startCol, int width, std::string_view ch, Style const& style)
{
    if (row < 0 || row >= _area.height)
        return;

    int const endCol = std::min(startCol + width, _area.width);
    for (auto const col: std::views::iota(std::max(0, startCol), std::max({ 0, startCol, endCol })))
    {
        put(row, col, ch, style);
    }
}

void Canvas::drawVLine(int col, int startRow, int height, std::string_view ch, Style const& style)
{
    if (col < 0 || col >= _area.width)
        return;

    int const endRow = std::min(startRow + height, _area.height);
    for (auto const row: std::views::iota(std::max(0, startRow), std::max({ 0, startRow, endRow })))
    {
        put(row, col, ch, style);
    }
}

void Canvas::drawImage(int row, int col, int columnSpan, int lineSpan, std::string_view encodedSixel)
{
    auto const bufRow = toBufferRow(row);
    auto const bufCol = toBufferCol(col);
    auto const cellArea = Rect { .x = bufCol, .y = bufRow, .width = columnSpan, .height = lineSpan };
    _buffer.addImage(cellArea, std::string(encodedSixel));
}

void Canvas::addHyperlink(int row, int col, int columnSpan, std::string_view uri)
{
    if (uri.empty() || columnSpan <= 0 || row < 0 || row >= _area.height)
        return;

    // Clip the run to this canvas so a subcanvas cannot claim columns it does not own.
    auto const startCol = std::max(0, col);
    auto const endCol = std::min(_area.width, col + columnSpan);
    if (endCol <= startCol)
        return;

    auto const cellArea = Rect {
        .x = toBufferCol(startCol),
        .y = toBufferRow(row),
        .width = endCol - startCol,
        .height = 1,
    };
    _buffer.addHyperlink(cellArea, std::string(uri));
}

void Canvas::mark(int row, int column, std::string sequence)
{
    if (!inBounds(row, column))
        return;
    _buffer.addMark(toBufferRow(row), toBufferCol(column), std::move(sequence));
}

void Canvas::setCursor(int row, int col)
{
    if (inBounds(row, col))
    {
        _buffer.setCursor(toBufferRow(row), toBufferCol(col));
        _buffer.setCursorVisible(true);
    }
}

void Canvas::hideCursor()
{
    _buffer.setCursorVisible(false);
}

Canvas Canvas::subcanvas(Rect area) const
{
    // Clip the requested area to our bounds
    Rect const clipped = clipToLocal(area);

    // Translate to buffer coordinates
    Rect const bufferArea = toBufferRect(clipped);

    return { _buffer, bufferArea, _theme };
}

Rect Canvas::clipToLocal(Rect localArea) const noexcept
{
    // Intersect with our local bounds (0,0 to width,height)
    return localArea.intersect(Rect { .x = 0, .y = 0, .width = _area.width, .height = _area.height });
}

Rect Canvas::toBufferRect(Rect localArea) const noexcept
{
    return localArea.offset(_area.x, _area.y);
}

int putStringWithHighlights(Canvas& canvas,
                            int row,
                            int col,
                            std::string_view text,
                            Style const& normalStyle,
                            Style const& matchStyle,
                            std::vector<size_t> const& matchPositions)
{
    auto currentCol = col;
    size_t graphemeIndex = 0;

    auto segmenter = unicode::utf8_grapheme_segmenter(text);
    auto next = segmenter.begin();
    while (next != segmenter.end())
    {
        auto it = next;
        ++next;
        auto const clusterIndex = graphemeIndex;
        ++graphemeIndex;

        auto const& cluster = *it;

        char const* clusterStart = it._clusterStart;
        char const* clusterEnd = (next != segmenter.end()) ? next._clusterStart : (text.data() + text.size());
        auto const grapheme = std::string_view(clusterStart, static_cast<size_t>(clusterEnd - clusterStart));

        auto const graphemeWidth = graphemeClusterWidth(cluster);

        auto const isMatch = std::ranges::find(matchPositions, clusterIndex) != matchPositions.end();
        auto const& style = isMatch ? matchStyle : normalStyle;

        canvas.put(row, currentCol, grapheme, style);
        currentCol += graphemeWidth;
    }

    return currentCol - col;
}

} // namespace core::tui
