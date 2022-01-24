/**
 * This file is part of the "libterminal" project
 *   Copyright (c) 2019-2021 Christian Parpart <christian@parpart.family>
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
#include <contour/helper.h>
#include <contour/opengl/OpenGLRenderer.h>
#include <contour/opengl/ShaderConfig.h>

#include <terminal_renderer/TextureAtlas.h>

#include <crispy/algorithm.h>
#include <crispy/assert.h>
#include <crispy/utils.h>

#include <range/v3/all.hpp>

#include <algorithm>
#include <array>
#include <utility>
#include <vector>

using std::array;
using std::min;
using std::move;
using std::nullopt;
using std::optional;
using std::pair;
using std::string;
using std::vector;

using terminal::Height;
using terminal::ImageSize;
using terminal::RGBAColor;
using terminal::Width;

namespace atlas = terminal::renderer::atlas;

namespace contour::opengl
{

namespace atlas = terminal::renderer::atlas;

#if !defined(NDEBUG)
    #define CHECKED_GL(code)                                                      \
        do                                                                        \
        {                                                                         \
            (code);                                                               \
            GLenum err {};                                                        \
            while ((err = glGetError()) != GL_NO_ERROR)                           \
                LOGSTORE(DisplayLog)("OpenGL error {} for call: {}", err, #code); \
        } while (0)
#else
    #define CHECKED_GL(code) \
        do                   \
        {                    \
            (code);          \
        } while (0)
#endif

namespace
{
    static constexpr bool isPowerOfTwo(uint32_t value) noexcept
    {
        //.
        return (value & (value - 1)) == 0;
    }

    template <typename T, typename Fn>
    inline void bound(T& _bindable, Fn&& _callable)
    {
        _bindable.bind();
        try
        {
            _callable();
        }
        catch (...)
        {
            _bindable.release();
            throw;
        }
        _bindable.release();
    }

    template <typename F>
    inline void checkedGL(F&& region) noexcept
    {
        region();
        auto err = GLenum {};
        while ((err = glGetError()) != GL_NO_ERROR)
            LOGSTORE(DisplayLog)("OpenGL error {} for call.", err);
    }

    int glFormat(atlas::Format _format)
    {
        switch (_format)
        {
        case atlas::Format::RGBA: return GL_RGBA;
        case atlas::Format::RGB: return GL_RGB;
        case atlas::Format::Red: return GL_RED;
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
} // namespace

constexpr int MaxColorTextureSize = 2048;

/**
 * Text rendering input:
 *  - vec3 screenCoord    (x/y/z)
 *  - vec4 textureCoord   (x/y and w/h)
 *  - vec4 textColor      (r/g/b/a)
 *
 */

OpenGLRenderer::OpenGLRenderer(ShaderConfig const& textShaderConfig,
                               ShaderConfig const& rectShaderConfig,
                               ImageSize targetSurfaceSize,
                               ImageSize textureTileSize,
                               terminal::renderer::PageMargin margin):
    _renderTargetSize { targetSurfaceSize },
    _projectionMatrix { ortho(0.0f,
                              float(*targetSurfaceSize.width), // left, right
                              0.0f,
                              float(*targetSurfaceSize.height) // bottom, top
                              ) },
    _margin { margin },

    _textShader { createShader(textShaderConfig) },
    _textProjectionLocation { _textShader->uniformLocation("vs_projection") },
    _rectShader { createShader(rectShaderConfig) },
    _rectProjectionLocation { _rectShader->uniformLocation("u_projection") }
{
    initialize();

    setRenderSize(targetSurfaceSize);

    assert(_textProjectionLocation != -1);

    CHECKED_GL(glEnable(GL_BLEND));
    CHECKED_GL(glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE));
    // glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ZERO, GL_ONE);
    //  //glBlendFunc(GL_SRC1_COLOR, GL_ONE_MINUS_SRC1_COLOR);

    bound(*_textShader, [&]() {
        CHECKED_GL(_textShader->setUniformValue("fs_textureAtlas", 0)); // GL_TEXTURE0?
        auto const textureAtlasWidth = unbox<GLfloat>(_textureAtlas.textureSize.width);
        CHECKED_GL(_textShader->setUniformValue("pixel_x", 1.0f / textureAtlasWidth));
    });

    initializeRectRendering();
    initializeTextureRendering();
}

crispy::ImageSize OpenGLRenderer::colorTextureSizeHint() noexcept
{
    return ImageSize { Width(min(MaxColorTextureSize, maxTextureSize())),
                       Height(min(MaxColorTextureSize, maxTextureSize())) };
}

void OpenGLRenderer::setRenderSize(ImageSize targetSurfaceSize)
{
    _renderTargetSize = targetSurfaceSize;
    _projectionMatrix = ortho(0.0f,
                              float(*_renderTargetSize.width), // left, right
                              0.0f,
                              float(*_renderTargetSize.height) // bottom, top
    );
}

void OpenGLRenderer::setMargin(terminal::renderer::PageMargin margin) noexcept
{
    _margin = margin;
}

atlas::AtlasBackend& OpenGLRenderer::textureScheduler()
{
    return *this;
}

void OpenGLRenderer::initializeRectRendering()
{
    CHECKED_GL(glGenVertexArrays(1, &_rectVAO));
    CHECKED_GL(glBindVertexArray(_rectVAO));

    CHECKED_GL(glGenBuffers(1, &_rectVBO));
    CHECKED_GL(glBindBuffer(GL_ARRAY_BUFFER, _rectVBO));
    CHECKED_GL(glBufferData(GL_ARRAY_BUFFER, 0, nullptr, GL_STREAM_DRAW));

    auto constexpr BufferStride = 7 * sizeof(GLfloat);
    auto const VertexOffset = (void const*) (0 * sizeof(GLfloat));
    auto const ColorOffset = (void const*) (3 * sizeof(GLfloat));

    // 0 (vec3): vertex buffer
    CHECKED_GL(glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, BufferStride, VertexOffset));
    CHECKED_GL(glEnableVertexAttribArray(0));

    // 1 (vec4): color buffer
    CHECKED_GL(glVertexAttribPointer(1, 4, GL_FLOAT, GL_FALSE, BufferStride, ColorOffset));
    CHECKED_GL(glEnableVertexAttribArray(1));
}

void OpenGLRenderer::initializeTextureRendering()
{
    CHECKED_GL(glGenVertexArrays(1, &_vao));
    CHECKED_GL(glBindVertexArray(_vao));

    auto constexpr BufferStride = (3 + 4 + 4) * sizeof(GLfloat);
    auto constexpr VertexOffset = (void const*) 0;
    auto const TexCoordOffset = (void const*) (3 * sizeof(GLfloat));
    auto const ColorOffset = (void const*) (7 * sizeof(GLfloat));

    CHECKED_GL(glGenBuffers(1, &_vbo));
    CHECKED_GL(glBindBuffer(GL_ARRAY_BUFFER, _vbo));
    CHECKED_GL(glBufferData(GL_ARRAY_BUFFER, 0, nullptr, GL_STREAM_DRAW));

    // 0 (vec3): vertex buffer
    CHECKED_GL(glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, BufferStride, VertexOffset));
    CHECKED_GL(glEnableVertexAttribArray(0));

    // 1 (vec4): texture coordinates buffer
    CHECKED_GL(glVertexAttribPointer(1, 4, GL_FLOAT, GL_FALSE, BufferStride, TexCoordOffset));
    CHECKED_GL(glEnableVertexAttribArray(1));

    // 2 (vec4): color buffer
    CHECKED_GL(glVertexAttribPointer(2, 4, GL_FLOAT, GL_FALSE, BufferStride, ColorOffset));
    CHECKED_GL(glEnableVertexAttribArray(2));

    // setup EBO
    // glGenBuffers(1, &_ebo);
    // glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, _ebo);
    // static const GLuint indices[6] = { 0, 1, 3, 1, 2, 3 };
    // glBufferData(GL_ELEMENT_ARRAY_BUFFER, sizeof(indices), indices, GL_STATIC_DRAW);

    // glVertexAttribDivisor(0, 1); // TODO: later for instanced rendering
}

OpenGLRenderer::~OpenGLRenderer()
{
    LOGSTORE(DisplayLog)("~OpenGLRenderer");
    CHECKED_GL(glDeleteVertexArrays(1, &_rectVAO));
    CHECKED_GL(glDeleteBuffers(1, &_rectVBO));
}

void OpenGLRenderer::initialize()
{
    if (!_initialized)
    {
        _initialized = true;
        initializeOpenGLFunctions();
    }
}

void OpenGLRenderer::clearCache()
{
}

int OpenGLRenderer::maxTextureDepth()
{
    initialize();

    GLint value = {};
    CHECKED_GL(glGetIntegerv(GL_MAX_3D_TEXTURE_SIZE, &value));
    return static_cast<int>(value);
}

int OpenGLRenderer::maxTextureSize()
{
    initialize();

    GLint value = {};
    CHECKED_GL(glGetIntegerv(GL_MAX_TEXTURE_SIZE, &value));
    return static_cast<int>(value);
}

// {{{ AtlasBackend impl
ImageSize OpenGLRenderer::atlasSize() const noexcept
{
    return _textureAtlas.textureSize;
}

void OpenGLRenderer::configureAtlas(atlas::ConfigureAtlas atlas)
{
    // schedule atlas creation
    _scheduledExecutions.configureAtlas.emplace(atlas);
    _textureAtlas.textureSize = atlas.size;
    _textureAtlas.properties = atlas.properties;

    // clang-format off
    LOGSTORE(DisplayLog)("configureAtlas: {} {}", atlas.size, atlas.properties.format);
    // clang-format on
}

void OpenGLRenderer::uploadTile(atlas::UploadTile tile)
{
    // clang-format off
    // LOGSTORE(DisplayLog)("uploadTile: atlas {} @ {}:{}",
    //                      tile.location.atlasID.value,
    //                      tile.location.x.value,
    //                      tile.location.y.value);
    // clang-format on

    _scheduledExecutions.uploadTiles.emplace_back(move(tile));
}

void OpenGLRenderer::renderTile(atlas::RenderTile tile)
{
    RenderBatch& batch = _scheduledExecutions.renderBatch;

    // atlas texture Vertices to locate the tile
    auto const x = static_cast<GLfloat>(tile.x.value);
    auto const y = static_cast<GLfloat>(tile.y.value);
    auto const z = static_cast<GLfloat>(0);

    // tile bitmap size on target render surface
    GLfloat const r = unbox<GLfloat>(tile.bitmapSize.width); // r/s: target size
    GLfloat const s = unbox<GLfloat>(tile.bitmapSize.height);

    // normalized TexCoords
    GLfloat const nx = tile.normalizedLocation.x;
    GLfloat const ny = tile.normalizedLocation.y;
    GLfloat const nw = tile.normalizedLocation.width;
    GLfloat const nh = tile.normalizedLocation.height;

    // These two are currently not used.
    // This used to be used for the z-plane into the 3D texture,
    // but I've reverted back to a 2D texture atlas for now.
    GLfloat const i = 0;

    // Tile dependant userdata.
    // This is current the fragment shader's selector that
    // determines how to operate on this tile (images vs gray-scale anti-aliased
    // glyphs vs LCD subpixel antialiased glyphs)
    GLfloat const u = static_cast<GLfloat>(tile.fragmentShaderSelector);

    // color
    GLfloat const cr = tile.color[0];
    GLfloat const cg = tile.color[1];
    GLfloat const cb = tile.color[2];
    GLfloat const ca = tile.color[3];

    // clang-format off
    GLfloat const vertices[6 * 11] = {
        // first triangle
    // <X      Y      Z> <X        Y        I  U>  <R   G   B   A>
        x,     y + s, z,  nx,      ny + nh, i, u,  cr, cg, cb, ca, // left top
        x,     y,     z,  nx,      ny,      i, u,  cr, cg, cb, ca, // left bottom
        x + r, y,     z,  nx + nw, ny,      i, u,  cr, cg, cb, ca, // right bottom

        // second triangle
        x,     y + s, z,  nx,      ny + nh, i, u,  cr, cg, cb, ca, // left top
        x + r, y,     z,  nx + nw, ny,      i, u,  cr, cg, cb, ca, // right bottom
        x + r, y + s, z,  nx + nw, ny + nh, i, u,  cr, cg, cb, ca, // right top

        // buffer contains
        // - 3 vertex coordinates (XYZ)
        // - 4 texture coordinates (XYIU), I is unused currently, U selects which texture to use
        // - 4 color values (RGBA)
    };
    // clang-format on

    batch.renderTiles.emplace_back(tile);
    crispy::copy(vertices, back_inserter(batch.buffer));
}
// }}}

// {{{ executor impl
ImageSize OpenGLRenderer::renderBufferSize()
{
#if 0
    return renderTargetSize_;
#else
    auto width = unbox<GLint>(_renderTargetSize.width);
    auto height = unbox<GLint>(_renderTargetSize.height);
    CHECKED_GL(glGetRenderbufferParameteriv(GL_RENDERBUFFER, GL_RENDERBUFFER_WIDTH, &width));
    CHECKED_GL(glGetRenderbufferParameteriv(GL_RENDERBUFFER, GL_RENDERBUFFER_HEIGHT, &height));
    return ImageSize { Width(width), Height(height) };
#endif
}

void OpenGLRenderer::execute()
{
    // FIXME
    // glEnable(GL_BLEND);
    // glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE);
    // glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ZERO, GL_ONE);
    // glBlendFunc(GL_SRC1_COLOR, GL_ONE_MINUS_SRC1_COLOR);

    // render filled rects
    //
    if (!_rectBuffer.empty())
    {
        bound(*_rectShader, [&]() {
            _rectShader->setUniformValue(_rectProjectionLocation, _projectionMatrix);

            glBindVertexArray(_rectVAO);
            glBindBuffer(GL_ARRAY_BUFFER, _rectVBO);
            glBufferData(GL_ARRAY_BUFFER,
                         static_cast<GLsizeiptr>(_rectBuffer.size() * sizeof(GLfloat)),
                         _rectBuffer.data(),
                         GL_STREAM_DRAW);

            glDrawArrays(GL_TRIANGLES, 0, static_cast<GLsizei>(_rectBuffer.size() / 7));
            glBindVertexArray(0);
        });
        _rectBuffer.clear();
    }

    // render textures
    //
    bound(*_textShader, [&]() {
        // TODO: only upload when it actually DOES change
        _textShader->setUniformValue(_textProjectionLocation, _projectionMatrix);
        executeRenderTextures();
    });

    if (_pendingScreenshotCallback)
    {
        auto result = takeScreenshot();
        _pendingScreenshotCallback.value()(result.second, result.first);
        _pendingScreenshotCallback.reset();
    }
}

void OpenGLRenderer::executeRenderTextures()
{
    _currentTextureId = std::numeric_limits<int>::max();

    // potentially (re-)configure atlas
    if (_scheduledExecutions.configureAtlas)
        executeConfigureAtlas(*_scheduledExecutions.configureAtlas);

    // potentially upload any new textures
    for (auto const& params: _scheduledExecutions.uploadTiles)
        executeUploadTile(params);

    // upload vertices and render
    RenderBatch& batch = _scheduledExecutions.renderBatch;
    if (!batch.renderTiles.empty())
    {
        glActiveTexture(static_cast<GLenum>(GL_TEXTURE0 + batch.userdata));
        bindTexture(_textureAtlas.textureId);
        glBindVertexArray(_vao);

        // upload buffer
        // clang-format off
        glBindBuffer(GL_ARRAY_BUFFER, _vbo);
        glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizei>(batch.buffer.size() * sizeof(GLfloat)), batch.buffer.data(), GL_STREAM_DRAW);
        glDrawArrays(GL_TRIANGLES, 0, static_cast<GLsizei>(batch.renderTiles.size() * 6));
        // clang-format on
    }

    _scheduledExecutions.clear();
}

void OpenGLRenderer::executeConfigureAtlas(atlas::ConfigureAtlas const& param)
{
    if (_textureAtlas.textureId)
        glDeleteTextures(1, &_textureAtlas.textureId);

    CHECKED_GL(glGenTextures(1, &_textureAtlas.textureId));
    bindTexture(_textureAtlas.textureId);

    Require(isPowerOfTwo(unbox<uint32_t>(param.size.width)));
    Require(isPowerOfTwo(unbox<uint32_t>(param.size.height)));

    // Already initialized.
    // _textureAtlas.textureSize = param.size;
    // _textureAtlas.properties = param.properties;

    CHECKED_GL(glTexParameteri(GL_TEXTURE_2D,
                               GL_TEXTURE_MAG_FILTER,
                               GL_NEAREST)); // NEAREST, because LINEAR yields borders at the edges
    CHECKED_GL(glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST));
    CHECKED_GL(glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_R, GL_CLAMP_TO_EDGE));
    CHECKED_GL(glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE));
    CHECKED_GL(glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE));

    // clang-format off
    LOGSTORE(DisplayLog)("GL configure atlas: {} {} GL texture Id {}",
                         param.size, param.properties.format, _textureAtlas.textureId);
    // clang-format on

    auto constexpr target = GL_TEXTURE_2D;
    auto constexpr levelOfDetail = 0;
    auto constexpr type = GL_UNSIGNED_BYTE;

    std::vector<uint8_t> stub;
    // {{{ fill stub
    stub.resize(param.size.area() * element_count(param.properties.format));
    auto t = stub.begin();
    switch (param.properties.format)
    {
    case atlas::Format::Red:
        for (auto i = 0; i < param.size.area(); ++i)
            *t++ = 0x40;
        break;
    case atlas::Format::RGB:
        for (auto i = 0; i < param.size.area(); ++i)
        {
            *t++ = 0x00;
            *t++ = 0x00;
            *t++ = 0x80;
        }
        break;
    case atlas::Format::RGBA:
        for (auto i = 0; i < param.size.area(); ++i)
        {
            *t++ = 0x00;
            *t++ = 0xA0;
            *t++ = 0x00;
            *t++ = 0xC0;
        }
        break;
    }
    // }}}

    GLenum const glFmt = glFormat(param.properties.format);
    GLint constexpr UnusedParam = 0;
    CHECKED_GL(glTexImage2D(target,
                            levelOfDetail,
                            glFmt,
                            unbox<int>(param.size.width),
                            unbox<int>(param.size.height),
                            UnusedParam,
                            glFmt,
                            type,
                            stub.data()));
}

void OpenGLRenderer::executeUploadTile(atlas::UploadTile const& param)
{
    auto const textureId = _textureAtlas.textureId;

    auto constexpr target = GL_TEXTURE_2D;
    auto constexpr LevelOfDetail = 0;
    auto constexpr BitmapType = GL_UNSIGNED_BYTE;

    // clang-format off
    // LOGSTORE(DisplayLog)("-> uploadTile: tex {} location {} format {} size {}",
    //                      textureId, param.location, param.bitmapFormat, param.bitmapSize);
    // clang-format on

    bindTexture(textureId);

    // Image row alignment is 1 byte (OpenGL defaults to 4).
    CHECKED_GL(glPixelStorei(GL_UNPACK_ALIGNMENT, 1));

    CHECKED_GL(glTexSubImage2D(target,
                               LevelOfDetail,
                               param.location.x.value,
                               param.location.y.value,
                               param.bitmapSize.width.value,
                               param.bitmapSize.height.value,
                               glFormat(param.bitmapFormat),
                               BitmapType,
                               param.bitmap.data()));
}

void OpenGLRenderer::executeDestroyAtlas()
{
    glDeleteTextures(1, &_textureAtlas.textureId);
    _textureAtlas.textureId = 0;
}

void OpenGLRenderer::bindTexture(GLuint textureId)
{
    if (_currentTextureId != textureId)
    {
        CHECKED_GL(glBindTexture(GL_TEXTURE_2D, textureId));
        _currentTextureId = textureId;
    }
}

void OpenGLRenderer::renderRectangle(int ix, int iy, Width width, Height height, RGBAColor color)
{
    auto const x = static_cast<GLfloat>(ix);
    auto const y = static_cast<GLfloat>(iy);
    auto const z = GLfloat { 0.0f };
    auto const r = unbox<GLfloat>(width);
    auto const s = unbox<GLfloat>(height);
    auto const [cr, cg, cb, ca] = atlas::normalize(color);

    // clang-format off
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
    // clang-format on

    crispy::copy(vertices, back_inserter(_rectBuffer));
}

optional<terminal::renderer::AtlasTextureScreenshot> OpenGLRenderer::readAtlas()
{
    // NB: to get all atlas pages, call this from instance base id up to and including current
    // instance id of the given allocator.

    auto output = terminal::renderer::AtlasTextureScreenshot {};
    output.atlasInstanceId = 0;
    output.size = _textureAtlas.textureSize;
    output.format = _textureAtlas.properties.format;
    output.buffer.resize(_textureAtlas.textureSize.area() * element_count(_textureAtlas.properties.format));

    // Reading texture data to host CPU (including for RGB textures) only works via framebuffers
    auto fbo = GLuint {};
    CHECKED_GL(glGenFramebuffers(1, &fbo));
    CHECKED_GL(glBindFramebuffer(GL_FRAMEBUFFER, fbo));
    CHECKED_GL(glFramebufferTexture2D(
        GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, _textureAtlas.textureId, 0));
    CHECKED_GL(glReadPixels(0,
                            0,
                            unbox<GLsizei>(output.size.width),
                            unbox<GLsizei>(output.size.height),
                            GL_RGBA,
                            GL_UNSIGNED_BYTE,
                            output.buffer.data()));
    CHECKED_GL(glBindFramebuffer(GL_FRAMEBUFFER, 0));
    CHECKED_GL(glDeleteFramebuffers(1, &fbo));

    return { move(output) };
}

void OpenGLRenderer::scheduleScreenshot(ScreenshotCallback callback)
{
    _pendingScreenshotCallback = move(callback);
}

pair<ImageSize, vector<uint8_t>> OpenGLRenderer::takeScreenshot()
{
    ImageSize const imageSize = renderBufferSize();

    vector<uint8_t> buffer;
    buffer.resize(imageSize.area() * 4 /* 4 because RGBA */);

    LOGSTORE(DisplayLog)("Capture screenshot ({}/{}).", imageSize, _renderTargetSize);

    CHECKED_GL(
        glReadPixels(0, 0, *imageSize.width, *imageSize.height, GL_RGBA, GL_UNSIGNED_BYTE, buffer.data()));

    return { imageSize, buffer };
}

void OpenGLRenderer::clear(terminal::RGBAColor fillColor)
{
    if (fillColor != _renderStateCache.backgroundColor)
    {
        auto const clearColor = atlas::normalize(fillColor);
        glClearColor(clearColor[0], clearColor[1], clearColor[2], clearColor[3]);
        _renderStateCache.backgroundColor = fillColor;
    }

    glClear(GL_COLOR_BUFFER_BIT);
}

// }}}

void OpenGLRenderer::inspect(std::ostream& output) const
{
}

} // namespace contour::opengl
