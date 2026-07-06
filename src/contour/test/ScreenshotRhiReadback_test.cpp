// SPDX-License-Identifier: Apache-2.0
//
// End-to-end RHI readback test for the screenshot path. Where a (software) OpenGL RHI can be created in the
// test environment, this exercises the REAL Qt RHI readback contract the screenshot capture depends on:
// render into an owned RGBA8 texture render target, schedule readBackTexture(), and verify the pixels come
// back correctly through normalizeScreenshotBuffer() — the same buffer transform
// RhiRenderer::deliverScreenshot applies. This guards the offscreen-texture-readback assumptions (top-left
// origin, RGBA8, deferred completion) that the pure ScreenshotReadback_test cannot cover.
//
// The test SKIPS (does not fail) when no GL context / RHI is available (common in headless CI without a
// software GL stack), so it hardens coverage where possible without becoming a flaky gate.

#include <contour/display/ScreenshotReadback.h>

#include <QtGui/QOffscreenSurface>
#include <QtGui/QOpenGLContext>

#include <rhi/qrhi.h>
// QRhiGles2InitParams (the OpenGL backend init struct) lives in this private header; the umbrella
// rhi/qrhi.h does not pull it. Linked via Qt6::GuiPrivate.
#include <QtGui/private/qrhigles2_p.h>

#include <catch2/catch_test_macros.hpp>

#include <memory>

using namespace contour::display;

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

} // namespace

TEST_CASE("RHI readback: an offscreen texture cleared to a known color reads back correctly",
          "[screenshot][rhi]")
{
    auto env = makeOffscreenRhi();
    if (!env.valid())
    {
        WARN("Skipping RHI readback test: no usable OpenGL context in this environment.");
        SUCCEED("no GL context; skipped");
        return;
    }

    auto* rhi = env.rhi.get();
    constexpr int W = 8;
    constexpr int H = 6;
    QSize const size(W, H);

    // Owned color texture usable as a render target and readback source (as RhiRenderer's screenshot target).
    std::unique_ptr<QRhiTexture> tex(rhi->newTexture(
        QRhiTexture::RGBA8, size, 1, QRhiTexture::RenderTarget | QRhiTexture::UsedAsTransferSource));
    REQUIRE(tex->create());

    std::unique_ptr<QRhiTextureRenderTarget> rt(
        rhi->newTextureRenderTarget(QRhiTextureRenderTargetDescription({ QRhiColorAttachment(tex.get()) })));
    std::unique_ptr<QRhiRenderPassDescriptor> rp(rt->newCompatibleRenderPassDescriptor());
    rt->setRenderPassDescriptor(rp.get());
    REQUIRE(rt->create());

    // A distinctive opaque color so channel order (RGBA) and correctness are both observable.
    // R=0x11, G=0x22, B=0x33, A=0xFF.
    QColor const clearColor(0x11, 0x22, 0x33, 0xFF);

    QRhiCommandBuffer* cb = nullptr;
    if (rhi->beginOffscreenFrame(&cb) != QRhi::FrameOpSuccess)
    {
        WARN("Skipping RHI readback test: beginOffscreenFrame failed.");
        SUCCEED("offscreen frame unavailable; skipped");
        return;
    }

    cb->beginPass(rt.get(), clearColor, { 1.0f, 0 });
    cb->endPass();

    QRhiReadbackResult readback;
    auto* batch = rhi->nextResourceUpdateBatch();
    batch->readBackTexture(QRhiReadbackDescription(tex.get()), &readback);
    cb->resourceUpdate(batch);

    rhi->endOffscreenFrame(); // submits + completes the readback

    REQUIRE_FALSE(readback.data.isEmpty());
    CHECK(readback.pixelSize == size);

    auto const* bytes = reinterpret_cast<uint8_t const*>(readback.data.constData());
    auto const source = std::span<uint8_t const>(bytes, static_cast<size_t>(readback.data.size()));

    // Offscreen-texture readback is top-left origin => no flip, matching RhiRenderer::deliverScreenshot.
    auto const flip = screenshotNeedsVerticalFlip(/*capturedFromTexture*/ true, rhi->isYUpInFramebuffer());
    CHECK_FALSE(flip);

    auto const image = normalizeScreenshotBuffer(source, W, H, flip);
    REQUIRE(image.size() == screenshotBufferSize(W, H));

    // Every pixel must be the clear color, in RGBA byte order.
    bool allMatch = true;
    for (size_t px = 0; px < static_cast<size_t>(W) * H; ++px)
    {
        auto const i = px * 4;
        if (image[i + 0] != 0x11 || image[i + 1] != 0x22 || image[i + 2] != 0x33 || image[i + 3] != 0xFF)
        {
            allMatch = false;
            break;
        }
    }
    CHECK(allMatch);
}
