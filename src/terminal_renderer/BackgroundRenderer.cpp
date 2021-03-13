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

namespace terminal::renderer {

BackgroundRenderer::BackgroundRenderer(GridMetrics const& _gridMetrics,
                                       RGBColor const& _defaultColor) :
    gridMetrics_{ _gridMetrics },
    defaultColor_{ _defaultColor }
{
}

void BackgroundRenderer::setRenderTarget(RenderTarget& _renderTarget)
{
    Renderable::setRenderTarget(_renderTarget);
}

void BackgroundRenderer::renderCell(Coordinate const& _pos, RGBColor const& _color)
{
    if (row_ == _pos.row && color_ == _color)
        columnCount_++;
    else
    {
        renderPendingCells();

        startColumn_ = _pos.column;
        row_ = _pos.row;
        color_ = _color;
        columnCount_ = 1;
    }
}

void BackgroundRenderer::renderOnce(Coordinate const& _pos, RGBColor const& _color, unsigned _count)
{
    renderPendingCells();

    startColumn_ = _pos.column;
    row_ = _pos.row;
    columnCount_ = _count;
    color_ = _color;

    renderCellRange();
}

void BackgroundRenderer::renderCellRange()
{
    if (color_ == defaultColor_)
        return;

    auto const pos = gridMetrics_.map(startColumn_, row_);

    renderTarget().renderRectangle(
        static_cast<unsigned>(pos.x),
        static_cast<unsigned>(pos.y),
        gridMetrics_.cellSize.width * columnCount_,
        gridMetrics_.cellSize.height,
        static_cast<float>(color_.red) / 255.0f,
        static_cast<float>(color_.green) / 255.0f,
        static_cast<float>(color_.blue) / 255.0f,
        opacity_
    );

    columnCount_ = 0;
    startColumn_ = 0;
    row_ = 0;
}

void BackgroundRenderer::renderPendingCells()
{
    if (columnCount_)
        renderCellRange();
}

void BackgroundRenderer::finish()
{
    startColumn_ = 0;
    row_ = 0;
    color_ = RGBColor{};
    columnCount_ = 0;
}

} // end namespace
