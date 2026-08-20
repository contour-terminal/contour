// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <contour/config/Actions.hpp>
#include <contour/config/Config.hpp>
#include <contour/display/ContentScale.hpp>
#include <contour/display/RenderState.hpp>
#include <contour/display/TerminalRenderNode.hpp>
#include <contour/display/WindowHost.hpp>
#include <contour/input/MouseMapping.hpp>
#include <contour/session/DisplaySurface.hpp>
#include <contour/session/SessionInput.hpp>
#include <contour/session/TerminalSession.hpp>

#include <vtbackend/core/Color.hpp>
#include <vtbackend/core/Primitives.hpp>

#include <vtrasterizer/ReGISFontRasterizer.hpp>
#include <vtrasterizer/Renderer.hpp>

#include <crispy/Deferred.hpp>

#include <QtCore/QPoint>
#include <QtCore/QSize>
#include <QtCore/QTimer>
#include <QtGui/QVector4D>
#include <QtQml/QtQml>
#include <QtQuick/QQuickItem>
#include <QtQuick/QQuickWindow>
#include <QtQuick/QSGRenderNode>

#include <filesystem>
#include <memory>
#include <optional>
#include <variant>
#ifdef CONTOUR_PERF_STATS
    #include <atomic>
#endif

QT_BEGIN_NAMESPACE
class QRhi;
class QRhiCommandBuffer;
class QRhiRenderTarget;
class QRhiRenderPassDescriptor;
class QScreen;
QT_END_NAMESPACE

namespace contour::session
{
class TerminalSessionManager;
}

namespace contour::display
{

class RhiRenderer;

// It currently can handles multiple terminals inside via tabs support.
// that is managed by TerminalSessionManager.
// The DisplaySurface overrides below are `final` rather than `override`, and the CLASS is not: QML
// registration derives from it (QQmlPrivate::QQmlElement<T>), so a final class does not compile. Marking
// each implementation final says the same thing where it matters -- this is the leaf -- and it is what
// devirtualizes the surface calls this class makes on ITSELF from its own constructor and destructor
// (window(), contentScale()), where dispatching virtually would be the classic bypass bug.
class TerminalDisplay: public QQuickItem, public session::DisplaySurface
{
    Q_OBJECT
    Q_PROPERTY(session::TerminalSession* session READ getSessionHelper WRITE setSession NOTIFY sessionChanged)
    Q_PROPERTY(QString profile READ profileName WRITE setProfileName NOTIFY profileNameChanged)
    Q_PROPERTY(QString title READ title NOTIFY titleChanged)
    QML_ELEMENT

    session::TerminalSession* getSessionHelper() { return _session; }

  public:
    explicit TerminalDisplay(QQuickItem* parent = nullptr);
    ~TerminalDisplay() override;

    // {{{ QML property helper
    [[nodiscard]] QString title() const
    {
        if (_session)
            return _session->title();
        else
            return "No session";
    }

    // NB: The title-bar visibility is WINDOW state and lives on the WindowController (the window
    // authority), not per display: storing it per pane made a ToggleTitleBar silently revert on the
    // next pane-focus change or tab switch. setSession()/handleWindowChanged() only SEED the window's
    // initial value from the profile (first-write-wins) via WindowController::seedTitleBarVisible().
    // }}}

    [[nodiscard]] config::TerminalProfile const& profile() const noexcept
    {
        assert(_session != nullptr);
        return _session->profile();
    }

    [[nodiscard]] vtbackend::Terminal const& terminal() const noexcept
    {
        assert(_session != nullptr);
        return _session->terminal();
    }

    [[nodiscard]] vtbackend::Terminal& terminal() noexcept
    {
        assert(_session != nullptr);
        return _session->terminal();
    }

    [[nodiscard]] bool hasSession() const noexcept override { return _session != nullptr; }

    /// Whether the RHI render target currently exists. False before the first scene-graph sync and
    /// after a scene-graph invalidation (destroyRenderer) — posted GUI callbacks that reach
    /// render-target-dependent methods (e.g. setFonts) must re-check this at dispatch time, the same
    /// way they re-check window().
    [[nodiscard]] bool hasRenderTarget() const noexcept final { return _renderTarget != nullptr; }

    // NB: Use TerminalSession.attachDisplay, that one is calling this here. TODO(PR) ?
    void setSession(session::TerminalSession* newSession);

    /// Clears this display's session pointer, and — if the session's back-pointer still names this
    /// display — the session's back-pointer too. Called by TerminalSession::attachDisplay when another
    /// display takes over the session (the back-pointer is repointed right after), so a stale display
    /// (e.g. the hidden single-pane view after a split) stops believing it is attached; and by
    /// setSession(nullptr) on the transient-null collapse path, where clearing the back-pointer keeps
    /// the session from posting into this display after it is destroyed.
    void releaseSession() final;

    [[nodiscard]] session::TerminalSession& session() noexcept final
    {
        assert(_session != nullptr);
        return *_session;
    }

#if QT_CONFIG(im)
    [[nodiscard]] QVariant inputMethodQuery(Qt::InputMethodQuery query) const override;
#endif

    /// Prepares (stages) one terminal frame for the RHI, called by TerminalRenderNode from
    /// QSGRenderNode::prepare() — before the scene graph begins the render pass.
    ///
    /// Builds the renderer's RHI graphics pipelines for @p rpDesc (cached; rebuilt only on RHI / render-pass
    /// change), hands the live submission handles (@p rhi, @p cb, @p rt) to the renderer, installs the scene
    /// graph's transform (@p itemToClip) and clip rectangle (from @p state), then runs the terminal render.
    /// The renderer uploads its geometry/atlas and records cb->resourceUpdate() here, because Qt's RHI
    /// render-node contract requires resource uploads to be issued before the render pass (in prepare()),
    /// leaving only draw commands for render(). The draw commands themselves are issued later by
    /// recordFrameRhi() once the pass is recording.
    /// @param rhi        The scene graph's RHI instance.
    /// @param cb         The command buffer to queue resource updates onto.
    /// @param rt         The render target the pass renders into.
    /// @param rpDesc     The render-pass descriptor the pipelines are baked against.
    /// @param itemToClip The composed projection * node-matrix transform mapping item-local pixels to clip.
    /// @param itemOriginDevice This item's top-left corner inside the render target, in device pixels
    ///                   (top-left origin) — the offset that maps the rasterizer's item-relative inner
    ///                   scissor into render-target coordinates (a split pane is not at the origin).
    void prepareFrameRhi(QRhi* rhi,
                         QRhiCommandBuffer* cb,
                         QRhiRenderTarget* rt,
                         QRhiRenderPassDescriptor* rpDesc,
                         QMatrix4x4 const& itemToClip,
                         QPoint itemOriginDevice);

    /// Records the staged frame's draw commands into the command buffer, called by TerminalRenderNode from
    /// QSGRenderNode::render() — inside the active render pass.
    ///
    /// The geometry/atlas were uploaded during prepareFrameRhi() (the node's prepare() phase); this installs
    /// the node clip (Qt's @p state scissor, only known now the pass records) and issues the
    /// pipeline/viewport/scissor/draw commands so the terminal composites in z-order under popups.
    /// @param state The scene-graph render state, for the node clip rectangle applied to the draws.
    void recordFrameRhi(QSGRenderNode::RenderState const* state);

    /// Releases the OpenGL renderer owned via the scene-graph node. Called by TerminalRenderNode on the
    /// render thread when the node is destroyed (or the scene graph is invalidated), where GL teardown
    /// must happen. Safe to call when no renderer exists.
    void releaseRenderResources();

    // {{{ TerminalDisplay API
    void closeDisplay() final;
    void post(std::function<void()> fn) final;

    /// The OS window this item is in, or nullptr while it is in none.
    ///
    /// Re-declared only so QQuickItem's own (non-virtual) window() answers session::DisplaySurface's
    /// virtual one: without it the two names are ambiguous here and the surface stays abstract. The
    /// answer is QQuickItem's, unchanged.
    [[nodiscard]] QQuickWindow* window() const final { return QQuickItem::window(); }

    /// Announces a caret move to the platform: refreshes the IME cursor rectangle and, when assistive
    /// technology is listening, the accessible caret. The IME update is kept alongside the accessibility
    /// path because e.g. Windows Magnifier follows the IME rectangle rather than a UIA text range.
    ///
    /// GUI THREAD ONLY: QInputMethod::update() is not fire-and-forget — the Wayland backend synchronously
    /// re-queries inputMethodQuery(), which reads the grid — and the terminal-side notification arrives
    /// on the terminal or render thread with the terminal's state mutex possibly already held.
    void reportCursorMoved() final;

    /// Tells assistive technology where the caret is, if it moved somewhere worth announcing.
    ///
    /// GUI THREAD ONLY: the terminal-side notification posts here rather than deciding in place, because
    /// that notification arrives with the terminal's state mutex already held.
    void reportAccessibleCaret();

    /// Forgets what was last announced about the caret, so the next report is made afresh.
    ///
    /// Call when something other than the caret invalidated the client's picture — focus leaving this
    /// pane, most importantly, since a pane that regains focus at an unchanged position would otherwise
    /// stay silent while the client still points at the pane it left.
    void resetAccessibleCaret();

    /// Tells assistive technology that the cell-to-screen mapping moved, so the rectangles it last read
    /// are stale.
    ///
    /// Separate from @ref reportAccessibleCaret on purpose: QAccessible::LocationChanged describes THIS
    /// OBJECT's geometry, which a moving caret does not alter. Emitting it per caret move asked the
    /// platform to re-read our geometry on every keystroke, and told it nothing it did not know.
    ///
    /// Called from the seams where that mapping actually changes: @ref geometryChange (this item resized
    /// or repositioned within its parent) and @ref notifyCellGeometryChanged (cell size, margins or
    /// content scale). NOT covered: a window MOVE, which changes global coordinates while leaving this
    /// item's parent-relative geometry alone — QQuickItem has no hook for it, and it would need
    /// QWindow::x/yChanged wired up in configureScreenHooks().
    ///
    /// Coalesced, so calling it liberally is cheap. GUI THREAD ONLY.
    void reportAccessibleLocation();

    // Attributes
    [[nodiscard]] vtbackend::RefreshRate refreshRate() const final;
    text::DPI fontDPI() const noexcept;
    [[nodiscard]] bool isFullScreen() const;

    [[nodiscard]] vtbackend::ImageSize pixelSize() const final;
    [[nodiscard]] vtbackend::ImageSize cellSize() const final;

    /// The pixel size to tell applications about for a page of @p totalPageSize.
    ///
    /// The one place that answers "what is a cell, as far as an application is concerned": every
    /// Terminal::resizeScreen() caller goes through here. Deliberately NOT pixelSize() -- that adds
    /// margins because it sizes a window, and resizeScreen() divides what it is given by the page to
    /// recover the cell, so margins in that number come back as cell-size error.
    /// @param totalPageSize The page the report is for; callers mid-resize must pass the NEW page,
    ///                      not the terminal's current one.
    [[nodiscard]] vtbackend::ImageSize reportedPixelSize(vtbackend::PageSize totalPageSize) const final;

    /// Device pixels per reported pixel: the divisor taking a device-pixel extent into the unit
    /// `pixel_reporting` selects.
    ///
    /// The one place that decides that unit. Anything an application is told a pixel count in must pass
    /// through here -- an extent left in device pixels while the rest reports logical is not a smaller
    /// number, it is a number in a unit the application does not know it is reading.
    /// @return 1.0 when reporting device pixels, the content scale when reporting logical ones.
    [[nodiscard]] double reportedPixelScale() const final;

    void resizeTerminalToDisplaySize() final;

    // (user requested) actions
    vtbackend::FontDef getFontDef() final;
    void inspect() final;
    void resizeWindow(vtbackend::LineCount, vtbackend::ColumnCount) final;
    void resizeWindow(vtbackend::Width, vtbackend::Height) final;
    void setFonts(vtrasterizer::FontDescriptions fontDescriptions) final;
    bool setFontSize(text::FontSize newFontSize) final;

    /// The font size the renderer has actually loaded (its published font descriptions), which may differ
    /// from a just-requested size while a staged change is pending or after a swallowed font-load failure.
    [[nodiscard]] text::FontSize fontSize() const { return _renderer->fontDescriptions().size; }

    void setMouseCursorShape(input::MouseCursorShape newCursorShape) final;
    void setWindowFullScreen();
    void setWindowMaximized();
    void setWindowNormal();
    void setBlurBehind(bool enable) final;
    void toggleFullScreen() final;
    void toggleTitleBar() final;

    /// Sets this WINDOW's tab bar visibility mode, for as long as the window lives.
    ///
    /// Window state, like the title bar above: a change made from any pane applies to the whole window
    /// and survives pane-focus changes and tab switches. A configuration reload supersedes it, which is
    /// what reloading a configuration means (see WindowController::applyTabBarFromConfig).
    void setTabBarVisibility(config::TabBarVisibility mode) final;

    /// Sets this WINDOW's tab bar placement. @see setTabBarVisibility.
    void setTabBarPosition(config::TabBarPosition position) final;
    void toggleInputMethodEditorHandling() final;
    void setHyperlinkDecoration(vtrasterizer::Decorator normal, vtrasterizer::Decorator hover) final;

    // terminal events
    void scheduleRedraw() final;
    void renderBufferUpdated() final;
    void onSelectionCompleted();
    void bufferChanged(vtbackend::ScreenType) final;
    void discardImage(vtbackend::Image const&) final;
    void setScreenshotOutput(ScreenshotOutput where) final { _saveScreenshot = std::move(where); }
    // }}}

    /// How large glyphs are rasterized, in device pixels per logical pixel.
    ///
    /// Feeds fontDPI() and nothing else. KDE's forceFontDPI replaces the device-pixel ratio here to make
    /// text physically larger; that is a font decision, so it must not leak into geometry.
    [[nodiscard]] double contentScale() const final;

    /// Hardware pixels per logical pixel for this item's window — the ratio Qt's scene graph sizes its
    /// render target and builds its projection with.
    ///
    /// EVERY logical<->device geometry conversion uses this, never contentScale(): the item's device
    /// extent, the page fit, the margins, the mouse/IME mapping and what is reported to applications.
    /// The two are equal unless a forced font DPI is configured. @see devicePixelRatioForWindow.
    [[nodiscard]] double devicePixelRatio() const final;

    Q_INVOKABLE void logDisplayInfo();

    [[nodiscard]] QString profileName() const { return QString::fromStdString(_profileName); }
    void setProfileName(QString const& name) { _profileName = name.toStdString(); }

  protected:
    // {{{ Input handling — kept protected, matching QQuickItem's own visibility for these hooks
    void keyPressEvent(QKeyEvent* keyEvent) override;
    void keyReleaseEvent(QKeyEvent* keyEvent) override;
    void wheelEvent(QWheelEvent* wheelEvent) override;
    void mousePressEvent(QMouseEvent* mousePressEvent) override;
    void mouseReleaseEvent(QMouseEvent* mouseReleaseEvent) override;
    void mouseMoveEvent(QMouseEvent* mouseMoveEvent) override;
    void hoverMoveEvent(QHoverEvent* event) override;
    void hoverLeaveEvent(QHoverEvent* event) override;
    void focusInEvent(QFocusEvent* event) override;
    void focusOutEvent(QFocusEvent* event) override;
#if QT_CONFIG(im)
    void inputMethodEvent(QInputMethodEvent* event) override;
#endif
    bool event(QEvent* event) override;
    // }}}

    void releaseResources() override;

    /// Scene-graph extension point: creates/updates the TerminalRenderNode that draws the terminal in
    /// z-order. On first call it lazily creates the OpenGL renderer (formerly done from the render
    /// signals) and the node; on subsequent calls it marks the node dirty so Qt re-renders. Runs on the
    /// render thread with the GUI thread blocked.
    /// @param oldNode The previously returned node, or nullptr on first call.
    /// @return The TerminalRenderNode for this item (owned by the scene graph).
    QSGNode* updatePaintNode(QSGNode* oldNode, UpdatePaintNodeData* data) override;

    /// One callback per committed item-geometry change (Qt's canonical resize hook, replacing the two
    /// per-axis widthChanged/heightChanged signals): drives the window->grid reflow.
    void geometryChange(QRectF const& newGeometry, QRectF const& oldGeometry) override;

  public Q_SLOTS:
    void onAutoScrollTick();
    void onSceneGrapheInitialized();
    void onBeforeSynchronize();

    void handleWindowChanged(QQuickWindow* newWindow);
    void cleanup();

    void onScrollBarValueChanged(int value);
    void onRefreshRateChanged();
    void applyFontDPI();
    /// (Re)builds the ReGIS text rasterizer from the active profile's font when the font or DPI has
    /// changed, and injects it into the bound session's terminal.
    void updateReGISTextRasterizer();
    void onScreenChanged();
    void doDumpState();

    /// Re-resolves the font DPI after a content-scale change and guarantees the resulting cell metrics
    /// are materialized SYNCHRONOUSLY — even before the render target exists, where applyFontDPI()
    /// alone only stages the reload (the staged apply is CPU-side FreeType work; pattern borrowed from
    /// createRenderer()). Callers may derive geometry from cellSize() immediately afterwards.
    void applyContentScaleChange() final;

  signals:
    void profileNameChanged();
    void titleChanged(QString const&);
    void sessionChanged(session::TerminalSession*);
    void terminalBufferChanged(vtbackend::ScreenType);
    void terminated();
    void showNotification(QString const& title, QString const& body);

  public:
    Q_INVOKABLE [[nodiscard]] int pageLineCount() const noexcept
    {
        if (!_session)
            return 1;
        return unbox(terminal().pageSize().lines);
    }

    /// How far the view can be scrolled up, in rows the user can actually reach.
    ///
    /// Forwarded rather than re-derived, so a QML caller reaching either this or the session's own
    /// accessor is told the same thing about the same viewport -- and the reason it is the scrollable
    /// count rather than the raw history depth is stated once, there.
    Q_INVOKABLE [[nodiscard]] int historyLineCount() const noexcept
    {
        if (!_session)
            return 0;
        return _session->historyLineCount();
    }

  private:
    // helper methods
    //

    /// Blocks until no scene-graph frame is executing render-thread code for this display.
    ///
    /// prepare()/render() run on the render thread with the GUI thread UNBLOCKED, so a null check on a
    /// member that path dereferences is a TOCTOU, not a guard. A NoStage job runs once the current frame
    /// completes; frame N+1 cannot slip in behind it, as its sync phase blocks on THIS thread. No-op
    /// without a window. Costs the remaining time of the in-flight frame, on user-driven transitions
    /// only — never per frame.
    ///
    /// For members whose object we do not own the lifetime of (@ref _session). Where destruction is
    /// itself deferred to a render-thread job (@ref _renderTarget), pin the pointer instead;
    /// @see prepareFrameRhi.
    ///
    /// Do NOT add a hasRenderTarget()/hasSession() early-out: the GUI thread can null the member *after*
    /// a frame passed that same check, so the null says nothing about whether a frame is running. That
    /// is the bug this replaces.
    ///
    /// GUI THREAD ONLY, holding no lock the render thread takes mid-frame (the terminal state mutex, the
    /// renderer's _applyMutex), or this deadlocks.
    void fenceRenderThread() const;

    /// The ONLY writer of @ref _session: fences the render thread out, then stores @p newSession.
    ///
    /// A choke point rather than a rule to remember, since the two writers reach it by different routes
    /// (setSession() rebinds, releaseSession() detaches). GUI THREAD ONLY. Does not emit
    /// sessionChanged() — setSession() emits only once the new session is fully wired up.
    void assignSession(session::TerminalSession* newSession);

    void doDumpStateInternal();

    /// Logs the resolved Qt RHI backend (graphics API + device, and the OpenGL context version when
    /// that backend is in use) once, complementing the "[FYI] Qt platform" configuration line.
    /// Called from the sync phase, where QQuickWindow::rhi() is live (it is still null at
    /// sceneGraphInitialized time).
    void logRhiBackendInfoOnce();

    /// Requests a deferred screenshot of the terminal's rendered output and delivers it as a QImage.
    ///
    /// The RHI captures the terminal into an offscreen texture and reads it back one frame later (a texture
    /// readback only completes after the frame is submitted), so this cannot return synchronously. @p onReady
    /// is invoked (on the render thread, from the frame that completes the readback) with the captured image.
    /// @param onReady Receives the captured QImage once the readback completes.
    void requestScreenshot(std::function<void(QImage)> onReady);

    /// Wraps a raw RGBA8 readback buffer (tightly packed, top-left origin — as RenderTarget's
    /// ScreenshotCallback guarantees) into a QImage that owns a copy of the pixels.
    /// @param rgbaBuffer Tightly-packed RGBA8 pixels, width*height*4 bytes.
    /// @param pixelSize  The image size in pixels.
    /// @return A QImage owning a deep copy of the pixels (safe after @p rgbaBuffer is freed).
    [[nodiscard]] static QImage screenshotImageFromBuffer(std::vector<uint8_t> const& rgbaBuffer,
                                                          vtbackend::ImageSize pixelSize);
    void createRenderer();

    /// Runs the OpenGL terminal render (grid, cursor, decorations) into the scene graph's current target.
    /// Invoked from renderFrame() after the transform and clip have been installed on the renderer. Also
    /// drives the per-frame bookkeeping: consuming a lazily applied font/DPI reconfiguration, taking a
    /// pending screenshot, and scheduling the next frame via update().
    void paint();

    /// Frees the OpenGL renderer (GL resources). Idempotent and shared by every teardown path (the
    /// scene-graph node's releaseResources, sceneGraphInvalidated → cleanup). Must run on the render
    /// thread with the GL context current.
    void destroyRenderer();
    [[nodiscard]] QMatrix4x4 createModelMatrix() const;
    void configureScreenHooks();
    [[nodiscard]] float uptime() const noexcept;

    /// Applies a staged font/DPI reconfiguration synchronously on the GUI thread and re-derives geometry
    /// (page size, margin, the resizeScreen()/SIGWINCH to the child, and the WM size hints) against the
    /// resulting cell size. This is the single policy for all discrete font reconfigurations (size,
    /// family, DPI); see the definition for why these are applied inline rather than deferred to a frame.
    ///
    /// @return true if the cell size changed (and the geometry recompute against it ran). A DPI change
    ///         that rounds to the same cell pixel size returns false but still needs the DPR-derived
    ///         WM size hints refreshed — applyFontDPI() does that unconditionally.
    bool applyStagedFontReconfigNow();

    /// Re-derives the geometry that depends on the cell size after a font/DPI reconfiguration: the
    /// terminal page size + margin (and the resizeScreen()/SIGWINCH to the child) and the WM size hints.
    /// Shared by the synchronous (applyStagedFontReconfigNow) and the deferred frame (paint()) reconfig
    /// paths so the two cannot diverge. Requires a live display.
    void recomputeGeometryAfterFontReconfig();

    /// This display item's extent in DEVICE PIXELS: the item width/height multiplied by the device pixel
    /// ratio. The scene graph supplies the item→clip placement transform at render time, so callers need
    /// only the size — not the scene position the terminal used to derive its own translation.
    struct DevicePixelGeometry
    {
        double width = 0.0;  //!< Item width, in device pixels.
        double height = 0.0; //!< Item height, in device pixels.
    };

    /// @return This item's device-pixel extent (see DevicePixelGeometry).
    [[nodiscard]] DevicePixelGeometry itemDevicePixelGeometry() const;

    /// Window->grid, the ONLY reaction to a size change: floors this item's device-pixel extent to a
    /// grid via the geometry module and applies it (helper::applyResize). Never mutates the QWindow, so
    /// a resize event can never re-enter itself. Shared by geometryChange() and
    /// resizeTerminalToDisplaySize().
    void applyDisplaySizeToGrid();

    /// The OS window this display is shown in (@ref WindowHost — the window-geometry authority), or
    /// nullptr when the display is not registered with a manager (offscreen tests).
    [[nodiscard]] WindowHost* windowHost();

    /// Notifies the window-geometry authority that this display's cell geometry (font/DPI/margins)
    /// changed, so it refreshes the WM size hints. No-op without a controller.
    void notifyCellGeometryChanged();

    void statsSummary();

  public:
    /// A snapshot of the grid's device-pixel geometry (cell size, page margin).
    ///
    /// Public so the accessibility interface can map cells to screen rectangles. Returned BY VALUE, and
    /// callers should take one copy per query: two separate reads could straddle a concurrent font apply
    /// and mix a new cell size with an old margin.
    [[nodiscard]] vtrasterizer::GridMetrics gridMetrics() const noexcept final
    {
        return _renderer->gridMetrics();
    }

  private:
    /// Flags the screen as dirty.
    ///
    /// @returns boolean indicating whether the screen was clean before and made dirty (true), false
    /// otherwise.
    bool setScreenDirty()
    {
#ifdef CONTOUR_PERF_STATS
        stats_.updatesSinceRendering++;
#endif
        return _state.touch();
    }

    // private data fields
    //
    std::string _profileName;
    std::string _programPath;
    session::TerminalSession* _session = nullptr;
    /// The session manager this display is registered with, cached the first time the display learns
    /// of one (focus-in / setSession). Used by ~TerminalDisplay to evict this display from the
    /// manager's per-display bookkeeping even when the session has already been detached (a closed
    /// split pane is destroyed session-less), which a _session-routed call could not reach.
    session::TerminalSessionManager* _manager = nullptr;
    std::chrono::steady_clock::time_point _startTime;
    text::DPI _lastFontDPI {};
    /// The app-wide forced-font-DPI provider (see ContentScale.h), injected in setSession(). Null until
    /// then (and in tests): contentScale() falls back to the window DPR.
    ForcedFontDpiProvider const* _forcedFontDpiProvider = nullptr;
    std::unique_ptr<vtrasterizer::Renderer> _renderer;
    std::shared_ptr<vtrasterizer::ReGISFontRasterizer> _regisTextRasterizer;
    /// The font description and DPI @ref _regisTextRasterizer was built with, so it is rebuilt only
    /// when they change (a font-family change or profile switch), not on every session rebind.
    text::FontDescription _regisTextRasterizerFont {};
    text::DPI _regisTextRasterizerDpi {};
    bool _renderingPressure = false;
    /// The RHI render target. Owned here; released on the render thread (its GPU resources must be)
    /// via destroyRenderer() / CleanupJob, or by ~TerminalDisplay's RAII on a bare-window teardown.
    std::unique_ptr<display::RhiRenderer> _renderTarget;
    /// Shared liveness cell handed to every TerminalRenderNode (see DisplayLiveness): the node's
    /// render-phase callbacks load the display from it, and ~TerminalDisplay publishes null + fences
    /// the render thread, making GUI-thread destruction safe against an in-flight frame.
    DisplayLiveness _nodeLiveness = std::make_shared<std::atomic<TerminalDisplay*>>(this);
    bool _sessionChanged = false;
    bool _rhiBackendLogged = false; ///< One-shot latch for logRhiBackendInfoOnce().
    /// The screen the per-screen hooks (refresh rate, logical DPI) are currently connected to;
    /// re-homed by configureScreenHooks() whenever the window changes screens.
    QScreen* _hookedScreen = nullptr;
    // update() timer used to animate the blinking cursor.
    QTimer _updateTimer;

    // {{{ Auto-scroll state for drag-selection outside window
    QTimer _autoScrollTimer;
    session::AutoScrollInfo _autoScrollState;
    // }}}

    RenderStateManager _state;
    /// Guards against piling up accessible-location posts while a resize or divider drag commits one
    /// geometry change per input event. GUI thread only, hence a plain bool.
    bool _locationReportPending = false;
    bool _doDumpState = false;
    std::optional<std::variant<std::filesystem::path, std::monostate>> _saveScreenshot { std::nullopt };

    vtbackend::LineCount _lastHistoryLineCount = vtbackend::LineCount(0);

    // ======================================================================

#ifdef CONTOUR_PERF_STATS
    struct Stats
    {
        std::atomic<uint64_t> updatesSinceRendering = 0;
        std::atomic<uint64_t> consecutiveRenderCount = 0;
    };
    Stats stats_;
    std::atomic<uint64_t> renderCount_ = 0;
#endif
};

} // namespace contour::display
