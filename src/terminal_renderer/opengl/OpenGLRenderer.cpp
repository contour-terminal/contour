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
#include <crispy/utils.h>

#include <range/v3/view.hpp>

#include <algorithm>
#include <vector>

using crispy::Size;
using std::min;
using std::optional;
using std::nullopt;
using std::string;
using std::vector;

namespace terminal::renderer::opengl {

#if !defined(NDEBUG)
#define CHECKED_GL(code) \
    do { \
        (code); \
        GLenum err{}; \
        while ((err = glGetError()) != GL_NO_ERROR) \
            debuglog(OpenGLRendererTag).write("OpenGL error {} for call: {}", err, #code); \
    } while (0)
#else
#define CHECKED_GL(code) do { (code); } while (0)
#endif

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

constexpr int MaxMonochromeTextureSize = 1024;
constexpr int MaxColorTextureSize = 2048;
constexpr int MaxAtlasDepth = 1;
constexpr int MaxInstanceCount = 24;

struct OpenGLRenderer::TextureScheduler : public atlas::AtlasBackend
{
    using CreateAtlas = atlas::CreateAtlas;
    using UploadTexture = atlas::UploadTexture;
    using RenderTexture = atlas::RenderTexture;
    using DestroyAtlas = atlas::DestroyAtlas;

    struct RenderBatch
    {
        std::vector<RenderTexture> renderTextures;
        std::vector<GLfloat> buffer;
    };

    std::vector<CreateAtlas> createAtlases;
    std::vector<UploadTexture> uploadTextures;
    std::unordered_map<int, RenderBatch> renderBatches;
    int renderTextureCount = 0;

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
        // This is factored out of renderTexture() to make sure it's not writing to anything else
        addRenderTexture(_render, renderBatches[_render.texture.get().atlas]);
        renderTextureCount++;
    }

    void addRenderTexture(RenderTexture const& _render, RenderBatch& _batch) const
    {
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

        _batch.renderTextures.emplace_back(_render);
        crispy::copy(vertices, back_inserter(_batch.buffer));
    }

    void destroyAtlas(DestroyAtlas const& _atlas) override
    {
        destroyAtlases.push_back(_atlas);
    }

    size_t size() const noexcept
    {
        return createAtlases.size()
             + uploadTextures.size()
             + renderTextureCount
             + destroyAtlases.size();
    }

    void reset()
    {
        createAtlases.clear();
        uploadTextures.clear();
        destroyAtlases.clear();

        // Don't simply do renderBatch.clear() to reuse already allocated memory in future render calls.
        for (auto& renderBatch: renderBatches)
        {
            renderBatch.second.renderTextures.clear();
            renderBatch.second.buffer.clear();
        }
        renderTextureCount = 0;
    }
};

template <typename T, typename Fn>
inline void bound(T& _bindable, Fn&& _callable)
{
    _bindable.bind();
    try {
        _callable();
    } catch (...) {
        _bindable.release();
        throw;
    }
    _bindable.release();
}

OpenGLRenderer::OpenGLRenderer(ShaderConfig const& _textShaderConfig,
                               ShaderConfig const& _rectShaderConfig,
                               Size _size,
                               int _leftMargin,
                               int _bottomMargin) :
    size_{ _size },
    projectionMatrix_{ortho(
        0.0f, float(_size.width),      // left, right
        0.0f, float(_size.height)      // bottom, top
    )},
    leftMargin_{ _leftMargin },
    bottomMargin_{ _bottomMargin },
    textShader_{ createShader(_textShaderConfig) },
    textProjectionLocation_{ textShader_->uniformLocation("vs_projection") },
    // texture
    textureScheduler_{std::make_unique<TextureScheduler>()},
    monochromeAtlasAllocator_{
        0,
        min(MaxMonochromeTextureSize, maxTextureSize()),
        min(MaxMonochromeTextureSize, maxTextureSize()),
        MaxAtlasDepth, // maxTextureSize() / maxTextureDepth(),
        MaxInstanceCount, // TODO: better runtime compute with max(x,y)
        atlas::Format::Red,
        *textureScheduler_,
        "monochromeAtlas"
    },
    coloredAtlasAllocator_{
        1,
        min(MaxColorTextureSize, maxTextureSize()),
        min(MaxColorTextureSize, maxTextureSize()),
        MaxAtlasDepth, // maxTextureSize() / maxTextureDepth(),
        MaxInstanceCount, // TODO: better runtime compute with max(x,y)
        atlas::Format::RGBA,
        *textureScheduler_,
        "colorAtlas"
    },
    lcdAtlasAllocator_{
        2,
        min(MaxColorTextureSize, maxTextureSize()),
        min(MaxColorTextureSize, maxTextureSize()),
        MaxAtlasDepth, // maxTextureSize() / maxTextureDepth(),
        MaxInstanceCount, // TODO: better runtime compute with max(x,y)
        atlas::Format::RGB,
        *textureScheduler_,
        "lcdAtlas"
    },
    // rect
    rectShader_{ createShader(_rectShaderConfig) },
    rectProjectionLocation_{ rectShader_->uniformLocation("u_projection") }
{
    initialize();

    setRenderSize(_size);

    assert(textProjectionLocation_ != -1);

    CHECKED_GL( glEnable(GL_BLEND) );
    CHECKED_GL( glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE) );
    //glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ZERO, GL_ONE);
    // //glBlendFunc(GL_SRC1_COLOR, GL_ONE_MINUS_SRC1_COLOR);

    bound(*textShader_, [&]() {
        CHECKED_GL( textShader_->setUniformValue("fs_monochromeTextures", monochromeAtlasAllocator_.instanceBaseId()) );
        CHECKED_GL( textShader_->setUniformValue("fs_colorTextures", coloredAtlasAllocator_.instanceBaseId()) );
        CHECKED_GL( textShader_->setUniformValue("fs_lcdTexture", lcdAtlasAllocator_.instanceBaseId()) );
        CHECKED_GL( textShader_->setUniformValue("pixel_x", 1.0f / float(lcdAtlasAllocator_.width())) );
    });

    initializeRectRendering();
    initializeTextureRendering();
}

void OpenGLRenderer::setRenderSize(Size _size)
{
    size_ = _size;
    projectionMatrix_ = ortho(
        0.0f, float(size_.width),      // left, right
        0.0f, float(size_.height)      // bottom, top
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

atlas::AtlasBackend& OpenGLRenderer::textureScheduler()
{
    return *textureScheduler_;
}

void OpenGLRenderer::initializeRectRendering()
{
    CHECKED_GL( glGenVertexArrays(1, &rectVAO_) );
    CHECKED_GL( glBindVertexArray(rectVAO_) );

    CHECKED_GL( glGenBuffers(1, &rectVBO_) );
    CHECKED_GL( glBindBuffer(GL_ARRAY_BUFFER, rectVBO_) );
    CHECKED_GL( glBufferData(GL_ARRAY_BUFFER, 0, nullptr, GL_STREAM_DRAW) );

    auto constexpr BufferStride = 7 * sizeof(GLfloat);
    auto const VertexOffset = (void const*) (0 * sizeof(GLfloat));
    auto const ColorOffset = (void const*) (3 * sizeof(GLfloat));

    // 0 (vec3): vertex buffer
    CHECKED_GL( glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, BufferStride, VertexOffset) );
    CHECKED_GL( glEnableVertexAttribArray(0) );

    // 1 (vec4): color buffer
    CHECKED_GL( glVertexAttribPointer(1, 4, GL_FLOAT, GL_FALSE, BufferStride, ColorOffset) );
    CHECKED_GL( glEnableVertexAttribArray(1) );
}

void OpenGLRenderer::initializeTextureRendering()
{
    CHECKED_GL( glGenVertexArrays(1, &vao_) );
    CHECKED_GL( glBindVertexArray(vao_) );

    auto constexpr BufferStride = (3 + 4 + 4) * sizeof(GLfloat);
    auto constexpr VertexOffset = (void const*) 0;
    auto const TexCoordOffset = (void const*) (3 * sizeof(GLfloat));
    auto const ColorOffset = (void const*) (7 * sizeof(GLfloat));

    CHECKED_GL( glGenBuffers(1, &vbo_) );
    CHECKED_GL( glBindBuffer(GL_ARRAY_BUFFER, vbo_) );
    CHECKED_GL( glBufferData(GL_ARRAY_BUFFER, 0/* sizeof(GLfloat) * 6 * 11 * 200 * 100*/, nullptr, GL_STREAM_DRAW) );

    // 0 (vec3): vertex buffer
    CHECKED_GL( glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, BufferStride, VertexOffset) );
    CHECKED_GL( glEnableVertexAttribArray(0) );

    // 1 (vec3): texture coordinates buffer
    CHECKED_GL( glVertexAttribPointer(1, 4, GL_FLOAT, GL_FALSE, BufferStride, TexCoordOffset) );
    CHECKED_GL( glEnableVertexAttribArray(1) );

    // 2 (vec4): color buffer
    CHECKED_GL( glVertexAttribPointer(2, 4, GL_FLOAT, GL_FALSE, BufferStride, ColorOffset) );
    CHECKED_GL( glEnableVertexAttribArray(2) );

    // setup EBO
    // glGenBuffers(1, &ebo_);
    // glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ebo_);
    // static const GLuint indices[6] = { 0, 1, 3, 1, 2, 3 };
    // glBufferData(GL_ELEMENT_ARRAY_BUFFER, sizeof(indices), indices, GL_STATIC_DRAW);

    //glVertexAttribDivisor(0, 1); // TODO: later for instanced rendering
}

OpenGLRenderer::~OpenGLRenderer()
{
    CHECKED_GL( glDeleteVertexArrays(1, &rectVAO_) );
    CHECKED_GL( glDeleteBuffers(1, &rectVBO_) );
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

int OpenGLRenderer::maxTextureDepth()
{
    initialize();

    GLint value = {};
    CHECKED_GL( glGetIntegerv(GL_MAX_3D_TEXTURE_SIZE, &value) );
    return static_cast<int>(value);
}

int OpenGLRenderer::maxTextureSize()
{
    initialize();

    GLint value = {};
    CHECKED_GL( glGetIntegerv(GL_MAX_TEXTURE_SIZE, &value) );
    return static_cast<int>(value);
}

void OpenGLRenderer::clearTexture2DArray(GLuint _textureId, int _width, int _height, atlas::Format _format)
{
    bindTexture2DArray(_textureId);

    auto constexpr target = GL_TEXTURE_2D;
    auto constexpr levelOfDetail = 0;
    //auto constexpr depth = 1;
    auto constexpr type = GL_UNSIGNED_BYTE;
    //auto constexpr x0 = 0;
    //auto constexpr y0 = 0;
    //auto constexpr z0 = 0;

    std::vector<uint8_t> stub;
    stub.resize(_width * _height * atlas::element_count(_format)); // {{{ fill stub
    auto t = stub.begin();
    switch (_format)
    {
        case atlas::Format::Red:
            for (auto i = 0; i < _width * _height; ++i)
                *t++ = 0x40;
            break;
        case atlas::Format::RGB:
            for (auto i = 0; i < _width * _height; ++i)
            {
                *t++ = 0x00;
                *t++ = 0x00;
                *t++ = 0x80;
            }
            break;
        case atlas::Format::RGBA:
            for (auto i = 0; i < _width * _height; ++i)
            {
                *t++ = 0x00;
                *t++ = 0x80;
                *t++ = 0x00;
                *t++ = 0x80;
            }
            break;
    }
    assert(t == stub.end()); // }}}

    //glTexSubImage2D(target, levelOfDetail, x0, y0, _width, _height, glFormat(_format), type, stub.data());

    GLenum const glFmt = glFormat(_format);
    GLint constexpr UnusedParam = 0;
    CHECKED_GL( glTexImage2D(target, levelOfDetail, glFmt, _width, _height, UnusedParam, glFmt, type, stub.data()) );
    // glTexSubImage2D(target, levelOfDetail, x0, y0, z0, _width, _height, depth,
    //                 glFormat(_format), type, stub.data());
}

void OpenGLRenderer::createAtlas(atlas::CreateAtlas const& _param)
{
    GLuint textureId{};
    CHECKED_GL( glGenTextures(1, &textureId) );
    bindTexture2DArray(textureId);

    //glTexStorage3D(GL_TEXTURE_2D, 1, glFormatZ(_param.format), _param.width, _param.height, _param.depth);

    CHECKED_GL( glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST) ); // NEAREST, because LINEAR yields borders at the edges
    CHECKED_GL( glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST) );
    CHECKED_GL( glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_R, GL_CLAMP_TO_EDGE) );
    CHECKED_GL( glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE) );
    CHECKED_GL( glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE) );

    auto const key = AtlasKey{_param.atlas};
    atlasMap_[key] = textureId;

    clearTexture2DArray(textureId, _param.width, _param.height, _param.format);
}

void OpenGLRenderer::uploadTexture(atlas::UploadTexture const& _param)
{
    auto const& texture = _param.texture.get();
    auto const key = AtlasKey{texture.atlas};
    [[maybe_unused]] auto const textureIdIter = atlasMap_.find(key);
    assert(textureIdIter != atlasMap_.end() && "Texture ID not found in atlas map!");
    auto const textureId = atlasMap_[key];
    auto const x0 = texture.x;
    auto const y0 = texture.y;
    //auto const z0 = texture.z;

    //debuglog(OpenGLRendererTag).write("({}): {}", textureId, _param);

    auto constexpr target = GL_TEXTURE_2D;
    auto constexpr levelOfDetail = 0;
    //auto constexpr depth = 1;
    auto constexpr type = GL_UNSIGNED_BYTE;

    bindTexture2DArray(textureId);

    switch (_param.format)
    {
        case atlas::Format::RGB:
        case atlas::Format::Red:
            CHECKED_GL( glPixelStorei(GL_UNPACK_ALIGNMENT, 1) );
            break;
        case atlas::Format::RGBA:
            CHECKED_GL( glPixelStorei(GL_UNPACK_ALIGNMENT, 4) );
            break;
    }

    CHECKED_GL( glTexSubImage2D(target, levelOfDetail, x0, y0, texture.width, texture.height, glFormat(_param.format), type, _param.data.data()) );
    // glTexSubImage3D(target, levelOfDetail, x0, y0, z0, texture.width, texture.height, depth,
    //                 glFormat(_param.format), type, _param.data.data());
}

void OpenGLRenderer::renderTexture(atlas::RenderTexture const& _param)
{
    auto const key = AtlasKey{_param.texture.get().atlas};
    if (auto const it = atlasMap_.find(key); it != atlasMap_.end())
    {
        GLuint const textureUnit = _param.texture.get().atlas;
        GLuint const textureId = it->second;

        //debuglog(OpenGLRendererTag).write("({}/{}): {}\n", textureUnit, textureId, _param);

        selectTextureUnit(textureUnit);
        bindTexture2DArray(textureId);
    }
}

void OpenGLRenderer::destroyAtlas(atlas::DestroyAtlas const& _param)
{
    auto const key = AtlasKey{_param.atlas};
    if (auto const it = atlasMap_.find(key); it != atlasMap_.end())
    {
        GLuint const textureId = it->second;
        atlasMap_.erase(it);
        glDeleteTextures(1, &textureId);
    }
}

void OpenGLRenderer::bindTexture2DArray(int _textureId)
{
    if (currentTextureId_ != _textureId)
    {
        glBindTexture(GL_TEXTURE_2D, _textureId);
        currentTextureId_ = _textureId;
    }
}

void OpenGLRenderer::selectTextureUnit(int _id)
{
    if (currentActiveTexture_ != _id)
    {
        glActiveTexture(static_cast<GLenum>(GL_TEXTURE0 + _id));
        currentActiveTexture_ = _id;
    }
}

void OpenGLRenderer::renderRectangle(int _x, int _y, int _width, int _height,
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

optional<AtlasTextureInfo> OpenGLRenderer::readAtlas(atlas::TextureAtlasAllocator const& _allocator, int _instanceId)
{
    // NB: to get all atlas pages, call this from instance base id up to and including current
    // instance id of the given allocator.

    auto const textureId = atlasMap_.at(AtlasKey{_instanceId});

    AtlasTextureInfo output{};
    output.atlasName = _allocator.name();
    output.atlasInstanceId = _instanceId;
    output.size = Size{int(_allocator.width()), int(_allocator.height())};
    output.format = atlas::Format::RGBA;
    output.buffer.resize(_allocator.width() * _allocator.height() * 4);

    // Reading texture data to host CPU (including for RGB textures) only works via framebuffers
    GLuint fbo;
    CHECKED_GL( glGenFramebuffers(1, &fbo) );
    CHECKED_GL( glBindFramebuffer(GL_FRAMEBUFFER, fbo) );
    CHECKED_GL( glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, textureId, 0) );
    CHECKED_GL( glReadPixels(0, 0, output.size.width, output.size.height, GL_RGBA, GL_UNSIGNED_BYTE, output.buffer.data()) );
    CHECKED_GL( glBindFramebuffer(GL_FRAMEBUFFER, 0) );
    CHECKED_GL( glDeleteFramebuffers(1, &fbo) );

    return output;
}

void OpenGLRenderer::scheduleScreenshot(ScreenshotCallback _callback)
{
    pendingScreenshotCallback_ = std::move(_callback);
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
        bound(*rectShader_, [&]() {
            rectShader_->setUniformValue(rectProjectionLocation_, projectionMatrix_);

            glBindVertexArray(rectVAO_);
            glBindBuffer(GL_ARRAY_BUFFER, rectVBO_);
            glBufferData(GL_ARRAY_BUFFER, rectBuffer_.size() * sizeof(GLfloat), rectBuffer_.data(), GL_STREAM_DRAW);

            glDrawArrays(GL_TRIANGLES, 0, static_cast<GLsizei>(rectBuffer_.size() / 7));
            glBindVertexArray(0);
        });
        rectBuffer_.clear();
    }

    // render textures
    //
    bound(*textShader_, [&]() {
        // TODO: only upload when it actually DOES change
        textShader_->setUniformValue(textProjectionLocation_, projectionMatrix_);
        executeRenderTextures();
    });

    if (pendingScreenshotCallback_)
    {
        Size bufferSize = renderBufferSize();
        vector<uint8_t> buffer;
        buffer.resize(bufferSize.width * bufferSize.height * 4);

        debuglog(OpenGLRendererTag).write("Capture screenshot ({}/{}).", bufferSize, size_);

        CHECKED_GL( glReadPixels(0, 0, bufferSize.width, bufferSize.height, GL_RGBA, GL_UNSIGNED_BYTE, buffer.data()) );

        pendingScreenshotCallback_.value()(buffer, bufferSize);
        pendingScreenshotCallback_.reset();
    }
}

Size OpenGLRenderer::renderBufferSize()
{
#if 0
    return size_;
#else
    auto width = GLint(size_.width);
    auto height = GLint(size_.height);
    CHECKED_GL( glGetRenderbufferParameteriv(GL_RENDERBUFFER, GL_RENDERBUFFER_WIDTH, &width) );
    CHECKED_GL( glGetRenderbufferParameteriv(GL_RENDERBUFFER, GL_RENDERBUFFER_HEIGHT, &height) );
    return Size{width, height};
#endif
}

void OpenGLRenderer::executeRenderTextures()
{
    // debuglog(OpenGLRendererTag).write(
    //     "OpenGLRenderer::executeRenderTextures() upload={} render={}",
    //     textureScheduler_->uploadTextures.size(),
    //     textureScheduler_->renderTextures.size()
    // );

    // potentially create new atlases
    for (auto const& params : textureScheduler_->createAtlases)
        createAtlas(params);

    // potentially upload any new textures
    for (auto const& params : textureScheduler_->uploadTextures)
        uploadTexture(params);

    for (auto const& batch: textureScheduler_->renderBatches | ranges::views::values)
    {
        for (auto const& params : batch.renderTextures)
            renderTexture(params);

        // upload vertices and render (iff there is anything to render)
        if (!batch.renderTextures.empty())
        {
            glBindVertexArray(vao_);

            // upload buffer
            glBindBuffer(GL_ARRAY_BUFFER, vbo_);
            glBufferData(GL_ARRAY_BUFFER,
                         batch.buffer.size() * sizeof(GLfloat),
                         batch.buffer.data(),
                         GL_STREAM_DRAW);
            glDrawArrays(GL_TRIANGLES, 0, batch.renderTextures.size() * 6);

            // TODO: Instead of on glDrawArrays (and many if's in the shader for each GL_TEXTUREi),
            //       make a loop over each GL_TEXTUREi and draw a sub range of the vertices and a
            //       fixed GL_TEXTURE0. - will this be noticable faster?
        }
    }

    // destroy any pending atlases that were meant to be destroyed
    for (auto const& params : textureScheduler_->destroyAtlases)
        destroyAtlas(params);

    // reset execution state
    textureScheduler_->reset();
    currentActiveTexture_ = std::numeric_limits<int>::max();
    currentTextureId_ = std::numeric_limits<int>::max();
}

} // end namespace
