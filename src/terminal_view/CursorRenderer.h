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

#include <terminal_view/GridMetrics.h>

#include <crispy/Atlas.h>

#include <QtCore/QPoint>
#include <QtGui/QVector4D>

#include <array>

namespace terminal::view {

/// Takes care of rendering the text cursor.
class CursorRenderer {
  public:
    CursorRenderer(crispy::atlas::CommandListener& _commandListener,
                   crispy::atlas::TextureAtlasAllocator& _monochromeTextureAtlas,
                   GridMetrics const& _gridMetrics,
                   CursorShape _shape,
                   QVector4D const& _color);

    CursorShape shape() const noexcept { return shape_; }
    void setShape(CursorShape _shape);
    void setColor(QVector4D const& _color);

    void render(QPoint _pos, int _columnWidth);
    void clearCache();

  private:
    using TextureAtlas = crispy::atlas::MetadataTextureAtlas<CursorShape, int>;
    using DataRef = TextureAtlas::DataRef;

    void rebuild();
    std::optional<DataRef> getDataRef(CursorShape _shape);

  private:
    crispy::atlas::CommandListener& commandListener_;
    TextureAtlas textureAtlas_;
    GridMetrics const& gridMetrics_;

    CursorShape shape_;
    std::array<float, 4> color_;
    int columnWidth_;
};

} // namespace terminal::view
