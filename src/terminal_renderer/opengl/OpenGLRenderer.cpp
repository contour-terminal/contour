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
#include <terminal_renderer/opengl/OpenGLRenderer.h>
#include <terminal_renderer/opengl/ShaderConfig.h>
#include <terminal_renderer/Atlas.h>

#include <crispy/algorithm.h>

#include <algorithm>

using std::min;

namespace terminal::renderer::opengl {

namespace // {{{ helper
{
    constexpr int glFormatZ(atlas::Format _f)
    {
        switch (_f)
        {
            case atlas::Format::Red:
                return GL_R8;
            case atlas::Format::RGB:
                return GL_RGB8;
            case atlas::Format::RGBA:
                return GL_RGBA8;
        }
        return GL_R8; // just in case
    }

    int glFormat(atlas::Format _format)
    {
        switch (_format)
        {
            case atlas::Format::RGBA:
                return GL_RGBA;
            case atlas::Format::RGB:
                return GL_RGB;
            case atlas::Format::Red:
                return GL_RED;
        }
        return GL_RED;
    }

    QMatrix4x4 ortho(float left, float right, float bottom, float top)
    {
        constexpr float nearPlane = -1.0f;
        constexpr float farPlane = 1.0f;

        QMatrix4x4 mat;
        mat.ortho(left, right, bottom, top, nearPlane, farPlane);
        return mat;
    }
} // }}}

constexpr unsigned MaxInstanceCount = 1;
constexpr unsigned MaxMonochromeTextureSize = 1024;
constexpr unsigned MaxColorTextureSize = 2048;

struct OpenGLRenderer::TextureScheduler : public atlas::CommandListener
{
    using CreateAtlas = atlas::CreateAtlas;
    using UploadTexture= atlas::UploadTexture;
    using RenderTexture = atlas::RenderTexture;
    using DestroyAtlas = atlas::DestroyAtlas;

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
            x,     y + s, z,  rx,     ry + h, i, u,  cr, cg, cb, ca, // left top
            x,     y,     z,  rx,     ry,     i, u,  cr, cg, cb, ca, // left bottom
            x + r, y,     z,  rx + w, ry,     i, u,  cr, cg, cb, ca, // right bottom

            // second triangle
            x,     y + s, z,  rx,     ry + h, i, u,  cr, cg, cb, ca, // left top
            x + r, y,     z,  rx + w, ry,     i, u,  cr, cg, cb, ca, // right bottom
            x + r, y + s, z,  rx + w, ry + h, i, u,  cr, cg, cb, ca, // right top
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
                               int _width,
                               int _height,
                               int _leftMargin,
                               int _bottomMargin) :
    projectionMatrix_{ },
    leftMargin_{ _leftMargin },
    bottomMargin_{ _bottomMargin },
    textShader_{ createShader(_textShaderConfig) },
    textProjectionLocation_{ textShader_->uniformLocation("vs_projection") },
    // texture
    textureScheduler_{std::make_unique<TextureScheduler>()},
    monochromeAtlasAllocator_{
        0,
        MaxInstanceCount,
        maxTextureSize() / maxTextureDepth(),
        min(MaxMonochromeTextureSize, maxTextureSize()),
        min(MaxMonochromeTextureSize, maxTextureSize()),
        atlas::Format::Red,
        *textureScheduler_,
        "monochromeAtlas"
    },
    coloredAtlasAllocator_{
        1,
        MaxInstanceCount,
        maxTextureSize() / maxTextureDepth(),
        min(MaxColorTextureSize, maxTextureSize()),
        min(MaxColorTextureSize, maxTextureSize()),
        atlas::Format::RGBA,
        *textureScheduler_,
        "colorAtlas"
    },
    lcdAtlasAllocator_{
        2,
        MaxInstanceCount,
        maxTextureSize() / maxTextureDepth(),
        min(MaxColorTextureSize, maxTextureSize()),
        min(MaxColorTextureSize, maxTextureSize()),
        atlas::Format::RGB,
        *textureScheduler_,
        "lcdAtlas"
    },
    // rect
    rectShader_{ createShader(_rectShaderConfig) },
    rectProjectionLocation_{ rectShader_->uniformLocation("u_projection") }
{
    initialize();

    setRenderSize(_width, _height);

    assert(textProjectionLocation_ != -1);

    glEnable(GL_BLEND);
    glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE);
    //glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ZERO, GL_ONE);
    // //glBlendFunc(GL_SRC1_COLOR, GL_ONE_MINUS_SRC1_COLOR);

    textShader_->bind();
    textShader_->setUniformValue("fs_monochromeTextures", monochromeAtlasAllocator_.instanceBaseId());
    textShader_->setUniformValue("fs_colorTextures", coloredAtlasAllocator_.instanceBaseId());
    textShader_->setUniformValue("fs_lcdTexture", lcdAtlasAllocator_.instanceBaseId());
    textShader_->setUniformValue("pixel_x", 1.0f / float(lcdAtlasAllocator_.width()));
    textShader_->release();

    initializeRectRendering();
    initializeTextureRendering();
}

void OpenGLRenderer::setRenderSize(int _width, int _height)
{
    projectionMatrix_ = ortho(
        0.0f, float(_width),      // left, right
        0.0f, float(_height)      // bottom, top
    );
}

void OpenGLRenderer::setMargin(int _left, int _bottom) noexcept
{
    leftMargin_ = _left;
    bottomMargin_ = _bottom;
}

atlas::TextureAtlasAllocator& OpenGLRenderer::monochromeAtlasAllocator() noexcept
{
    return monochromeAtlasAllocator_;
}

atlas::TextureAtlasAllocator& OpenGLRenderer::coloredAtlasAllocator() noexcept
{
    return coloredAtlasAllocator_;
}

atlas::TextureAtlasAllocator& OpenGLRenderer::lcdAtlasAllocator() noexcept
{
    return lcdAtlasAllocator_;
}

atlas::CommandListener& OpenGLRenderer::textureScheduler()
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

void OpenGLRenderer::createAtlas(atlas::CreateAtlas const& _param)
{
    GLuint textureId{};
    glGenTextures(1, &textureId);
    bindTexture2DArray(textureId);

    glTexStorage3D(GL_TEXTURE_2D_ARRAY, 1, glFormatZ(_param.format), _param.width, _param.height, _param.depth);

    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MAG_FILTER, GL_NEAREST); // NEAREST, because LINEAR yields borders at the edges
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_WRAP_R, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

#if 1
    // pre-initialize texture for better debugging (qrenderdoc)
    auto constexpr target = GL_TEXTURE_2D_ARRAY;
    auto constexpr levelOfDetail = 0;
    auto constexpr depth = 1;
    auto constexpr type = GL_UNSIGNED_BYTE;
    auto constexpr x0 = 0;
    auto constexpr y0 = 0;
    auto constexpr z0 = 0;

    std::vector<uint8_t> stub;
    stub.resize(_param.width * _param.height * atlas::element_count(_param.format));
    auto t = stub.begin();
    switch (_param.format)
    {
        case atlas::Format::Red:
            for (auto i = 0u; i < _param.width * _param.height; ++i)
                *t++ = 0x40;
            break;
        case atlas::Format::RGB:
            for (auto i = 0u; i < _param.width * _param.height; ++i)
            {
                *t++ = 0x00;
                *t++ = 0x00;
                *t++ = 0x80;
            }
            break;
        case atlas::Format::RGBA:
            for (auto i = 0u; i < _param.width * _param.height; ++i)
            {
                *t++ = 0x00;
                *t++ = 0x00;
                *t++ = 0x80;
                *t++ = 0x00;
            }
            break;
    }
    assert(t == stub.end());

    glTexSubImage3D(target, levelOfDetail, x0, y0, z0, _param.width, _param.height, depth,
                    glFormat(_param.format), type, stub.data());
#endif

    auto const key = AtlasKey{_param.atlasName, _param.atlas};
    atlasMap_[key] = textureId;
}

void OpenGLRenderer::uploadTexture(atlas::UploadTexture const& _param)
{
    auto const& texture = _param.texture.get();
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
        case atlas::Format::RGB:
        case atlas::Format::Red:
            glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
            break;
        case atlas::Format::RGBA:
            glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
            break;
    }

    glTexSubImage3D(target, levelOfDetail, x0, y0, z0, texture.width, texture.height, depth,
                    glFormat(_param.format), type, _param.data.data());
}

void OpenGLRenderer::renderTexture(atlas::RenderTexture const& _param)
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

void OpenGLRenderer::destroyAtlas(atlas::DestroyAtlas const& _param)
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

void OpenGLRenderer::renderRectangle(unsigned _x, unsigned _y, unsigned _width, unsigned _height,
                                     float _r, float _g, float _b, float _a)
{
    GLfloat const x = _x;
    GLfloat const y = _y;
    GLfloat const z = 0.0f;
    GLfloat const r = _width;
    GLfloat const s = _height;
    GLfloat const cr = _r;
    GLfloat const cg = _g;
    GLfloat const cb = _b;
    GLfloat const ca = _a;

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
    //FIXME
    //glEnable(GL_BLEND);
    //glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE);
    //glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ZERO, GL_ONE);
    //glBlendFunc(GL_SRC1_COLOR, GL_ONE_MINUS_SRC1_COLOR);

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
