// SPDX-License-Identifier: Apache-2.0
#include <contour/display/Blur.h>
#include <contour/display/OpenGLRenderer.h>
#include <contour/display/ShaderConfig.h>
#include <contour/helper.h>

#include <vtbackend/primitives.h>

#include <vtrasterizer/TextureAtlas.h>

#include <crispy/algorithm.h>
#include <crispy/assert.h>
#include <crispy/defines.h>
#include <crispy/utils.h>

#include <range/v3/all.hpp>

#include <QtCore/QtGlobal>
#include <QtGui/QGuiApplication>
#include <QtGui/QImage>

#include <algorithm>
#include <array>
#include <chrono>
#include <memory>
#include <optional>
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

using vtbackend::Height;
using vtbackend::ImageSize;
using vtbackend::RGBAColor;
using vtbackend::Width;

using namespace std::string_view_literals;

namespace chrono = std::chrono;
namespace atlas = vtrasterizer::atlas;

namespace contour::display
{

namespace ZAxisDepths
{
    constexpr GLfloat BackgroundSGR = 0.0f;
    constexpr GLfloat Text = 0.0f;
} // namespace ZAxisDepths

namespace
{
    struct CRISPY_PACKED vec2 // NOLINT
    {
        float x;
        float y;
    };

    struct CRISPY_PACKED vec3 // NOLINT
    {
        float x;
        float y;
        float z;
    };

    struct CRISPY_PACKED vec4 // NOLINT
    {
        float x;
        float y;
        float z;
        float w;
    };

    constexpr bool isPowerOfTwo(uint32_t value) noexcept
    {
        //.
        return (value & (value - 1)) == 0;
    }

    template <typename T, typename Fn>
    inline void bound(T& bindable, Fn&& callable)
    {
        bindable.bind();
        try
        {
            callable();
        }
        catch (...)
        {
            bindable.release();
            throw;
        }
        bindable.release();
    }

    template <typename F>
    inline void checkedGL(F&& region,
                          logstore::source_location location = logstore::source_location::current()) noexcept
    {
        region();
        auto err = GLenum {};
        while ((err = glGetError()) != GL_NO_ERROR)
            displayLog(location)("OpenGL error {} for call.", err);
    }

    QMatrix4x4 ortho(float left, float right, float bottom, float top)
    {
        constexpr float NearPlane = -1.0f;
        constexpr float FarPlane = 1.0f;

        QMatrix4x4 mat;
        mat.ortho(left, right, bottom, top, NearPlane, FarPlane);
        return mat;
    }

    GLenum glFormat(vtbackend::ImageFormat format)
    {
        switch (format)
        {
            case vtbackend::ImageFormat::RGB: return GL_RGB;
            case vtbackend::ImageFormat::RGBA: return GL_RGBA;
        }
        Guarantee(false);
        crispy::unreachable();
    }

    struct OpenGLContextGuard
    {
        QOpenGLContext* context;
        QSurface* surface;

        OpenGLContextGuard():
            context { QOpenGLContext::currentContext() }, surface { context ? context->surface() : nullptr }
        {
        }

        ~OpenGLContextGuard()
        {
            if (context)
                context->makeCurrent(surface);
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

OpenGLRenderer::OpenGLRenderer(ShaderConfig textShaderConfig,
                               ShaderConfig rectShaderConfig,
                               vtbackend::ImageSize viewSize,
                               vtbackend::ImageSize targetSurfaceSize,
                               [[maybe_unused]] vtbackend::ImageSize textureTileSize,
                               vtrasterizer::PageMargin margin):
    _startTime { chrono::steady_clock::now().time_since_epoch() },
    _viewSize { viewSize },
    _margin { margin },
    _textShaderConfig { std::move(textShaderConfig) },
    _rectShaderConfig { std::move(rectShaderConfig) }
{
    displayLog()("OpenGLRenderer: Constructing with render size {}.", _renderTargetSize);
    setRenderSize(targetSurfaceSize);
}

void OpenGLRenderer::setRenderSize(vtbackend::ImageSize targetSurfaceSize)
{
    if (_renderTargetSize == targetSurfaceSize)
        return;

    _renderTargetSize = targetSurfaceSize;
    _projectionMatrix = ortho(/* left */ 0.0f,
                              /* right */ unbox<float>(_renderTargetSize.width),
                              /* bottom */ unbox<float>(_renderTargetSize.height),
                              /* top */ 0.0f);

    displayLog()("Setting render target size to {}.", _renderTargetSize);
}

void OpenGLRenderer::setTranslation(float x, float y, float z) noexcept
{
    _viewMatrix.setToIdentity();
    _viewMatrix.translate(x, y, z);
}

void OpenGLRenderer::setModelMatrix(QMatrix4x4 matrix) noexcept
{
    _modelMatrix = matrix;
}

void OpenGLRenderer::setMargin(vtrasterizer::PageMargin margin) noexcept
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

    constexpr auto const BufferStride = 7 * sizeof(GLfloat);
    const auto* const VertexOffset = (void const*) (0 * sizeof(GLfloat)); // NOLINT
    const auto* const ColorOffset = (void const*) (3 * sizeof(GLfloat));  // NOLINT

    // 0 (vec3): vertex buffer
    CHECKED_GL(glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, BufferStride, VertexOffset));
    CHECKED_GL(glEnableVertexAttribArray(0));

    // 1 (vec4): color buffer
    CHECKED_GL(glVertexAttribPointer(1, 4, GL_FLOAT, GL_FALSE, BufferStride, ColorOffset));
    CHECKED_GL(glEnableVertexAttribArray(1));

    CHECKED_GL(glBindVertexArray(0));
}

void OpenGLRenderer::initializeTextureRendering()
{
    CHECKED_GL(glGenVertexArrays(1, &_textVAO));
    CHECKED_GL(glBindVertexArray(_textVAO));

    constexpr auto const BufferStride = (3 + 4 + 4) * sizeof(GLfloat);
    constexpr auto* const VertexOffset = (void const*) nullptr;
    const auto* const TexCoordOffset = (void const*) (3 * sizeof(GLfloat)); // NOLINT
    const auto* const ColorOffset = (void const*) (7 * sizeof(GLfloat));    // NOLINT

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

    CHECKED_GL(glBindVertexArray(0));
}

OpenGLRenderer::~OpenGLRenderer()
{
    displayLog()("~OpenGLRenderer");
    CHECKED_GL(glDeleteVertexArrays(1, &_rectVAO));
    CHECKED_GL(glDeleteBuffers(1, &_rectVBO));
}

void OpenGLRenderer::initialize()
{
    if (_initialized)
        return;

    Q_ASSERT(_window != nullptr);
    QSGRendererInterface* rif = _window->rendererInterface();
    Q_ASSERT(rif->graphicsApi() == QSGRendererInterface::OpenGL);

    _initialized = true;

    initializeOpenGLFunctions();
    CONSUME_GL_ERRORS();

    // clang-format off
    CHECKED_GL(_textShader = createShader(_textShaderConfig));
    CHECKED_GL(_textProjectionLocation = _textShader->uniformLocation("vs_projection")); // NOLINT(cppcoreguidelines-prefer-member-initializer)
    CHECKED_GL(_textTextureAtlasLocation = _textShader->uniformLocation("fs_textureAtlas"));
    CHECKED_GL(_textTimeLocation = _textShader->uniformLocation("u_time")); // NOLINT(cppcoreguidelines-prefer-member-initializer)
    CHECKED_GL(_rectShader = createShader(_rectShaderConfig));
    CHECKED_GL(_rectProjectionLocation = _rectShader->uniformLocation("u_projection")); // NOLINT(cppcoreguidelines-prefer-member-initializer)
    CHECKED_GL(_rectTimeLocation = _rectShader->uniformLocation("u_time")); // NOLINT(cppcoreguidelines-prefer-member-initializer)
    // clang-format on

    // Image row alignment is 1 byte (OpenGL defaults to 4).
    _transferOptions.setAlignment(1);

    setRenderSize(_renderTargetSize);

    assert(_textProjectionLocation != -1);

    bound(*_textShader, [&]() {
        auto const textureAtlasWidth = unbox<GLfloat>(_textureAtlas.textureSize.width);
        CHECKED_GL(_textShader->setUniformValue("pixel_x", 1.0f / textureAtlasWidth));
        CHECKED_GL(_textShader->setUniformValue(_textTextureAtlasLocation, 0)); // GL_TEXTURE0?
    });

    initializeRectRendering();
    initializeTextureRendering();

    logInfo();
}

void OpenGLRenderer::logInfo()
{
    Require(QOpenGLContext::currentContext() != nullptr);
    QOpenGLFunctions& glFunctions = *QOpenGLContext::currentContext()->functions();

    auto const openGLTypeString = QOpenGLContext::currentContext()->isOpenGLES() ? "OpenGL/ES"sv : "OpenGL"sv;
    displayLog()("[FYI] OpenGL type         : {}", openGLTypeString);
    displayLog()("[FYI] OpenGL renderer     : {}", (char const*) glFunctions.glGetString(GL_RENDERER));

    GLint versionMajor {};
    GLint versionMinor {};
    glFunctions.glGetIntegerv(GL_MAJOR_VERSION, &versionMajor);
    glFunctions.glGetIntegerv(GL_MINOR_VERSION, &versionMinor);
    displayLog()("[FYI] OpenGL version      : {}.{}", versionMajor, versionMinor);
    displayLog()("[FYI] Widget size         : {} ({})", _renderTargetSize, _viewSize);

    string glslVersions = (char const*) glFunctions.glGetString(GL_SHADING_LANGUAGE_VERSION);
#if 0 // defined(GL_NUM_SHADING_LANGUAGE_VERSIONS)
    QOpenGLExtraFunctions& glFunctionsExtra = *QOpenGLContext::currentContext()->extraFunctions();
    GLint glslNumShaderVersions {};
    glFunctions.glGetIntegerv(GL_NUM_SHADING_LANGUAGE_VERSIONS, &glslNumShaderVersions);
    glFunctions.glGetError(); // consume possible OpenGL error.
    if (glslNumShaderVersions > 0)
    {
        glslVersions += " (";
        for (GLint k = 0, l = 0; k < glslNumShaderVersions; ++k)
            if (auto const str = glFunctionsExtra.glGetStringi(GL_SHADING_LANGUAGE_VERSION, GLuint(k)); str && *str)
            {
                glslVersions += (l ? ", " : "");
                glslVersions += (char const*) str;
                l++;
            }
        glslVersions += ')';
    }
#endif
    displayLog()("[FYI] GLSL version        : {}", glslVersions);
}

void OpenGLRenderer::clearCache()
{
}

int OpenGLRenderer::maxTextureDepth()
{
    GLint value = {};
    CHECKED_GL(glGetIntegerv(GL_MAX_3D_TEXTURE_SIZE, &value));
    return static_cast<int>(value);
}

int OpenGLRenderer::maxTextureSize()
{
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

    displayLog()("configureAtlas: {} {}", atlas.size, atlas.properties.format);
}

void OpenGLRenderer::uploadTile(atlas::UploadTile tile)
{
    // clang-format off
    // displayLog()("uploadTile: atlas {} @ {}:{}",
    //              tile.location.atlasID.value,
    //              tile.location.x.value,
    //              tile.location.y.value);
    if (!(tile.bitmapSize.width <= _textureAtlas.properties.tileSize.width))
        errorLog()("uploadTile assertion alert: width {} <= {} failed.", tile.bitmapSize.width, _textureAtlas.properties.tileSize.width);
    if (!(tile.bitmapSize.height <= _textureAtlas.properties.tileSize.height))
        errorLog()("uploadTile assertion alert: height {} <= {} failed.", tile.bitmapSize.height, _textureAtlas.properties.tileSize.height);
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
    auto const z = ZAxisDepths::Text;

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

struct ScopedRenderEnvironment
{
    QOpenGLExtraFunctions& gl;

    bool savedBlend;          // QML seems to explicitly disable that, but we need it.
    GLenum savedDepthFunc {}; // Shuold be GL_LESS, but you never know.
    GLuint savedVAO {};       // QML sets that before and uses it later, so we need to back it up, too.
    GLenum savedBlendSource {};
    GLenum savedBlendDestination {};

    ScopedRenderEnvironment(QOpenGLExtraFunctions& glIn):
        gl { glIn }, // clang-format off
        savedBlend { gl.glIsEnabled(GL_BLEND) != GL_FALSE } // clang-format on
    {
        gl.glGetIntegerv(GL_VERTEX_ARRAY_BINDING, (GLint*) &savedVAO);

        gl.glGetIntegerv(GL_DEPTH_FUNC, (GLint*) &savedDepthFunc);
        gl.glDepthFunc(GL_LEQUAL);
        gl.glDepthMask(GL_FALSE);

        // Enable color blending to allow drawing text/images on top of background.
        gl.glGetIntegerv(GL_BLEND_SRC, (GLint*) &savedBlendSource);
        gl.glGetIntegerv(GL_BLEND_DST, (GLint*) &savedBlendDestination);
        gl.glEnable(GL_BLEND);
        // _gl.glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_SRC_ALPHA, GL_ONE);
        gl.glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE);
    }

    ~ScopedRenderEnvironment()
    {
        gl.glBlendFunc(savedBlendSource, savedBlendDestination);
        gl.glDepthFunc(savedDepthFunc);
        if (!savedBlend)
            gl.glDisable(GL_BLEND);

        gl.glBindVertexArray(savedVAO);
        gl.glDepthMask(GL_TRUE);
    }
};

void OpenGLRenderer::execute(std::chrono::steady_clock::time_point now)
{
    Require(_initialized);

    auto const _ = ScopedRenderEnvironment { *this };

    auto const timeValue = uptime(now);

    // displayLog()("execute {} rects, {} uploads, {} renders\n",
    //              _rectBuffer.size() / 7,
    //              _scheduledExecutions.uploadTiles.size(),
    //              _scheduledExecutions.renderBatch.renderTiles.size());

    auto const mvp = _projectionMatrix * _viewMatrix * _modelMatrix;

    // render filled rects
    //
    if (!_rectBuffer.empty())
    {
        bound(*_rectShader, [&]() {
            _rectShader->setUniformValue(_rectProjectionLocation, mvp);
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

    // potentially (re-)configure atlas
    //
    if (_scheduledExecutions.configureAtlas)
        executeConfigureAtlas(*_scheduledExecutions.configureAtlas);

    // potentially upload any new textures
    //
    if (!_scheduledExecutions.uploadTiles.empty())
    {
        _textureAtlas.gpuTexture.bind();
        for (auto const& params: _scheduledExecutions.uploadTiles)
            executeUploadTile(params);
        _textureAtlas.gpuTexture.release();
    }

    // render textures
    //
    bound(*_textShader, [&]() {
        // TODO: only upload when it actually DOES change
        _textShader->setUniformValue(_textProjectionLocation, mvp);
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
    // upload vertices and render
    RenderBatch& batch = _scheduledExecutions.renderBatch;
    if (!batch.renderTiles.empty())
    {
        _textureAtlas.gpuTexture.bind();
        glBindVertexArray(_textVAO);

        // upload buffer
        glBindBuffer(GL_ARRAY_BUFFER, _textVBO);
        glBufferData(GL_ARRAY_BUFFER,
                     static_cast<GLsizei>(batch.buffer.size() * sizeof(GLfloat)),
                     batch.buffer.data(),
                     GL_STREAM_DRAW);
        glDrawArrays(GL_TRIANGLES, 0, static_cast<GLsizei>(batch.renderTiles.size() * 6));

        glBindVertexArray(0);
        _textureAtlas.gpuTexture.release();
    }

    _scheduledExecutions.clear();
}

void OpenGLRenderer::executeConfigureAtlas(atlas::ConfigureAtlas const& param)
{
    Require(isPowerOfTwo(unbox(param.size.width)));
    Require(isPowerOfTwo(unbox(param.size.height)));
    Require(param.properties.format == atlas::Format::RGBA);

    // Already initialized.
    // _textureAtlas.textureSize = param.size;
    // _textureAtlas.properties = param.properties;

    if (_textureAtlas.gpuTexture.isCreated())
        _textureAtlas.gpuTexture.destroy();

    _textureAtlas.gpuTexture.setMipLevels(0);
    _textureAtlas.gpuTexture.setAutoMipMapGenerationEnabled(false);
    _textureAtlas.gpuTexture.setFormat(QOpenGLTexture::TextureFormat::RGBA8_UNorm);
    _textureAtlas.gpuTexture.setSize(unbox<int>(param.size.width), unbox<int>(param.size.height));
    _textureAtlas.gpuTexture.setMagnificationFilter(QOpenGLTexture::Filter::Nearest);
    _textureAtlas.gpuTexture.setMinificationFilter(QOpenGLTexture::Filter::Nearest);
    _textureAtlas.gpuTexture.setWrapMode(QOpenGLTexture::WrapMode::ClampToEdge);
    _textureAtlas.gpuTexture.create();
    Require(_textureAtlas.gpuTexture.isCreated());

    QImage stubData(QSize(unbox<int>(param.size.width), unbox<int>(param.size.height)),
                    QImage::Format::Format_RGBA8888);
    stubData.fill(qRgba(0x00, 0xA0, 0x00, 0xC0));
    _textureAtlas.gpuTexture.setData(stubData);

    displayLog()(
        "GL configure atlas: {} {} GL texture Id {}", param.size, param.properties.format, textureAtlasId());
}

void OpenGLRenderer::executeUploadTile(atlas::UploadTile const& param)
{
    Require(textureAtlasId() != 0);

    // clang-format off
    // displayLog()("-> uploadTile: tex {} location {} format {} size {}",
    //              textureId, param.location, param.bitmapFormat, param.bitmapSize);
    // clang-format on

    // {{{ Force RGBA as OpenGL ES cannot implicitly convert on the driver-side.
    auto const* bitmapData = (void const*) param.bitmap.data();
    auto bitmapConverted = atlas::Buffer();
    switch (param.bitmapFormat)
    {
        case atlas::Format::Red: {
            bitmapConverted.resize(param.bitmapSize.area() * 4);
            bitmapData = bitmapConverted.data();
            auto const* s = param.bitmap.data();
            auto const* const e = param.bitmap.data() + param.bitmap.size();
            auto* t = bitmapConverted.data();
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
            bitmapConverted.resize(param.bitmapSize.area() * 4);
            bitmapData = bitmapConverted.data();
            auto const* s = param.bitmap.data();
            auto const* const e = param.bitmap.data() + param.bitmap.size();
            auto* t = bitmapConverted.data();
            while (s != e)
            {
                *t++ = *s++; // red
                *t++ = *s++; // green
                *t++ = *s++; // blue
                *t++ = 0xFF; // alpha
            }
            break;
        }
        case atlas::Format::RGBA:
            // Already in expected format
            break;
    }
        // }}}

#if QT_VERSION >= QT_VERSION_CHECK(5, 14, 0)
    _textureAtlas.gpuTexture.setData(param.location.x.value,
                                     param.location.y.value,
                                     0, // z
                                     unbox<int>(param.bitmapSize.width),
                                     unbox<int>(param.bitmapSize.height),
                                     0, // depth
                                     QOpenGLTexture::PixelFormat::RGBA,
                                     QOpenGLTexture::PixelType::UInt8,
                                     bitmapData,
                                     &_transferOptions);
#else
    glTexSubImage2D(GL_TEXTURE_2D,
                    0, // level of detail
                    param.location.x.value,
                    param.location.y.value,
                    unbox<GLsizei>(param.bitmapSize.width),
                    unbox<GLsizei>(param.bitmapSize.height),
                    GL_RGBA,          // source format
                    GL_UNSIGNED_BYTE, // source type
                    bitmapData);
#endif
}

void OpenGLRenderer::renderRectangle(int ix, int iy, Width width, Height height, RGBAColor color)
{
    auto const x = static_cast<GLfloat>(ix);
    auto const y = static_cast<GLfloat>(iy);
    auto const z = ZAxisDepths::BackgroundSGR;
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

optional<vtrasterizer::AtlasTextureScreenshot> OpenGLRenderer::readAtlas()
{
    // NB: to get all atlas pages, call this from instance base id up to and including current
    // instance id of the given allocator.
    auto output = vtrasterizer::AtlasTextureScreenshot {};
    output.atlasInstanceId = 0;
    output.size = _textureAtlas.textureSize;
    output.format = _textureAtlas.properties.format;
    output.buffer.resize(_textureAtlas.textureSize.area() * element_count(_textureAtlas.properties.format));

    // Reading texture data to host CPU (including for RGB textures) only works via framebuffers
    auto fbo = GLuint {};
    CHECKED_GL(glGenFramebuffers(1, &fbo));
    CHECKED_GL(glBindFramebuffer(GL_FRAMEBUFFER, fbo));
    CHECKED_GL(
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, textureAtlasId(), 0));
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

    displayLog()("Capture screenshot ({}/{}).", imageSize, _renderTargetSize);

    CHECKED_GL(glReadPixels(0,
                            0,
                            unbox<GLsizei>(imageSize.width),
                            unbox<GLsizei>(imageSize.height),
                            GL_RGBA,
                            GL_UNSIGNED_BYTE,
                            buffer.data()));

    return { imageSize, buffer };
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

GLuint OpenGLRenderer::createAndUploadImage(QSize imageSize,
                                            vtbackend::ImageFormat format,
                                            int rowAlignment,
                                            uint8_t const* pixels)
{
    auto textureId = GLuint {};
    CHECKED_GL(glGenTextures(1, &textureId));
    CHECKED_GL(glBindTexture(GL_TEXTURE_2D, textureId));

    CHECKED_GL(glTexParameteri(GL_TEXTURE_2D,
                               GL_TEXTURE_MAG_FILTER,
                               GL_NEAREST)); // NEAREST, because LINEAR yields borders at the edges
    CHECKED_GL(glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST));
    CHECKED_GL(glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_R, GL_CLAMP_TO_EDGE));
    CHECKED_GL(glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE));
    CHECKED_GL(glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE));
    CHECKED_GL(glPixelStorei(GL_UNPACK_ALIGNMENT, rowAlignment));

    constexpr auto const Target = GL_TEXTURE_2D;
    constexpr auto const LevelOfDetail = 0;
    constexpr auto const Type = GL_UNSIGNED_BYTE;
    constexpr auto const UnusedParam = 0;
    constexpr auto const InternalFormat = GL_RGBA;

    auto const imageFormat = glFormat(format);
    auto const textureWidth = static_cast<GLsizei>(imageSize.width());
    auto const textureHeight = static_cast<GLsizei>(imageSize.height());

    Require(imageFormat == InternalFormat); // OpenGL ES cannot handle implicit conversion.

    CHECKED_GL(glTexImage2D(Target,
                            LevelOfDetail,
                            InternalFormat,
                            textureWidth,
                            textureHeight,
                            UnusedParam,
                            imageFormat,
                            Type,
                            pixels));
    return textureId;
}
// }}}

} // namespace contour::display
