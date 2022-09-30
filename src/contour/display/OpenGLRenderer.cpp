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
#include <contour/display/Blur.h>
#include <contour/display/OpenGLRenderer.h>
#include <contour/display/ShaderConfig.h>
#include <contour/helper.h>

#include <terminal_renderer/TextureAtlas.h>

#include <crispy/algorithm.h>
#include <crispy/assert.h>
#include <crispy/utils.h>

#include <range/v3/all.hpp>

#include <QtCore/QtGlobal>
#include <QtGui/QImage>

#include <algorithm>
#include <array>
#include <chrono>
#include <memory>
#include <utility>
#include <vector>

using std::array;
using std::get;
using std::holds_alternative;
using std::make_shared;
using std::min;
using std::move;
using std::nullopt;
using std::optional;
using std::pair;
using std::shared_ptr;
using std::string;
using std::vector;

using terminal::Height;
using terminal::ImageSize;
using terminal::RGBAColor;
using terminal::Width;

namespace chrono = std::chrono;
namespace atlas = terminal::renderer::atlas;

#if !defined(_WIN32)
    #define CRISPY_PACKED __attribute__((packed))
#else
    #define CRISPY_PACKED /*!*/
#endif

namespace contour::display
{

namespace
{
    struct CRISPY_PACKED vec2
    {
        float x;
        float y;
    };

    struct CRISPY_PACKED vec3
    {
        float x;
        float y;
        float z;
    };

    struct CRISPY_PACKED vec4
    {
        float x;
        float y;
        float z;
        float w;
    };

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
    inline void checkedGL(F&& region,
                          logstore::source_location location = logstore::source_location::current()) noexcept
    {
        region();
        auto err = GLenum {};
        while ((err = glGetError()) != GL_NO_ERROR)
            DisplayLog(location)("OpenGL error {} for call.", err);
    }

    GLenum glFormat(atlas::Format _format)
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

    GLenum glFormat(terminal::ImageFormat format)
    {
        switch (format)
        {
            case terminal::ImageFormat::RGB: return GL_RGB;
            case terminal::ImageFormat::RGBA: return GL_RGBA;
        }
        Guarantee(false);
        crispy::unreachable();
    }

    struct OpenGLContextGuard
    {
        QOpenGLContext* _context;
        QSurface* _surface;

        OpenGLContextGuard():
            _context { QOpenGLContext::currentContext() },
            _surface { _context ? _context->surface() : nullptr }
        {
        }

        ~OpenGLContextGuard()
        {
            if (_context)
                _context->makeCurrent(_surface);
        }
    };

    // Returns first non-zero argument.
    template <typename T, typename... More>
    constexpr T firstNonZero(T a, More... more) noexcept
    {
        if constexpr (sizeof...(More) == 0)
            return a;
        else
        {
            if (a != T(0))
                return a;
            else
                return firstNonZero<More...>(std::forward<More>(more)...);
        }
    }

} // namespace

/**
 * Text rendering input:
 *  - vec3 screenCoord    (x/y/z)
 *  - vec4 textureCoord   (x/y and w/h)
 *  - vec4 textColor      (r/g/b/a)
 *
 */

OpenGLRenderer::OpenGLRenderer(ShaderConfig const& textShaderConfig,
                               ShaderConfig const& rectShaderConfig,
                               ShaderConfig const& backgroundImageShaderConfig,
                               ImageSize targetSurfaceSize,
                               ImageSize /*textureTileSize*/,
                               terminal::renderer::PageMargin margin):
    _startTime { chrono::steady_clock::now().time_since_epoch() },
    _now { _startTime },
    _renderTargetSize { targetSurfaceSize },
    _projectionMatrix { ortho(0.0f,
                              unbox<float>(targetSurfaceSize.width),  // left, right
                              unbox<float>(targetSurfaceSize.height), // bottom, top
                              0.0f) },
    _margin { margin },
    _textShader { createShader(textShaderConfig) },
    _textProjectionLocation { _textShader->uniformLocation("vs_projection") },
    _textTimeLocation { _textShader->uniformLocation("u_time") },
    _backgroundShader { createShader(backgroundImageShaderConfig) },
    _rectShader { createShader(rectShaderConfig) },
    _rectProjectionLocation { _rectShader->uniformLocation("u_projection") },
    _rectTimeLocation { _rectShader->uniformLocation("u_time") }
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

    initializeBackgroundRendering();
    initializeRectRendering();
    initializeTextureRendering();
}

void OpenGLRenderer::setRenderSize(ImageSize targetSurfaceSize)
{
    if (_renderTargetSize == targetSurfaceSize)
        return;

    // glOrtho
    _renderTargetSize = targetSurfaceSize;
    DisplayLog()("Setting render target size to {}.", _renderTargetSize);
    _projectionMatrix = ortho(0.0f,
                              unbox<float>(_renderTargetSize.width),  // left, right
                              unbox<float>(_renderTargetSize.height), // bottom, top
                              0.0f);
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
    CHECKED_GL(glGenVertexArrays(1, &_textVAO));
    CHECKED_GL(glBindVertexArray(_textVAO));

    auto constexpr BufferStride = (3 + 4 + 4) * sizeof(GLfloat);
    auto constexpr VertexOffset = (void const*) 0;
    auto const TexCoordOffset = (void const*) (3 * sizeof(GLfloat));
    auto const ColorOffset = (void const*) (7 * sizeof(GLfloat));

    CHECKED_GL(glGenBuffers(1, &_textVBO));
    CHECKED_GL(glBindBuffer(GL_ARRAY_BUFFER, _textVBO));
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
    DisplayLog()("~OpenGLRenderer");
    CHECKED_GL(glDeleteVertexArrays(1, &_rectVAO));
    CHECKED_GL(glDeleteBuffers(1, &_rectVBO));

    if (_backgroundImageTexture)
        CHECKED_GL(glDeleteTextures(1, &_backgroundImageTexture));
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
    DisplayLog()("configureAtlas: {} {}", atlas.size, atlas.properties.format);
    // clang-format on
}

void OpenGLRenderer::uploadTile(atlas::UploadTile tile)
{
    // clang-format off
    // DisplayLog()("uploadTile: atlas {} @ {}:{}",
    //              tile.location.atlasID.value,
    //              tile.location.x.value,
    //              tile.location.y.value);
    if (!(tile.bitmapSize.width <= _textureAtlas.properties.tileSize.width))
        errorlog()("uploadTile assertion alert: width {} <= {} failed.", tile.bitmapSize.width, _textureAtlas.properties.tileSize.width);
    if (!(tile.bitmapSize.height <= _textureAtlas.properties.tileSize.height))
        errorlog()("uploadTile assertion alert: height {} <= {} failed.", tile.bitmapSize.height, _textureAtlas.properties.tileSize.height);
    // clang-format on

    // Require(tile.bitmapSize.width <= _textureAtlas.properties.tileSize.width);
    // Require(tile.bitmapSize.height <= _textureAtlas.properties.tileSize.height);

    _scheduledExecutions.uploadTiles.emplace_back(std::move(tile));
}

void OpenGLRenderer::renderTile(atlas::RenderTile tile)
{
    RenderBatch& batch = _scheduledExecutions.renderBatch;

    // atlas texture Vertices to locate the tile
    auto const x = static_cast<GLfloat>(tile.x.value);
    auto const y = static_cast<GLfloat>(tile.y.value);
    auto const z = static_cast<GLfloat>(0);

    // tile bitmap size on target render surface
    GLfloat const r = unbox<GLfloat>(firstNonZero(tile.targetSize.width, tile.bitmapSize.width));
    GLfloat const s = unbox<GLfloat>(firstNonZero(tile.targetSize.height, tile.bitmapSize.height));

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
    auto const u = static_cast<GLfloat>(tile.fragmentShaderSelector);

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
    return ImageSize { Width::cast_from(width), Height::cast_from(height) };
#endif
}

void OpenGLRenderer::execute()
{
    _currentTextureId = std::numeric_limits<GLuint>::max();

    // FIXME
    // glEnable(GL_BLEND);
    // glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE);
    // glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ZERO, GL_ONE);
    // glBlendFunc(GL_SRC1_COLOR, GL_ONE_MINUS_SRC1_COLOR);

    auto const timeValue = uptime();

    if (_backgroundImageTexture)
    {
        bound(*_backgroundShader, [&]() { executeRenderBackground(timeValue); });
    }

    // render filled rects
    //
    if (!_rectBuffer.empty())
    {
        bound(*_rectShader, [&]() {
            _rectShader->setUniformValue(_rectProjectionLocation, _projectionMatrix);
            _rectShader->setUniformValue(_rectTimeLocation, timeValue);

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
        _textShader->setUniformValue(_textTimeLocation, timeValue);
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
        glBindVertexArray(_textVAO);

        // upload buffer
        // clang-format off
        glBindBuffer(GL_ARRAY_BUFFER, _textVBO);
        glBufferData(GL_ARRAY_BUFFER,
                     static_cast<GLsizei>(batch.buffer.size() * sizeof(GLfloat)),
                     batch.buffer.data(), GL_STREAM_DRAW);
        glDrawArrays(GL_TRIANGLES,
                     0,
                     static_cast<GLsizei>(batch.renderTiles.size() * 6));
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
    DisplayLog()("GL configure atlas: {} {} GL texture Id {}",
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
            for (auto i = 0u; i < param.size.area(); ++i)
                *t++ = 0x40;
            break;
        case atlas::Format::RGB:
            for (auto i = 0u; i < param.size.area(); ++i)
            {
                *t++ = 0x00;
                *t++ = 0x00;
                *t++ = 0x80;
            }
            break;
        case atlas::Format::RGBA:
            for (auto i = 0u; i < param.size.area(); ++i)
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
                            (int) glFmt,
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
    Require(textureId != 0);

    auto constexpr target = GL_TEXTURE_2D;
    auto constexpr LevelOfDetail = 0;
    auto constexpr BitmapType = GL_UNSIGNED_BYTE;

    // clang-format off
    // DisplayLog()("-> uploadTile: tex {} location {} format {} size {}",
    //              textureId, param.location, param.bitmapFormat, param.bitmapSize);
    // clang-format on

    bindTexture(textureId);

    // Image row alignment is 1 byte (OpenGL defaults to 4).
    CHECKED_GL(glPixelStorei(GL_UNPACK_ALIGNMENT, param.rowAlignment));

    // Forece RGBA as OpenGL ES cannot implicitly convert on the driver-side.
    auto bitmapFormat = glFormat(param.bitmapFormat);
    auto bitmapData = (void const*) param.bitmap.data();
    auto bitmapConverted = atlas::Buffer();
    switch (param.bitmapFormat)
    {
        case atlas::Format::Red: {
            bitmapFormat = GL_RGBA;
            bitmapConverted.resize(param.bitmapSize.area() * 4);
            bitmapData = bitmapConverted.data();
            auto s = param.bitmap.data();
            auto e = param.bitmap.data() + param.bitmap.size();
            auto t = bitmapConverted.data();
            while (s != e)
            {
                *t++ = *s++; // red
                *t++ = 0x00; // green
                *t++ = 0x00; // blue
                *t++ = 0xFF; // alpha
            }
            break;
        }
        case atlas::Format::RGB: {
            bitmapFormat = GL_RGBA;
            bitmapConverted.resize(param.bitmapSize.area() * 4);
            bitmapData = bitmapConverted.data();
            auto s = param.bitmap.data();
            auto e = param.bitmap.data() + param.bitmap.size();
            auto t = bitmapConverted.data();
            while (s != e)
            {
                *t++ = *s++; // red
                *t++ = *s++; // green
                *t++ = *s++; // blue
                *t++ = 0xFF; // alpha
            }
            break;
        }
        case atlas::Format::RGBA: break;
    }

    CHECKED_GL(glTexSubImage2D(target,
                               LevelOfDetail,
                               param.location.x.value,
                               param.location.y.value,
                               unbox<GLsizei>(param.bitmapSize.width),
                               unbox<GLsizei>(param.bitmapSize.height),
                               bitmapFormat,
                               BitmapType,
                               bitmapData));
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

    return { std::move(output) };
}

void OpenGLRenderer::scheduleScreenshot(ScreenshotCallback callback)
{
    _pendingScreenshotCallback = std::move(callback);
}

pair<ImageSize, vector<uint8_t>> OpenGLRenderer::takeScreenshot()
{
    ImageSize const imageSize = renderBufferSize();

    vector<uint8_t> buffer;
    buffer.resize(imageSize.area() * 4 /* 4 because RGBA */);

    DisplayLog()("Capture screenshot ({}/{}).", imageSize, _renderTargetSize);

    CHECKED_GL(glReadPixels(0,
                            0,
                            unbox<GLsizei>(imageSize.width),
                            unbox<GLsizei>(imageSize.height),
                            GL_RGBA,
                            GL_UNSIGNED_BYTE,
                            buffer.data()));

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

void OpenGLRenderer::inspect(std::ostream& /*output*/) const
{
}

// {{{ background (image)
struct CRISPY_PACKED BackgroundShaderParams
{
    vec3 vertices;
    vec2 textureCoords;
};

void OpenGLRenderer::initializeBackgroundRendering()
{
    bound(*_backgroundShader, [&]() {
        // clang-format off
        CHECKED_GL(_backgroundShader->setUniformValue("fs_backgroundImage", 0)); // GL_TEXTURE0
        CHECKED_GL(_backgroundUniformLocations.projection = _backgroundShader->uniformLocation("u_projection"));
        CHECKED_GL(_backgroundUniformLocations.viewportResolution = _backgroundShader->uniformLocation("u_viewportResolution"));
        CHECKED_GL(_backgroundUniformLocations.backgroundResolution = _backgroundShader->uniformLocation("u_backgroundResolution"));
        CHECKED_GL(_backgroundUniformLocations.blur = _backgroundShader->uniformLocation("u_blur"));
        CHECKED_GL(_backgroundUniformLocations.opacity = _backgroundShader->uniformLocation("u_opacity"));
        CHECKED_GL(_backgroundUniformLocations.time = _backgroundShader->uniformLocation("u_time"));
        // clang-format on
    });

    // Setup VAO
    CHECKED_GL(glGenVertexArrays(1, &_backgroundVAO));
    CHECKED_GL(glBindVertexArray(_backgroundVAO));

    glGenBuffers(1, &_backgroundVBO);
    CHECKED_GL(glBindBuffer(GL_ARRAY_BUFFER, _backgroundVBO));
    CHECKED_GL(glBufferData(GL_ARRAY_BUFFER, 0, nullptr, GL_STREAM_DRAW));

    auto constexpr BufferStride = sizeof(BackgroundShaderParams);
    auto const VertexOffset = (void const*) offsetof(BackgroundShaderParams, vertices);
    auto const TexCoordOffset = (void const*) offsetof(BackgroundShaderParams, textureCoords);

    // 0 (vec3): vertex buffer
    CHECKED_GL(glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, BufferStride, VertexOffset));
    CHECKED_GL(glEnableVertexAttribArray(0));

    // 1 (vec2): texture coordinates buffer
    CHECKED_GL(glVertexAttribPointer(1, 2, GL_FLOAT, GL_TRUE, BufferStride, TexCoordOffset));
    CHECKED_GL(glEnableVertexAttribArray(1));

    // release
    CHECKED_GL(glBindVertexArray(0));
}

void OpenGLRenderer::setBackgroundImage(shared_ptr<terminal::BackgroundImage const> const& backgroundImageOpt)
{
    if (!backgroundImageOpt || backgroundImageOpt->hash != _renderStateCache.backgroundImageHash)
    {
        _renderStateCache.backgroundImageOpacity = 1.0f;
        if (_backgroundImageTexture)
        {
            glDeleteTextures(1, &_backgroundImageTexture);
            _backgroundImageTexture = 0;
        }
    }

    if (!backgroundImageOpt)
        return;

    auto& backgroundImage = *backgroundImageOpt;
    _renderStateCache.backgroundImageOpacity = backgroundImage.opacity;
    _renderStateCache.backgroundImageBlur = backgroundImage.blur;

    if (holds_alternative<FileSystem::path>(backgroundImage.location))
    {
        auto const filePath = get<FileSystem::path>(backgroundImage.location);
        auto const qFilePath = QString::fromStdString(filePath.string());
        auto qImage = QImage(qFilePath);

        if (backgroundImage.blur)
        {
            DisplayLog()("Blurring background image: {}", filePath.string());
            auto const contextGuard = OpenGLContextGuard {};
            auto blur = Blur {};
            // auto const offsetStr = qgetenv("CONTOUR_BLUR_OFFSET").toStdString();
            // auto const iterStr = qgetenv("CONTOUR_BLUR_ITERATIONS").toStdString();
            // auto const offset = offsetStr.empty() ? 5 : std::stoi(offsetStr);
            // auto const iterations = iterStr.empty() ? 5 : std::stoi(iterStr);
            // qImage = blur.blurDualKawase(std::move(qImage), offset, iterations);
            qImage = blur.blurGaussian(std::move(qImage));
        }

        qImage = qImage.convertToFormat(QImage::Format_RGBA8888);
        if (qImage.format() != QImage::Format_RGBA8888)
        {
            errorlog()("Unsupported image format {} for background image at {}.",
                       static_cast<int>(qImage.format()),
                       filePath.string());
            return;
        }
        auto const imageFormat = terminal::ImageFormat::RGBA;
        auto const rowAlignment = 4; // This is default. Can it be any different?
        DisplayLog()("Background image from disk: {}x{} {}", qImage.width(), qImage.height(), imageFormat);
        _renderStateCache.backgroundImageHash = crispy::StrongHash::compute(filePath.string());
        _renderStateCache.backgroundResolution = qImage.size();
        _backgroundImageTexture =
            createAndUploadImage(qImage.size(), imageFormat, rowAlignment, qImage.constBits());
    }
    else if (holds_alternative<terminal::ImageDataPtr>(backgroundImage.location))
    {
        auto const& imageData = *get<terminal::ImageDataPtr>(backgroundImage.location);
        DisplayLog()("Background inline image: {} {}", imageData.size, imageData.format);
        _renderStateCache.backgroundImageHash = imageData.hash;
        _backgroundImageTexture =
            createAndUploadImage(QSize(unbox<int>(imageData.size.width), unbox<int>(imageData.size.height)),
                                 imageData.format,
                                 imageData.rowAlignment,
                                 imageData.pixels.data());
    }
}

GLuint OpenGLRenderer::createAndUploadImage(QSize imageSize,
                                            terminal::ImageFormat format,
                                            int rowAlignment,
                                            uint8_t const* pixels)
{
    auto textureId = GLuint {};
    CHECKED_GL(glGenTextures(1, &textureId));
    CHECKED_GL(bindTexture(textureId));

    CHECKED_GL(glTexParameteri(GL_TEXTURE_2D,
                               GL_TEXTURE_MAG_FILTER,
                               GL_NEAREST)); // NEAREST, because LINEAR yields borders at the edges
    CHECKED_GL(glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST));
    CHECKED_GL(glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_R, GL_CLAMP_TO_EDGE));
    CHECKED_GL(glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE));
    CHECKED_GL(glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE));
    CHECKED_GL(glPixelStorei(GL_UNPACK_ALIGNMENT, rowAlignment));

    auto constexpr target = GL_TEXTURE_2D;
    auto constexpr levelOfDetail = 0;
    auto constexpr type = GL_UNSIGNED_BYTE;
    auto constexpr UnusedParam = 0;
    auto constexpr internalFormat = GL_RGBA;

    auto const imageFormat = glFormat(format);
    auto const textureWidth = static_cast<GLsizei>(imageSize.width());
    auto const textureHeight = static_cast<GLsizei>(imageSize.height());

    Require(imageFormat == internalFormat); // OpenGL ES cannot handle implicit conversion.

    CHECKED_GL(glTexImage2D(target,
                            levelOfDetail,
                            internalFormat,
                            textureWidth,
                            textureHeight,
                            UnusedParam,
                            imageFormat,
                            type,
                            pixels));
    return textureId;
}

void OpenGLRenderer::executeRenderBackground(float timeValue)
{
    Require(_backgroundImageTexture != 0);

    auto const w = unbox<float>(_renderTargetSize.width);
    auto const h = unbox<float>(_renderTargetSize.height);

    // {{{ setup uniforms
    // clang-format off
    auto const opacity = float(_renderStateCache.backgroundColor.alpha()) / 255.0f * _renderStateCache.backgroundImageOpacity;
    auto const qViewportSize = QSize(unbox<int>(_renderTargetSize.width), unbox<int>(_renderTargetSize.height));
    auto const blur = 0.0f; // _renderStateCache.backgroundImageBlur ? 1.0f : 0.0f;
    // NOTE: We currently hard disable the live shader ability, as most people
    // won't have a top-end graphics card to still deliver great performance
    // when wanting an awesome blurred image at the same time.
    // Blur is now implemented offscreen via Blur::blurDualKawase().
    // clang-format on

    _backgroundShader->setUniformValue(_backgroundUniformLocations.projection, _projectionMatrix);
    _backgroundShader->setUniformValue(_backgroundUniformLocations.backgroundResolution,
                                       _renderStateCache.backgroundResolution);
    _backgroundShader->setUniformValue(_backgroundUniformLocations.viewportResolution, qViewportSize);
    _backgroundShader->setUniformValue(_backgroundUniformLocations.blur, blur);
    _backgroundShader->setUniformValue(_backgroundUniformLocations.opacity, float { opacity });
    _backgroundShader->setUniformValue(_backgroundUniformLocations.time, timeValue);
    // }}}

    // clang-format off
    auto vertices = array {
        // triangle 1
        BackgroundShaderParams{vec3{ 0.0f, 0.0f, 0.0f }, vec2 { 0.0f, 1.0f } }, // bottom left
        BackgroundShaderParams{vec3{    w, 0.0f, 0.0f }, vec2 { 1.0f, 1.0f } }, // bottom right
        BackgroundShaderParams{vec3{    w,    h, 0.0f }, vec2 { 1.0f, 0.0f } }, // top right
        // riangle 2
        BackgroundShaderParams{vec3{    w,    h, 0.0f }, vec2 { 1.0f, 0.0f } }, // top right
        BackgroundShaderParams{vec3{    0,    h, 0.0f }, vec2 { 0.0f, 0.0f } }, // top left
        BackgroundShaderParams{vec3{ 0.0f,    0, 0.0f }, vec2 { 0.0f, 1.0f } }, // bottom left
    };

    CHECKED_GL(glActiveTexture(GL_TEXTURE0));
    CHECKED_GL(bindTexture(_backgroundImageTexture));
    CHECKED_GL(glBindVertexArray(_backgroundVAO));
    CHECKED_GL(glBindBuffer(GL_ARRAY_BUFFER, _backgroundVBO));

    CHECKED_GL(glBufferData(GL_ARRAY_BUFFER, vertices.size() * sizeof(BackgroundShaderParams), vertices.data(), GL_STREAM_DRAW));

    auto constexpr ElementCount = sizeof(BackgroundShaderParams) / sizeof(float);
    CHECKED_GL(glDrawArrays(GL_TRIANGLES, 0, 6 * ElementCount));
    // clang-format on
}
// }}}

} // namespace contour::display
