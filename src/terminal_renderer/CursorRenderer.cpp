/**
 * This file is part of the "libterminal" project
 *   Copyright (c) 2019-2020 Christian Parpart <christian@parpart.family>
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
#include <terminal_renderer/CursorRenderer.h>

#include <stdexcept>
#include <vector>

using std::array;
using std::get;
using std::max;
using std::nullopt;
using std::optional;
using std::runtime_error;
using std::string;

namespace terminal::renderer
{

namespace
{
    std::array<float, 4> toVec4(RGBColor v)
    {
        return { float(v.red) / 255.0f, float(v.green) / 255.0f, float(v.blue) / 255.0f, 1.0f };
    }
} // namespace

CursorRenderer::CursorRenderer(GridMetrics const& _gridMetrics, CursorShape _shape):
    gridMetrics_ { _gridMetrics }, shape_ { _shape }, columnWidth_ { 1 }
{
}

void CursorRenderer::setRenderTarget(RenderTarget& _renderTarget)
{
    Renderable::setRenderTarget(_renderTarget);
    clearCache();
}

void CursorRenderer::setShape(CursorShape _shape)
{
    if (_shape != shape_)
        shape_ = _shape;
}

void CursorRenderer::clearCache()
{
    textureAtlas_ = std::make_unique<TextureAtlas>(renderTarget().monochromeAtlasAllocator());
}

void CursorRenderer::rebuild()
{
    clearCache();

    auto const width = Width(*gridMetrics_.cellSize.width * columnWidth_);
    auto const baseline = gridMetrics_.baseline;
    auto constexpr LineThickness = 1;

    { // {{{ CursorShape::Block
        auto const height = gridMetrics_.cellSize.height;
        auto image = atlas::Buffer(*width * *height, 0xFFu);

        textureAtlas_->insert(
            CursorShape::Block, ImageSize { width, height }, ImageSize { width, height }, move(image));
    } // }}}
    { // {{{ CursorShape::Underscore
        auto const thickness = max(LineThickness * baseline / 3, 1);
        auto const height = Height(baseline);
        auto const base_y = max((*height - thickness) / 2, 0u);
        auto image = atlas::Buffer(*width * *height, 0);

        for (int y = 1; y <= thickness; ++y)
            for (int x = 0; x < width.as<int>(); ++x)
                image[(base_y + y) * width.as<int>() + x] = 0xFF;

        textureAtlas_->insert(
            CursorShape::Underscore, ImageSize { width, height }, ImageSize { width, height }, move(image));
    } // }}}
    { // {{{ CursorShape::Bar
        auto const thickness = max(LineThickness * baseline / 3, 1);
        auto const height = gridMetrics_.cellSize.height;
        // auto const base_y = max((height - thickness) / 2, 0);
        auto image = atlas::Buffer(*width * *height, 0);

        for (int x = 0; x < thickness; ++x)
            for (int y = 0; y < *height; ++y)
                image[y * *width + x] = 0xFF;

        textureAtlas_->insert(
            CursorShape::Bar, ImageSize { width, height }, ImageSize { width, height }, move(image));
    } // }}}
    { // {{{ CursorShape::Rectangle
        auto const height = gridMetrics_.cellSize.height;
        auto image = atlas::Buffer(*width * *height, 0xFFu);
        auto const thickness = max(*width / 12, 1u);

        auto const innerWidth = width - 2 * thickness;
        auto const innerHeight = height - 2 * thickness;

        for (int y = thickness; y <= *innerHeight; ++y)
            for (int x = thickness; x <= *innerWidth; ++x)
                image[y * *width + x] = 0;

        textureAtlas_->insert(
            CursorShape::Rectangle, ImageSize { width, height }, ImageSize { width, height }, move(image));
    } // }}}
}

optional<CursorRenderer::DataRef> CursorRenderer::getDataRef(CursorShape _shape)
{
    if (optional<DataRef> const dataRef = textureAtlas_->get(_shape); dataRef.has_value())
        return dataRef;

    if (textureAtlas_->empty())
        rebuild();

    if (optional<DataRef> const dataRef = textureAtlas_->get(_shape); dataRef.has_value())
        return dataRef;

    return nullopt;
}

void CursorRenderer::render(crispy::Point _pos, int _columnWidth, RGBColor _color)
{
    if (columnWidth_ != _columnWidth) // TODO we should optimize here by keying for (shape, columnWidth).
    {
        columnWidth_ = _columnWidth;
        rebuild();
    }

    if (optional<DataRef> const dataRef = getDataRef(shape_); dataRef.has_value())
    {
        auto const& textureInfo = get<0>(dataRef.value()).get();
        auto const x = _pos.x;
        auto const y = _pos.y;
        auto constexpr z = 0;
        textureScheduler().renderTexture({ textureInfo, x, y, z, toVec4(_color) });
    }
}

} // namespace terminal::renderer
