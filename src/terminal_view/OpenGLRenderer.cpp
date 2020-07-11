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

#include <crispy/algorithm.h>

#include <algorithm>

using std::min;

namespace terminal::view {

constexpr unsigned MaxInstanceCount = 1;
constexpr unsigned MaxMonochromeTextureSize = 1024;
constexpr unsigned MaxColorTextureSize = 2048;

OpenGLRenderer::OpenGLRenderer(ShaderConfig const& _textShaderConfig,
                               ShaderConfig const& _rectShaderConfig,
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
    },
    rectShader_{ createShader(_rectShaderConfig) },
    rectProjectionLocation_{ rectShader_->uniformLocation("u_projection") }
{
    initialize();

    glEnable(GL_BLEND);
    glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE);
    //glBlendFunc(GL_SRC1_COLOR, GL_ONE_MINUS_SRC1_COLOR);

    textShader_->bind();
    textShader_->setUniformValue("fs_monochromeTextures", 0);
    textShader_->setUniformValue("fs_colorTextures", 1);
    textShader_->release();

    // setup filled-rectangle rendering
    //
    glGenVertexArrays(1, &rectVAO_);
    glBindVertexArray(rectVAO_);

    glGenBuffers(1, &rectVBO_);
    glBindBuffer(GL_ARRAY_BUFFER, rectVBO_);
    glBufferData(GL_ARRAY_BUFFER, 0, nullptr, GL_STREAM_DRAW);

    auto constexpr BufferStride = 7 * sizeof(GLfloat);
    auto const VertexOffset = (void const*) (0 * sizeof(GLfloat));
    auto const ColorOffset = (void const*) (3 * sizeof(GLfloat));

    // 0 (vec3): vertex buffer
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, BufferStride, VertexOffset);
    glEnableVertexAttribArray(0);

    // 1 (vec4): color buffer
    glVertexAttribPointer(1, 4, GL_FLOAT, GL_FALSE, BufferStride, ColorOffset);
    glEnableVertexAttribArray(1);
}

OpenGLRenderer::~OpenGLRenderer()
{
    glDeleteVertexArrays(1, &rectVAO_);
    glDeleteBuffers(1, &rectVBO_);
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

void OpenGLRenderer::createAtlas(crispy::atlas::CreateAtlas const& _param)
{
    textureRenderer_.scheduler().createAtlas(_param);
}

void OpenGLRenderer::uploadTexture(crispy::atlas::UploadTexture const& _param)
{
    textureRenderer_.scheduler().uploadTexture(_param);
}

void OpenGLRenderer::renderTexture(crispy::atlas::RenderTexture const& _param)
{
    textureRenderer_.scheduler().renderTexture(_param);
}

void OpenGLRenderer::destroyAtlas(crispy::atlas::DestroyAtlas const& _param)
{
    textureRenderer_.scheduler().destroyAtlas(_param);
}

void OpenGLRenderer::renderRectangle(unsigned _x, unsigned _y, unsigned _width, unsigned _height, QVector4D const& _color)
{
    GLfloat const x = _x;
    GLfloat const y = _y;
    GLfloat const z = 0.0f;
    GLfloat const r = _width;
    GLfloat const s = _height;
    GLfloat const cr = _color[0];
    GLfloat const cg = _color[1];
    GLfloat const cb = _color[2];
    GLfloat const ca = _color[3];

    GLfloat const vertices[6 * 7] = {
        // first triangle
        x,     y + s, z, cr, cg, cb, ca,
        x,     y,     z, cr, cg, cb, ca,
        x + r, y,     z, cr, cg, cb, ca,

        // second triangle
        x,     y + s, z, cr, cg, cb, ca,
        x + r, y,     z, cr, cg, cb, ca,
        x + r, y + s, z, cr, cg, cb, ca
    };

    crispy::copy(vertices, back_inserter(rectBuffer_));
}

void OpenGLRenderer::execute()
{
    // render filled rects
    //
    if (!rectBuffer_.empty())
    {
        rectShader_->bind();
        rectShader_->setUniformValue(rectProjectionLocation_, projectionMatrix_);

        glBindVertexArray(rectVAO_);
        glBindBuffer(GL_ARRAY_BUFFER, rectVBO_);
        glBufferData(GL_ARRAY_BUFFER, rectBuffer_.size() * sizeof(GLfloat), rectBuffer_.data(), GL_STREAM_DRAW);

        glDrawArrays(GL_TRIANGLES, 0, rectBuffer_.size() / 7);

        rectShader_->release();
        glBindVertexArray(0);
        rectBuffer_.clear();
    }

    // render textures
    //
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

} // end namespace
