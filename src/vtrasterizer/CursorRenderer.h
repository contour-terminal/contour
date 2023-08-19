// SPDX-License-Identifier: Apache-2.0
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
