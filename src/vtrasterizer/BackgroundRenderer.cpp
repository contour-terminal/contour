// SPDX-License-Identifier: Apache-2.0
#include <vtrasterizer/BackgroundRenderer.h>
#include <vtrasterizer/GridMetrics.h>
#include <vtrasterizer/RenderTarget.h>

#include <crispy/algorithm.h>

#include <iostream>

namespace vtrasterizer
{

BackgroundRenderer::BackgroundRenderer(GridMetrics const& gridMetrics,
                                       vtbackend::RGBColor const& defaultColor):
    Renderable { gridMetrics }, _defaultColor { defaultColor }
{
}

void BackgroundRenderer::setRenderTarget(RenderTarget& renderTarget,
                                         DirectMappingAllocator& directMappingAllocator)
{
    Renderable::setRenderTarget(renderTarget, directMappingAllocator);
}

void BackgroundRenderer::renderLine(vtbackend::RenderLine const& line)
{
    auto const scale = line.flags.test(vtbackend::LineFlag::DoubleWidth) ? 2 : 1;

    if (line.textAttributes.backgroundColor != _defaultColor)
    {
        auto const position =
            vtbackend::CellLocation { .line = line.lineOffset, .column = vtbackend::ColumnOffset(0) };
        auto const pos = _gridMetrics.mapTopLeft(position, _smoothScrollYOffset);
        auto const width =
            _gridMetrics.cellSize.width * vtbackend::Width::cast_from(line.usedColumns * scale);

        renderTarget().renderRectangle(pos.x,
                                       pos.y,
                                       width,
                                       _gridMetrics.cellSize.height,
                                       vtbackend::RGBAColor(line.textAttributes.backgroundColor, _opacity));
    }

    if (line.fillAttributes.backgroundColor != _defaultColor)
    {
        auto const position = vtbackend::CellLocation {
            .line = line.lineOffset,
            .column = boxed_cast<vtbackend::ColumnOffset>(line.usedColumns * scale),
        };
        auto const pos = _gridMetrics.mapTopLeft(position, _smoothScrollYOffset);
        auto const width = _gridMetrics.cellSize.width
                           * vtbackend::Width::cast_from((line.displayWidth - line.usedColumns) * scale);

        renderTarget().renderRectangle(pos.x,
                                       pos.y,
                                       width,
                                       _gridMetrics.cellSize.height,
                                       vtbackend::RGBAColor(line.fillAttributes.backgroundColor, _opacity));
    }
}

void BackgroundRenderer::renderCell(vtbackend::RenderCell const& cell)
{
    if (cell.attributes.backgroundColor == _defaultColor)
        return;

    auto const pos = _gridMetrics.mapTopLeft(cell.position, _smoothScrollYOffset);
    auto const scale = cell.attributes.lineFlags.test(vtbackend::LineFlag::DoubleWidth) ? 2 : 1;

    renderTarget().renderRectangle(pos.x,
                                   pos.y,
                                   _gridMetrics.cellSize.width
                                       * vtbackend::Width::cast_from(cell.width * scale),
                                   _gridMetrics.cellSize.height,
                                   vtbackend::RGBAColor(cell.attributes.backgroundColor, _opacity));
}

void BackgroundRenderer::inspect(std::ostream& /*output*/) const
{
}

} // namespace vtrasterizer
