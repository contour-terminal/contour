// SPDX-License-Identifier: Apache-2.0
#include <contour/display/RhiTransform.h>
#include <contour/display/TerminalDisplay.h>
#include <contour/display/TerminalRenderNode.h>

#include <QtQuick/QQuickWindow>
#include <QtQuick/QSGRendererInterface>

#include <rhi/qrhi.h>

namespace contour::display
{

namespace
{
    /// Fetches the scene graph's live QRhi instance from the item's window.
    ///
    /// QSGRenderNode exposes the per-frame command buffer and render target directly but not the QRhi; the
    /// supported way to reach it is the window's renderer interface. Returns nullptr if the window or its
    /// RHI is not available yet (e.g. before the scene graph is initialized).
    /// @param window The Quick window the render node draws into.
    /// @return The QRhi the scene graph renders with, or nullptr.
    [[nodiscard]] QRhi* rhiOf(QQuickWindow* window) noexcept
    {
        if (window == nullptr)
            return nullptr;
        auto* rendererInterface = window->rendererInterface();
        if (rendererInterface == nullptr)
            return nullptr;
        return static_cast<QRhi*>(rendererInterface->getResource(window, QSGRendererInterface::RhiResource));
    }
} // namespace

void TerminalRenderNode::prepare()
{
    // Load the display ONCE per callback through the liveness cell: prepare()/render() run in the
    // render phase with the GUI thread unblocked, so the display may be destroyed concurrently —
    // its destructor publishes null here first and then fences the in-flight frame out.
    auto* const display = this->display();
    if (display == nullptr)
        return;

    auto* rhi = rhiOf(display->window());
    auto* cb = commandBuffer();
    auto* rt = renderTarget();
    if (rhi == nullptr || cb == nullptr || rt == nullptr)
        return;

    // Qt's contract: a vertex's clip-space position is projectionMatrix() * matrix() * vertex. matrix() maps
    // the item's LOGICAL coordinate space to scene space and projectionMatrix() maps scene space (also in
    // logical/device-independent pixels) to clip space — i.e. the scene graph works in logical pixels and
    // applies the device-pixel ratio internally when rasterizing into the device-pixel render target.
    //
    // The terminal rasterizer, however, emits vertices in DEVICE pixels (its cell metrics and glyph atlas are
    // built at contentScale()/DPR for crisp 1:1 hardware-pixel text — matching the master branch). Feeding
    // device-pixel vertices straight into the logical-space transform would scale the grid up by the DPR
    // (oversized font, the grid overflowing past the status line, and off-by-DPR text selection). The DPR
    // correction lives in composeItemToClip() (RhiTransform.h), extracted so it can be unit-tested without a
    // window: it pre-scales the device-pixel vertices back to logical space with 1/DPR so the grid is
    // positioned correctly while each device-resolution glyph texel still maps 1:1 to a hardware pixel.
    auto const dpr = static_cast<float>(display->contentScale());
    auto const itemToClip = composeItemToClip(*projectionMatrix(), *matrix(), dpr);

    // This item's top-left corner inside the render target, in device pixels. matrix() maps item-local
    // (0,0) to scene space in logical pixels (translation for Quick item placement); the scene graph
    // rasterizes scene coordinates at the DPR. The renderer needs this to translate the rasterizer's
    // item-relative inner scissor into render-target coordinates — a split pane sits at an offset, and
    // scissoring with untranslated coordinates would clip the WRONG pane's region.
    auto const originScene = matrix()->map(QPointF(0, 0));
    auto const itemOriginDevice = QPoint(qRound(originScene.x() * dpr), qRound(originScene.y() * dpr));

    // Stage the frame BEFORE the render pass begins: build pipelines and queue all resource uploads onto the
    // command buffer. Qt's RHI render-node contract requires resource uploads to happen in prepare() (before
    // beginPass), leaving only draw commands for render().
    display->prepareFrameRhi(rhi, cb, rt, rt->renderPassDescriptor(), itemToClip, itemOriginDevice);
}

void TerminalRenderNode::render(const RenderState* state)
{
    auto* const display = this->display(); // one load; see prepare()
    if (display == nullptr)
        return;

    // Inside the active render pass: issue only the draw commands the prepare() phase staged. The node clip
    // comes from @p state and is applied to the draws by the display.
    display->recordFrameRhi(state);
}

QSGRenderNode::StateFlags TerminalRenderNode::changedStates() const
{
    // We submit purely through QRhi and only drive the viewport + scissor via the command buffer; the RHI
    // tracks both. Declaring them lets the scene graph re-establish its own viewport/scissor for nodes
    // rendered after us. We no longer poke blend/depth/color/stencil state directly.
    return ScissorState | ViewportState;
}

QSGRenderNode::RenderingFlags TerminalRenderNode::flags() const
{
    // NoExternalRendering: submission is entirely QRhi (no native/external API calls). DepthAwareRendering:
    // the node participates in the scene graph's depth-ordered pass (its pipeline depth-tests against Qt's
    // projection-supplied per-node depth) — required for the node's output to survive into the frame, as in
    // Qt's custom-render-node example.
    return NoExternalRendering | DepthAwareRendering;
}

QRectF TerminalRenderNode::rect() const
{
    auto* const display = this->display();
    return display != nullptr ? display->boundingRect() : QRectF {};
}

void TerminalRenderNode::releaseResources()
{
    if (auto* const display = this->display())
        display->releaseRenderResources();
    _liveness.reset();
}

} // namespace contour::display
