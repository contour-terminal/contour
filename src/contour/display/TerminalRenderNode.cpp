// SPDX-License-Identifier: Apache-2.0
#include <contour/display/Logging.hpp>
#include <contour/display/RhiTransform.hpp>
#include <contour/display/TerminalDisplay.hpp>
#include <contour/display/TerminalRenderNode.hpp>

#include <QtQuick/QQuickWindow>
#include <QtQuick/QSGRendererInterface>

#include <cmath>

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
    //
    // That divisor is derived from THIS frame's render target rather than read from the display's content
    // scale (#2040). The two are not interchangeable: contentScale() resolves QWindow::devicePixelRatio()
    // (and, under KDE, a forced font DPI that replaces it outright), while the scene graph builds
    // projectionMatrix() from QQuickWindow::effectiveDevicePixelRatio(). When they disagree the quads are
    // scaled by the ratio, and because the atlas sampler is Nearest, glyph columns are duplicated and
    // dropped instead of merely displaced — mangled text in one pane, which is what the issue reported.
    // The window extent comes from the snapshot updatePaintNode() took at the sync point, where the GUI
    // thread is blocked. Reading QWindow::size() here instead — as this did — is a live read from the
    // render phase with the GUI thread running, so mid-resize it can pair a window size with a render
    // target from a different moment; the ratio derived from that pair is then wrong for the whole frame,
    // which is the scale mismatch this function exists to rule out. The fallback covers the first frame,
    // before any sync has happened.
    auto const windowLogicalSize = [this, display]() -> QSizeF {
        if (!_windowLogicalSize.isEmpty())
            return _windowLogicalSize;
        if (auto const* window = display->window())
            return { static_cast<qreal>(window->size().width()),
                     static_cast<qreal>(window->size().height()) };
        return {};
    }();
    // The snapshot is refreshed only when this item is dirtied, while the scene graph calls prepare()
    // on EVERY frame. A resize that reaches the swapchain render target without putting this item on
    // the dirty list -- a pane whose own geometry an anchor holds fixed while a sibling absorbs the
    // delta, or a frame driven by another item's update() -- pairs the new render target with the
    // previous window extent, and the non-empty test above accepts that pair rather than falling back.
    // effectiveDevicePixelRatio() is the scale the scene graph itself built projectionMatrix() with, so
    // when the derived ratio disagrees with it the derivation is the stale half; prefer the live scalar,
    // which is a single value and so cannot pair two moments. Kept as a cross-check rather than the
    // primary source because the render-target-derived ratio is what makes a layered or offscreen target
    // come out right, and it agrees with this whenever the snapshot is current.
    auto const dpr = [&]() -> qreal {
        auto const derived = deviceToLogicalScale(rt->pixelSize(), windowLogicalSize);
        auto const* const window = display->window();
        if (window == nullptr)
            return derived;
        auto const effective = window->effectiveDevicePixelRatio();
        if (effective > 0.0 && std::abs(derived - effective) > 0.001)
            return effective;
        return derived;
    }();
    auto const itemToClip = composeItemToClip(*projectionMatrix(), *matrix(), dpr);

    // {{{ #2040 geometry probe
    //
    // Reports the frame whose composed transform does not map the rasterizer's device-pixel vertices
    // 1:1 onto hardware pixels. That is the precondition the Nearest-sampled glyph atlas needs: at any
    // other scale the sample points drift across texel boundaries and glyph columns duplicate or drop.
    //
    // The check is on the COMPOSED matrix rather than on `dpr` alone, because dpr is only one of three
    // inputs -- projectionMatrix() and matrix() come from the scene graph and are not derived here, so
    // a correct dpr composed against a projection built for a different render-target size still
    // yields a scaled quad. Multiplying a unit X step through itemToClip and comparing against the
    // clip-space width of one device pixel measures the thing that actually matters, whatever produced
    // it.
    if (geometryProbeLog)
    {
        auto const targetPixelSize = rt->pixelSize();
        auto const scaleError = devicePixelScaleError(itemToClip, targetPixelSize.width());

        // The tolerance sits well inside the ~6% mismatch that visibly duplicates a column, while
        // absorbing float noise from the matrix multiply.
        if (std::abs(scaleError - 1.0f) > 0.001f)
            geometryProbeLog()(
                "device-pixel scale is {:.6f}, not 1.0 -- a glyph texel does not map to a hardware "
                "pixel, so Nearest sampling duplicates and drops columns. target={}x{} "
                "windowLogical={}x{} dpr={:.6f}",
                scaleError,
                targetPixelSize.width(),
                targetPixelSize.height(),
                windowLogicalSize.width(),
                windowLogicalSize.height(),
                dpr);
    }
    // }}}

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

void TerminalRenderNode::render(RenderState const* state)
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
