// SPDX-License-Identifier: Apache-2.0
//
// Unit tests for the RHI GPU data-layout contracts (vertex strides/offsets and std140 uniform-block
// layouts) and the scissor-clip nesting policy used by the RHI terminal renderer (RhiRenderer).
//
// These are the binding agreement between the C++ side (which writes vertex/uniform buffers via
// QRhiResourceUpdateBatch) and the baked `.qsb` shaders (whose attribute locations and std140 uniform
// block must match byte-for-byte). A drift here renders garbage or a black terminal, so the constants are
// pinned against the documented shader contract here — without needing a GPU/RHI context.

#include <contour/display/RhiTransform.hpp>
#include <contour/display/RhiVertexLayout.hpp>
#include <contour/display/ScissorRect.hpp>

#include <QtGui/QMatrix4x4>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

using namespace contour::display;
using contour::display::ScissorRect;

// {{{ Vertex layout — must match the shaders' layout(location=...) inputs and the interleaved buffers.
TEST_CASE("rhilayout: background-rect vertex layout matches the shader contract", "[rhi][layout]")
{
    // background.vert: layout(location=0) vec3 vs_vertex; layout(location=1) vec4 vs_colors.
    // Interleaved as [x y z | r g b a] = 7 floats, 28 bytes.
    CHECK(rhilayout::FloatSize == 4);
    CHECK(rhilayout::RectVertexFloats == 7);
    CHECK(rhilayout::RectVertexStride == 28);
    CHECK(rhilayout::RectPositionOffset == 0);
    CHECK(rhilayout::RectColorOffset == 12); // after the vec3 position (3 floats)
    // The two attributes must not overlap and must fit within the stride.
    CHECK(rhilayout::RectColorOffset == rhilayout::RectPositionOffset + (3 * rhilayout::FloatSize));
    CHECK(rhilayout::RectColorOffset + (4 * rhilayout::FloatSize) == rhilayout::RectVertexStride);
}

TEST_CASE("rhilayout: text-glyph vertex layout matches the shader contract", "[rhi][layout]")
{
    // text.vert: location 0 vec3 vs_vertex; location 1 vec4 vs_texCoords; location 2 vec4 vs_colors.
    // Interleaved as [x y z | u v w q | r g b a] = 11 floats, 44 bytes.
    CHECK(rhilayout::TextVertexFloats == 11);
    CHECK(rhilayout::TextVertexStride == 44);
    CHECK(rhilayout::TextPositionOffset == 0);
    CHECK(rhilayout::TextTexCoordOffset == 12); // after vec3 position
    CHECK(rhilayout::TextColorOffset == 28);    // after vec3 position + vec4 texCoords
    CHECK(rhilayout::TextTexCoordOffset == rhilayout::TextPositionOffset + (3 * rhilayout::FloatSize));
    CHECK(rhilayout::TextColorOffset == rhilayout::TextTexCoordOffset + (4 * rhilayout::FloatSize));
    CHECK(rhilayout::TextColorOffset + (4 * rhilayout::FloatSize) == rhilayout::TextVertexStride);
}
// }}}

// {{{ std140 uniform-block layout — must match the `layout(std140, binding=0) uniform Buf {...}` in shaders.
TEST_CASE("rhilayout: background std140 uniform block { mat4 u_transform; float u_time; }", "[rhi][layout]")
{
    CHECK(rhilayout::Mat4Size == 64);
    CHECK(rhilayout::RectUniformTransformOffset == 0);
    CHECK(rhilayout::RectUniformTimeOffset == 64); // immediately after the mat4
    CHECK(rhilayout::RectUniformTimeOffset == rhilayout::RectUniformTransformOffset + rhilayout::Mat4Size);
    // Reflection extent is 68 bytes; std140 rounds the whole block up to a multiple of 16 => 80.
    CHECK(rhilayout::RectUniformBlockSize == 80);
    CHECK(rhilayout::RectUniformBlockSize % 16 == 0);
    CHECK(rhilayout::RectUniformBlockSize >= rhilayout::RectUniformTimeOffset + rhilayout::FloatSize);
}

TEST_CASE("rhilayout: text std140 uniform block { mat4; float; float; vec4 }", "[rhi][layout]")
{
    // mat4 u_transform @0, float u_time @64, float pixel_x @68, then 8 bytes of std140 padding so the
    // vec4 u_textOutlineColor starts on a 16-byte boundary @80. Block size 96 (already a multiple of 16).
    CHECK(rhilayout::Vec4Size == 16);
    CHECK(rhilayout::TextUniformTransformOffset == 0);
    CHECK(rhilayout::TextUniformTimeOffset == 64);
    CHECK(rhilayout::TextUniformPixelXOffset == 68);
    // The vec4 must be 16-byte aligned per std140 (this is the classic foot-gun the test guards).
    CHECK(rhilayout::TextUniformOutlineColorOffset == 80);
    CHECK(rhilayout::TextUniformOutlineColorOffset % 16 == 0);
    CHECK(rhilayout::TextUniformOutlineColorOffset > rhilayout::TextUniformPixelXOffset);
    CHECK(rhilayout::TextUniformBlockSize == 96);
    CHECK(rhilayout::TextUniformBlockSize % 16 == 0);
    CHECK(rhilayout::TextUniformBlockSize == rhilayout::TextUniformOutlineColorOffset + rhilayout::Vec4Size);
}
// }}}

// {{{ Effective clip nesting policy (computeEffectiveClip): inner scissor ∩ scene-graph node clip.
TEST_CASE("computeEffectiveClip: no inner and no node clip yields no clip", "[rhi][scissor]")
{
    // Neither present => std::nullopt => the caller scissors to the full render target.
    CHECK_FALSE(computeEffectiveClip(std::nullopt, std::nullopt).has_value());
}

TEST_CASE("computeEffectiveClip: only the inner scissor applies", "[rhi][scissor]")
{
    auto const inner = contour::display::ScissorRect { .x = 10, .y = 20, .width = 100, .height = 50 };
    auto const clip = computeEffectiveClip(inner, std::nullopt);
    REQUIRE(clip.has_value());
    CHECK(clip->x == 10);
    CHECK(clip->y == 20);
    CHECK(clip->width == 100);
    CHECK(clip->height == 50);
}

TEST_CASE("computeEffectiveClip: only the node clip applies", "[rhi][scissor]")
{
    auto const node = contour::display::ScissorRect { .x = 0, .y = 33, .width = 1024, .height = 600 };
    auto const clip = computeEffectiveClip(std::nullopt, node);
    REQUIRE(clip.has_value());
    CHECK(clip->x == 0);
    CHECK(clip->y == 33);
    CHECK(clip->width == 1024);
    CHECK(clip->height == 600);
}

TEST_CASE("computeEffectiveClip: both present yields their intersection (only shrinks)", "[rhi][scissor]")
{
    // The node clips the terminal to its item rect; the inner scissor confines smooth-scroll to the main
    // area. The effective clip is the overlap and is never larger than either.
    auto const node = contour::display::ScissorRect { .x = 0, .y = 0, .width = 1024, .height = 600 };
    auto const inner = contour::display::ScissorRect { .x = 100, .y = 50, .width = 2000, .height = 2000 };
    auto const clip = computeEffectiveClip(inner, node);
    REQUIRE(clip.has_value());
    CHECK(clip->x == 100);
    CHECK(clip->y == 50);
    CHECK(clip->width == 924);  // 1024 - 100
    CHECK(clip->height == 550); // 600 - 50
    // Symmetric: argument order must not matter for the covered area.
    auto const swapped = computeEffectiveClip(node, inner);
    CHECK(swapped->width == clip->width);
    CHECK(swapped->height == clip->height);
}

TEST_CASE("computeEffectiveClip: disjoint inner and node yield an empty (clip-everything) rect",
          "[rhi][scissor]")
{
    auto const node = contour::display::ScissorRect { .x = 0, .y = 0, .width = 100, .height = 100 };
    auto const inner = contour::display::ScissorRect { .x = 500, .y = 500, .width = 100, .height = 100 };
    auto const clip = computeEffectiveClip(inner, node);
    REQUIRE(clip.has_value());
    CHECK(clip->empty());
}
// }}}

// {{{ DPR transform (composeItemToClip): device-pixel vertices must map 1:1 into the clip cube.
//
// Builds the projection the scene graph uses at a given DPR: an orthographic projection over the LOGICAL
// extent (device extent / DPR), top-left origin (y flipped), matching what Qt feeds a render node. The
// terminal rasterizer emits DEVICE-pixel vertices, so composeItemToClip must pre-scale by 1/DPR; the test
// asserts the device-pixel render-target corner lands exactly on the clip-cube edge (the bug mapped it to
// 2.5 instead of +1 at DPR 1.75, scaling the grid up by the DPR).
namespace
{
QMatrix4x4 logicalOrtho(int deviceWidth, int deviceHeight, float dpr)
{
    auto const logicalW = static_cast<float>(deviceWidth) / dpr;
    auto const logicalH = static_cast<float>(deviceHeight) / dpr;
    QMatrix4x4 m;
    m.ortho(0.0f, logicalW, logicalH, 0.0f, -1.0f, 1.0f); // top-left origin
    return m;
}
} // namespace

TEST_CASE("composeItemToClip: device-pixel corners map to the clip cube at fractional DPR", "[rhi][dpr]")
{
    constexpr int DeviceW = 1274;
    constexpr int DeviceH = 868;
    constexpr float Dpr = 1.75f;

    auto const projection = logicalOrtho(DeviceW, DeviceH, Dpr);
    auto const nodeMatrix = QMatrix4x4 {}; // identity (no extra item transform)

    auto const itemToClip = composeItemToClip(projection, nodeMatrix, Dpr);

    // Device-pixel top-left (0,0) -> clip (-1, +1).
    auto const tl = itemToClip.map(QVector3D(0.0f, 0.0f, 0.0f));
    CHECK(tl.x() == Catch::Approx(-1.0f).margin(1e-4));
    CHECK(tl.y() == Catch::Approx(1.0f).margin(1e-4));

    // Device-pixel bottom-right (DeviceW, DeviceH) -> clip (+1, -1). The pre-DPR-fix bug mapped this to
    // (+2.5, -2.64) because device-pixel vertices were fed into a logical-space projection.
    auto const br = itemToClip.map(QVector3D(static_cast<float>(DeviceW), static_cast<float>(DeviceH), 0.0f));
    CHECK(br.x() == Catch::Approx(1.0f).margin(1e-3));
    CHECK(br.y() == Catch::Approx(-1.0f).margin(1e-3));
}

TEST_CASE("composeItemToClip: at DPR 1.0 it is the plain projection*node product", "[rhi][dpr]")
{
    auto const projection = logicalOrtho(800, 600, 1.0f);
    auto const node = QMatrix4x4 {};
    auto const composed = composeItemToClip(projection, node, 1.0f);
    auto const plain = projection * node;
    // 1/DPR == 1 => the device->logical scale is identity, so the result equals the uncorrected product.
    CHECK(composed == plain);
}

TEST_CASE("composeItemToClip: non-positive DPR is treated as identity (no scale)", "[rhi][dpr]")
{
    auto const projection = logicalOrtho(800, 600, 1.0f);
    auto const node = QMatrix4x4 {};
    CHECK(composeItemToClip(projection, node, 0.0f) == projection * node);
    CHECK(composeItemToClip(projection, node, -1.0f) == projection * node);
}

// Regression pins for #2040: the divisor composeItemToClip() is given must be the SAME ratio the scene
// graph built the projection with. Any other value scales the rasterizer's device-pixel quads, and since
// the glyph atlas is sampled with QRhiSampler::Nearest that duplicates and drops whole glyph columns
// rather than merely displacing them (the readback case in ScreenshotRhiReadback_test demonstrates it on
// real pixels). deviceToLogicalScale() removes the guesswork by deriving the ratio from the frame itself.
TEST_CASE("deviceToLogicalScale: recovers the ratio the scene graph rasterizes at", "[rhi][dpr]")
{
    CHECK(deviceToLogicalScale(QSize(1600, 1200), QSizeF(800, 600)) == Catch::Approx(2.0f));
    CHECK(deviceToLogicalScale(QSize(1000, 750), QSizeF(800, 600)) == Catch::Approx(1.25f));
    CHECK(deviceToLogicalScale(QSize(800, 600), QSizeF(800, 600)) == Catch::Approx(1.0f));
}

TEST_CASE("deviceToLogicalScale: degenerate extents fall back to identity", "[rhi][dpr]")
{
    // Nothing sensible to divide by — before the surface has a size, or after it has been torn down.
    CHECK(deviceToLogicalScale(QSize(0, 0), QSizeF(800, 600)) == Catch::Approx(1.0f));
    CHECK(deviceToLogicalScale(QSize(800, 600), QSizeF(0, 0)) == Catch::Approx(1.0f));
    // A degenerate WIDTH alone still has an answer: the height carries the same ratio.
    CHECK(deviceToLogicalScale(QSize(0, 1200), QSizeF(0, 600)) == Catch::Approx(2.0f));
}

TEST_CASE("deviceToLogicalScale: the derived scale maps device pixels 1:1 into the projection", "[rhi][dpr]")
{
    // The property that matters, stated end to end: take a surface, build the projection the scene graph
    // would build for it, derive the scale from the same surface, and the render target's far corner in
    // DEVICE pixels must land exactly on the clip-cube edge. A separately-sampled DPR (contentScale(),
    // which reads QWindow::devicePixelRatio() while Qt uses effectiveDevicePixelRatio()) is not
    // guaranteed to satisfy this — which is the defect.
    constexpr int DeviceW = 1000;
    constexpr int DeviceH = 750;
    constexpr float SceneGraphDpr = 1.25f;

    auto const projection = logicalOrtho(DeviceW, DeviceH, SceneGraphDpr);
    auto const derived = deviceToLogicalScale(QSize(DeviceW, DeviceH),
                                              QSizeF(DeviceW / SceneGraphDpr, DeviceH / SceneGraphDpr));

    auto const itemToClip = composeItemToClip(projection, QMatrix4x4 {}, derived);
    auto const br = itemToClip.map(QVector3D(DeviceW, DeviceH, 0.0f));
    CHECK(br.x() == Catch::Approx(1.0f).margin(1e-3));
    CHECK(br.y() == Catch::Approx(-1.0f).margin(1e-3));

    // And the counter-example: a divisor off by the amount two Qt DPR accessors can disagree by does NOT
    // land on the edge, so the glyph texels no longer map 1:1 to hardware pixels.
    auto const wrong = composeItemToClip(projection, QMatrix4x4 {}, 1.0f);
    CHECK(wrong.map(QVector3D(DeviceW, DeviceH, 0.0f)).x() != Catch::Approx(1.0f).margin(1e-3));
}
// }}}

// {{{ devicePixelScaleError — the #2040 geometry probe's detector.
//
// Builds the same matrices the scene graph feeds TerminalRenderNode::prepare() so the detector is
// exercised against realistic inputs rather than hand-made ones. A projection for a target of W
// device pixels at scale `dpr` is what Qt hands the node.

namespace
{
/// An orthographic projection over a logical-pixel viewport, as the scene graph builds it.
QMatrix4x4 sceneProjection(float logicalWidth, float logicalHeight)
{
    auto m = QMatrix4x4 {};
    m.ortho(0.0f, logicalWidth, logicalHeight, 0.0f, -1.0f, 1.0f);
    return m;
}
} // namespace

TEST_CASE("rhitransform: a matched frame maps one device pixel to one hardware pixel", "[rhi][geometry]")
{
    // Target 1000 device px over a 1000 logical-px window: dpr 1, the steady state on a
    // non-scaled display.
    auto const projection = sceneProjection(1000.0f, 600.0f);
    auto const dpr = deviceToLogicalScale(QSize(1000, 600), QSizeF(1000.0, 600.0));
    auto const itemToClip = composeItemToClip(projection, QMatrix4x4 {}, dpr);

    CHECK(devicePixelScaleError(itemToClip, 1000) == Catch::Approx(1.0f).epsilon(0.0001f));
}

TEST_CASE("rhitransform: a matched frame at DPR 2 is still 1:1", "[rhi][geometry]")
{
    // 2000 device px over a 1000 logical-px window. The projection is built in LOGICAL space, so it
    // spans 1000; the 1/dpr pre-scale is what brings the device-pixel vertices back onto it.
    auto const projection = sceneProjection(1000.0f, 600.0f);
    auto const dpr = deviceToLogicalScale(QSize(2000, 1200), QSizeF(1000.0, 600.0));
    CHECK(dpr == Catch::Approx(2.0f));

    auto const itemToClip = composeItemToClip(projection, QMatrix4x4 {}, dpr);
    CHECK(devicePixelScaleError(itemToClip, 2000) == Catch::Approx(1.0f).epsilon(0.0001f));
}

TEST_CASE("rhitransform: a projection from a stale render-target size is detected", "[rhi][geometry]")
{
    // THE MID-RESIZE CASE. The window resized 500 -> 494 logical px. The node's dpr is derived from
    // THIS frame (494/494 = 1), which is individually correct -- but the scene graph handed a
    // projection still built for the previous 500-px extent. composeItemToClip() cannot see that,
    // which is exactly why the probe measures the composed result instead of dpr.
    auto const staleProjection = sceneProjection(500.0f, 600.0f);
    auto const dpr = deviceToLogicalScale(QSize(494, 600), QSizeF(494.0, 600.0));
    CHECK(dpr == Catch::Approx(1.0f)); // dpr alone looks fine

    auto const itemToClip = composeItemToClip(staleProjection, QMatrix4x4 {}, dpr);
    auto const error = devicePixelScaleError(itemToClip, 494);

    // 494/500 = 0.988 -- a 1.2% shortfall. Every quad is scaled by it, and under Nearest sampling
    // that drifts the sample points across texel boundaries.
    CHECK(error == Catch::Approx(494.0f / 500.0f).epsilon(0.001f));
    CHECK(std::abs(error - 1.0f) > 0.001f); // i.e. the probe reports this frame
}

TEST_CASE("rhitransform: a mismatched DPR is detected", "[rhi][geometry]")
{
    // The originally-reported shape: the divisor and the projection disagree by ~6%, which is what
    // duplicates whole glyph columns rather than merely blurring them.
    auto const projection = sceneProjection(1000.0f, 600.0f);
    auto const itemToClip = composeItemToClip(projection, QMatrix4x4 {}, 1.06f);

    auto const error = devicePixelScaleError(itemToClip, 1000);
    CHECK(error == Catch::Approx(1.0f / 1.06f).epsilon(0.001f));
    CHECK(std::abs(error - 1.0f) > 0.001f);
}

TEST_CASE("rhitransform: a fractional pane origin is NOT reported", "[rhi][geometry]")
{
    // A translation leaves the quad spanning as many device pixels as the tile has texels, so the
    // sample points still step one texel per pixel. This was investigated and disproven as a cause
    // in #2040; the probe must not resurrect it as a false positive.
    auto const projection = sceneProjection(1000.0f, 600.0f);
    auto nodeMatrix = QMatrix4x4 {};
    nodeMatrix.translate(4.5f, 7.5f);

    auto const itemToClip = composeItemToClip(projection, nodeMatrix, 1.0f);
    CHECK(devicePixelScaleError(itemToClip, 1000) == Catch::Approx(1.0f).epsilon(0.0001f));
}

TEST_CASE("rhitransform: a degenerate target width reports no error", "[rhi][geometry]")
{
    CHECK(devicePixelScaleError(QMatrix4x4 {}, 0) == Catch::Approx(1.0f));
    CHECK(devicePixelScaleError(QMatrix4x4 {}, -8) == Catch::Approx(1.0f));
}
// }}}
