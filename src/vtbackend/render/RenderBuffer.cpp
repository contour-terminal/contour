// SPDX-License-Identifier: Apache-2.0
#include <vtbackend/render/RenderBuffer.hpp>

#include <libunicode/width.h>

#include <algorithm>
#include <format>
#include <mutex>

namespace vtbackend
{

ColumnOffset layoutRenderText(std::vector<RenderCell>& output,
                              std::u32string_view text,
                              RenderAttributes const& attributes,
                              CellLocation origin,
                              ColumnOffset end)
{
    auto const first = output.size();
    auto column = origin.column;
    for (auto const codepoint: text)
    {
        auto const width = static_cast<int>(unicode::width(codepoint));

        // A combining mark or a variation selector belongs to the glyph before it -- one of this text's.
        if (width == 0 && output.size() > first)
        {
            output.back().codepoints.push_back(codepoint);
            continue;
        }

        auto const columns = std::max(1, width);
        if (column + ColumnOffset(columns) > end)
            break;

        output.push_back(RenderCell { .codepoints = std::u32string(1, codepoint),
                                      .image = {},
                                      .position = CellLocation { .line = origin.line, .column = column },
                                      .attributes = attributes,
                                      .width = static_cast<uint8_t>(columns),
                                      .sizing = {},
                                      .groupStart = false,
                                      .groupEnd = false });
        column += ColumnOffset(columns);
    }

    markShapingGroup(std::span { output }.subspan(first));
    return column;
}

void markShapingGroup(std::span<RenderCell> cells) noexcept
{
    if (cells.empty())
        return;
    cells.front().groupStart = true;
    cells.back().groupEnd = true;
}

bool RenderDoubleBuffer::swapBuffers(std::chrono::steady_clock::time_point now) noexcept
{
    // If the terminal thread (writer) cannot try_lock (w/o wait time)
    // the front buffer, it'll just flush back buffer instead of swapping
    // buffers as the front buffer is apparently still in use by the
    // renderer thread and we want to avoid render-thread imposed
    // wait times in the terminal thread as much as possible.

    if (!readerLock.try_lock())
        return false;

    auto const _ = std::lock_guard<decltype(readerLock)>(readerLock, std::adopt_lock);

    for (;;)
    {
        auto current = currentBackBufferIndex.load();
        if (currentBackBufferIndex.compare_exchange_weak(current, (current + 1) % 2))
            break;
    }

    // Only the writer thread swaps, so the buffer just put in front stays there while this reads it.
    auto const& front = buffers[(currentBackBufferIndex.load() + 1) % 2];
    // Only a header drawn over the grid takes a press on the row it covers; one in the chrome covers none.
    frontStickyHeaderBlock =
        front.stickyHeader && front.stickyHeader->presentation == StickyHeaderPresentation::InGrid
            ? front.stickyHeader->block
            : CommandBlockId {};

    lastUpdate.store(now, std::memory_order_relaxed);
    state = RenderBufferState::WaitingForRefresh;
    return true;
}

} // namespace vtbackend
