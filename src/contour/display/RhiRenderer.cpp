// SPDX-License-Identifier: Apache-2.0
#include <contour/display/RhiRenderer.h>
#include <contour/display/RhiVertexLayout.h>
#include <contour/display/ScreenshotReadback.h>
#include <contour/display/ShaderConfig.h>
#include <contour/helper.h>

#include <vtbackend/primitives.h>

#include <vtrasterizer/TextureAtlas.h>

#include <crispy/algorithm.h>
#include <crispy/assert.h>
#include <crispy/defines.h>
#include <crispy/utils.h>

#include <QtCore/QFile>
#include <QtCore/QVarLengthArray>
#include <QtCore/QtGlobal>
#include <QtGui/QGuiApplication>
#include <QtGui/QImage>

#include <array>
#include <bit>
#include <chrono>
#include <memory>
#include <optional>
#include <span>
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

    // {{{ Data-driven render-pass table
    //
    // The background/rect and text/glyph graphics pipelines are identical except for a handful of axes:
    // which baked shaders they load, the size of their std140 uniform block, their interleaved vertex
    // stride + attribute layout, and whether they sample the glyph atlas (binding 1). Everything else
    // (Triangles topology, depth-test-no-write LessOrEqual, the SrcAlpha/OneMinusSrcAlpha blend, the
    // UsesScissor flag, the binding-0 uniform buffer) is shared. Describing the variation as data and
    // interpreting it in a single createPipeline() loop means adding a third pass tomorrow is one more
    // row here, not another near-duplicate builder function.

    // QRhiVertexInputAttribute is not a literal type, so these attribute tables are file-scope const
    // (initialized once at load time) rather than constexpr; they back the PassDescriptor::attributes span.

    /// The interleaved vertex-input attributes for the background/rect pass: vec3 position @loc0,
    /// vec4 color @loc1 (single binding 0).
    const std::array rectVertexAttributes {
        QRhiVertexInputAttribute(0, 0, QRhiVertexInputAttribute::Float3, RectPositionOffset),
        QRhiVertexInputAttribute(0, 1, QRhiVertexInputAttribute::Float4, RectColorOffset),
    };

    /// The interleaved vertex-input attributes for the text/glyph pass: vec3 position @loc0,
    /// vec4 texCoords @loc1, vec4 color @loc2 (single binding 0).
    const std::array textVertexAttributes {
        QRhiVertexInputAttribute(0, 0, QRhiVertexInputAttribute::Float3, TextPositionOffset),
        QRhiVertexInputAttribute(0, 1, QRhiVertexInputAttribute::Float4, TextTexCoordOffset),
        QRhiVertexInputAttribute(0, 2, QRhiVertexInputAttribute::Float4, TextColorOffset),
    };
    // The PassDescriptor struct itself is declared as a private member of RhiRenderer (RhiRenderer.h). The
    // concrete two-row pass table lives in RhiRenderer::passDescriptor() (defined below), so both
    // createPipelines() (swapchain) and createScreenshotPipeline() (offscreen) interpret the same data.
    // }}}

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
                         [[maybe_unused]] vtbackend::ImageSize textureTileSize):
    _startTime { chrono::steady_clock::now().time_since_epoch() }
{
    // Log the requested argument, not _renderTargetSize: setRenderSize() below is what assigns the member,
    // so reading it here would always report the default-constructed 0x0.
    displayLog()("RhiRenderer: Constructing with render size {}.", targetSurfaceSize);
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
    // Deliberate no-op: content placement is baked into the vertices by GridMetrics::map() (the
    // vtrasterizer::Renderer offsets every cell by the page margin before they reach this render
    // target), so the margin needs no render-target state. The override exists only because
    // RenderTarget declares it pure virtual.
    (void) margin;
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
    // and the frame's render-pass descriptor are available (texture uploads set their own row alignment via
    // QRhiTextureSubresourceUploadDescription). So there is nothing to do here but flip the initialized flag.
    if (_initialized)
        return;

    _initialized = true;
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

QVarLengthArray<QRhiShaderResourceBinding, 2> RhiRenderer::atlasSrbBindings(QRhiBuffer* uniformBuffer,
                                                                            bool hasSampler) const
{
    // Binding 0 (the uniform block) is shared by every pass; binding 1 (the atlas sampler) is added only for
    // sampling passes.
    QVarLengthArray<QRhiShaderResourceBinding, 2> bindings;
    bindings.append(QRhiShaderResourceBinding::uniformBuffer(
        0, QRhiShaderResourceBinding::VertexStage | QRhiShaderResourceBinding::FragmentStage, uniformBuffer));
    if (hasSampler)
        bindings.append(QRhiShaderResourceBinding::sampledTexture(
            1, QRhiShaderResourceBinding::FragmentStage, _atlasTexture.get(), _atlasSampler.get()));
    return bindings;
}

void RhiRenderer::rebindAtlasTexture(RhiPipeline& pipeline, QRhiBuffer* uniformBuffer)
{
    if (!pipeline.srb)
        return;
    // The binding layout is unchanged; only the atlas texture object differs after a recreate. Rebuild the
    // QRhiShaderResourceBindings in place (same layout as createPipeline via atlasSrbBindings) so the
    // pipeline keeps its srb object while binding 1 points at the new _atlasTexture. Every atlas rebind is a
    // sampling pass, so hasSampler is always true here.
    auto const bindings = atlasSrbBindings(uniformBuffer, /*hasSampler*/ true);
    pipeline.srb->setBindings(bindings.cbegin(), bindings.cend());
    pipeline.srb->create();
}

RhiRenderer::PassDescriptor RhiRenderer::passDescriptor(bool isText)
{
    if (isText)
        return PassDescriptor { .vertexShaderPath = TextVertexShaderPath,
                                .fragmentShaderPath = TextFragmentShaderPath,
                                .uniformBlockSize = TextUniformBlockSize,
                                .vertexStride = TextVertexStride,
                                .attributes = textVertexAttributes,
                                .hasSampler = true,
                                .debugName = "text/glyph" };
    return PassDescriptor { .vertexShaderPath = BackgroundVertexShaderPath,
                            .fragmentShaderPath = BackgroundFragmentShaderPath,
                            .uniformBlockSize = RectUniformBlockSize,
                            .vertexStride = RectVertexStride,
                            .attributes = rectVertexAttributes,
                            .hasSampler = false,
                            .debugName = "background/rect" };
}

void RhiRenderer::createPipeline(QRhi* rhi,
                                 QRhiRenderPassDescriptor* rpDesc,
                                 PassDescriptor const& desc,
                                 RhiPipeline& out,
                                 QRhiBuffer* sharedUniformBuffer)
{
    auto const vertexShader = loadShader(desc.vertexShaderPath);
    auto const fragmentShader = loadShader(desc.fragmentShaderPath);
    if (!vertexShader.isValid() || !fragmentShader.isValid())
        return;

    // A pass that samples the glyph atlas needs the atlas texture + sampler to already exist (they are
    // referenced by binding 1); createPipelines() guarantees this ordering before invoking us.
    if (desc.hasSampler)
    {
        Require(_atlasTexture != nullptr);
        Require(_atlasSampler != nullptr);
    }

    // Dynamic uniform buffer feeding both stages (std140 `Buf` block at binding 0). The vertex buffer is
    // created lazily (as an Immutable buffer) in the pass's record step once the per-frame geometry size is
    // known, and re-created when it must grow. When a shared uniform buffer is supplied (the screenshot
    // pipelines), bind it instead of creating one, so the swapchain set's per-frame uniform writes drive
    // this pipeline too.
    QRhiBuffer* uniformBuffer = sharedUniformBuffer;
    if (uniformBuffer == nullptr)
    {
        out.uniformBuffer.reset(
            rhi->newBuffer(QRhiBuffer::Dynamic, QRhiBuffer::UniformBuffer, desc.uniformBlockSize));
        out.uniformBuffer->create();
        uniformBuffer = out.uniformBuffer.get();
    }

    auto const bindings = atlasSrbBindings(uniformBuffer, desc.hasSampler);
    out.srb.reset(rhi->newShaderResourceBindings());
    out.srb->setBindings(bindings.cbegin(), bindings.cend());
    out.srb->create();

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

    // Vertex input: a single interleaved binding at the pass's stride, with the pass's attribute list.
    QRhiVertexInputLayout inputLayout;
    inputLayout.setBindings({ QRhiVertexInputBinding(desc.vertexStride) });
    inputLayout.setAttributes(desc.attributes.begin(), desc.attributes.end());
    pipeline->setVertexInputLayout(inputLayout);

    pipeline->setShaderResourceBindings(out.srb.get());
    pipeline->setRenderPassDescriptor(rpDesc);
    if (!pipeline->create())
        errorLog()("Failed to create RHI {} graphics pipeline.", desc.debugName);

    out.pipeline.reset(pipeline);
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

    // Ensure the atlas texture + sampler exist before the text pipeline references them (its descriptor sets
    // hasSampler). If configureAtlas has not been observed yet, fall back to the currently known atlas size
    // (or a 1x1 placeholder) so the shader-resource-bindings are valid; the real atlas is (re)created on the
    // next configureAtlas.
    if (!_atlasTexture)
    {
        auto const size =
            _atlasTextureSize.area() != 0 ? _atlasTextureSize : ImageSize { Width(1), Height(1) };
        createAtlasTexture(rhi, size);
    }

    // The render passes, described as data (passDescriptor()), each paired with the pipeline slot it
    // populates. Adding a pass is one more row here plus its FramePass tag and record step — no new
    // near-duplicate builder function.
    struct PassEntry
    {
        PassDescriptor descriptor;
        RhiPipeline* slot = nullptr;
    };
    std::array const passes {
        PassEntry { .descriptor = passDescriptor(/*isText*/ false), .slot = &_rectPipeline },
        PassEntry { .descriptor = passDescriptor(/*isText*/ true), .slot = &_textPipeline },
    };
    for (auto const& entry: passes)
        createPipeline(rhi, rpDesc, entry.descriptor, *entry.slot);

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

void RhiRenderer::createImageTexture(atlas::CreateImageTexture param)
{
    // Deferred like every other GPU-touching command: this is called while the frame is being
    // scheduled, which may be before a QRhi exists at all.
    _scheduledExecutions.imageCreates.emplace_back(std::move(param));
}

void RhiRenderer::destroyImageTexture(atlas::DestroyImageTexture param)
{
    _scheduledExecutions.imageDestroys.emplace_back(param);
}

vtrasterizer::atlas::ImageTextureBackend& RhiRenderer::imageScheduler()
{
    return *this;
}

void RhiRenderer::renderImageQuad(atlas::RenderImageQuad param)
{
    // Same quad the atlas path builds, against a whole-image texture instead of a tile. Quads are
    // appended to the current run while they sample the same texture, so a full-screen image costs
    // one draw item rather than one per cell.
    auto& batches =
        param.aboveText ? _scheduledExecutions.imageQuadsAboveText : _scheduledExecutions.imageQuadsBelowText;
    if (batches.empty() || batches.back().texture != param.texture)
        batches.emplace_back(ImageQuadBatch { .texture = param.texture, .buffer = {} });
    auto& buffer = batches.back().buffer;

    auto const x = static_cast<float>(param.x);
    auto const y = static_cast<float>(param.y);
    auto const z = ZAxisDepths::Text;
    auto const r = unbox<float>(param.targetSize.width);
    auto const s = unbox<float>(param.targetSize.height);

    auto const nx = param.source.x;
    auto const ny = param.source.y;
    auto const nw = param.source.width;
    auto const nh = param.source.height;

    float const i = 0;
    auto const u = static_cast<float>(FRAGMENT_SELECTOR_IMAGE_BGRA);

    float const cr = param.color[0];
    float const cg = param.color[1];
    float const cb = param.color[2];
    float const ca = param.color[3];

    // clang-format off
    float const vertices[6 * 11] = {
    // <X      Y      Z> <X        Y        I  U>  <R   G   B   A>
        x,     y + s, z,  nx,      ny + nh, i, u,  cr, cg, cb, ca, // left top
        x,     y,     z,  nx,      ny,      i, u,  cr, cg, cb, ca, // left bottom
        x + r, y,     z,  nx + nw, ny,      i, u,  cr, cg, cb, ca, // right bottom

        x,     y + s, z,  nx,      ny + nh, i, u,  cr, cg, cb, ca, // left top
        x + r, y,     z,  nx + nw, ny,      i, u,  cr, cg, cb, ca, // right bottom
        x + r, y + s, z,  nx + nw, ny + nh, i, u,  cr, cg, cb, ca, // right top
    };
    // clang-format on

    crispy::copy(vertices, back_inserter(buffer));
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

    for (auto& create: _scheduledExecutions.imageCreates)
        executeCreateImageTexture(*_frameUpdates, create);
    for (auto const& destroy: _scheduledExecutions.imageDestroys)
        _imageTextures.erase(destroy.id.value);

    // Append this step's vertex geometry + draw items (with the active inner scissor). The order
    // here IS the z-order: every vertex sits at the same depth, so only draw order composites.
    recordRectPass();
    recordImagePass(_scheduledExecutions.imageQuadsBelowText);
    recordTextPass();
    recordImagePass(_scheduledExecutions.imageQuadsAboveText);

    // A pending screenshot is serviced in the render phase (recordScreenshotPass), where the frame's draw
    // items are replayed into an owned offscreen texture and read back deferred — not here in the staging
    // phase, which has no color pixels to capture yet.

    _rectBuffer.clear();
    _scheduledExecutions.clear();
}

void RhiRenderer::uploadRectUniforms(QRhiResourceUpdateBatch& updates,
                                     QRhiBuffer& uniformBuffer,
                                     QMatrix4x4 const& mvp,
                                     float timeValue)
{
    updates.updateDynamicBuffer(
        &uniformBuffer, rhilayout::RectUniformTransformOffset, rhilayout::Mat4Size, mvp.constData());
    updates.updateDynamicBuffer(&uniformBuffer, rhilayout::RectUniformTimeOffset, sizeof(float), &timeValue);
}

void RhiRenderer::uploadTextUniforms(QRhiResourceUpdateBatch& updates,
                                     QRhiBuffer& uniformBuffer,
                                     QMatrix4x4 const& mvp,
                                     float timeValue)
{
    updates.updateDynamicBuffer(
        &uniformBuffer, rhilayout::TextUniformTransformOffset, rhilayout::Mat4Size, mvp.constData());
    updates.updateDynamicBuffer(&uniformBuffer, rhilayout::TextUniformTimeOffset, sizeof(float), &timeValue);

    auto const pixelX =
        _atlasTextureSize.width.value != 0 ? 1.0f / unbox<float>(_atlasTextureSize.width) : 0.0f;
    updates.updateDynamicBuffer(&uniformBuffer, rhilayout::TextUniformPixelXOffset, sizeof(float), &pixelX);

    auto const [outR, outG, outB, outA] = atlas::normalize(_textOutlineColor);
    float const outlineColor[4] = { outR, outG, outB, outA };
    updates.updateDynamicBuffer(
        &uniformBuffer, rhilayout::TextUniformOutlineColorOffset, sizeof(outlineColor), outlineColor);
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

void RhiRenderer::executeCreateImageTexture(QRhiResourceUpdateBatch& updates,
                                            atlas::CreateImageTexture& param)
{
    if (_rhi == nullptr)
        return;

    auto const pixelSize = QSize(unbox<int>(param.size.width), unbox<int>(param.size.height));
    auto resources =
        ImageTextureResources { .texture = {}, .srb = {}, .screenshotSrb = {}, .size = param.size };

    resources.texture.reset(_rhi->newTexture(QRhiTexture::RGBA8, pixelSize, 1, {}));
    if (!resources.texture->create())
    {
        errorLog()("Failed to create RHI image texture of size {}.", param.size);
        return;
    }

    auto subresource =
        QRhiTextureSubresourceUploadDescription(param.data.data(), static_cast<int>(param.data.size()));
    subresource.setDataStride(unbox<quint32>(param.size.width) * 4);
    updates.uploadTexture(resources.texture.get(), QRhiTextureUploadDescription({ 0, 0, subresource }));

    // Binding 1 names this image instead of the atlas; binding 0 must name the uniform buffer of
    // whichever pipeline ends up drawing, hence one set per pipeline.
    auto const makeSrb = [&](QRhiBuffer* uniformBuffer) -> QRhiShaderResourceBindings* {
        auto* srb = _rhi->newShaderResourceBindings();
        srb->setBindings(
            { QRhiShaderResourceBinding::uniformBuffer(0,
                                                       QRhiShaderResourceBinding::VertexStage
                                                           | QRhiShaderResourceBinding::FragmentStage,
                                                       uniformBuffer),
              QRhiShaderResourceBinding::sampledTexture(1,
                                                        QRhiShaderResourceBinding::FragmentStage,
                                                        resources.texture.get(),
                                                        _atlasSampler.get()) });
        if (!srb->create())
        {
            errorLog()("Failed to create RHI image shader resource bindings.");
            delete srb;
            return nullptr;
        }
        return srb;
    };

    // The atlas sampler is Nearest + ClampToEdge, which is exactly what the CPU resampler this
    // replaces did (it truncates the source coordinate). Anything else would move pixels.
    if (_textPipeline.uniformBuffer)
        resources.srb.reset(makeSrb(_textPipeline.uniformBuffer.get()));
    if (_screenshotTextPipeline.uniformBuffer)
        resources.screenshotSrb.reset(makeSrb(_screenshotTextPipeline.uniformBuffer.get()));

    _imageTextures.insert_or_assign(param.id.value, std::move(resources));
}

void RhiRenderer::recordImagePass(std::vector<ImageQuadBatch> const& batches)
{
    for (auto const& batch: batches)
    {
        if (batch.buffer.empty())
            continue;

        // Images ride the text pass's vertex buffer: identical layout, so the only thing that makes
        // this a separate draw is which texture binding 1 names.
        auto const firstVertex =
            static_cast<quint32>(_frameTextVertices.size() / rhilayout::TextVertexFloats);
        _frameTextVertices.reserve(_frameTextVertices.size() + batch.buffer.size());
        _frameTextVertices.insert(_frameTextVertices.end(), batch.buffer.begin(), batch.buffer.end());
        _frameDrawItems.push_back(FrameDrawItem {
            .pass = FramePass::Image,
            .firstVertex = firstVertex,
            .vertexCount = static_cast<quint32>(batch.buffer.size() / rhilayout::TextVertexFloats),
            .scissor = _innerScissor,
            .imageTexture = batch.texture,
        });
    }
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
        uploadRectUniforms(*_frameUpdates, *_rectPipeline.uniformBuffer, mvp, timeValue);
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
        uploadTextUniforms(*_frameUpdates, *_textPipeline.uniformBuffer, mvp, timeValue);
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

void RhiRenderer::replayDrawItems(QRhiCommandBuffer* cb, QSize targetPixelSize, bool offscreen)
{
    auto const viewport = QRhiViewport(0.0f,
                                       0.0f,
                                       static_cast<float>(targetPixelSize.width()),
                                       static_cast<float>(targetPixelSize.height()));

    // Selects the pipeline to draw with: the offscreen screenshot set (built against the offscreen
    // render-pass descriptor, with its own uniform buffers carrying the item-local transform) or the
    // swapchain set. Both share the swapchain set's vertex buffers (geometry is identical), so
    // `geometry` always refers to the swapchain pipeline that owns them.
    auto const& drawSet = [&](bool isRect) -> RhiPipeline const& {
        if (offscreen)
            return isRect ? _screenshotRectPipeline : _screenshotTextPipeline;
        return isRect ? _rectPipeline : _textPipeline;
    };

    for (auto const& item: _frameDrawItems)
    {
        if (item.vertexCount == 0)
            continue;

        auto const isRect = item.pass == FramePass::Rect;
        auto const& drawPipeline = drawSet(isRect);
        auto const& geometry = isRect ? _rectPipeline : _textPipeline;
        auto const vertexStride = isRect ? RectVertexStride : TextVertexStride;

        // An image draw is a text draw whose binding 1 names one whole image instead of the atlas.
        // Same pipeline, same vertex layout, same buffer — only the resource set differs.
        auto* const shaderResources = [&]() -> QRhiShaderResourceBindings* {
            if (item.pass != FramePass::Image)
                return drawPipeline.srb.get();
            auto const resources = _imageTextures.find(item.imageTexture.value);
            if (resources == _imageTextures.end())
                return nullptr;
            auto const& srb = offscreen ? resources->second.screenshotSrb : resources->second.srb;
            return srb.get();
        }();
        if (shaderResources == nullptr)
            continue; // the texture went away before its quads were drawn

        cb->setGraphicsPipeline(drawPipeline.pipeline.get());
        cb->setViewport(viewport);
        applyScissor(drawPipeline.pipeline.get(), item.scissor, targetPixelSize, offscreen);
        cb->setShaderResources(shaderResources);
        QRhiCommandBuffer::VertexInput const vertexBinding(geometry.vertexBuffer.get(),
                                                           item.firstVertex * vertexStride);
        cb->setVertexInput(0, 1, &vertexBinding);
        cb->draw(item.vertexCount);
    }
}

void RhiRenderer::recordDraws()
{
    // The *draw* half of the frame, driven from the render node's render() (inside the active render pass).
    // The prepare() phase staged the geometry/atlas and flushFrame() uploaded it; here we replay the queued
    // draw items, each binding its pass's pipeline + per-frame vertex buffer and its captured scissor.
    if (_commandBuffer == nullptr || _frameRenderTarget == nullptr || !pipelinesReady())
        return;

    replayDrawItems(_commandBuffer, _frameRenderTarget->pixelSize(), /*offscreen*/ false);

    // The per-frame handles + accumulators are valid only for the duration of this frame.
    _commandBuffer = nullptr;
    _frameRenderTarget = nullptr;
    _frameDrawItems.clear();
    _frameRectVertices.clear();
    _frameTextVertices.clear();
}

void RhiRenderer::applyScissor(QRhiGraphicsPipeline* pipeline,
                               std::optional<ScissorRect> const& innerScissor,
                               QSize targetPixelSize,
                               bool offscreen)
{
    if ((pipeline->flags() & QRhiGraphicsPipeline::UsesScissor) == 0)
        return;

    // The inner scissor (smooth scroll / cursor clip) is ITEM-relative — vtrasterizer computes it against
    // renderSize(), the item's extent — while QRhiScissor addresses the render target. Drawing into the
    // window, translate it by the item's device-pixel offset first (a split pane not anchored at the
    // window's bottom-left would otherwise clip the WRONG pane's region; the full-window single pane only
    // worked because the two frames coincide there), then intersect with the node clip (Qt's
    // RenderState::scissorRect for this node, set just before recordDraws()) so nesting can only shrink
    // the clipped region. Drawing into the item-sized offscreen screenshot target, the item frame IS the
    // target frame and the window-space node clip does not apply. The pure translation/nesting policies
    // live in ScissorRect.h so they can be unit-tested without a command buffer.
    auto const targetInner = [&]() -> std::optional<ScissorRect> {
        if (!innerScissor.has_value() || offscreen)
            return innerScissor;
        return itemScissorToTarget(*innerScissor,
                                   _itemOriginLeftDevice,
                                   _itemOriginTopDevice,
                                   unbox<int>(_renderTargetSize.height),
                                   targetPixelSize.height());
    }();
    auto const clip =
        computeEffectiveClip(targetInner, offscreen ? std::optional<ScissorRect> {} : _nodeScissor);

    // A pipeline that declares UsesScissor must be given an explicit scissor every draw: the RHI otherwise
    // keeps the *last* scissor set in this render pass, which — since the scene graph clips earlier nodes
    // (e.g. the tab strip's ListView with clip:true sets a small scissor) — would confine the terminal to a
    // stale sub-rectangle. So when no clip applies, scissor to the full render target (the whole viewport).
    if (!clip.has_value())
    {
        _commandBuffer->setScissor(QRhiScissor(0, 0, targetPixelSize.width(), targetPixelSize.height()));
        return;
    }

    // QRhiScissor uses bottom-left-origin pixels, matching ScissorRect (and the scene graph's scissorRect()
    // that fed _nodeScissor), so the rectangle maps across directly. A zero-area clip clips everything away.
    auto const& r = *clip;
    _commandBuffer->setScissor(QRhiScissor(r.x, r.y, std::max(0, r.width), std::max(0, r.height)));
}

void RhiRenderer::executeConfigureAtlas(atlas::ConfigureAtlas const& param)
{
    // std::has_single_bit is the standard power-of-two predicate (unbox() yields the unsigned extent it
    // requires); it also rejects 0, which a zero-sized atlas would be, unlike the old hand-rolled check.
    Require(std::has_single_bit(unbox(param.size.width)));
    Require(std::has_single_bit(unbox(param.size.height)));
    Require(param.properties.format == atlas::Format::RGBA);

    // (Re)create the atlas texture only when its GPU extent actually changes — a ConfigureAtlas may be
    // scheduled repeatedly at the same size (e.g. on the initial resize burst), and recreating a multi-MB
    // texture every time is wasteful. createPipelines() may also have stood up a 1x1 placeholder texture
    // before the real size was known, which this first real configure replaces. When recreated, refresh
    // EVERY atlas-sampling pipeline's shader-resource bindings so they reference the new texture object; the
    // sampler and uniform buffers are reused and the binding layout is unchanged, so each pipeline keeps its
    // QRhiShaderResourceBindings object. The offscreen screenshot text pipeline (built once by
    // ensureScreenshotTarget, which has a same-size early-out) samples the same atlas and shares the
    // swapchain text pipeline's uniform buffer, so it must be rebound here too — otherwise a later
    // screenshot samples the freed old atlas texture (use-after-free).
    if (!_atlasTexture || _atlasCreatedSize != param.size)
    {
        createAtlasTexture(_rhi, param.size);
        // Rebind EVERY atlas-sampling pipeline so none is left holding a reference to the freed texture.
        // Each rebinds against its OWN uniform buffer (the screenshot pipeline owns one so its offscreen
        // pass can use an item-local transform). Listed here as the single place that enumerates the
        // atlas-sampling set — add a future one here and both build and rebind stay correct.
        for (RhiPipeline* pipeline: { &_textPipeline, &_screenshotTextPipeline })
            rebindAtlasTexture(*pipeline, pipeline->uniformBuffer.get());
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
    // Store the callback; the next frame's prepare() phase (recordScreenshotPass) renders the terminal into
    // the offscreen texture and schedules a deferred readback, and deliverScreenshot() forwards the captured
    // pixels here once the readback completes (one frame later).
    _pendingScreenshotCallback = std::move(callback);
}

bool RhiRenderer::ensureScreenshotTarget(QRhi* rhi, ImageSize size)
{
    auto const pixelSize = QSize(unbox<int>(size.width), unbox<int>(size.height));
    if (pixelSize.isEmpty())
        return false;

    // Reuse the existing offscreen resources when the capture size is unchanged and the swapchain pipelines
    // (whose vertex buffers + atlas the screenshot pipelines share) are ready.
    if (_screenshotRenderTarget != nullptr && _screenshotSize == size
        && _screenshotRectPipeline.pipeline != nullptr && _screenshotTextPipeline.pipeline != nullptr)
        return true;

    if (!pipelinesReady())
        return false;

    // Color target: an RGBA8 texture usable as a render target and as a readback transfer source.
    _screenshotTexture.reset(rhi->newTexture(
        QRhiTexture::RGBA8, pixelSize, 1, QRhiTexture::RenderTarget | QRhiTexture::UsedAsTransferSource));
    if (!_screenshotTexture->create())
    {
        errorLog()("Screenshot: failed to create offscreen color texture of size {}.", size);
        return false;
    }

    // Depth-stencil buffer: the pipelines depth-test (LessOrEqual), so the offscreen pass needs a matching
    // depth-stencil attachment for its render-pass descriptor to be compatible with the pipelines.
    _screenshotDepthStencil.reset(rhi->newRenderBuffer(QRhiRenderBuffer::DepthStencil, pixelSize, 1));
    if (!_screenshotDepthStencil->create())
    {
        errorLog()("Screenshot: failed to create offscreen depth-stencil buffer of size {}.", size);
        return false;
    }

    QRhiTextureRenderTargetDescription rtDesc({ QRhiColorAttachment(_screenshotTexture.get()) });
    rtDesc.setDepthStencilBuffer(_screenshotDepthStencil.get());
    _screenshotRenderTarget.reset(rhi->newTextureRenderTarget(rtDesc));
    _screenshotRpDesc.reset(_screenshotRenderTarget->newCompatibleRenderPassDescriptor());
    _screenshotRenderTarget->setRenderPassDescriptor(_screenshotRpDesc.get());
    if (!_screenshotRenderTarget->create())
    {
        errorLog()("Screenshot: failed to create offscreen render target of size {}.", size);
        return false;
    }

    // Build the pass pipelines against the offscreen render-pass descriptor. They reuse the swapchain
    // pipelines' vertex buffers and atlas texture/sampler, but own their uniform buffers: the offscreen
    // pass renders into an ITEM-sized target and needs an item-local transform (uploaded by
    // recordScreenshotPass), not the swapchain's item→window transform.
    createScreenshotPipeline(rhi, /*isText*/ false);
    createScreenshotPipeline(rhi, /*isText*/ true);
    if (_screenshotRectPipeline.pipeline == nullptr || _screenshotTextPipeline.pipeline == nullptr)
    {
        errorLog()("Screenshot: failed to create offscreen pipelines.");
        return false;
    }

    _screenshotSize = size;
    return true;
}

void RhiRenderer::createScreenshotPipeline(QRhi* rhi, bool isText)
{
    // Reuse the data-driven pipeline builder with the offscreen render-pass descriptor. Unlike the vertex
    // buffers and atlas (shared with the swapchain set), the screenshot pipeline OWNS its uniform buffer
    // (sharedUniformBuffer == nullptr): the offscreen pass renders into an ITEM-sized target, so it needs
    // an item-local transform — replaying with the swapchain's item→window transform captured every
    // screenshot shifted by the item's window offset and scaled by the item/window size ratio.
    // recordScreenshotPass() uploads the screenshot uniforms per capture.
    auto const desc = passDescriptor(isText);
    auto& out = isText ? _screenshotTextPipeline : _screenshotRectPipeline;
    createPipeline(rhi, _screenshotRpDesc.get(), desc, out, /*sharedUniformBuffer=*/nullptr);
}

void RhiRenderer::recordScreenshotPass(QRhi* rhi, QRhiCommandBuffer* cb)
{
    if (!_pendingScreenshotCallback || rhi == nullptr || cb == nullptr || !pipelinesReady())
        return;

    // Capture at the terminal's device-pixel render size (terminal-only pixels — no window chrome).
    auto const size = _renderTargetSize;
    if (!ensureScreenshotTarget(rhi, size))
        return;

    // The offscreen target is ITEM-sized, so the replayed draws need an item-local transform: the
    // rasterizer's device-pixel, top-left-origin item vertices map directly onto the target via a plain
    // ortho (Qt's convention: y = 0 at the top), with the backend's clip-space correction applied. The
    // swapchain uniform buffers hold the item→WINDOW transform — replaying with those captured every
    // screenshot shifted by the item's window offset and scaled by the item/window size ratio (blank
    // tab-strip band, split panes mostly cropped) — hence the screenshot pipelines' own uniform buffers,
    // uploaded here per capture. The model matrix (QML transforms) is item-local and still applies.
    auto projection = rhi->clipSpaceCorrMatrix();
    projection.ortho(0.0f, unbox<float>(size.width), unbox<float>(size.height), 0.0f, -1.0f, 1.0f);
    auto const mvp = projection * _modelMatrix;
    auto const timeValue = uptime(std::chrono::steady_clock::now());
    auto* uniformUploads = rhi->nextResourceUpdateBatch();
    if (_screenshotRectPipeline.uniformBuffer)
        uploadRectUniforms(*uniformUploads, *_screenshotRectPipeline.uniformBuffer, mvp, timeValue);
    if (_screenshotTextPipeline.uniformBuffer)
        uploadTextUniforms(*uniformUploads, *_screenshotTextPipeline.uniformBuffer, mvp, timeValue);

    // Replay this frame's staged draw items into the offscreen target. This runs in prepare(), before the
    // scene graph begins its own render pass, so a fresh beginPass/endPass on the command buffer is legal
    // (passes must be sequential, not nested). The vertex uploads queued by flushFrame() precede this on
    // the command buffer, so the shared vertex buffers are populated by the time these draws execute; the
    // screenshot uniform uploads ride on beginPass so they are processed before the pass renders.
    QColor const clearColor(0, 0, 0, 0);
    cb->beginPass(_screenshotRenderTarget.get(), clearColor, { 1.0f, 0 }, uniformUploads);
    replayDrawItems(cb, _screenshotRenderTarget->pixelSize(), /*offscreen*/ true);
    cb->endPass();

    // Schedule the deferred readback of the offscreen texture (completes after this frame is submitted).
    auto* batch = rhi->nextResourceUpdateBatch();
    batch->readBackTexture(QRhiReadbackDescription(_screenshotTexture.get()), &_screenshotReadbackResult);
    cb->resourceUpdate(batch);
    _screenshotReadbackPending = true;
    _screenshotSize = size;

    // Readback hands back the texture's texel rows in index order. For a texture we RENDERED into, which row
    // is texel row 0 follows the backend's framebuffer origin: bottom-left on OpenGL (isYUpInFramebuffer(),
    // whose readBackTexture() is a plain glReadPixels off an FBO), so the image arrives bottom-up and its
    // rows must be reversed; top-left on D3D/Vulkan/Metal, where it already matches. (The glyph atlas is
    // UPLOADED rather than rendered into, so it round-trips in the row order we wrote it and readAtlas()
    // rightly never flips.) The orientation is a property of THIS capture, so it is recorded with it —
    // deliverScreenshot() then reverses the rows without re-deriving it.
    _screenshotFlipRows = rhi->isYUpInFramebuffer();
    displayLog()("Screenshot: scheduled offscreen capture ({}).", size);
}

void RhiRenderer::deliverScreenshot()
{
    // Deliver a completed capture to the pending callback. The readback scheduled on the previous frame has
    // its data available once that frame was submitted; guard on a non-empty result so we never deliver
    // before the capture completes.
    if (!_screenshotReadbackPending || !_pendingScreenshotCallback)
        return;
    if (_screenshotReadbackResult.data.isEmpty())
        return;

    auto const width = unbox<int>(_screenshotSize.width);
    auto const height = unbox<int>(_screenshotSize.height);

    // Normalize the raw readback into a tightly-packed, top-left-origin RGBA8 buffer, reversing the rows
    // iff the capture came off a Y-up (bottom-left origin) framebuffer — recorded with the capture.
    auto const* bytes = reinterpret_cast<uint8_t const*>(_screenshotReadbackResult.data.constData());
    auto const source =
        std::span<uint8_t const>(bytes, static_cast<size_t>(_screenshotReadbackResult.data.size()));
    auto buffer = normalizeScreenshotBuffer(source, width, height, _screenshotFlipRows);

    displayLog()("Screenshot: delivering captured frame ({}).", _screenshotSize);
    _pendingScreenshotCallback.value()(buffer, _screenshotSize);

    _pendingScreenshotCallback.reset();
    _screenshotReadbackPending = false;
    _screenshotReadbackResult.data.clear();
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
