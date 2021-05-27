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

void BackgroundRenderer::renderCell(RenderCell const& _cell)
{
    if (_cell.backgroundColor == defaultColor_)
        return;

    renderTarget().renderRectangle(
        _cell.position.x,
        _cell.position.y,
        gridMetrics_.cellSize.width,
        gridMetrics_.cellSize.height,
        static_cast<float>(_cell.backgroundColor.red) / 255.0f,
        static_cast<float>(_cell.backgroundColor.green) / 255.0f,
        static_cast<float>(_cell.backgroundColor.blue) / 255.0f,
        opacity_
    );
}

} // end namespace
