// SPDX-License-Identifier: Apache-2.0
#include <vtrasterizer/TextClusterGrouper.h>

#include <libunicode/utf8_grapheme_segmenter.h>

#include <cassert>

namespace vtrasterizer
{

namespace
{
    uint8_t graphemeClusterWidth(std::u32string_view text) noexcept
    {
        assert(!text.empty());
        auto const baseWidth = static_cast<uint8_t>(unicode::width(text[0]));
        for (size_t i = 1; i < text.size(); ++i)
            if (text[i] == 0xFE0F)
                return 2;
        return baseWidth;
    }
} // namespace

TextClusterGrouper::TextClusterGrouper(Events& events): _events { events }
{
}

void TextClusterGrouper::beginFrame() noexcept
{
    Require(_codepoints.empty());
    Require(_clusters.empty());

    auto constexpr DefaultColor = vtbackend::RGBColor {};
    _style = TextStyle::Invalid;
    _color = DefaultColor;
}

void TextClusterGrouper::renderLine(std::string_view text,
                                    vtbackend::LineOffset lineOffset,
                                    vtbackend::RGBColor foregroundColor,
                                    TextStyle style)
{
    if (text.empty())
        return;

    auto graphemeClusterSegmenter = unicode::utf8_grapheme_segmenter(text);
    auto columnOffset = vtbackend::ColumnOffset(0);

    _initialPenPosition = vtbackend::CellLocation { .line = lineOffset, .column = columnOffset };

    for (auto const& graphemeCluster: graphemeClusterSegmenter)
    {
        auto const gridPosition = vtbackend::CellLocation { .line = lineOffset, .column = columnOffset };
        auto const width = graphemeClusterWidth(graphemeCluster);
        renderCell(gridPosition, graphemeCluster, style, foregroundColor);

        for (int i = 1; std::cmp_less(i, width); ++i)
            renderCell(vtbackend::CellLocation { .line = gridPosition.line, .column = columnOffset + i },
                       U" ",
                       style,
                       foregroundColor);

        columnOffset += vtbackend::ColumnOffset::cast_from(width);
    }

    if (!_codepoints.empty())
        flushTextClusterGroup();
}

void TextClusterGrouper::renderCell(vtbackend::CellLocation position,
                                    std::u32string_view graphemeCluster,
                                    TextStyle style,
                                    vtbackend::RGBColor foregroundColor)
{
    if (_forceUpdateInitialPenPosition)
    {
        assert(_codepoints.empty());
        _initialPenPosition = position;
        _forceUpdateInitialPenPosition = false;
    }

    bool const isBoxDrawingCharacter =
        graphemeCluster.size() == 1 && BoxDrawingRenderer::renderable(graphemeCluster[0]);

    if (isBoxDrawingCharacter)
    {
        auto const success = _events.renderBoxDrawingCell(position, graphemeCluster[0], foregroundColor);
        if (success)
        {
            flushTextClusterGroup();
            _forceUpdateInitialPenPosition = true;
            return;
        }
    }

    appendCellTextToClusterGroup(graphemeCluster, style, foregroundColor);
}

void TextClusterGrouper::appendCellTextToClusterGroup(std::u32string_view codepoints,
                                                      TextStyle style,
                                                      vtbackend::RGBColor color)
{
    bool const attribsChanged = color != _color || style != _style;
    bool const cellIsEmpty = codepoints.empty() || codepoints[0] == 0x20;
    bool const textStartsNewCluster = _cellCount == 0 && !cellIsEmpty;

    if (attribsChanged || textStartsNewCluster)
    {
        if (_cellCount)
            flushTextClusterGroup(); // also increments text start position
        _color = color;
        _style = style;
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
                                _color);
    }

    resetAndMovePenForward(vtbackend::ColumnOffset::cast_from(_cellCount));
}

void TextClusterGrouper::endFrame()
{
    if (!_codepoints.empty())
        flushTextClusterGroup();
}

} // namespace vtrasterizer
