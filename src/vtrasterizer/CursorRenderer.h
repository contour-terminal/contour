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
#pragma once

#include <vtbackend/Color.h>
#include <vtbackend/primitives.h>

#include <vtrasterizer/GridMetrics.h>
#include <vtrasterizer/RenderTarget.h>

#include <crispy/point.h>

#include <array>

namespace terminal::rasterizer
{

/// Takes care of rendering the text cursor.
class CursorRenderer: public Renderable
{
  public:
    CursorRenderer(GridMetrics const& gridMetrics, CursorShape shape);

    void setRenderTarget(RenderTarget& renderTarget, DirectMappingAllocator& directMappingAllocator) override;
    void setTextureAtlas(TextureAtlas& atlas) override;

    void clearCache() override;

    [[nodiscard]] CursorShape shape() const noexcept { return _shape; }
    void setShape(CursorShape shape);

    void render(crispy::point pos, int columnWidth, RGBColor color);

    void inspect(std::ostream& output) const override;

  private:
    void initializeDirectMapping();
    using Renderable::createTileData;
    [[nodiscard]] TextureAtlas::TileCreateData createTileData(CursorShape shape,
                                                              int columnWidth,
                                                              atlas::TileLocation tileLocation);

    DirectMapping _directMapping {};
    CursorShape _shape;
};

} // namespace terminal::rasterizer
