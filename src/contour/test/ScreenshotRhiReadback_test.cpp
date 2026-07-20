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

#include <contour/display/RhiRenderer.h>
#include <contour/display/ScreenshotReadback.h>

#include <vtbackend/Color.h>
#include <vtbackend/primitives.h>

#include <QtGui/QColor>
#include <QtGui/QOffscreenSurface>
#include <QtGui/QOpenGLContext>

#include <rhi/qrhi.h>
// QRhiGles2InitParams (the OpenGL backend init struct) lives in this private header; the umbrella
// rhi/qrhi.h does not pull it. Linked via Qt6::GuiPrivate.
#include <QtGui/private/qrhigles2_p.h>

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

/// Owns an offscreen OpenGL context + surface + QRhi for a headless readback test, or nothing if the
/// platform cannot provide a GL context. Cleans up in reverse order.
struct OffscreenRhi
{
    std::unique_ptr<QOpenGLContext> context;
    std::unique_ptr<QOffscreenSurface> surface;
    std::unique_ptr<QRhi> rhi;

    [[nodiscard]] bool valid() const noexcept { return rhi != nullptr; }
};

/// Attempts to build an offscreen GLES2/OpenGL QRhi. Returns an OffscreenRhi whose valid() is false when the
/// environment has no usable GL context (the caller then SKIPs rather than fails).
OffscreenRhi makeOffscreenRhi()
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
    auto renderer = RhiRenderer(captureSize, ImageSize { Width(4), Height(4) });
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
