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
#include <terminal_view/TextRenderer.h>

#include <crispy/Atlas.h>
#include <crispy/algorithm.h>

#include <algorithm>

using std::min;

namespace terminal::view {

constexpr unsigned MaxInstanceCount = 1;
constexpr unsigned MaxMonochromeTextureSize = 1024;
constexpr unsigned MaxColorTextureSize = 2048;

struct OpenGLRenderer::TextureScheduler : public crispy::atlas::CommandListener
{
    using CreateAtlas = crispy::atlas::CreateAtlas;
    using UploadTexture= crispy::atlas::UploadTexture;
    using RenderTexture = crispy::atlas::RenderTexture;
    using DestroyAtlas = crispy::atlas::DestroyAtlas;

    std::vector<CreateAtlas> createAtlases;
    std::vector<UploadTexture> uploadTextures;
    std::vector<RenderTexture> renderTextures;
    std::vector<GLfloat> buffer;
    GLsizei vertexCount = 0;
    std::vector<DestroyAtlas> destroyAtlases;

    void createAtlas(CreateAtlas const& _atlas) override
    {
        createAtlases.emplace_back(_atlas);
    }

    void uploadTexture(UploadTexture const& _texture) override
    {
        uploadTextures.emplace_back(_texture);
    }

    void renderTexture(RenderTexture const& _render) override
    {
        renderTextures.emplace_back(_render);

        // Vertices
        GLfloat const x = _render.x;
        GLfloat const y = _render.y;
        GLfloat const z = _render.z;
      //GLfloat const w = _render.w;
        GLfloat const r = _render.texture.get().targetWidth;
        GLfloat const s = _render.texture.get().targetHeight;

        // TexCoords
        GLfloat const rx = _render.texture.get().relativeX;
        GLfloat const ry = _render.texture.get().relativeY;
        GLfloat const w = _render.texture.get().relativeWidth;
        GLfloat const h = _render.texture.get().relativeHeight;
        GLfloat const i = _render.texture.get().z;
        GLfloat const u = _render.texture.get().user;

        // color
        GLfloat const cr = _render.color[0];
        GLfloat const cg = _render.color[1];
        GLfloat const cb = _render.color[2];
        GLfloat const ca = _render.color[3];

        GLfloat const vertices[6 * 11] = {
            // first triangle
        // <X      Y      Z> <X       Y       I  U>  <R   G   B   A>
            x,     y + s, z,  rx,     ry,     i, u,  cr, cg, cb, ca,
            x,     y,     z,  rx,     ry + h, i, u,  cr, cg, cb, ca,
            x + r, y,     z,  rx + w, ry + h, i, u,  cr, cg, cb, ca,

            // second triangle
            x,     y + s, z,  rx,     ry,     i, u,  cr, cg, cb, ca,
            x + r, y,     z,  rx + w, ry + h, i, u,  cr, cg, cb, ca,
            x + r, y + s, z,  rx + w, ry,     i, u,  cr, cg, cb, ca,
        };

        crispy::copy(vertices, back_inserter(buffer));
        vertexCount += 6;
    }

    void destroyAtlas(DestroyAtlas const& _atlas) override
    {
        destroyAtlases.push_back(_atlas);
    }

    size_t size() const noexcept
    {
        return createAtlases.size()
             + uploadTextures.size()
             + renderTextures.size()
             + destroyAtlases.size();
    }

    void reset()
    {
        createAtlases.clear();
        uploadTextures.clear();
        renderTextures.clear();
        destroyAtlases.clear();
        buffer.clear();
        vertexCount = 0;
    }
};

OpenGLRenderer::OpenGLRenderer(ShaderConfig const& _textShaderConfig,
                               ShaderConfig const& _rectShaderConfig,
                               QMatrix4x4 const& _projectionMatrix,
                               int _leftMargin,
                               int _bottomMargin,
                               Size const& _cellSize) :
    projectionMatrix_{ _projectionMatrix },
    leftMargin_{ _leftMargin },
    bottomMargin_{ _bottomMargin },
    cellSize_{ _cellSize },
    textShader_{ createShader(_textShaderConfig) },
    textProjectionLocation_{ textShader_->uniformLocation("vs_projection") },
    marginLocation_{ textShader_->uniformLocation("vs_margin") },
    cellSizeLocation_{ textShader_->uniformLocation("vs_cellSize") },
    // texture
    textureScheduler_{std::make_unique<TextureScheduler>()},
    monochromeAtlasAllocator_{
        0,
        MaxInstanceCount,
        maxTextureSize() / maxTextureDepth(),
        min(MaxMonochromeTextureSize, maxTextureSize()),
        min(MaxMonochromeTextureSize, maxTextureSize()),
        crispy::atlas::Format::Red,
        *textureScheduler_,
        "monochromeAtlas"
    },
    coloredAtlasAllocator_{
        1,
        MaxInstanceCount,
        maxTextureSize() / maxTextureDepth(),
        min(MaxColorTextureSize, maxTextureSize()),
        min(MaxColorTextureSize, maxTextureSize()),
        crispy::atlas::Format::RGBA,
        *textureScheduler_,
        "colorAtlas"
    },
    lcdAtlasAllocator_{
        2,
        MaxInstanceCount,
        maxTextureSize() / maxTextureDepth(),
        min(MaxColorTextureSize, maxTextureSize()),
        min(MaxColorTextureSize, maxTextureSize()),
        crispy::atlas::Format::RGB,
        *textureScheduler_,
        "lcdAtlas"
    },
    // rect
    rectShader_{ createShader(_rectShaderConfig) },
    rectProjectionLocation_{ rectShader_->uniformLocation("u_projection") }
{
    initialize();

    glEnable(GL_BLEND);
    glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE);
    //glBlendFunc(GL_SRC1_COLOR, GL_ONE_MINUS_SRC1_COLOR);

    textShader_->bind();
    textShader_->setUniformValue("fs_monochromeTextures", monochromeAtlasAllocator_.instanceBaseId());
    textShader_->setUniformValue("fs_colorTextures", coloredAtlasAllocator_.instanceBaseId());
    textShader_->setUniformValue("fs_lcdTexture", lcdAtlasAllocator_.instanceBaseId());
    textShader_->release();

    initializeRectRendering();
    initializeTextureRendering();
}

crispy::atlas::CommandListener& OpenGLRenderer::textureScheduler()
{
    return *textureScheduler_;
}

void OpenGLRenderer::initializeRectRendering()
{
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

void OpenGLRenderer::initializeTextureRendering()
{
    glGenVertexArrays(1, &vao_);
    glBindVertexArray(vao_);

    auto constexpr BufferStride = (3 + 4 + 4) * sizeof(GLfloat);
    auto constexpr VertexOffset = (void const*) 0;
    auto const TexCoordOffset = (void const*) (3 * sizeof(GLfloat));
    auto const ColorOffset = (void const*) (7 * sizeof(GLfloat));

    glGenBuffers(1, &vbo_);
    glBindBuffer(GL_ARRAY_BUFFER, vbo_);
    glBufferData(GL_ARRAY_BUFFER, 0/* sizeof(GLfloat) * 6 * 11 * 200 * 100*/, nullptr, GL_STREAM_DRAW);

    // 0 (vec3): vertex buffer
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, BufferStride, VertexOffset);
    glEnableVertexAttribArray(0);

    // 1 (vec3): texture coordinates buffer
    glVertexAttribPointer(1, 4, GL_FLOAT, GL_FALSE, BufferStride, TexCoordOffset);
    glEnableVertexAttribArray(1);

    // 2 (vec4): color buffer
    glVertexAttribPointer(2, 4, GL_FLOAT, GL_FALSE, BufferStride, ColorOffset);
    glEnableVertexAttribArray(2);

    // setup EBO
    // glGenBuffers(1, &ebo_);
    // glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ebo_);
    // static const GLuint indices[6] = { 0, 1, 3, 1, 2, 3 };
    // glBufferData(GL_ELEMENT_ARRAY_BUFFER, sizeof(indices), indices, GL_STATIC_DRAW);

    //glVertexAttribDivisor(0, 1); // TODO: later for instanced rendering
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
    lcdAtlasAllocator_.clear();
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

constexpr int glFormat(crispy::atlas::Format _f)
{
    switch (_f)
    {
        case crispy::atlas::Format::Red:
            return GL_R8;
        case crispy::atlas::Format::RGB:
            return GL_RGB8;
        case crispy::atlas::Format::RGBA:
            return GL_RGBA8;
    }
    return GL_R8; // just in case
}

void OpenGLRenderer::createAtlas(crispy::atlas::CreateAtlas const& _param)
{
    GLuint textureId{};
    glGenTextures(1, &textureId);
    bindTexture2DArray(textureId);

    glTexStorage3D(GL_TEXTURE_2D_ARRAY, 1, glFormat(_param.format), _param.width, _param.height, _param.depth);

    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_WRAP_R, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    auto const key = AtlasKey{_param.atlasName, _param.atlas};
    atlasMap_[key] = textureId;
}

void OpenGLRenderer::uploadTexture(crispy::atlas::UploadTexture const& _param)
{
    auto const& texture = _param.texture.get();
    auto const glFormat = [&]() {
        switch (_param.format)
        {
            case crispy::atlas::Format::RGBA:
                return GL_RGBA;
            case crispy::atlas::Format::RGB:
                return GL_RGB;
            case crispy::atlas::Format::Red:
                return GL_RED;
        }
        return GL_RED;
    }();
    auto const key = AtlasKey{texture.atlasName, texture.atlas};
    [[maybe_unused]] auto const textureIdIter = atlasMap_.find(key);
    assert(textureIdIter != atlasMap_.end() && "Texture ID not found in atlas map!");
    auto const textureId = atlasMap_[key];
    auto const x0 = texture.x;
    auto const y0 = texture.y;
    auto const z0 = texture.z;

    // cout << fmt::format("atlas::Renderer.uploadTexture({}): {}\n", textureId, _param);

    auto constexpr target = GL_TEXTURE_2D_ARRAY;
    auto constexpr levelOfDetail = 0;
    auto constexpr depth = 1;
    auto constexpr type = GL_UNSIGNED_BYTE;

    bindTexture2DArray(textureId);

    switch (_param.format)
    {
        case crispy::atlas::Format::RGB:
        case crispy::atlas::Format::Red:
            glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
            break;
        case crispy::atlas::Format::RGBA:
            glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
            break;
    }

    glTexSubImage3D(target, levelOfDetail, x0, y0, z0, texture.width, texture.height, depth,
                    glFormat, type, _param.data.data());
}

void OpenGLRenderer::renderTexture(crispy::atlas::RenderTexture const& _param)
{
    auto const key = AtlasKey{_param.texture.get().atlasName, _param.texture.get().atlas};
    if (auto const it = atlasMap_.find(key); it != atlasMap_.end())
    {
        GLuint const textureUnit = _param.texture.get().atlas;
        GLuint const textureId = it->second;

        // cout << fmt::format("atlas::Renderer.renderTexture({}/{}): {}\n", textureUnit, textureId, _param);

        selectTextureUnit(textureUnit);
        bindTexture2DArray(textureId);
    }
}

void OpenGLRenderer::destroyAtlas(crispy::atlas::DestroyAtlas const& _param)
{
    auto const key = AtlasKey{_param.atlasName.get(), _param.atlas};
    if (auto const it = atlasMap_.find(key); it != atlasMap_.end())
    {
        GLuint const textureId = it->second;
        atlasMap_.erase(it);
        glDeleteTextures(1, &textureId);
    }
}

void OpenGLRenderer::bindTexture2DArray(GLuint _textureId)
{
    if (currentTextureId_ != _textureId)
    {
        glBindTexture(GL_TEXTURE_2D_ARRAY, _textureId);
        currentTextureId_ = _textureId;
    }
}

void OpenGLRenderer::selectTextureUnit(unsigned _id)
{
    if (currentActiveTexture_ != _id)
    {
        glActiveTexture(GL_TEXTURE0 + _id);
        currentActiveTexture_ = _id;
    }
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

        glDrawArrays(GL_TRIANGLES, 0, static_cast<GLsizei>(rectBuffer_.size() / 7));

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

    executeRenderTextures();

    textShader_->release();
}

void OpenGLRenderer::executeRenderTextures()
{
    // std::cout << fmt::format("OpenGLRenderer::executeRenderTextures() upload={} render={}\n",
    //     textureScheduler_->uploadTextures.size(),
    //     textureScheduler_->renderTextures.size()
    // );

    // potentially create new atlases
    for (auto const& params : textureScheduler_->createAtlases)
        createAtlas(params);

    // potentially upload any new textures
    for (auto const& params : textureScheduler_->uploadTextures)
        uploadTexture(params);

    // order and prepare texture geometry
    sort(textureScheduler_->renderTextures.begin(),
         textureScheduler_->renderTextures.end(),
         [](auto const& a, auto const& b) { return a.texture.get().atlas < b.texture.get().atlas; });

    for (auto const& params : textureScheduler_->renderTextures)
        renderTexture(params);

    // upload vertices and render (iff there is anything to render)
    if (!textureScheduler_->renderTextures.empty())
    {
        glBindVertexArray(vao_);

        // upload buffer
        glBindBuffer(GL_ARRAY_BUFFER, vbo_);
        glBufferData(GL_ARRAY_BUFFER,
                     textureScheduler_->buffer.size() * sizeof(GLfloat),
                     textureScheduler_->buffer.data(),
                     GL_STREAM_DRAW);

        glDrawArrays(GL_TRIANGLES, 0, textureScheduler_->vertexCount);

        // TODO: Instead of on glDrawArrays (and many if's in the shader for each GL_TEXTUREi),
        //       make a loop over each GL_TEXTUREi and draw a sub range of the vertices and a
        //       fixed GL_TEXTURE0. - will this be noticable faster?
    }

    // destroy any pending atlases that were meant to be destroyed
    for (auto const& params : textureScheduler_->destroyAtlases)
        destroyAtlas(params);

    // reset execution state
    textureScheduler_->reset();
    currentActiveTexture_ = std::numeric_limits<GLuint>::max();
    currentTextureId_ = std::numeric_limits<GLuint>::max();
}

} // end namespace
