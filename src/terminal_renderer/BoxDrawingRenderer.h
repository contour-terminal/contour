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
#include <terminal_renderer/Atlas.h>
#include <terminal_renderer/GridMetrics.h>
#include <terminal_renderer/RenderTarget.h>

#include <crispy/point.h>

#include <array>

namespace terminal::renderer {

// TODO: I think I should cincerely rename this class to
// something more suitable. it's not about box-drawing alone anymore,
// but about manually rendering anything that needs to properly fit
// into the grid cell.
// - box drawing symbols
// - symbols for legacy computing
// - mathematical symbols

/// Takes care of rendering the text cursor.
class BoxDrawingRenderer : public Renderable {
  public:
    explicit BoxDrawingRenderer(GridMetrics const& _gridMetrics):
        gridMetrics_{_gridMetrics},
        textureAtlas_{}
    {}

    void setRenderTarget(RenderTarget& _renderTarget) override;
    void clearCache() override;

    bool renderable(char32_t codepoint) const noexcept;

    /// Renders boxdrawing character.
    ///
    /// @param _char the boxdrawing character's codepoint.
    bool render(LineOffset _line, ColumnOffset _column, char32_t codepoint, RGBColor _color);

  private:
    using TextureAtlas = atlas::MetadataTextureAtlas<char32_t, int>;
    using DataRef = TextureAtlas::DataRef;

    std::optional<DataRef> getDataRef(char32_t codepoint);
    std::optional<atlas::Buffer> buildBoxElements(char32_t codepoint, ImageSize _size, int _lineThickness);
    std::optional<atlas::Buffer> buildElements(char32_t codepoint);

    // private fields
    //
    GridMetrics const& gridMetrics_;
    std::unique_ptr<TextureAtlas> textureAtlas_;
};

} // namespace terminal::view
