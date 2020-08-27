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
#include <terminal/Size.h>

#include <QtGui/QMatrix4x4>
#include <QtGui/QOpenGLExtraFunctions>
#include <QtGui/QOpenGLShaderProgram>

#include <memory>

namespace terminal::view {

struct ShaderConfig;

class OpenGLRenderer :
    public crispy::atlas::CommandListener,
    public QOpenGLExtraFunctions
{
  public:
    OpenGLRenderer(ShaderConfig const& _textShaderConfig,
                   ShaderConfig const& _rectShaderConfig,
                   QMatrix4x4 const& _projectionMatrix,
                   int _leftMargin,
                   int _bottomMargin,
                   Size const& _cellSize);

    ~OpenGLRenderer();

    void clearCache();

    constexpr void setMargin(int _left, int _bottom) noexcept { leftMargin_ = _left; bottomMargin_ = _bottom; }
    constexpr void setCellSize(Size const& _cellSize) noexcept { cellSize_ = _cellSize; }
    constexpr void setProjection(QMatrix4x4 const& _projectionMatrix) noexcept { projectionMatrix_ = _projectionMatrix; }

    void renderRectangle(unsigned _x, unsigned _y, unsigned _width, unsigned _height, QVector4D const& _color);
    void createAtlas(crispy::atlas::CreateAtlas const& _param) override;
    void uploadTexture(crispy::atlas::UploadTexture const& _param) override;
    void renderTexture(crispy::atlas::RenderTexture const& _param) override;
    void destroyAtlas(crispy::atlas::DestroyAtlas const& _param) override;

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
    Size cellSize_;

    std::unique_ptr<QOpenGLShaderProgram> textShader_;
    int textProjectionLocation_;
    int marginLocation_;
    int cellSizeLocation_;

    crispy::atlas::Renderer textureRenderer_;
    crispy::atlas::TextureAtlasAllocator monochromeAtlasAllocator_;
    crispy::atlas::TextureAtlasAllocator coloredAtlasAllocator_;

    // filled rectangles
    //
    std::vector<GLfloat> rectBuffer_;
    std::unique_ptr<QOpenGLShaderProgram> rectShader_;
    GLint rectProjectionLocation_;
    GLuint rectVAO_;
    GLuint rectVBO_;
};

} // end namespace
