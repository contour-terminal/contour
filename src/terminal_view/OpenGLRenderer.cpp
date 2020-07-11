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
#include <terminal_view/OpenGLRenderer.h>

#include <algorithm>

using std::min;

namespace terminal::view {

constexpr unsigned MaxInstanceCount = 1;
constexpr unsigned MaxMonochromeTextureSize = 1024;
constexpr unsigned MaxColorTextureSize = 2048;

OpenGLRenderer::OpenGLRenderer(ShaderConfig const& _textShaderConfig,
                               QMatrix4x4 const& _projectionMatrix,
                               int _leftMargin,
                               int _bottomMargin,
                               CellSize const& _cellSize) :
    projectionMatrix_{ _projectionMatrix },
    leftMargin_{ _leftMargin },
    bottomMargin_{ _bottomMargin },
    cellSize_{ _cellSize },
    textShader_{ createShader(_textShaderConfig) },
    textProjectionLocation_{ textShader_->uniformLocation("vs_projection") },
    marginLocation_{ textShader_->uniformLocation("vs_margin") },
    cellSizeLocation_{ textShader_->uniformLocation("vs_cellSize") },
    monochromeAtlasAllocator_{
        0,
        MaxInstanceCount,
        maxTextureSize() / maxTextureDepth(),
        min(MaxMonochromeTextureSize, maxTextureSize()),
        min(MaxMonochromeTextureSize, maxTextureSize()),
        GL_R8,
        textureRenderer_.scheduler(),
        "monochromeAtlas"
    },
    coloredAtlasAllocator_{
        1,
        MaxInstanceCount,
        maxTextureSize() / maxTextureDepth(),
        min(MaxColorTextureSize, maxTextureSize()),
        min(MaxColorTextureSize, maxTextureSize()),
        GL_RGBA8,
        textureRenderer_.scheduler(),
        "colorAtlas"
    }
{
    glEnable(GL_BLEND);
    glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE);
    //glBlendFunc(GL_SRC1_COLOR, GL_ONE_MINUS_SRC1_COLOR);

    textShader_->bind();
    textShader_->setUniformValue("fs_monochromeTextures", 0);
    textShader_->setUniformValue("fs_colorTextures", 1);
    textShader_->release();
}

void OpenGLRenderer::initialize()
{
    if (!initialized_)
    {
        initialized_ = true;
        initializeOpenGLFunctions();
    }
}

void OpenGLRenderer::clearCache()
{
    monochromeAtlasAllocator_.clear();
    coloredAtlasAllocator_.clear();
}

void OpenGLRenderer::execute()
{
    textShader_->bind();

    // TODO: only upload when it actually DOES change
    textShader_->setUniformValue(textProjectionLocation_, projectionMatrix_);
    textShader_->setUniformValue(marginLocation_, QVector2D(
        static_cast<float>(leftMargin_),
        static_cast<float>(bottomMargin_)
    ));
    textShader_->setUniformValue(cellSizeLocation_, QVector2D(
        static_cast<float>(cellSize_.width),
        static_cast<float>(cellSize_.height)
    ));

    textureRenderer_.execute();

    textShader_->release();
}

unsigned OpenGLRenderer::maxTextureDepth()
{
    initialize();

    GLint value;
    glGetIntegerv(GL_MAX_3D_TEXTURE_SIZE, &value);
    return static_cast<unsigned>(value);
}

unsigned OpenGLRenderer::maxTextureSize()
{
    initialize();

    GLint value = {};
    glGetIntegerv(GL_MAX_TEXTURE_SIZE, &value);
    return static_cast<unsigned>(value);
}

} // end namespace
