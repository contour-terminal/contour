// SPDX-License-Identifier: Apache-2.0
#include <vtrasterizer/TextClusterGrouper.hpp>

#include <core/log/Assert.hpp>

#include <cassert>

namespace vtrasterizer
{

TextClusterGrouper::TextClusterGrouper(Events& events): _events { events }
{
}

void TextClusterGrouper::beginFrame() noexcept
{
    if (!SoftRequire(_codepoints.empty() && _clusters.empty()))
    {
        _codepoints.clear();
        _clusters.clear();
    }

    auto constexpr DefaultColor = vtbackend::RGBColor {};
    _style = TextStyle::Invalid;
    _color = DefaultColor;
}

void TextClusterGrouper::renderLine(std::u32string_view text,
                                    vtbackend::LineOffset lineOffset,
                                    vtbackend::RGBColor foregroundColor,
                                    TextStyle style,
                                    vtbackend::LineFlags flags)
{
    if (text.empty())
        return;

    auto columnOffset = vtbackend::ColumnOffset(0);
    auto const columnScale = flags.test(vtbackend::LineFlag::DoubleWidth) ? 2 : 1;

    _initialPenPosition = vtbackend::CellLocation { .line = lineOffset, .column = columnOffset };

    // Iterate char32_t codepoints directly — no UTF-8 decode needed.
    //
    // Each codepoint here is a grapheme cluster of its own: a cell holding more than one takes its
    // line off the batched path before it ever gets here (@see vtbackend::appendCodepointToCluster).
    // Its WIDTH is still a question — a wide character normally leaves the path too, through the
    // continuation cell it writes, but Screen::clearAndAdvance skips that fill when only one column
    // remains, so a full-width character on the last writable column does arrive. Hence the measure
    // and the filler loop below, which RenderBufferBuilder::renderGridText mirrors exactly: the two
    // draw the same line whenever a selection switches between them.
    //
    // Positions are display columns, two per character on a double-width line, which is how
    // RenderBufferBuilder::displayColumn places the cells of a line that is not trivial: a group then
    // starts, and its clusters count, in the same unit whichever way the line arrives.
    for (auto const cp: text)
    {
        auto const gridPosition = vtbackend::CellLocation { .line = lineOffset, .column = columnOffset };
        auto const width = unicode::width(cp);
        renderCell(gridPosition, std::u32string_view(&cp, 1), foregroundColor, style, flags);

        for (unsigned i = 1; i < width; ++i)
            renderCell(vtbackend::CellLocation { .line = gridPosition.line,
                                                 .column = columnOffset + static_cast<int>(i) * columnScale },
                       U" ",
                       foregroundColor,
                       style,
                       flags);

        columnOffset += vtbackend::ColumnOffset::cast_from(std::max(1u, width) * columnScale);
    }

    if (!_codepoints.empty())
        flushTextClusterGroup();
}

void TextClusterGrouper::renderCell(vtbackend::CellLocation position,
                                    std::u32string_view graphemeCluster,
                                    vtbackend::RGBColor foregroundColor,
                                    TextStyle style,
                                    vtbackend::LineFlags flags,
                                    vtbackend::GlyphSizing const& sizing)
{
    if (_forceUpdateInitialPenPosition)
    {
        if (!SoftRequire(_codepoints.empty()))
            flushTextClusterGroup();
        _forceUpdateInitialPenPosition = false;
    }

    bool const isBoxDrawingCharacter =
        graphemeCluster.size() == 1 && BoxDrawingRenderer::renderable(graphemeCluster[0]);

    if (isBoxDrawingCharacter)
    {
        auto const success =
            _events.renderBoxDrawingCell(position, graphemeCluster[0], foregroundColor, flags);
        if (success)
        {
            flushTextClusterGroup();
            _forceUpdateInitialPenPosition = true;
            return;
        }
    }

    appendCellTextToClusterGroup(position, graphemeCluster, style, foregroundColor, flags, sizing);
}

void TextClusterGrouper::appendCellTextToClusterGroup(vtbackend::CellLocation position,
                                                      std::u32string_view codepoints,
                                                      TextStyle style,
                                                      vtbackend::RGBColor color,
                                                      vtbackend::LineFlags flags,
                                                      vtbackend::GlyphSizing const& sizing)
{
    bool const attribsChanged =
        color != _color || style != _style || flags != _lineFlags || sizing != _sizing;
    bool const cellIsEmpty = codepoints.empty() || codepoints[0] == 0x20;

    if (attribsChanged || (_codepoints.empty() && !cellIsEmpty))
    {
        flushTextClusterGroup();
        _color = color;
        _style = style;
        _lineFlags = flags;
        _sizing = sizing;
    }

    if (cellIsEmpty)
    {
        flushTextClusterGroup();
        _forceUpdateInitialPenPosition = true;
        return;
    }

    // A group starts where its first cell is, and each cell's cluster is the column it starts at,
    // counted from there -- not the number of cells before it, which a wide character written as one
    // cell without a continuation cell after it (RenderBuffer's layoutRenderText) would undercount.
    if (_codepoints.empty())
        _initialPenPosition = position;
    auto const cluster = static_cast<unsigned>(unbox(position.column - _initialPenPosition.column));
    for (char32_t const codepoint: codepoints)
    {
        _codepoints.emplace_back(codepoint);
        _clusters.emplace_back(cluster);
    }
}

void TextClusterGrouper::flushTextClusterGroup()
{
    if (!_codepoints.empty())
    {
        _events.renderTextGroup(std::u32string_view(_codepoints.data(), _codepoints.size()),
                                gsl::span(_clusters.data(), _clusters.size()),
                                _initialPenPosition,
                                _style,
                                _color,
                                _lineFlags,
                                _sizing);
    }

    _codepoints.clear();
    _clusters.clear();
}

void TextClusterGrouper::endFrame()
{
    if (!_codepoints.empty())
        flushTextClusterGroup();
}

} // namespace vtrasterizer
