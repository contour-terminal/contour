// SPDX-License-Identifier: Apache-2.0
#include <vtrasterizer/BackgroundRenderer.h>
#include <vtrasterizer/GridMetrics.h>
#include <vtrasterizer/RenderTarget.h>

#include <crispy/algorithm.h>

#include <algorithm>
#include <iostream>

namespace terminal::rasterizer
{

BackgroundRenderer::BackgroundRenderer(GridMetrics const& gridMetrics, RGBColor const& defaultColor):
    Renderable { gridMetrics }, _defaultColor { defaultColor }
{
}

void BackgroundRenderer::setRenderTarget(RenderTarget& renderTarget,
                                         DirectMappingAllocator& directMappingAllocator)
{
    Renderable::setRenderTarget(renderTarget, directMappingAllocator);
}

void BackgroundRenderer::renderLine(RenderLine const& line)
{
    if (line.textAttributes.backgroundColor != _defaultColor)
    {
        auto const position = CellLocation { line.lineOffset, ColumnOffset(0) };
        auto const pos = _gridMetrics.mapTopLeft(position);
        auto const width = _gridMetrics.cellSize.width * Width::cast_from(line.usedColumns);

        renderTarget().renderRectangle(pos.x,
                                       pos.y,
                                       width,
                                       _gridMetrics.cellSize.height,
                                       RGBAColor(line.textAttributes.backgroundColor, _opacity));
    }

    if (line.fillAttributes.backgroundColor != _defaultColor)
    {
        auto const position = CellLocation { line.lineOffset, boxed_cast<ColumnOffset>(line.usedColumns) };
        auto const pos = _gridMetrics.mapTopLeft(position);
        auto const width =
            _gridMetrics.cellSize.width * Width::cast_from(line.displayWidth - line.usedColumns);

        renderTarget().renderRectangle(pos.x,
                                       pos.y,
                                       width,
                                       _gridMetrics.cellSize.height,
                                       RGBAColor(line.fillAttributes.backgroundColor, _opacity));
    }
}

void BackgroundRenderer::renderCell(RenderCell const& cell)
{
    if (cell.attributes.backgroundColor == _defaultColor)
        return;

    auto const pos = _gridMetrics.mapTopLeft(cell.position);

    renderTarget().renderRectangle(pos.x,
                                   pos.y,
                                   _gridMetrics.cellSize.width * Width::cast_from(cell.width),
                                   _gridMetrics.cellSize.height,
                                   RGBAColor(cell.attributes.backgroundColor, _opacity));
}

void BackgroundRenderer::inspect(std::ostream& /*output*/) const
{
}

} // namespace terminal::rasterizer
