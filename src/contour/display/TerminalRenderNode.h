// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <QtQuick/QSGRenderNode>

#include <atomic>
#include <memory>

namespace contour::display
{

class TerminalDisplay;

/// Shared liveness cell between a TerminalDisplay and its render node.
///
/// The node's prepare()/render() run in the scene graph's RENDER phase, when the GUI thread is NOT
/// blocked — so the display can be destroyed on the GUI thread while a frame is mid-flight on the
/// render thread. The display owns one cell for its lifetime and publishes null there FIRST in its
/// destructor (then fences the in-flight frame out; see ~TerminalDisplay); the node loads the cell
/// once per callback and no-ops on null. The shared_ptr keeps the cell alive for whichever side
/// dies last.
using DisplayLiveness = std::shared_ptr<std::atomic<TerminalDisplay*>>;

/// Scene-graph render node that draws a TerminalDisplay's terminal content *inside* the Qt Quick scene
/// graph's render pass, at the owning item's z-position, submitting through the Qt 6 RHI command buffer.
///
/// Historically the terminal painted from the QQuickWindow::afterRendering signal — i.e. after the whole
/// QML scene (popups, the custom title bar, dialogs) had been composited — and blitted its OpenGL output
/// straight into the window framebuffer, clipped only to a hand-rolled glScissor of the item rectangle.
/// Any in-scene overlay that opened over the terminal (the tab context menu, the color flyout, permission
/// dialogs) was therefore overpainted. Driving the terminal through a QSGRenderNode makes it a first-class
/// scene-graph citizen: Qt composites it in z-order, supplies the item→clip transform and the clip
/// rectangle. Raw GL from a render node does not reach Qt 6's RHI-managed render target, so the node feeds
/// the live QRhi / QRhiCommandBuffer / QRhiRenderTarget to the renderer, which records its draws via the
/// RHI command-buffer API. Overlays drawn later in the pass paint above the terminal for free, and the
/// manual title-bar scissor is no longer needed.
///
/// The node is a thin adapter: prepare() builds the renderer's RHI pipelines for the frame's render pass,
/// and render() establishes Qt's transform/clip and the per-frame RHI handles on the renderer, then
/// delegates the actual frame to TerminalDisplay::renderFrameRhi(). The subtle render-thread bookkeeping
/// (font/DPI reconfig consumption, redraw scheduling, the dirty-state handshake) stays in TerminalDisplay
/// so it has a single source of truth.
///
/// @note Owned by the scene graph (returned from TerminalDisplay::updatePaintNode). Qt creates and destroys
///       render nodes on the render thread, which is where RHI resource teardown must happen.
class TerminalRenderNode: public QSGRenderNode
{
  public:
    /// @param liveness The owning display's liveness cell (see DisplayLiveness): every callback loads
    ///                  the display from it, so a display mid-destruction (cell already nulled) can
    ///                  never be called into.
    explicit TerminalRenderNode(DisplayLiveness liveness): _liveness { std::move(liveness) } {}

    /// Builds the renderer's RHI pipelines for this frame's render target, before render() records draws.
    ///
    /// Qt invokes prepare() on the render thread with the RHI and the frame's render target known but the
    /// render pass not yet recording. That is the correct point to (lazily) create the graphics pipelines,
    /// shader-resource-bindings and GPU buffers, which must be baked against the render pass descriptor. The
    /// renderer early-outs when its pipelines are already valid for the same RHI + render pass.
    void prepare() override;

    /// Renders one terminal frame into the scene graph's current target.
    ///
    /// Composes the renderer's model-view-projection from Qt's supplied matrices
    /// (@c state->projectionMatrix() and the node's @c matrix()), hands the live RHI command buffer and
    /// render target to the renderer so it can record draws against the scene graph's render pass, forwards
    /// Qt's clip rectangle so the terminal's smooth-scroll scissor can intersect against it, and then
    /// delegates to TerminalDisplay::renderFrameRhi(). The scene graph clips the node; the terminal submits
    /// exclusively through QRhi (no raw GL).
    /// @param state Per-frame scene-graph render state (projection, scissor, stencil, clip region).
    void render(const RenderState* state) override;

    /// @return The state categories this node mutates so Qt restores the rest afterwards. Submission is
    ///         pure QRhi now (no raw GL state poking); the only fixed-function state we drive through the
    ///         command buffer is the viewport and the scissor, which the RHI tracks itself. We still declare
    ///         them so the scene graph re-establishes its own viewport/scissor for subsequent nodes.
    [[nodiscard]] StateFlags changedStates() const override;

    /// @return Rendering hints: @c NoExternalRendering | @c DepthAwareRendering. NoExternalRendering tells
    ///         Qt we only issue QRhi commands (no external/native API calls), so it need not isolate native
    ///         GL state around us. DepthAwareRendering opts the node into the scene graph's depth-ordered
    ///         pass (its pipeline depth-tests against Qt's projection-supplied per-node depth); this is
    ///         required for the node's output to survive into the frame, matching Qt's custom-render-node
    ///         example. OpaqueRendering is deliberately NOT set: the terminal may be transparent (see-through
    ///         window). See TerminalRenderNode.cpp for the authoritative flag choice.
    [[nodiscard]] RenderingFlags flags() const override;

    /// @return The node's bounding rectangle in item-local coordinates (the full terminal item).
    [[nodiscard]] QRectF rect() const override;

    /// Releases the OpenGL resources owned via the display's renderer. Called by Qt on the render thread
    /// when the node is being destroyed or the scene graph is invalidated; also clears the display
    /// back-pointer so a late render() cannot touch a torn-down display.
    void releaseResources() override;

    /// Re-establishes the liveness link after a release-then-reuse cycle.
    ///
    /// Qt may call releaseResources() (which drops @c _liveness) on a node it then *reuses* rather than
    /// destroys — on scene-graph invalidation paths such as a render-loop switch, a window hide/show or
    /// some DPR changes. updatePaintNode() calls this on every reused node so a released-but-reused node
    /// re-links to its display; without it prepare()/render() would early-out and the terminal would
    /// stay black until the node was actually destroyed and recreated.
    /// @param liveness The owning display's liveness cell.
    void setLiveness(DisplayLiveness liveness) noexcept { _liveness = std::move(liveness); }

  private:
    /// Loads the owning display for one callback, or nullptr when the display is gone/dying or the
    /// node was released.
    [[nodiscard]] TerminalDisplay* display() const noexcept
    {
        return _liveness ? _liveness->load(std::memory_order_acquire) : nullptr;
    }

    DisplayLiveness _liveness; //!< Owning display's liveness cell; dropped in releaseResources() and
                               //!< re-established by updatePaintNode() via setLiveness() on reuse.
};

} // namespace contour::display
