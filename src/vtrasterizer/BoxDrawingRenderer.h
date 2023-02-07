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

#include <vtrasterizer/GridMetrics.h>
#include <vtrasterizer/RenderTarget.h>
#include <vtrasterizer/TextureAtlas.h>

#include <crispy/point.h>

#include <array>

namespace terminal::rasterizer
{

// TODO: I think I should cincerely rename this class to
// something more suitable. it's not about box-drawing alone anymore,
// but about manually rendering anything that needs to properly fit
// into the grid cell.
// - box drawing symbols
// - symbols for legacy computing
// - mathematical symbols

/// Takes care of rendering the text cursor.
class BoxDrawingRenderer: public Renderable
{
  public:
    explicit BoxDrawingRenderer(GridMetrics const& gridMetrics): Renderable { gridMetrics } {}

    void setRenderTarget(RenderTarget& renderTarget, DirectMappingAllocator& directMappingAllocator) override;
    void clearCache() override;

    [[nodiscard]] static bool renderable(char32_t codepoint) noexcept;

    /// Renders boxdrawing character.
    ///
    /// @param codepoint     the boxdrawing character's codepoint.
    [[nodiscard]] bool render(LineOffset line, ColumnOffset column, char32_t codepoint, RGBColor color);

    void inspect(std::ostream& output) const override;

  private:
    AtlasTileAttributes const* getOrCreateCachedTileAttributes(char32_t codepoint);

    using Renderable::createTileData;
    [[nodiscard]] std::optional<TextureAtlas::TileCreateData> createTileData(
        char32_t codepoint, atlas::TileLocation tileLocation);

    [[nodiscard]] static std::optional<atlas::Buffer> buildBoxElements(char32_t codepoint,
                                                                       ImageSize size,
                                                                       int lineThickness);
    [[nodiscard]] std::optional<atlas::Buffer> buildElements(char32_t codepoint);
};

} // namespace terminal::rasterizer
