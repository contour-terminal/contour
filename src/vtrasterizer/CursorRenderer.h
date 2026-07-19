// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <vtbackend/Color.h>
#include <vtbackend/primitives.h>

#include <vtrasterizer/GridMetrics.h>
#include <vtrasterizer/RenderTarget.h>

#include <crispy/point.h>

#include <libunicode/bidi.h>

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

    /// @param direction      writing direction of the character the cursor sits on; decides which edge
    ///                       of the cell a Bar cursor is drawn against.
    /// @param mixedDirection whether the cursor's paragraph mixes writing directions, which is the
    ///                       only case in which @p direction is worth showing.
    void render(crispy::point pos,
                int columnWidth,
                vtbackend::RGBColor color,
                unicode::Bidi_Direction direction = unicode::Bidi_Direction::Left_To_Right,
                bool mixedDirection = false);

    void inspect(std::ostream& output) const override;

  private:
    void initializeDirectMapping();
    using Renderable::createTileData;
    [[nodiscard]] TextureAtlas::TileCreateData createTileData(vtbackend::CursorShape shape,
                                                              int columnWidth,
                                                              atlas::TileLocation tileLocation);

    /// Builds the small square that turns a Bar cursor into a direction hint.
    [[nodiscard]] TextureAtlas::TileCreateData createDirectionHintTileData(atlas::TileLocation tileLocation);

    DirectMapping _directMapping {};
    vtbackend::CursorShape _shape;
};

} // namespace vtrasterizer
