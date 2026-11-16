// SPDX-License-Identifier: Apache-2.0
#include <core/tui/FrameDiff.hpp>

#include <core/tui/ByteSinkOutput.hpp>
#include <core/tui/HyperlinkEmitter.hpp>
#include <core/tui/TerminalOutput.hpp>

#include <algorithm>
#include <cstddef>
#include <iterator>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace core::tui
{

namespace
{

    /// @brief A frame's emittable row marks, grouped by row and sorted by column.
    ///
    /// Marks no cell write can reach are left out: those outside the buffer and those on a wide
    /// character's continuation column, which no cell write starts at.
    class RowMarkIndex
    {
      public:
        /// @param buffer The frame whose marks to index. Must outlive this object.
        explicit RowMarkIndex(Buffer const& buffer)
        {
            _sorted.reserve(buffer.marks().size());
            for (auto const& mark: buffer.marks())
            {
                auto const* cell = buffer.cellAt({ .x = mark.column, .y = mark.row });
                if (cell != nullptr && !cell->isContinuation())
                    _sorted.push_back(&mark);
            }
            std::ranges::stable_sort(_sorted, [](RowMark const* a, RowMark const* b) {
                return std::pair { a->row, a->column } < std::pair { b->row, b->column };
            });
        }

        /// @brief Returns @p row's marks, sorted by column (registration order among equals).
        [[nodiscard]] std::span<RowMark const* const> row(int row) const
        {
            auto const range = std::ranges::equal_range(_sorted, row, {}, &RowMark::row);
            return { range.begin(), range.end() };
        }

        /// @brief Whether @p row's marks (column and sequence, in order) differ from @p other's.
        [[nodiscard]] bool rowDiffers(int row, RowMarkIndex const& other) const
        {
            return !std::ranges::equal(
                this->row(row), other.row(row), [](RowMark const* a, RowMark const* b) {
                    return a->column == b->column && a->sequence == b->sequence;
                });
        }

      private:
        std::vector<RowMark const*> _sorted;
    };

    /// @brief Emits a frame's row marks ahead of the cells they govern.
    ///
    /// A row's marks, sorted by column, split it into segments `[mark.column, next.column)`.
    /// Before a repainted cell is written, the mark of the segment holding it is written at the
    /// mark's own column, unless that segment's mark was already the last one written on this
    /// row. Marks are zero-width, so the cursor is moved to the mark's column and back.
    class RowMarkEmitter
    {
      public:
        /// @param out Sink receiving the marks. Must outlive this object.
        /// @param index The marks of the frame being written. Must outlive this object.
        RowMarkEmitter(TerminalOutput& out, RowMarkIndex const& index): _out(out), _index(index) {}

        /// @brief Selects @p row's marks and forgets the last segment written.
        void beginRow(int row)
        {
            _row = _index.row(row);
            _lastSegment.reset();
        }

        /// @brief Writes the mark governing column @p col if it is not already in force.
        /// @param col The column of the cell about to be written; the cursor is there.
        void beforeWrite(int col)
        {
            if (_row.empty())
                return;

            auto const next = std::ranges::upper_bound(_row, col, {}, &RowMark::column);
            if (next == _row.begin())
                return; // Left of the row's first mark: no segment, nothing to establish.

            auto const segment = static_cast<std::size_t>(std::distance(_row.begin(), next) - 1);
            if (_lastSegment == segment)
                return;

            auto const& mark = *_row[segment];
            auto const distance = col - mark.column;
            if (distance > 0)
                _out.moveLeft(distance);
            _out.writeRaw(mark.sequence);
            if (distance > 0)
                _out.moveRight(distance);
            _lastSegment = segment;
        }

      private:
        TerminalOutput& _out;
        RowMarkIndex const& _index;
        std::span<RowMark const* const> _row;
        std::optional<std::size_t> _lastSegment;
    };

    /// @brief The out-of-band state one diff carries: both frames' links and marks, each empty
    /// where its option says to omit it, so the cell loops ask the same questions either way.
    class DiffContext
    {
      public:
        DiffContext(TerminalOutput& out,
                    Buffer const& previous,
                    Buffer const& next,
                    DiffOptions const& options):
            _diffing(options.repaint == DiffRepaint::Changed),
            _links(out,
                   options.hyperlinks == DiffHyperlinks::Emit ? next : _none,
                   options.hyperlinks == DiffHyperlinks::Emit && _diffing ? &previous : nullptr),
            _currentMarks(options.rowMarks == DiffRowMarks::Emit ? next : _none),
            _previousMarks(options.rowMarks == DiffRowMarks::Emit ? previous : _none),
            _marks(out, _currentMarks),
            _previous(previous)
        {
        }

        /// @brief Whether only changed cells are written.
        [[nodiscard]] bool diffing() const noexcept { return _diffing; }

        /// @brief Selects @p row's marks; answers whether they changed, which repaints the row whole.
        [[nodiscard]] bool beginRow(int row)
        {
            _marks.beginRow(row);
            return _diffing && _currentMarks.rowDiffers(row, _previousMarks);
        }

        /// @brief Whether @p cell at (@p row, @p col) differs from what the previous frame showed there.
        [[nodiscard]] bool changed(Cell const& cell, int row, int col) const noexcept
        {
            auto const* previous = _previous.cellAt({ .x = col, .y = row });
            return previous == nullptr || cell != *previous || _links.retargeted(row, col);
        }

        /// @brief Writes what must precede the cell at (@p row, @p col): its mark, then its link.
        void beforeWrite(int row, int col)
        {
            _marks.beforeWrite(col);
            _links.emitter.beforeWrite(row, col);
        }

        /// @brief Closes any open link, as the end of a row or an erase requires.
        void closeLink() { _links.emitter.close(); }

      private:
        /// An empty frame stands in for the one an omitted option does not read.
        Buffer _none;
        bool _diffing;
        FlushLinks _links;
        RowMarkIndex _currentMarks;
        RowMarkIndex _previousMarks;
        RowMarkEmitter _marks;
        Buffer const& _previous;
    };

    /// @brief Whether @p image is in @p frame unchanged: same cells, same pixels.
    [[nodiscard]] bool hasImage(Buffer const& frame, ImageRegion const& image) noexcept
    {
        return std::ranges::any_of(frame.images(), [&](ImageRegion const& other) {
            return other.cellArea == image.cellArea && other.contentHash == image.contentHash;
        });
    }

    /// @brief Whether (@p row, @p col) lies in one of @p images' areas.
    [[nodiscard]] bool covered(std::span<ImageRegion const* const> images, int row, int col) noexcept
    {
        return std::ranges::any_of(images, [&](ImageRegion const* image) {
            return image->cellArea.contains(Point { .x = col, .y = row });
        });
    }

    /// @brief Absolute addressing: a cursor move to each written cell, then the images.
    void writeAbsolute(TerminalOutput& out,
                       Buffer const& previous,
                       Buffer const& next,
                       DiffOptions const& options)
    {
        auto context = DiffContext { out, previous, next, options };
        auto const area =
            options.area.value_or(Rect { .x = 0, .y = 0, .width = next.cols(), .height = next.rows() });

        // With images on, the cells under one of the next frame's images are the image's; and a cell
        // an image of the previous frame covered, which the next frame does not keep, is repainted
        // even if its own content did not change, or the old pixels would stay over it.
        auto nextImages = std::vector<ImageRegion const*> {};
        auto staleImages = std::vector<ImageRegion const*> {};
        if (options.images == DiffImages::Emit)
        {
            for (auto const& image: next.images())
                nextImages.push_back(&image);
            if (context.diffing())
                for (auto const& image: previous.images())
                    if (!hasImage(next, image))
                        staleImages.push_back(&image);
        }

        // Clipped to the frame: an area wholly outside it writes nothing.
        auto const clipped =
            area.intersect(Rect { .x = 0, .y = 0, .width = next.cols(), .height = next.rows() });
        for (auto const row: std::views::iota(clipped.y, clipped.bottom()))
        {
            auto const rowMarksChanged = context.beginRow(row);
            auto col = clipped.x;
            while (col < clipped.right())
            {
                auto const& cell = *next.cellAt({ .x = col, .y = row });
                auto const advance = std::max(1, static_cast<int>(cell.width));

                // A continuation cell is written with the wide character before it; an image's cells
                // are the image's.
                if (cell.isContinuation() || covered(nextImages, row, col))
                {
                    col += cell.isContinuation() ? 1 : advance;
                    continue;
                }

                if (!context.diffing() || rowMarksChanged || covered(staleImages, row, col)
                    || context.changed(cell, row, col))
                {
                    out.moveTo(row + 1, col + 1); // Terminal is 1-based
                    context.beforeWrite(row, col);
                    out.writeText(cell.grapheme, cell.style);
                }
                col += advance;
            }
            // Never hold framing open across a row boundary: the next row's cells are reached by
            // absolute cursor motion, which the terminal does not treat as ending a link.
            context.closeLink();
        }

        for (auto const* image: nextImages)
        {
            auto const origin = Point { .x = image->cellArea.x, .y = image->cellArea.y };
            if (!area.contains(origin) || image->encodedSixel.empty())
                continue;
            if (context.diffing() && hasImage(previous, *image))
                continue;
            out.moveTo(origin.y + 1, origin.x + 1);
            out.writeSixel(image->encodedSixel);
        }
    }

    /// @brief Relative addressing: row by row from column 0 of the area's first row, ending on its last.
    void writeRelative(TerminalOutput& out,
                       Buffer const& previous,
                       Buffer const& next,
                       DiffOptions const& options)
    {
        auto context = DiffContext { out, previous, next, options };
        auto const area =
            options.area.value_or(Rect { .x = 0, .y = 0, .width = next.cols(), .height = next.rows() });
        auto const firstRow = std::max(0, area.y);
        auto const lastRow = std::min(area.bottom(), next.rows());

        // The image (if any) that starts at each row, and, when diffing, the previous frame's, so an
        // unchanged image is not re-emitted.
        auto imageAtRow = std::unordered_map<int, ImageRegion const*> {};
        auto previousImageAtRow = std::unordered_map<int, ImageRegion const*> {};
        if (options.images == DiffImages::Emit)
        {
            for (auto const& img: next.images())
                imageAtRow[img.cellArea.y] = &img;
            if (context.diffing())
                for (auto const& img: previous.images())
                    previousImageAtRow[img.cellArea.y] = &img;
        }

        auto row = firstRow;
        while (row < lastRow)
        {
            out.carriageReturn(); // Move to start of line

            // Check if an image starts at this row
            if (auto it = imageAtRow.find(row); it != imageAtRow.end())
            {
                auto const& img = *it->second;

                // Only emit sixel when the image actually changed from the previous frame.
                auto needsEmit = !context.diffing();
                if (!needsEmit)
                {
                    auto const prevIt = previousImageAtRow.find(row);
                    needsEmit =
                        (prevIt == previousImageAtRow.end() || prevIt->second->cellArea != img.cellArea
                         || prevIt->second->contentHash != img.contentHash);
                }

                if (needsEmit && !img.encodedSixel.empty())
                {
                    // Save cursor before sixel — the terminal may advance the cursor
                    // below the image (DECSIXEL mode 80 set), which would corrupt
                    // our cursor tracking. DECSC/DECRC restores the exact position.
                    out.saveCursor();
                    if (img.cellArea.x > 0)
                        out.moveRight(img.cellArea.x);
                    out.writeSixel(img.encodedSixel);
                    out.restoreCursor();
                }

                // Skip cell rendering for all rows covered by this image.
                // Don't clearToEndOfLine — the sixel covers the visual area and
                // clearing might erase the pixel data in some terminals.
                auto const skipRows = img.cellArea.height - 1;
                auto skipped = 0;
                while (skipped < skipRows && row + 1 < lastRow)
                {
                    out.linefeed();
                    ++row;
                    ++skipped;
                }
                if (row < lastRow - 1)
                    out.linefeed();
                ++row;
                continue;
            }

            if (options.reflow == DiffReflow::Inhibit)
                out.disableReflow();

            // Marks are diffed the way hyperlinks are: they live beside the cells, so Cell equality
            // cannot see a changed one. A row whose marks changed repaints whole, which re-emits each.
            auto const rowMarksChanged = context.beginRow(row);
            auto col = 0;
            while (col < next.cols())
            {
                auto const& cell = *next.cellAt({ .x = col, .y = row });

                if (cell.isContinuation())
                {
                    ++col;
                    continue;
                }

                if (!context.diffing() || rowMarksChanged || context.changed(cell, row, col))
                {
                    // Written in sequence: the cursor is already on this cell.
                    context.beforeWrite(row, col);
                    out.writeText(cell.grapheme, cell.style);
                }
                else
                {
                    // Move cursor past unchanged cells. Cursor motion does not carry the link
                    // attribute, and skipped cells already hold the link recorded when they were
                    // last written — which is the link we would otherwise have re-emitted.
                    out.moveRight(cell.width > 0 ? cell.width : 1);
                }

                col += std::max(1, static_cast<int>(cell.width));
            }

            // Close before the erase so the cleared tail is not attributed to the link.
            context.closeLink();
            out.clearToEndOfLine();

            if (row < lastRow - 1)
                out.linefeed();
            ++row;
        }
    }

} // namespace

void writeFrameDiff(TerminalOutput& out,
                    Buffer const& previous,
                    Buffer const& next,
                    DiffOptions const& options)
{
    switch (options.addressing)
    {
        case DiffAddressing::Absolute: writeAbsolute(out, previous, next, options); return;
        case DiffAddressing::Relative: writeRelative(out, previous, next, options); return;
    }
}

std::string diffFrames(Buffer const& previous, Buffer const& next, DiffOptions options)
{
    auto bytes = std::string {};
    auto out = ByteSinkOutput { [&bytes](std::string_view chunk) { bytes.append(chunk); },
                                next.size(),
                                SinkDestination::Plain };
    writeFrameDiff(out, previous, next, options);
    out.flush();
    return bytes;
}

} // namespace core::tui
