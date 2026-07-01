// SPDX-License-Identifier: Apache-2.0
#include <contour/display/Blur.h>
#include <contour/display/RhiRenderer.h>
#include <contour/display/RhiVertexLayout.h>
#include <contour/display/ShaderConfig.h>
#include <contour/helper.h>

#include <vtbackend/primitives.h>

#include <vtrasterizer/TextureAtlas.h>

#include <crispy/algorithm.h>
#include <crispy/assert.h>
#include <crispy/defines.h>
#include <crispy/utils.h>

#include <QtCore/QFile>
#include <QtCore/QtGlobal>
#include <QtGui/QGuiApplication>
#include <QtGui/QImage>

#include <chrono>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

using std::optional;
using std::pair;
using std::vector;

using vtbackend::Height;
using vtbackend::ImageSize;
using vtbackend::RGBAColor;
using vtbackend::Width;

namespace chrono = std::chrono;
namespace atlas = vtrasterizer::atlas;

namespace contour::display
{

namespace ZAxisDepths
{
    constexpr float BackgroundSGR = 0.0f;
    constexpr float Text = 0.0f;
} // namespace ZAxisDepths

namespace
{
    // Vertex strides/offsets and std140 uniform-block layouts live in the dependency-free RhiVertexLayout.h
    // (single source of truth, unit-tested against the shader contract in RhiVertexLayout_test). Alias the
    // ones used here so the local call sites stay terse.
    using rhilayout::RectColorOffset;
    using rhilayout::RectPositionOffset;
    using rhilayout::RectUniformBlockSize;
    using rhilayout::RectVertexStride;
    using rhilayout::TextColorOffset;
    using rhilayout::TextPositionOffset;
    using rhilayout::TextTexCoordOffset;
    using rhilayout::TextUniformBlockSize;
    using rhilayout::TextVertexStride;

    // {{{ baked .qsb resource paths (embedded via qt6_add_shaders in CMakeLists.txt)
    constexpr auto BackgroundVertexShaderPath = ":/contour/display/shaders/background.vert.qsb";
    constexpr auto BackgroundFragmentShaderPath = ":/contour/display/shaders/background.frag.qsb";
    constexpr auto TextVertexShaderPath = ":/contour/display/shaders/text.vert.qsb";
    constexpr auto TextFragmentShaderPath = ":/contour/display/shaders/text.frag.qsb";
    // }}}

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

RhiRenderer::RhiRenderer(vtbackend::ImageSize targetSurfaceSize,
                         [[maybe_unused]] vtbackend::ImageSize textureTileSize,
                         vtrasterizer::PageMargin margin):
    _startTime { chrono::steady_clock::now().time_since_epoch() }, _margin { margin }
{
    displayLog()("RhiRenderer: Constructing with render size {}.", _renderTargetSize);
    setRenderSize(targetSurfaceSize);
}

void RhiRenderer::setRenderSize(vtbackend::ImageSize targetSurfaceSize)
{
    if (_renderTargetSize == targetSurfaceSize)
        return;

    // The render size is the terminal item's device-pixel extent. It no longer drives a projection
    // matrix — the scene graph supplies that via setProjectionMatrix() — but the renderer's height is the
    // reference frame for the transient inner scissor (smooth scroll / cursor clip in vtrasterizer's
    // Renderer), which is expressed bottom-left relative to this size.
    _renderTargetSize = targetSurfaceSize;

    displayLog()("Setting render target size to {}.", _renderTargetSize);
}

void RhiRenderer::setProjectionMatrix(QMatrix4x4 const& matrix) noexcept
{
    _projectionMatrix = matrix;
}

void RhiRenderer::setModelMatrix(QMatrix4x4 const& matrix) noexcept
{
    _modelMatrix = matrix;
}

void RhiRenderer::setMargin(vtrasterizer::PageMargin margin) noexcept
{
    _margin = margin;
}

atlas::AtlasBackend& RhiRenderer::textureScheduler()
{
    return *this;
}

RhiRenderer::~RhiRenderer()
{
    displayLog()("~RhiRenderer");
    // RHI resources are released through their std::unique_ptr<QRhiResource*, QRhiResourceDeleter>
    // members in reverse declaration order; nothing raw to clean up here.
}

void RhiRenderer::initialize()
{
    // The renderer no longer owns raw OpenGL state. The RHI pipelines, buffers, atlas texture and sampler
    // are built lazily from the render node's prepare() via createPipelines() once the scene graph's QRhi
    // and the frame's render-pass descriptor are available. We only flip the initialized flag and set the
    // host image-row alignment used by texture uploads.
    if (_initialized)
        return;

    _initialized = true;

    // Image row alignment is 1 byte (OpenGL defaults to 4).
    _transferOptions.setAlignment(1);
}

void RhiRenderer::clearCache()
{
}

// {{{ RHI pipeline + atlas construction
QShader RhiRenderer::loadShader(QString const& resourcePath)
{
    QFile file(resourcePath);
    if (!file.open(QIODevice::ReadOnly))
    {
        errorLog()("Failed to open baked shader resource: {}", resourcePath.toStdString());
        return {};
    }
    QShader const shader = QShader::fromSerialized(file.readAll());
    if (!shader.isValid())
        errorLog()("Failed to deserialize baked shader resource: {}", resourcePath.toStdString());
    return shader;
}

void RhiRenderer::createAtlasTexture(QRhi* rhi, ImageSize size)
{
    auto const pixelSize = QSize(unbox<int>(size.width), unbox<int>(size.height));

    // (Re)create the atlas texture as a plain sampled RGBA8 texture (no extra usage flags): it is a CPU
    // upload destination (uploadTexture) and a fragment-shader sample source, both of which a default
    // sampled texture supports. The debug-only atlas readback uses readBackTexture(), which the RHI backs
    // with a transient copy where supported.
    _atlasTexture.reset(rhi->newTexture(QRhiTexture::RGBA8, pixelSize, 1, {}));
    if (!_atlasTexture->create())
        errorLog()("Failed to create RHI atlas texture of size {}.", size);
    _atlasTextureSize = size;
    _atlasCreatedSize = size;

    // Nearest filtering + clamp-to-edge mirrors the former QOpenGLTexture configuration; created once and
    // reused across atlas resizes.
    if (!_atlasSampler)
    {
        _atlasSampler.reset(rhi->newSampler(QRhiSampler::Nearest,
                                            QRhiSampler::Nearest,
                                            QRhiSampler::None,
                                            QRhiSampler::ClampToEdge,
                                            QRhiSampler::ClampToEdge));
        if (!_atlasSampler->create())
            errorLog()("Failed to create RHI atlas sampler.");
    }
}

void RhiRenderer::createRectPipeline(QRhi* rhi, QRhiRenderPassDescriptor* rpDesc)
{
    auto const vertexShader = loadShader(BackgroundVertexShaderPath);
    auto const fragmentShader = loadShader(BackgroundFragmentShaderPath);
    if (!vertexShader.isValid() || !fragmentShader.isValid())
        return;

    // Dynamic uniform buffer feeding both stages (std140 `Buf` block at binding 0).
    _rectPipeline.uniformBuffer.reset(
        rhi->newBuffer(QRhiBuffer::Dynamic, QRhiBuffer::UniformBuffer, RectUniformBlockSize));
    _rectPipeline.uniformBuffer->create();

    // The vertex buffer is created lazily (as an Immutable buffer) in recordRectPass() once the per-frame
    // geometry size is known, and re-created when it must grow.

    _rectPipeline.srb.reset(rhi->newShaderResourceBindings());
    _rectPipeline.srb->setBindings({
        QRhiShaderResourceBinding::uniformBuffer(0,
                                                 QRhiShaderResourceBinding::VertexStage
                                                     | QRhiShaderResourceBinding::FragmentStage,
                                                 _rectPipeline.uniformBuffer.get()),
    });
    _rectPipeline.srb->create();

    auto* pipeline = rhi->newGraphicsPipeline();
    pipeline->setTopology(QRhiGraphicsPipeline::Triangles);
    // The scene graph renders this node within its depth-ordered pass; depth-testing must be enabled (with
    // DepthAwareRendering on the node) for the node's output to survive into the frame, but depth is not
    // written since the terminal is alpha-blended translucent content. LessOrEqual (vs the default Less) so
    // geometry sitting exactly at the node's assigned depth slot still passes.
    pipeline->setDepthTest(true);
    pipeline->setDepthWrite(false);
    pipeline->setDepthOp(QRhiGraphicsPipeline::LessOrEqual);

    // The node clip and the transient inner scissor (smooth scroll / cursor) are applied dynamically via
    // QRhiCommandBuffer::setScissor, which requires the pipeline to opt into dynamic scissoring.
    pipeline->setFlags(QRhiGraphicsPipeline::UsesScissor);

    // Blend: glBlendFuncSeparate(SRC_ALPHA, ONE_MINUS_SRC_ALPHA, ONE, ONE), color write all.
    QRhiGraphicsPipeline::TargetBlend blend;
    blend.enable = true;
    blend.colorWrite = QRhiGraphicsPipeline::ColorMask(QRhiGraphicsPipeline::R | QRhiGraphicsPipeline::G
                                                       | QRhiGraphicsPipeline::B | QRhiGraphicsPipeline::A);
    blend.srcColor = QRhiGraphicsPipeline::SrcAlpha;
    blend.dstColor = QRhiGraphicsPipeline::OneMinusSrcAlpha;
    blend.opColor = QRhiGraphicsPipeline::Add;
    blend.srcAlpha = QRhiGraphicsPipeline::One;
    blend.dstAlpha = QRhiGraphicsPipeline::One;
    blend.opAlpha = QRhiGraphicsPipeline::Add;
    pipeline->setTargetBlends({ blend });

    pipeline->setShaderStages({
        { QRhiShaderStage::Vertex, vertexShader },
        { QRhiShaderStage::Fragment, fragmentShader },
    });

    // Vertex input: single interleaved binding, vec3 position @loc0, vec4 color @loc1.
    QRhiVertexInputLayout inputLayout;
    inputLayout.setBindings({ QRhiVertexInputBinding(RectVertexStride) });
    inputLayout.setAttributes({
        QRhiVertexInputAttribute(0, 0, QRhiVertexInputAttribute::Float3, RectPositionOffset),
        QRhiVertexInputAttribute(0, 1, QRhiVertexInputAttribute::Float4, RectColorOffset),
    });
    pipeline->setVertexInputLayout(inputLayout);

    pipeline->setShaderResourceBindings(_rectPipeline.srb.get());
    pipeline->setRenderPassDescriptor(rpDesc);
    if (!pipeline->create())
        errorLog()("Failed to create RHI background/rect graphics pipeline.");

    _rectPipeline.pipeline.reset(pipeline);
}

void RhiRenderer::createTextPipeline(QRhi* rhi, QRhiRenderPassDescriptor* rpDesc)
{
    auto const vertexShader = loadShader(TextVertexShaderPath);
    auto const fragmentShader = loadShader(TextFragmentShaderPath);
    if (!vertexShader.isValid() || !fragmentShader.isValid())
        return;

    Require(_atlasTexture != nullptr);
    Require(_atlasSampler != nullptr);

    _textPipeline.uniformBuffer.reset(
        rhi->newBuffer(QRhiBuffer::Dynamic, QRhiBuffer::UniformBuffer, TextUniformBlockSize));
    _textPipeline.uniformBuffer->create();

    // The vertex buffer is created lazily (as an Immutable buffer) in recordTextPass(), see createRect.

    _textPipeline.srb.reset(rhi->newShaderResourceBindings());
    _textPipeline.srb->setBindings({
        QRhiShaderResourceBinding::uniformBuffer(0,
                                                 QRhiShaderResourceBinding::VertexStage
                                                     | QRhiShaderResourceBinding::FragmentStage,
                                                 _textPipeline.uniformBuffer.get()),
        QRhiShaderResourceBinding::sampledTexture(
            1, QRhiShaderResourceBinding::FragmentStage, _atlasTexture.get(), _atlasSampler.get()),
    });
    _textPipeline.srb->create();

    auto* pipeline = rhi->newGraphicsPipeline();
    pipeline->setTopology(QRhiGraphicsPipeline::Triangles);
    // The scene graph renders this node within its depth-ordered pass; depth-testing must be enabled (with
    // DepthAwareRendering on the node) for the node's output to survive into the frame, but depth is not
    // written since the terminal is alpha-blended translucent content. LessOrEqual (vs the default Less) so
    // geometry sitting exactly at the node's assigned depth slot still passes.
    pipeline->setDepthTest(true);
    pipeline->setDepthWrite(false);
    pipeline->setDepthOp(QRhiGraphicsPipeline::LessOrEqual);

    // Dynamic scissoring for the node clip + transient inner scissor (smooth scroll / cursor), see above.
    pipeline->setFlags(QRhiGraphicsPipeline::UsesScissor);

    QRhiGraphicsPipeline::TargetBlend blend;
    blend.enable = true;
    blend.colorWrite = QRhiGraphicsPipeline::ColorMask(QRhiGraphicsPipeline::R | QRhiGraphicsPipeline::G
                                                       | QRhiGraphicsPipeline::B | QRhiGraphicsPipeline::A);
    blend.srcColor = QRhiGraphicsPipeline::SrcAlpha;
    blend.dstColor = QRhiGraphicsPipeline::OneMinusSrcAlpha;
    blend.opColor = QRhiGraphicsPipeline::Add;
    blend.srcAlpha = QRhiGraphicsPipeline::One;
    blend.dstAlpha = QRhiGraphicsPipeline::One;
    blend.opAlpha = QRhiGraphicsPipeline::Add;
    pipeline->setTargetBlends({ blend });

    pipeline->setShaderStages({
        { QRhiShaderStage::Vertex, vertexShader },
        { QRhiShaderStage::Fragment, fragmentShader },
    });

    // Vertex input: single interleaved binding, vec3 position @loc0, vec4 texCoords @loc1, vec4 color @loc2.
    QRhiVertexInputLayout inputLayout;
    inputLayout.setBindings({ QRhiVertexInputBinding(TextVertexStride) });
    inputLayout.setAttributes({
        QRhiVertexInputAttribute(0, 0, QRhiVertexInputAttribute::Float3, TextPositionOffset),
        QRhiVertexInputAttribute(0, 1, QRhiVertexInputAttribute::Float4, TextTexCoordOffset),
        QRhiVertexInputAttribute(0, 2, QRhiVertexInputAttribute::Float4, TextColorOffset),
    });
    pipeline->setVertexInputLayout(inputLayout);

    pipeline->setShaderResourceBindings(_textPipeline.srb.get());
    pipeline->setRenderPassDescriptor(rpDesc);
    if (!pipeline->create())
        errorLog()("Failed to create RHI text/glyph graphics pipeline.");

    _textPipeline.pipeline.reset(pipeline);
}

void RhiRenderer::createPipelines(QRhi* rhi, QRhiRenderPassDescriptor* rpDesc)
{
    Require(rhi != nullptr);
    Require(rpDesc != nullptr);

    // Nothing to rebuild if the pipelines are already valid for this RHI and render-pass layout.
    if (pipelinesReady() && _rhi == rhi && _renderPassDescriptor == rpDesc)
        return;

    _rhi = rhi;
    _renderPassDescriptor = rpDesc;

    // Ensure the atlas texture + sampler exist before the text pipeline references them. If configureAtlas
    // has not been observed yet, fall back to the currently known atlas size (or a 1x1 placeholder) so the
    // shader-resource-bindings are valid; the real atlas is (re)created on the next configureAtlas.
    if (!_atlasTexture)
    {
        auto const size =
            _atlasTextureSize.area() != 0 ? _atlasTextureSize : ImageSize { Width(1), Height(1) };
        createAtlasTexture(rhi, size);
    }

    createRectPipeline(rhi, rpDesc);
    createTextPipeline(rhi, rpDesc);

    displayLog()("createPipelines: rect={} text={}",
                 _rectPipeline.pipeline != nullptr,
                 _textPipeline.pipeline != nullptr);
}
// }}}

// {{{ AtlasBackend impl
ImageSize RhiRenderer::atlasSize() const noexcept
{
    return _atlasTextureSize;
}

void RhiRenderer::configureAtlas(atlas::ConfigureAtlas atlas)
{
    // schedule atlas creation
    _scheduledExecutions.configureAtlas.emplace(atlas);
    _atlasTextureSize = atlas.size;
    _atlasProperties = atlas.properties;

    displayLog()("configureAtlas: {} {}", atlas.size, atlas.properties.format);
}

void RhiRenderer::uploadTile(atlas::UploadTile tile)
{
    // clang-format off
    if (!(tile.bitmapSize.width <= _atlasProperties.tileSize.width))
        errorLog()("uploadTile assertion alert: width {} <= {} failed.", tile.bitmapSize.width, _atlasProperties.tileSize.width);
    if (!(tile.bitmapSize.height <= _atlasProperties.tileSize.height))
        errorLog()("uploadTile assertion alert: height {} <= {} failed.", tile.bitmapSize.height, _atlasProperties.tileSize.height);
    // clang-format on

    _scheduledExecutions.uploadTiles.emplace_back(std::move(tile));
}

void RhiRenderer::renderTile(atlas::RenderTile tile)
{
    RenderBatch& batch = _scheduledExecutions.renderBatch;

    // atlas texture Vertices to locate the tile
    auto const x = static_cast<float>(tile.x.value);
    auto const y = static_cast<float>(tile.y.value);
    auto const z = ZAxisDepths::Text;

    // tile bitmap size on target render surface
    float const r = unbox<float>(firstNonZero(tile.targetSize.width, tile.bitmapSize.width));
    float const s = unbox<float>(firstNonZero(tile.targetSize.height, tile.bitmapSize.height));

    // normalized TexCoords
    float const nx = tile.normalizedLocation.x;
    float const ny = tile.normalizedLocation.y;
    float const nw = tile.normalizedLocation.width;
    float const nh = tile.normalizedLocation.height;

    // These two are currently not used.
    // This used to be used for the z-plane into the 3D texture,
    // but I've reverted back to a 2D texture atlas for now.
    float const i = 0;

    // Tile dependant userdata.
    // This is current the fragment shader's selector that
    // determines how to operate on this tile (images vs gray-scale anti-aliased
    // glyphs vs LCD subpixel antialiased glyphs)
    auto const u = static_cast<float>(tile.fragmentShaderSelector);

    // color
    float const cr = tile.color[0];
    float const cg = tile.color[1];
    float const cb = tile.color[2];
    float const ca = tile.color[3];

    // clang-format off
    float const vertices[6 * 11] = {
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
void RhiRenderer::execute(std::chrono::steady_clock::time_point now)
{
    // One *staging* step of the frame, driven from the render node's prepare() (via the terminal render →
    // execute() call chain). vtrasterizer's Renderer may call execute() several times per frame (smooth
    // scroll: main content under a scissor, status line, cursor under a scissor), so this method does NOT
    // draw and does NOT submit on its own. It appends this step's geometry to the per-frame accumulators
    // (with the active inner scissor) and queues its atlas uploads into the per-frame resource batch.
    // flushFrame() uploads everything once at the end of prepare(); recordDraws() replays the draws.
    (void) now;

    if (!_initialized)
        return;

    // Without the per-frame handles or built pipelines we cannot stage anything; drop this step's geometry
    // so the queues do not grow.
    if (_rhi == nullptr || _commandBuffer == nullptr || _frameRenderTarget == nullptr || !pipelinesReady())
    {
        _rectBuffer.clear();
        _scheduledExecutions.clear();
        return;
    }

    // Lazily allocate this frame's single resource-update batch on the first execute() of the frame.
    if (_frameUpdates == nullptr)
        _frameUpdates = _rhi->nextResourceUpdateBatch();

    // (Re)configure / upload the glyph atlas before any text pass references it.
    if (_scheduledExecutions.configureAtlas)
        executeConfigureAtlas(*_scheduledExecutions.configureAtlas);

    for (auto const& tile: _scheduledExecutions.uploadTiles)
        executeUploadTile(*_frameUpdates, tile);

    // Append this step's vertex geometry + draw items (with the active inner scissor).
    recordRectPass();
    recordTextPass();

    // Service a pending screenshot request (currently a sized-but-empty buffer, see takeScreenshot()).
    if (_pendingScreenshotCallback)
    {
        auto result = takeScreenshot();
        _pendingScreenshotCallback.value()(result.second, result.first);
        _pendingScreenshotCallback.reset();
    }

    _rectBuffer.clear();
    _scheduledExecutions.clear();
}

void RhiRenderer::recordRectPass()
{
    if (_rectBuffer.empty())
        return;

    auto const firstVertex = static_cast<quint32>(_frameRectVertices.size() / rhilayout::RectVertexFloats);
    // Bulk append (single memcpy) rather than element-wise back_inserter — this runs on the per-frame path.
    _frameRectVertices.reserve(_frameRectVertices.size() + _rectBuffer.size());
    _frameRectVertices.insert(_frameRectVertices.end(), _rectBuffer.begin(), _rectBuffer.end());
    _frameDrawItems.push_back(FrameDrawItem {
        .pass = FramePass::Rect,
        .firstVertex = firstVertex,
        .vertexCount = static_cast<quint32>(_rectBuffer.size() / rhilayout::RectVertexFloats),
        .scissor = _innerScissor,
    });
}

void RhiRenderer::recordTextPass()
{
    RenderBatch const& batch = _scheduledExecutions.renderBatch;
    if (batch.renderTiles.empty())
        return;

    auto const firstVertex = static_cast<quint32>(_frameTextVertices.size() / rhilayout::TextVertexFloats);
    // Bulk append (single memcpy) rather than element-wise back_inserter — this runs on the per-frame path.
    _frameTextVertices.reserve(_frameTextVertices.size() + batch.buffer.size());
    _frameTextVertices.insert(_frameTextVertices.end(), batch.buffer.begin(), batch.buffer.end());
    _frameDrawItems.push_back(FrameDrawItem {
        .pass = FramePass::Text,
        .firstVertex = firstVertex,
        // Six vertices (two triangles) per glyph tile.
        .vertexCount = static_cast<quint32>(batch.renderTiles.size() * rhilayout::VerticesPerTile),
        .scissor = _innerScissor,
    });
}

void RhiRenderer::flushFrame()
{
    // End of the staging phase (the node's prepare()): upload the frame's accumulated vertex buffers and the
    // shared uniform blocks, schedule any deferred atlas readback, then queue the whole resource batch onto
    // the command buffer so it is processed before the render pass begins.
    if (_rhi == nullptr || _commandBuffer == nullptr || !pipelinesReady())
        return;

    if (_frameUpdates == nullptr)
        _frameUpdates = _rhi->nextResourceUpdateBatch();

    auto const mvp = _projectionMatrix * _modelMatrix;
    auto const timeValue = uptime(std::chrono::steady_clock::now());

    // Rect pass: upload the interleaved vertices (growing the Immutable buffer when needed) and the uniform
    // block (mat4 u_transform @0, float u_time @64).
    if (!_frameRectVertices.empty())
    {
        auto const bytes = static_cast<quint32>(_frameRectVertices.size() * sizeof(float));
        if (!_rectPipeline.vertexBuffer || _rectPipeline.vertexBuffer->size() < bytes)
        {
            _rectPipeline.vertexBuffer.reset(
                _rhi->newBuffer(QRhiBuffer::Immutable, QRhiBuffer::VertexBuffer, bytes));
            _rectPipeline.vertexBuffer->create();
        }
        _frameUpdates->uploadStaticBuffer(
            _rectPipeline.vertexBuffer.get(), 0, bytes, _frameRectVertices.data());
        _frameUpdates->updateDynamicBuffer(_rectPipeline.uniformBuffer.get(),
                                           rhilayout::RectUniformTransformOffset,
                                           rhilayout::Mat4Size,
                                           mvp.constData());
        _frameUpdates->updateDynamicBuffer(
            _rectPipeline.uniformBuffer.get(), rhilayout::RectUniformTimeOffset, sizeof(float), &timeValue);
    }

    // Text pass: upload the interleaved vertices and the uniform block (mat4 u_transform @0, float u_time
    // @64, float pixel_x @68, vec4 u_textOutlineColor @80).
    if (!_frameTextVertices.empty())
    {
        auto const bytes = static_cast<quint32>(_frameTextVertices.size() * sizeof(float));
        if (!_textPipeline.vertexBuffer || _textPipeline.vertexBuffer->size() < bytes)
        {
            _textPipeline.vertexBuffer.reset(
                _rhi->newBuffer(QRhiBuffer::Immutable, QRhiBuffer::VertexBuffer, bytes));
            _textPipeline.vertexBuffer->create();
        }
        _frameUpdates->uploadStaticBuffer(
            _textPipeline.vertexBuffer.get(), 0, bytes, _frameTextVertices.data());
        _frameUpdates->updateDynamicBuffer(_textPipeline.uniformBuffer.get(),
                                           rhilayout::TextUniformTransformOffset,
                                           rhilayout::Mat4Size,
                                           mvp.constData());
        _frameUpdates->updateDynamicBuffer(
            _textPipeline.uniformBuffer.get(), rhilayout::TextUniformTimeOffset, sizeof(float), &timeValue);

        auto const pixelX =
            _atlasTextureSize.width.value != 0 ? 1.0f / unbox<float>(_atlasTextureSize.width) : 0.0f;
        _frameUpdates->updateDynamicBuffer(
            _textPipeline.uniformBuffer.get(), rhilayout::TextUniformPixelXOffset, sizeof(float), &pixelX);

        auto const [outR, outG, outB, outA] = atlas::normalize(_textOutlineColor);
        float const outlineColor[4] = { outR, outG, outB, outA };
        _frameUpdates->updateDynamicBuffer(_textPipeline.uniformBuffer.get(),
                                           rhilayout::TextUniformOutlineColorOffset,
                                           sizeof(outlineColor),
                                           outlineColor);
    }

    // Deferred atlas readback (debug inspector): schedule the capture so it completes after this frame.
    if (_atlasReadbackRequested && _atlasTexture)
    {
        _frameUpdates->readBackTexture(QRhiReadbackDescription(_atlasTexture.get()), &_atlasReadbackResult);
        _atlasReadbackRequested = false;
    }

    _commandBuffer->resourceUpdate(_frameUpdates);
    _frameUpdates = nullptr;
}

void RhiRenderer::recordDraws()
{
    // The *draw* half of the frame, driven from the render node's render() (inside the active render pass).
    // The prepare() phase staged the geometry/atlas and flushFrame() uploaded it; here we replay the queued
    // draw items, each binding its pass's pipeline + per-frame vertex buffer and its captured scissor.
    if (_commandBuffer == nullptr || _frameRenderTarget == nullptr || !pipelinesReady())
        return;

    auto* cb = _commandBuffer;
    auto const pixelSize = _frameRenderTarget->pixelSize();
    auto const viewport = QRhiViewport(
        0.0f, 0.0f, static_cast<float>(pixelSize.width()), static_cast<float>(pixelSize.height()));

    for (auto const& item: _frameDrawItems)
    {
        if (item.vertexCount == 0)
            continue;

        auto const& pipeline = item.pass == FramePass::Rect ? _rectPipeline : _textPipeline;
        auto const vertexStride = item.pass == FramePass::Rect ? RectVertexStride : TextVertexStride;

        cb->setGraphicsPipeline(pipeline.pipeline.get());
        cb->setViewport(viewport);
        applyScissor(pipeline.pipeline.get(), item.scissor);
        cb->setShaderResources(pipeline.srb.get());
        QRhiCommandBuffer::VertexInput const vertexBinding(pipeline.vertexBuffer.get(),
                                                           item.firstVertex * vertexStride);
        cb->setVertexInput(0, 1, &vertexBinding);
        cb->draw(item.vertexCount);
    }

    // The per-frame handles + accumulators are valid only for the duration of this frame.
    _commandBuffer = nullptr;
    _frameRenderTarget = nullptr;
    _frameDrawItems.clear();
    _frameRectVertices.clear();
    _frameTextVertices.clear();
}

void RhiRenderer::applyScissor(QRhiGraphicsPipeline* pipeline, std::optional<ScissorRect> const& innerScissor)
{
    if ((pipeline->flags() & QRhiGraphicsPipeline::UsesScissor) == 0)
        return;

    // The effective clip is the node clip (Qt's RenderState::scissorRect for this node, set just before
    // recordDraws()) intersected with the pass's transient inner scissor (smooth scroll / cursor clip), so
    // nesting can only shrink the clipped region. The pure nesting policy lives in computeEffectiveClip()
    // (RhiVertexLayout.h) so it can be unit-tested without a command buffer.
    auto const clip = computeEffectiveClip(innerScissor, _nodeScissor);

    // A pipeline that declares UsesScissor must be given an explicit scissor every draw: the RHI otherwise
    // keeps the *last* scissor set in this render pass, which — since the scene graph clips earlier nodes
    // (e.g. the tab strip's ListView with clip:true sets a small scissor) — would confine the terminal to a
    // stale sub-rectangle. So when no clip applies, scissor to the full render target (the whole viewport).
    if (!clip.has_value())
    {
        auto const size = _frameRenderTarget->pixelSize();
        _commandBuffer->setScissor(QRhiScissor(0, 0, size.width(), size.height()));
        return;
    }

    // QRhiScissor uses bottom-left-origin pixels, matching ScissorRect (and the scene graph's scissorRect()
    // that fed _nodeScissor), so the rectangle maps across directly. A zero-area clip clips everything away.
    auto const& r = *clip;
    _commandBuffer->setScissor(QRhiScissor(r.x, r.y, std::max(0, r.width), std::max(0, r.height)));
}

void RhiRenderer::executeConfigureAtlas(atlas::ConfigureAtlas const& param)
{
    Require(isPowerOfTwo(unbox(param.size.width)));
    Require(isPowerOfTwo(unbox(param.size.height)));
    Require(param.properties.format == atlas::Format::RGBA);

    // (Re)create the atlas texture only when its GPU extent actually changes — a ConfigureAtlas may be
    // scheduled repeatedly at the same size (e.g. on the initial resize burst), and recreating a multi-MB
    // texture every time is wasteful. createPipelines() may also have stood up a 1x1 placeholder texture
    // before the real size was known, which this first real configure replaces. When recreated, refresh the
    // text pipeline's shader-resource bindings so they reference the new texture object (the sampler and
    // uniform buffer are reused, and the binding layout is unchanged, so the pipeline keeps using the same
    // QRhiShaderResourceBindings object).
    if (!_atlasTexture || _atlasCreatedSize != param.size)
    {
        createAtlasTexture(_rhi, param.size);
        if (_textPipeline.srb)
        {
            _textPipeline.srb->setBindings({
                QRhiShaderResourceBinding::uniformBuffer(0,
                                                         QRhiShaderResourceBinding::VertexStage
                                                             | QRhiShaderResourceBinding::FragmentStage,
                                                         _textPipeline.uniformBuffer.get()),
                QRhiShaderResourceBinding::sampledTexture(
                    1, QRhiShaderResourceBinding::FragmentStage, _atlasTexture.get(), _atlasSampler.get()),
            });
            _textPipeline.srb->create();
        }
    }

    // No full-texture clear: the rasterizer only ever samples atlas tiles it has uploaded, so untouched
    // regions are never read, and a multi-MB full-level clear upload (besides being pure overhead) is what
    // the OpenGL RHI backend rejected as an "invalid texture upload" on a freshly created large texture.
}

void RhiRenderer::executeUploadTile(QRhiResourceUpdateBatch& updates, atlas::UploadTile const& param)
{
    // {{{ Force RGBA: the atlas texture is RGBA8, but tiles may arrive as Red (alpha-mask glyphs) or RGB.
    auto const tileWidth = unbox<int>(param.bitmapSize.width);
    auto const tileHeight = unbox<int>(param.bitmapSize.height);

    // A zero-area tile (e.g. the blank glyph of a space) carries no pixels; uploading it would be rejected by
    // the RHI as an invalid (empty) texture upload, and there is nothing to sample anyway. Skip it.
    if (tileWidth <= 0 || tileHeight <= 0 || param.bitmap.empty())
        return;

    QByteArray rgba;
    switch (param.bitmapFormat)
    {
        case atlas::Format::Red: {
            rgba.resize(static_cast<qsizetype>(param.bitmapSize.area()) * 4);
            auto* t = reinterpret_cast<uint8_t*>(rgba.data());
            for (auto const c: param.bitmap)
            {
                *t++ = c;    // red
                *t++ = 0x00; // green
                *t++ = 0x00; // blue
                *t++ = 0xFF; // alpha
            }
            break;
        }
        case atlas::Format::RGB: {
            rgba.resize(static_cast<qsizetype>(param.bitmapSize.area()) * 4);
            auto* t = reinterpret_cast<uint8_t*>(rgba.data());
            auto const* s = param.bitmap.data();
            auto const* const e = param.bitmap.data() + param.bitmap.size();
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
            rgba = QByteArray(reinterpret_cast<char const*>(param.bitmap.data()),
                              static_cast<qsizetype>(param.bitmap.size()));
            break;
    }
    // }}}

    // Sub-image upload at the tile's atlas location. Row alignment is 1 byte (RGBA, tightly packed), which
    // QRhi's tightly-packed byte-data path assumes; this mirrors the former
    // glPixelStorei(UNPACK_ALIGNMENT,1).
    QRhiTextureSubresourceUploadDescription desc(rgba.constData(), static_cast<quint32>(rgba.size()));
    desc.setSourceSize(QSize(tileWidth, tileHeight));
    desc.setDestinationTopLeft(QPoint(param.location.x.value, param.location.y.value));
    updates.uploadTexture(_atlasTexture.get(),
                          QRhiTextureUploadDescription(QRhiTextureUploadEntry(0, 0, desc)));
}

void RhiRenderer::renderRectangle(int ix, int iy, Width width, Height height, RGBAColor color)
{
    auto const x = static_cast<float>(ix);
    auto const y = static_cast<float>(iy);
    auto const z = ZAxisDepths::BackgroundSGR;
    auto const r = unbox<float>(width);
    auto const s = unbox<float>(height);
    auto const [cr, cg, cb, ca] = atlas::normalize(color);

    // clang-format off
    float const vertices[6 * 7] = {
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

void RhiRenderer::setScissorRect(int x, int y, int width, int height)
{
    // A transient inner scissor (e.g. smooth scroll / cursor clip) staged by vtrasterizer's Renderer during
    // the staging phase (the node's prepare()). It is stored raw here; recordDraws() intersects it with the
    // node clip at draw time, because the node clip (Qt's RenderState) is only known once the pass records.
    _innerScissor = ScissorRect { .x = x, .y = y, .width = width, .height = height };
}

void RhiRenderer::clearScissorRect()
{
    // No transient inner scissor for subsequent geometry: draws fall back to the bare node clip (applied in
    // recordDraws()). std::nullopt means "no inner scissor", not "no clip".
    _innerScissor = std::nullopt;
}

void RhiRenderer::setNodeScissorRect(std::optional<ScissorRect> const& rect)
{
    _nodeScissor = rect;
}

void RhiRenderer::clearNodeScissorRect() noexcept
{
    // Only forget the stored node clip; do NOT touch GPU state. The scene graph re-establishes its own
    // scissor after the node renders, so clearing the staged rectangles is enough to prevent a later,
    // non-render code path from intersecting against a stale rectangle.
    _nodeScissor.reset();
    _innerScissor.reset();
}

optional<vtrasterizer::AtlasTextureScreenshot> RhiRenderer::readAtlas()
{
    // Atlas readback is deferred: a readBackTexture() scheduled into a frame's resource batch only completes
    // after the command buffer for that frame is submitted (past the end of the render pass). We therefore
    // request a capture on the next execute() and hand back whatever the most recently completed capture
    // produced. The first call (before any frame has captured) returns a correctly-sized zero buffer.
    _atlasReadbackRequested = true;

    auto output = vtrasterizer::AtlasTextureScreenshot {};
    output.atlasInstanceId = 0;
    output.size = _atlasTextureSize;
    output.format = _atlasProperties.format;

    auto const expectedBytes =
        static_cast<size_t>(_atlasTextureSize.area()) * element_count(_atlasProperties.format);

    if (!_atlasReadbackResult.data.isEmpty()
        && _atlasReadbackResult.pixelSize
               == QSize(unbox<int>(_atlasTextureSize.width), unbox<int>(_atlasTextureSize.height)))
    {
        auto const* bytes = reinterpret_cast<uint8_t const*>(_atlasReadbackResult.data.constData());
        output.buffer.assign(bytes,
                             bytes + std::min<size_t>(expectedBytes, _atlasReadbackResult.data.size()));
        output.buffer.resize(expectedBytes);
    }
    else
        output.buffer.resize(expectedBytes);

    return { std::move(output) };
}

void RhiRenderer::scheduleScreenshot(ScreenshotCallback callback)
{
    _pendingScreenshotCallback = std::move(callback);
}

pair<ImageSize, vector<uint8_t>> RhiRenderer::takeScreenshot()
{
    // TODO(rhi): full framebuffer readback. A render-target readback only completes after the pass ends, so
    // (like readAtlas) it would need a deferred/one-frame-latency capture; the live render target's color
    // attachment is also not guaranteed readback-capable on every backend. Screenshots are not on the
    // black-screen path, so for now we return a correctly-sized empty buffer to keep the callback contract.
    ImageSize const imageSize = _renderTargetSize;

    vector<uint8_t> buffer;
    buffer.resize(imageSize.area() * 4 /* 4 because RGBA */);

    displayLog()("Capture screenshot ({}).", imageSize);

    return { imageSize, buffer };
}
// }}}

void RhiRenderer::setTextOutline(float /*thickness*/, vtbackend::RGBAColor color)
{
    // The outline color is consumed by the text pipeline's uniform block at draw time (Phase 3 writes it
    // into the dynamic uniform buffer). Just store it here.
    _textOutlineColor = color;
}

void RhiRenderer::inspect(std::ostream& /*output*/) const
{
}

} // namespace contour::display
