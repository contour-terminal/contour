// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <vtbackend/Color.hpp>
#include <vtbackend/primitives.hpp>

#include <vtrasterizer/GridMetrics.hpp>
#include <vtrasterizer/RenderTarget.hpp>

#include <crispy/point.hpp>

namespace vtrasterizer
{

/// Takes care of rendering the text cursor.
class CursorRenderer: public Renderable
{
  public:
    CursorRenderer(GridMetrics const& gridMetrics, vtbackend::CursorShape shape);

    void setRenderTarget(RenderTarget& renderTarget, DirectMappingAllocator& directMappingAllocator) override;
    void setTextureAtlas(TextureAtlas& atlas) override;

    void clearCache() override;

    [[nodiscard]] vtbackend::CursorShape shape() const noexcept { return _shape; }
    void setShape(vtbackend::CursorShape shape);

    void render(crispy::Point pos, int columnWidth, vtbackend::RGBColor color);

    void inspect(std::ostream& output) const override;

  private:
    void initializeDirectMapping();
    using Renderable::createTileData;
    [[nodiscard]] TextureAtlas::TileCreateData createTileData(vtbackend::CursorShape shape,
                                                              int columnWidth,
                                                              atlas::TileLocation tileLocation);

    DirectMapping _directMapping {};
    vtbackend::CursorShape _shape;
};

} // namespace vtrasterizer
