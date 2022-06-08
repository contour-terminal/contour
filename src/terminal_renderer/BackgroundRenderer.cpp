/**
 * This file is part of the "contour" project.
 *   Copyright (c) 2020 Christian Parpart <christian@parpart.family>
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#include <terminal_renderer/BackgroundRenderer.h>
#include <terminal_renderer/GridMetrics.h>
#include <terminal_renderer/RenderTarget.h>

#include <crispy/algorithm.h>

#include <algorithm>
#include <iostream>

namespace terminal::renderer
{

BackgroundRenderer::BackgroundRenderer(GridMetrics const& gridMetrics, RGBColor const& defaultColor):
    Renderable { gridMetrics }, defaultColor_ { defaultColor }
{
}

void BackgroundRenderer::setRenderTarget(RenderTarget& renderTarget,
                                         DirectMappingAllocator& directMappingAllocator)
{
    Renderable::setRenderTarget(renderTarget, directMappingAllocator);
}

void BackgroundRenderer::renderLine(RenderLine const& line)
{
    if (line.backgroundColor == defaultColor_)
        return;

    auto const position = CellLocation { line.lineOffset, ColumnOffset(0) };
    auto const pos = _gridMetrics.map(position);
    auto const width = _gridMetrics.cellSize.width * Width::cast_from(line.usedColumns);

    renderTarget().renderRectangle(
        pos.x, pos.y, width, _gridMetrics.cellSize.height, RGBAColor(line.backgroundColor, opacity_));
}

void BackgroundRenderer::renderCell(RenderCell const& _cell)
{
    if (_cell.backgroundColor == defaultColor_)
        return;

    auto const pos = _gridMetrics.map(_cell.position);

    renderTarget().renderRectangle(pos.x,
                                   pos.y,
                                   _gridMetrics.cellSize.width,
                                   _gridMetrics.cellSize.height,
                                   RGBAColor(_cell.backgroundColor, opacity_));
}

void BackgroundRenderer::inspect(std::ostream& /*output*/) const
{
}

} // namespace terminal::renderer
