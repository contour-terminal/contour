// SPDX-License-Identifier: Apache-2.0
#include <vtrasterizer/TextClusterGrouper.h>

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
    // For trivial lines, each codepoint is a single-codepoint grapheme cluster.
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

        columnOffset += vtbackend::ColumnOffset::cast_from(std::max(1u, width));
    }

    if (!_codepoints.empty())
        flushTextClusterGroup();
}

void TextClusterGrouper::renderCell(vtbackend::CellLocation position,
                                    std::u32string_view graphemeCluster,
                                    vtbackend::RGBColor foregroundColor,
                                    TextStyle style,
                                    vtbackend::LineFlags flags)
{
    if (_forceUpdateInitialPenPosition)
    {
        if (!SoftRequire(_codepoints.empty()))
            flushTextClusterGroup();
        _initialPenPosition = position;
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

    appendCellTextToClusterGroup(graphemeCluster, style, foregroundColor, flags);
}

void TextClusterGrouper::appendCellTextToClusterGroup(std::u32string_view codepoints,
                                                      TextStyle style,
                                                      vtbackend::RGBColor color,
                                                      vtbackend::LineFlags flags)
{
    bool const attribsChanged = color != _color || style != _style || flags != _lineFlags;
    bool const cellIsEmpty = codepoints.empty() || codepoints[0] == 0x20;
    bool const textStartsNewCluster = _cellCount == 0 && !cellIsEmpty;

    if (attribsChanged || textStartsNewCluster)
    {
        if (_cellCount)
            flushTextClusterGroup(); // also increments text start position
        _color = color;
        _style = style;
        _lineFlags = flags;
    }

    if (!cellIsEmpty)
    {
        for (char32_t const codepoint: codepoints)
        {
            _codepoints.emplace_back(codepoint);
            _clusters.emplace_back(_cellCount);
        }
        _cellCount++;
    }
    else
    {
        flushTextClusterGroup(); // also increments text start position
        _forceUpdateInitialPenPosition = true;
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
                                _lineFlags);
    }

    resetAndMovePenForward(vtbackend::ColumnOffset::cast_from(_cellCount));
}

void TextClusterGrouper::endFrame()
{
    if (!_codepoints.empty())
        flushTextClusterGroup();
}

} // namespace vtrasterizer
