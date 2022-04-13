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

#include <terminal/Color.h>

#include <terminal_renderer/GridMetrics.h>
#include <terminal_renderer/RenderTarget.h>
#include <terminal_renderer/TextureAtlas.h>

#include <crispy/point.h>

#include <array>

namespace terminal::renderer
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

    [[nodiscard]] bool renderable(char32_t codepoint) const noexcept;

    /// Renders boxdrawing character.
    ///
    /// @param _char the boxdrawing character's codepoint.
    [[nodiscard]] bool render(LineOffset _line, ColumnOffset _column, char32_t codepoint, RGBColor _color);

    void inspect(std::ostream& output) const override;

  private:
    AtlasTileAttributes const* getOrCreateCachedTileAttributes(char32_t codepoint);

    using Renderable::createTileData;
    [[nodiscard]] std::optional<TextureAtlas::TileCreateData> createTileData(
        char32_t codepoint, atlas::TileLocation tileLocation);

    [[nodiscard]] std::optional<atlas::Buffer> buildBoxElements(char32_t codepoint,
                                                                ImageSize _size,
                                                                int _lineThickness);
    [[nodiscard]] std::optional<atlas::Buffer> buildElements(char32_t codepoint);
};

} // namespace terminal::renderer
