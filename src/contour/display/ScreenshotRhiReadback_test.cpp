// SPDX-License-Identifier: Apache-2.0
//
// End-to-end RHI readback tests for the screenshot path. Where a (software) OpenGL RHI can be created in the
// test environment, these exercise the REAL Qt RHI readback contract the screenshot capture depends on —
// including the one thing a pure test cannot know: which row of the readback buffer is the TOP row of the
// image. That is a property of the live backend (see RhiRenderer::recordScreenshotPass), and getting it
// wrong saved every screenshot upside down (#1986).
//
// The tests SKIP (do not fail) when no GL context / RHI is available (common in headless CI without a
// software GL stack), so they harden coverage where possible without becoming a flaky gate.

#include <contour/display/RhiRenderer.hpp>
#include <contour/display/RhiTransform.hpp>
#include <contour/display/ScreenshotReadback.hpp>

#include <vtbackend/core/Color.hpp>
#include <vtbackend/core/Primitives.hpp>

#include <vtrasterizer/shared_defines.h>

#include <QtGui/QColor>
#include <QtGui/QMatrix4x4>
#include <QtGui/QOffscreenSurface>
#include <QtGui/QOpenGLContext>

#include <rhi/qrhi.h>
// QRhiGles2InitParams (the OpenGL backend init struct) lives in this private header; the umbrella
// rhi/qrhi.h does not pull it. Linked via Qt6::GuiPrivate.
#include <QtGui/private/qrhigles2_p.h>
#ifdef Q_OS_MACOS
    // Same story for the Metal backend's init struct, which the macOS route below creates the RHI with.
    #include <QtGui/private/qrhimetal_p.h>
#endif

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <memory>
#include <ranges>
#include <span>
#include <vector>

using namespace contour::display;

using vtbackend::Height;
using vtbackend::ImageSize;
using vtbackend::RGBAColor;
using vtbackend::Width;

namespace
{

/// Owns an offscreen QRhi for a headless readback test (plus the GL context and surface the OpenGL
/// backend needs), or nothing if the platform can provide neither. Cleans up in reverse order.
struct OffscreenRhi
{
    std::unique_ptr<QOpenGLContext> context;
    std::unique_ptr<QOffscreenSurface> surface;
    std::unique_ptr<QRhi> rhi;

    [[nodiscard]] bool valid() const noexcept { return rhi != nullptr; }
};

/// Attempts to build an offscreen GLES2/OpenGL QRhi, which needs a current context on a fallback surface.
/// @return an OffscreenRhi whose valid() is false when the environment has no usable GL context.
OffscreenRhi makeOffscreenGlRhi()
{
    OffscreenRhi out;

    out.context = std::make_unique<QOpenGLContext>();
    if (!out.context->create())
        return {}; // no GL context available (headless without software GL) -> skip

    out.surface = std::make_unique<QOffscreenSurface>();
    out.surface->setFormat(out.context->format());
    out.surface->create();
    if (!out.surface->isValid())
        return {};

    if (!out.context->makeCurrent(out.surface.get()))
        return {};

    QRhiGles2InitParams params;
    params.fallbackSurface = out.surface.get();
    params.window = nullptr;
    out.rhi.reset(QRhi::create(QRhi::OpenGLES2, &params));
    return out;
}

/// Attempts to build an offscreen QRhi on the platform's native backend.
///
/// Metal is tried first on Apple platforms: it needs no context or surface, and it is the backend the
/// app itself runs on there (the cocoa/offscreen plugins refuse createPlatformOpenGLContext, so the
/// OpenGL route below can only ever SKIP on macOS — which silently cost these tests all their coverage
/// on that platform). Everything else takes the OpenGL route, Qt's default on Linux.
/// @return an OffscreenRhi whose valid() is false when no backend could be created (the caller SKIPs).
OffscreenRhi makeOffscreenRhi()
{
#ifdef Q_OS_MACOS
    OffscreenRhi metal;
    QRhiMetalInitParams params;
    metal.rhi.reset(QRhi::create(QRhi::Metal, &params));
    if (metal.valid())
        return metal;
#endif
    return makeOffscreenGlRhi();
}

/// A color + depth-stencil texture render target, mirroring the layout RhiRenderer::ensureScreenshotTarget()
/// builds for its offscreen screenshot pass — so pipelines baked against this target's render-pass descriptor
/// stay compatible with the one the renderer creates internally. Owns its resources; the caller keeps it
/// alive for as long as those pipelines are used.
struct TextureTarget
{
    QRhiResourcePtr<QRhiTexture> texture;
    QRhiResourcePtr<QRhiRenderBuffer> depthStencil;
    QRhiResourcePtr<QRhiTextureRenderTarget> renderTarget;
    QRhiResourcePtr<QRhiRenderPassDescriptor> rpDesc;

    [[nodiscard]] bool valid() const noexcept { return renderTarget != nullptr; }
};

/// Creates an RGBA8 color texture (render target + readback source) with a depth-stencil buffer and its
/// render target, or an invalid TextureTarget if any resource fails to build.
/// @param rhi  The RHI to create the resources on.
/// @param size Pixel size of the target.
TextureTarget makeTextureTarget(QRhi* rhi, QSize size)
{
    TextureTarget out;
    out.texture.reset(rhi->newTexture(
        QRhiTexture::RGBA8, size, 1, QRhiTexture::RenderTarget | QRhiTexture::UsedAsTransferSource));
    if (!out.texture->create())
        return {};

    out.depthStencil.reset(rhi->newRenderBuffer(QRhiRenderBuffer::DepthStencil, size, 1));
    if (!out.depthStencil->create())
        return {};

    QRhiTextureRenderTargetDescription rtDesc({ QRhiColorAttachment(out.texture.get()) });
    rtDesc.setDepthStencilBuffer(out.depthStencil.get());
    out.renderTarget.reset(rhi->newTextureRenderTarget(rtDesc));
    out.rpDesc.reset(out.renderTarget->newCompatibleRenderPassDescriptor());
    out.renderTarget->setRenderPassDescriptor(out.rpDesc.get());
    if (!out.renderTarget->create())
        return {};

    return out;
}

/// @return @p color as the QColor an RHI clear takes.
[[nodiscard]] QColor asQColor(RGBAColor color)
{
    return { color.red(), color.green(), color.blue(), color.alpha() };
}

/// @return the color of pixel (@p x, @p y) in a tightly-packed, top-left-origin RGBA8 image buffer — the
/// layout RenderTarget's ScreenshotCallback guarantees.
/// @param image  The delivered screenshot buffer.
/// @param width  The image width in pixels (its row stride in pixels).
[[nodiscard]] RGBAColor pixelAt(std::span<uint8_t const> image, int width, int x, int y)
{
    auto const offset = ((static_cast<size_t>(y) * static_cast<size_t>(width)) + static_cast<size_t>(x))
                        * ScreenshotBytesPerPixel;
    auto const px = image.subspan(offset, ScreenshotBytesPerPixel);
    return { px[0], px[1], px[2], px[3] };
}

} // namespace

TEST_CASE("RHI readback: an offscreen texture cleared to a known color reads back correctly",
          "[screenshot][rhi]")
{
    auto env = makeOffscreenRhi();
    if (!env.valid())
        SKIP("no usable OpenGL context in this environment");

    auto* rhi = env.rhi.get();
    constexpr int W = 8;
    constexpr int H = 6;

    auto const target = makeTextureTarget(rhi, QSize(W, H));
    if (!target.valid())
        SKIP("could not create a texture render target");

    // A color with four distinct channels, so the RGBA byte order is fully observable.
    auto constexpr Clear = RGBAColor { 0x11, 0x22, 0x33, 0xFF };

    QRhiCommandBuffer* cb = nullptr;
    if (rhi->beginOffscreenFrame(&cb) != QRhi::FrameOpSuccess)
        SKIP("beginOffscreenFrame failed");

    cb->beginPass(target.renderTarget.get(), asQColor(Clear), { 1.0f, 0 });
    cb->endPass();

    QRhiReadbackResult readback;
    auto* batch = rhi->nextResourceUpdateBatch();
    batch->readBackTexture(QRhiReadbackDescription(target.texture.get()), &readback);
    cb->resourceUpdate(batch);

    rhi->endOffscreenFrame(); // submits + completes the readback

    REQUIRE_FALSE(readback.data.isEmpty());
    CHECK(readback.pixelSize == QSize(W, H));

    auto const* bytes = reinterpret_cast<uint8_t const*>(readback.data.constData());
    auto const source = std::span<uint8_t const>(bytes, static_cast<size_t>(readback.data.size()));

    // A flat clear color is orientation-blind by construction, so this case pins the channel order and the
    // deferred-completion contract only; the row order is pinned by the case below.
    auto const image = normalizeScreenshotBuffer(source, W, H, /*flip*/ rhi->isYUpInFramebuffer());
    REQUIRE(image.size() == screenshotBufferSize(W, H));

    CHECK(std::ranges::all_of(std::views::iota(0, W * H),
                              [&](int i) { return pixelAt(image, W, i % W, i / W) == Clear; }));
}

TEST_CASE("RHI readback: a captured screenshot is delivered top-left origin, not upside down",
          "[screenshot][rhi]")
{
    // Regression pin for #1986 (screenshots saved vertically flipped). This drives the REAL RhiRenderer —
    // production shaders, the production offscreen transform, and the production flip decision — over a live
    // OpenGL RHI, which is the Y-up-framebuffer backend Qt picks by default on Linux. A rectangle is filled
    // across the TOP half of the item, so the delivered image is only correct if its first rows are the
    // image's top rows. Before the fix, RhiRenderer::deliverScreenshot() never reversed the rows (it assumed
    // texture readback is top-left on every backend, which OpenGL's plain glReadPixels is not), and this
    // capture came back with the rectangle in the BOTTOM half.
    auto env = makeOffscreenRhi();
    if (!env.valid())
        SKIP("no usable OpenGL context in this environment");

    auto* rhi = env.rhi.get();
    constexpr int W = 16;
    constexpr int H = 8;
    constexpr int TopHalf = H / 2;

    // The frame's render target: the renderer's pipelines are baked against its render-pass descriptor.
    auto const frameTarget = makeTextureTarget(rhi, QSize(W, H));
    if (!frameTarget.valid())
        SKIP("could not create a texture render target");

    auto const captureSize = ImageSize { Width(W), Height(H) };
    auto renderer = contour::display::RhiRenderer(captureSize, ImageSize { Width(4), Height(4) });
    renderer.initialize();
    renderer.createPipelines(rhi, frameTarget.rpDesc.get());
    if (!renderer.pipelinesReady())
        SKIP("the RHI pipelines could not be built");

    // An opaque red band across the top half of the item; the rest stays the pass's transparent clear.
    auto constexpr Red = RGBAColor { 0xFF, 0x00, 0x00, 0xFF };
    auto constexpr Transparent = RGBAColor { 0x00, 0x00, 0x00, 0x00 };

    auto captured = std::vector<uint8_t> {};
    auto capturedSize = ImageSize {};

    QRhiCommandBuffer* cb = nullptr;
    if (rhi->beginOffscreenFrame(&cb) != QRhi::FrameOpSuccess)
        SKIP("beginOffscreenFrame failed");

    // Drive one frame exactly as the render node's prepare() does: stage the geometry, arm the capture,
    // flush the uploads, then replay the staged draws into the offscreen screenshot target.
    renderer.beginFrame(rhi, cb, frameTarget.renderTarget.get());
    renderer.renderRectangle(0, 0, Width(W), Height(TopHalf), Red); // item coords: y=0 is the TOP
    renderer.execute(std::chrono::steady_clock::now());
    renderer.scheduleScreenshot([&](std::vector<uint8_t> const& rgba, ImageSize pixelSize) {
        captured = rgba;
        capturedSize = pixelSize;
    });
    renderer.flushFrame();
    REQUIRE(renderer.screenshotRequested());
    renderer.recordScreenshotPass(rhi, cb);

    rhi->endOffscreenFrame(); // submits the frame + completes the deferred readback

    renderer.deliverScreenshot();

    REQUIRE(capturedSize == captureSize);
    REQUIRE(captured.size() == screenshotBufferSize(W, H));

    // The delivered buffer is top-left origin: the red band occupies the FIRST rows, and the untouched
    // (transparent) clear the last ones. An unflipped Y-up readback would have these exactly swapped.
    auto const image = std::span<uint8_t const>(captured);
    CHECK(pixelAt(image, W, 0, 0) == Red);                 // top-left: inside the band
    CHECK(pixelAt(image, W, W - 1, TopHalf - 1) == Red);   // last row of the band
    CHECK(pixelAt(image, W, 0, TopHalf) == Transparent);   // first row below it
    CHECK(pixelAt(image, W, W - 1, H - 1) == Transparent); // bottom-right: outside the band
}

// {{{ #2040: a split pane at a fractional device origin must not resample its glyph tiles
namespace
{

/// Red-channel step between adjacent tile columns. Eight columns must stay inside a uint8_t, so the
/// brightest is 8 * 30 = 240 and every column is distinguishable from its neighbours and from the
/// transparent background.
constexpr int TileColumnStride = 30;

/// Edge length of the single atlas tile the pane-placement case renders, in pixels.
constexpr int TileExtent = 8;

/// Builds the orthographic projection the scene graph feeds a render node at a given DPR: over the
/// LOGICAL extent (device extent / DPR), top-left origin. Mirrors the helper in RhiVertexLayout_test.
QMatrix4x4 sceneOrtho(int deviceWidth, int deviceHeight, float dpr)
{
    QMatrix4x4 m;
    m.ortho(0.0f,
            static_cast<float>(deviceWidth) / dpr,
            static_cast<float>(deviceHeight) / dpr,
            0.0f,
            -1.0f,
            1.0f);
    return m;
}

/// Renders one atlas tile through the production draw path with the pane placed at @p paneOriginLogical,
/// and returns the readback image (top-left origin, RGBA8) of the whole render target.
///
/// The tile's texels carry a distinct red value per column, so the caller can tell exactly which source
/// columns survived to the framebuffer — the property #2040 breaks.
/// @return the normalized readback buffer, or an empty vector if the environment could not render.
std::vector<uint8_t> renderTileScaled(QRhi* rhi,
                                      TextureTarget const& target,
                                      int targetWidth,
                                      int targetHeight,
                                      float paneOriginLogical,
                                      float projectionDpr,
                                      float composeDpr)
{
    using namespace vtrasterizer;

    constexpr int AtlasExtent = 16; // power of two: executeConfigureAtlas requires it

    auto renderer = RhiRenderer(ImageSize { Width(targetWidth), Height(targetHeight) },
                                ImageSize { Width(TileExtent), Height(TileExtent) });
    renderer.initialize();
    renderer.createPipelines(rhi, target.rpDesc.get());
    if (!renderer.pipelinesReady())
        return {};

    // Qt's contract: projection * nodeMatrix * vertex, both in logical pixels. The node matrix is the
    // pane's placement — the fractional translation a SplitView hands its second child. The scene graph
    // builds its projection from the surface's REAL device-pixel ratio, while composeItemToClip divides
    // by whatever the display reports; passing the two separately lets a case pin what a disagreement
    // between them does to the glyph texels.
    auto nodeMatrix = QMatrix4x4 {};
    nodeMatrix.translate(paneOriginLogical, 0.0f);
    renderer.setProjectionMatrix(contour::display::composeItemToClip(
        sceneOrtho(targetWidth, targetHeight, projectionDpr), nodeMatrix, composeDpr));

    // One RGBA tile whose column i is opaque red = 32*(i+1): every column distinguishable from its
    // neighbours and from the transparent background, so a dropped or duplicated column is observable.
    auto bitmap = atlas::Buffer(static_cast<size_t>(TileExtent) * TileExtent * 4, 0);
    for (auto const row: std::views::iota(0, TileExtent))
        for (auto const column: std::views::iota(0, TileExtent))
        {
            auto* px = bitmap.data() + (((static_cast<size_t>(row) * TileExtent) + column) * 4);
            px[0] = static_cast<uint8_t>(TileColumnStride * (column + 1));
            px[3] = 0xFF;
        }

    QRhiCommandBuffer* cb = nullptr;
    if (rhi->beginOffscreenFrame(&cb) != QRhi::FrameOpSuccess)
        return {};

    renderer.beginFrame(rhi, cb, target.renderTarget.get());
    renderer.configureAtlas(atlas::ConfigureAtlas {
        .size = ImageSize { Width(AtlasExtent), Height(AtlasExtent) },
        .properties =
            atlas::AtlasProperties { .format = atlas::Format::RGBA,
                                     .tileSize = ImageSize { Width(TileExtent), Height(TileExtent) },
                                     .hashCount = crispy::StrongHashtableSize { 4 },
                                     .tileCount = crispy::LRUCapacity { 4 },
                                     .directMappingCount = 0 } });
    renderer.uploadTile(atlas::UploadTile {
        .location = atlas::TileLocation { atlas::TileLocation::X { 0 }, atlas::TileLocation::Y { 0 } },
        .bitmapSize = ImageSize { Width(TileExtent), Height(TileExtent) },
        .bitmapFormat = atlas::Format::RGBA,
        .bitmap = std::move(bitmap),
        .rowAlignment = 1 });
    renderer.renderTile(atlas::RenderTile {
        .x = atlas::RenderTile::X { 0 }, // item-local: the pane's own left edge
        .y = atlas::RenderTile::Y { 0 },
        .bitmapSize = ImageSize { Width(TileExtent), Height(TileExtent) },
        .targetSize = ImageSize { Width(TileExtent), Height(TileExtent) },
        .color = { 1.0f, 1.0f, 1.0f, 1.0f },
        .tileLocation = atlas::TileLocation { atlas::TileLocation::X { 0 }, atlas::TileLocation::Y { 0 } },
        .normalizedLocation = atlas::NormalizedTileLocation { .x = 0.0f,
                                                              .y = 0.0f,
                                                              .width = float(TileExtent) / AtlasExtent,
                                                              .height = float(TileExtent) / AtlasExtent },
        .fragmentShaderSelector = FRAGMENT_SELECTOR_IMAGE_BGRA });
    renderer.execute(std::chrono::steady_clock::now());
    renderer.flushFrame();

    cb->beginPass(target.renderTarget.get(), asQColor(RGBAColor { 0, 0, 0, 0 }), { 1.0f, 0 });
    renderer.recordDraws();
    cb->endPass();

    QRhiReadbackResult readback;
    auto* batch = rhi->nextResourceUpdateBatch();
    batch->readBackTexture(QRhiReadbackDescription(target.texture.get()), &readback);
    cb->resourceUpdate(batch);
    rhi->endOffscreenFrame();

    if (readback.data.isEmpty())
        return {};

    auto const* bytes = reinterpret_cast<uint8_t const*>(readback.data.constData());
    return normalizeScreenshotBuffer(
        std::span<uint8_t const>(bytes, static_cast<size_t>(readback.data.size())),
        targetWidth,
        targetHeight,
        /*flip*/ rhi->isYUpInFramebuffer());
}

/// @return the red channel of every non-transparent pixel along row @p y, left to right — i.e. the source
/// tile columns that actually reached the framebuffer, in order.
std::vector<int> survivingColumns(std::span<uint8_t const> image, int width, int y)
{
    auto out = std::vector<int> {};
    for (auto const x: std::views::iota(0, width))
        if (auto const px = pixelAt(image, width, x, y); px.alpha() != 0)
            out.push_back(px.red());
    return out;
}

} // namespace

TEST_CASE("#2040: a pane at a fractional device origin renders every glyph column exactly once",
          "[screenshot][rhi][splitpane]")
{
    // Regression pin for #2040 (split panes render mangled glyphs after a window resize). A SplitView
    // sizes its first child as `view.width * ratio` with no rounding (ui/PaneNode.qml), so the second
    // pane routinely starts at a fractional logical — hence fractional DEVICE — x. The glyph atlas is
    // sampled with QRhiSampler::Nearest so each texel maps 1:1 to a hardware pixel; displace the quad by
    // half a device pixel and the rasterizer's pixel centers land on texel boundaries, so source columns
    // are dropped and duplicated at random. That is the doubled stems, the stray 1px bar and the cursor
    // box missing its right edge in the issue's screenshots.
    //
    // The invariant this asserts is placement-independent: wherever the pane sits, each of the tile's
    // eight distinct columns must reach the framebuffer exactly once, in order.
    auto env = makeOffscreenRhi();
    if (!env.valid())
        SKIP("no usable OpenGL context in this environment");

    auto* rhi = env.rhi.get();
    constexpr int W = 32;
    constexpr int H = 16;
    constexpr float Dpr = 1.0f;

    auto const target = makeTextureTarget(rhi, QSize(W, H));
    if (!target.valid())
        SKIP("could not create a texture render target");

    // The eight column values the tile carries, in source order.
    auto const expected = [] {
        auto v = std::vector<int> {};
        for (auto const column: std::views::iota(0, TileExtent))
            v.push_back(TileColumnStride * (column + 1));
        return v;
    }();

    // The tile is drawn at item-local y = 0 and is TileExtent tall, so any row in [0, TileExtent) cuts
    // through every one of its columns. Take the middle one.
    constexpr int SampleRow = TileExtent / 2;

    SECTION("an integral pane origin renders the tile faithfully (the left pane / pre-split case)")
    {
        auto const image = renderTileScaled(rhi, target, W, H, /*paneOriginLogical*/ 4.0f, Dpr, Dpr);
        if (image.empty())
            SKIP("the RHI could not render this frame");
        CHECK(survivingColumns(image, W, SampleRow) == expected);
    }

    SECTION("a fractional pane origin renders it just as faithfully (the right pane)")
    {
        // A pane at x.5 is the ordinary case for a SplitView's second child. A pure TRANSLATION is
        // harmless under Nearest sampling however fractional it is: every sample point shifts by the same
        // amount, so each still lands in a distinct texel and the tile is at worst displaced by one pixel.
        // This case exists to keep that on the record — pane placement is not what #2040 was about.
        auto const image = renderTileScaled(rhi, target, W, H, /*paneOriginLogical*/ 4.5f, Dpr, Dpr);
        if (image.empty())
            SKIP("the RHI could not render this frame");
        CHECK(survivingColumns(image, W, SampleRow) == expected);
    }

    SECTION("a SCALE mismatch is what mangles the glyph: a column renders twice")
    {
        // The negative control, and the actual mechanism behind #2040. composeItemToClip divides the
        // rasterizer's device-pixel vertices by a DPR the caller supplies; Qt builds the projection from
        // the DPR of the surface it is about to rasterize into. Let those two disagree and the quad is
        // scaled by their ratio, so with Nearest sampling source columns are duplicated (and, at other
        // ratios, dropped) — the stray 1px stem and the uneven stroke weights in the issue's screenshots.
        //
        // 1.0 vs 0.94 is a ~6% scale, enough to duplicate exactly one column of an 8px tile. The
        // production path must never produce a ratio other than 1, which is what deviceToLogicalScale()
        // (RhiTransform.h) guarantees by deriving the divisor from the frame's own render target.
        auto const image = renderTileScaled(rhi, target, W, H, /*paneOriginLogical*/ 4.0f, 1.0f, 0.94f);
        if (image.empty())
            SKIP("the RHI could not render this frame");

        auto const columns = survivingColumns(image, W, SampleRow);
        CHECK(columns.size() == expected.size() + 1); // one column too many: it was resampled
        CHECK_FALSE(columns == expected);             // ... and so the tile is not faithful
        CHECK(std::ranges::is_sorted(columns));       // duplicated, not shuffled
    }
}
// }}}
