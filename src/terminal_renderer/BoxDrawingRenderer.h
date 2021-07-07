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

/// Takes care of rendering the text cursor.
class BoxDrawingRenderer : public Renderable {
  public:
    explicit BoxDrawingRenderer(GridMetrics const& _gridMetrics):
        gridMetrics_{_gridMetrics},
        textureAtlas_{}
    {}

    void setRenderTarget(RenderTarget& _renderTarget) override;
    void clearCache() override;

    /// Renders boxdrawing character.
    ///
    /// @param _char the boxdrawing character's codepoint modulo 0x2500 (a value between 0x00 and 0x7F).
    bool render(LinePosition _line, ColumnPosition _column, uint8_t _id, RGBColor _color);

  private:
    using TextureAtlas = atlas::MetadataTextureAtlas<uint8_t, int>;
    using DataRef = TextureAtlas::DataRef;

    std::optional<DataRef> getDataRef(uint8_t _id);
    std::optional<atlas::Buffer> build(uint8_t _id, ImageSize _size, int _lineThickness);

    // private fields
    //
    GridMetrics const& gridMetrics_;
    std::unique_ptr<TextureAtlas> textureAtlas_;
};

} // namespace terminal::view
