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

#include <crispy/Atlas.h>
#include <crispy/AtlasRenderer.h>
#include <terminal_view/TextRenderer.h> // FIXME CellSize lol

#include <QtGui/QMatrix4x4>
#include <QtGui/QOpenGLFunctions>
#include <QtGui/QOpenGLShaderProgram>

#include <memory>

namespace terminal::view {

struct ShaderConfig;

class OpenGLRenderer : public QOpenGLFunctions {
  public:
    OpenGLRenderer(ShaderConfig const& _textShaderConfig,
                   QMatrix4x4 const& _projectionMatrix,
                   int _leftMargin,
                   int _bottomMargin,
                   CellSize const& _cellSize);

    void clearCache();

    constexpr void setMargin(int _left, int _bottom) noexcept { leftMargin_ = _left; bottomMargin_ = _bottom; }
    constexpr void setCellSize(CellSize const& _cellSize) noexcept { cellSize_ = _cellSize; }
    constexpr void setProjection(QMatrix4x4 const& _projectionMatrix) noexcept { projectionMatrix_ = _projectionMatrix; }

    crispy::atlas::CommandListener& scheduler() noexcept { return textureRenderer_.scheduler(); }
    crispy::atlas::TextureAtlasAllocator& monochromeAtlasAllocator() noexcept { return monochromeAtlasAllocator_; }
    crispy::atlas::TextureAtlasAllocator& coloredAtlasAllocator() noexcept { return coloredAtlasAllocator_; }

    void execute();

  private:
    void initialize();
    unsigned maxTextureDepth();
    unsigned maxTextureSize();

  private:
    bool initialized_ = false;
    QMatrix4x4 projectionMatrix_;

    int leftMargin_ = 0;
    int bottomMargin_ = 0;
    CellSize cellSize_;

    std::unique_ptr<QOpenGLShaderProgram> textShader_;
    int textProjectionLocation_;
    int marginLocation_;
    int cellSizeLocation_;

    crispy::atlas::Renderer textureRenderer_;
    crispy::atlas::TextureAtlasAllocator monochromeAtlasAllocator_;
    crispy::atlas::TextureAtlasAllocator coloredAtlasAllocator_;
};

} // end namespace
