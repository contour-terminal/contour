// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <contour/Actions.h>
#include <contour/Config.h>
#include <contour/TerminalSession.h>
#include <contour/helper.h>

#include <vtbackend/Color.h>
#include <vtbackend/Metrics.h>
#include <vtbackend/primitives.h>

#include <vtrasterizer/Renderer.h>

#include <crispy/deferred.h>

#include <QtCore/QFileSystemWatcher>
#include <QtCore/QPoint>
#include <QtCore/QSize>
#include <QtCore/QTimer>
#include <QtGui/QOpenGLExtraFunctions>
#include <QtGui/QVector4D>
#include <QtQml/QtQml>
#include <QtQuick/QQuickItem>
#include <QtQuick/QSGRenderNode>

#include <filesystem>
#include <memory>
#include <optional>
#include <variant>
#if defined(CONTOUR_PERF_STATS)
    #include <atomic>
#endif

QT_BEGIN_NAMESPACE
class QRhi;
class QRhiCommandBuffer;
class QRhiRenderTarget;
class QRhiRenderPassDescriptor;
QT_END_NAMESPACE

namespace contour
{
class TerminalSessionManager;
}

namespace contour::display
{

class OpenGLRenderer;

// It currently can handles multiple terminals inside via tabs support.
// that is managed by TerminalSessionManager.
class TerminalDisplay: public QQuickItem
{
    Q_OBJECT
    Q_PROPERTY(TerminalSession* session READ getSessionHelper WRITE setSession NOTIFY sessionChanged)
    Q_PROPERTY(QString profile READ profileName WRITE setProfileName NOTIFY profileNameChanged)
    Q_PROPERTY(QString title READ title NOTIFY titleChanged)
    /// Whether the custom (client-side) title bar is shown. Initialized from the profile's
    /// show_title_bar setting on session attach and flipped by the ToggleTitleBar action; main.qml
    /// binds the custom TitleBar's visibility to it (the window stays frameless either way).
    Q_PROPERTY(
        bool titleBarVisible READ titleBarVisible WRITE setTitleBarVisible NOTIFY titleBarVisibleChanged)
    QML_ELEMENT

    TerminalSession* getSessionHelper() { return _session; }

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

    /// @return Whether the custom title bar is currently shown.
    [[nodiscard]] bool titleBarVisible() const noexcept { return _titleBarVisible; }

    /// Sets the custom title bar's visibility and emits titleBarVisibleChanged() on a change.
    ///
    /// On macOS the window keeps its NATIVE frame (main.qml uses Qt.Window there, not frameless,
    /// because client-side decoration is not the platform convention), so there is no custom bar to
    /// hide — this also toggles the native frame's Qt::FramelessWindowHint so show_title_bar:false
    /// actually removes the title bar on macOS. On the other platforms the window is QML-frameless and
    /// the custom TitleBar is what this controls; the native frame is left to main.qml's flags binding.
    /// @param visible The new visibility.
    void setTitleBarVisible(bool visible);

    /// Applies the native window-frame decoration for @p visible by toggling Qt::FramelessWindowHint
    /// on the attached window: shown => keep the native frame, hidden => frameless so the custom
    /// client-side TitleBar is the only decoration. This is the C++ counterpart of main.qml's `flags`
    /// binding and applies on EVERY platform (not macOS-only). No-op when no window is attached yet
    /// (the frame is re-applied from setSession() once a window exists).
    /// @param visible Whether the title bar (native frame) should be shown.
    void applyNativeTitleBar(bool visible);
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

    [[nodiscard]] bool hasSession() const noexcept { return _session != nullptr; }

    // NB: Use TerminalSession.attachDisplay, that one is calling this here. TODO(PR) ?
    void setSession(TerminalSession* newSession);

    /// Clears this display's session pointer without notifying the session. Called by
    /// TerminalSession::attachDisplay when another display takes over the session, so a stale
    /// display (e.g. the hidden single-pane view after a split) stops believing it is attached.
    /// Unlike setSession(nullptr) it does not call back into detachDisplay(), avoiding recursion
    /// and the detach precondition while the session is already reassigning its display.
    void releaseSession();

    [[nodiscard]] TerminalSession& session() noexcept
    {
        assert(_session != nullptr);
        return *_session;
    }

    [[nodiscard]] vtbackend::PageSize windowSize() const noexcept;

    // {{{ Input handling
    void keyPressEvent(QKeyEvent* keyEvent) override;
    void keyReleaseEvent(QKeyEvent* keyEvent) override;
    void wheelEvent(QWheelEvent* wheelEvent) override;
    void mousePressEvent(QMouseEvent* mousePressEvent) override;
    void mouseReleaseEvent(QMouseEvent* mouseReleaseEvent) override;
    void mouseMoveEvent(QMouseEvent* mouseMoveEvent) override;
    void hoverMoveEvent(QHoverEvent* event) override;
    void focusInEvent(QFocusEvent* event) override;
    void focusOutEvent(QFocusEvent* event) override;
#if QT_CONFIG(im)
    void inputMethodEvent(QInputMethodEvent* event) override;
    [[nodiscard]] QVariant inputMethodQuery(Qt::InputMethodQuery query) const override;
#endif
    bool event(QEvent* event) override;
    // }}}

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
    void prepareFrameRhi(QRhi* rhi,
                         QRhiCommandBuffer* cb,
                         QRhiRenderTarget* rt,
                         QRhiRenderPassDescriptor* rpDesc,
                         QMatrix4x4 const& itemToClip);

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
    void closeDisplay();
    void post(std::function<void()> fn);

    // Attributes
    [[nodiscard]] vtbackend::RefreshRate refreshRate() const;
    text::DPI fontDPI() const noexcept;
    [[nodiscard]] bool isFullScreen() const;

    [[nodiscard]] vtbackend::ImageSize pixelSize() const;
    [[nodiscard]] vtbackend::ImageSize cellSize() const;

    void resizeTerminalToDisplaySize();

    // (user requested) actions
    vtbackend::FontDef getFontDef();
    static void copyToClipboard(std::string_view /*_data*/);
    void inspect();
    void resizeWindow(vtbackend::LineCount, vtbackend::ColumnCount);
    void resizeWindow(vtbackend::Width, vtbackend::Height);
    void setFonts(vtrasterizer::FontDescriptions fontDescriptions);
    bool setFontSize(text::font_size newFontSize);

    /// The font size the renderer has actually loaded (its published font descriptions), which may differ
    /// from a just-requested size while a staged change is pending or after a swallowed font-load failure.
    [[nodiscard]] text::font_size fontSize() const { return _renderer->fontDescriptions().size; }

    bool setPageSize(vtbackend::PageSize newPageSize);

    /// Pushes the full geometry (page size, render-surface pixel size and margin) into the renderer.
    ///
    /// For callers that resize the terminal directly (bypassing setPageSize()/applyResize()), so the
    /// renderer's grid metrics — including the margin — stay consistent with the terminal's. Does not
    /// resize the terminal itself.
    ///
    /// @param totalPageSize  the terminal's total page size (including the status line) to publish.
    /// @param pixelSize      the render-surface size in pixels to publish.
    void syncRendererGeometry(vtbackend::PageSize totalPageSize, vtbackend::ImageSize pixelSize);

    void setMouseCursorShape(MouseCursorShape newCursorShape);
    void setWindowFullScreen();
    void setWindowMaximized();
    void setWindowNormal();
    void setBlurBehind(bool enable);
    void toggleFullScreen();
    void toggleTitleBar();
    void toggleInputMethodEditorHandling();
    void setHyperlinkDecoration(vtrasterizer::Decorator normal, vtrasterizer::Decorator hover);

    // terminal events
    void scheduleRedraw();
    void renderBufferUpdated();
    void onSelectionCompleted();
    void bufferChanged(vtbackend::ScreenType);
    void discardImage(vtbackend::Image const&);
    void setScreenshotOutput(auto&& where) { _saveScreenshot = std::forward<decltype(where)>(where); }
    // }}}

    [[nodiscard]] std::optional<double> queryContentScaleOverride() const;
    [[nodiscard]] double contentScale() const;

    Q_INVOKABLE void logDisplayInfo();

    void releaseResources() override;

    [[nodiscard]] QString profileName() const { return QString::fromStdString(_profileName); }
    void setProfileName(QString const& name) { _profileName = name.toStdString(); }

  protected:
    /// Scene-graph extension point: creates/updates the TerminalRenderNode that draws the terminal in
    /// z-order. On first call it lazily creates the OpenGL renderer (formerly done from the render
    /// signals) and the node; on subsequent calls it marks the node dirty so Qt re-renders. Runs on the
    /// render thread with the GUI thread blocked.
    /// @param oldNode The previously returned node, or nullptr on first call.
    /// @return The TerminalRenderNode for this item (owned by the scene graph).
    QSGNode* updatePaintNode(QSGNode* oldNode, UpdatePaintNodeData* data) override;

  public Q_SLOTS:
    void onAutoScrollTick();
    void onSceneGrapheInitialized();
    void onBeforeSynchronize();

    void handleWindowChanged(QQuickWindow* newWindow);
    void sizeChanged();
    void cleanup();

    void onScrollBarValueChanged(int value);
    void onRefreshRateChanged();
    void applyFontDPI();
    void onScreenChanged();
    void onDpiConfigChanged();
    void doDumpState();

  signals:
    void profileNameChanged();
    void titleChanged(QString const&);
    void sessionChanged(TerminalSession*);
    void titleBarVisibleChanged();
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

    Q_INVOKABLE [[nodiscard]] int historyLineCount() const noexcept
    {
        if (!_session)
            return 0;
        return unbox(terminal().currentScreen().historyLineCount());
    }

    [[nodiscard]] vtbackend::PageSize calculatePageSize() const
    {
        assert(_renderer);
        assert(_session);

        // auto const availablePixels = gridMetrics().cellSize * _session->terminal().pageSize();
        auto const availablePixels = pixelSize();
        // Use the lock-free publishedCellSize() for the divisor — the same accessor pixelSize() uses for
        // the dividend — so both reads resolve from one source (and one atomic load that cannot tear
        // against a concurrent render-thread font apply), rather than mixing it with the mutex-guarded
        // gridMetrics().cellSize.
        return pageSizeForPixels(availablePixels,
                                 _renderer->publishedCellSize(),
                                 applyContentScale(_session->profile().margins.value(), contentScale()));
    }

  private:
    // helper methods
    //
    void doDumpStateInternal();
    QImage screenshot();
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
    void watchKdeDpiSetting();
    [[nodiscard]] float uptime() const noexcept;

    /// Applies a staged font/DPI reconfiguration synchronously on the GUI thread and re-derives geometry
    /// (page size, implicit size, constraints, and the resizeScreen()/SIGWINCH to the child) against the
    /// resulting cell size. This is the single policy for all discrete font reconfigurations (size,
    /// family, DPI); see the definition for why these are applied inline rather than deferred to a frame.
    ///
    /// @return true if the cell size changed (and the geometry recompute against it ran). A DPI change
    ///         that rounds to the same cell pixel size returns false but still needs the DPR-derived
    ///         implicit size / size constraints recomputed — applyFontDPI() does that unconditionally.
    bool applyStagedFontReconfigNow();

    /// Re-derives the geometry that depends on the cell size after a font/DPI reconfiguration: the
    /// terminal page size + margin (and the resizeScreen()/SIGWINCH to the child) and the Qt window's
    /// implicit size + size constraints. Shared by the synchronous (applyStagedFontReconfigNow) and the
    /// deferred frame (paint()) reconfig paths so the two cannot diverge. Requires a live display.
    void recomputeGeometryAfterFontReconfig();

    /// This display item's extent in DEVICE PIXELS: the item width/height multiplied by the device pixel
    /// ratio. The scene graph supplies the item→clip placement transform at render time, so callers need
    /// only the size and dpr — not the scene position the terminal used to derive its own translation.
    struct DevicePixelGeometry
    {
        double dpr = 1.0;    //!< The device pixel ratio used for the scaling.
        double width = 0.0;  //!< Item width, in device pixels.
        double height = 0.0; //!< Item height, in device pixels.
    };

    /// @return This item's device-pixel extent (see DevicePixelGeometry).
    [[nodiscard]] DevicePixelGeometry itemDevicePixelGeometry() const;

    /// The window "chrome" offset: the window size minus this display item's size, i.e. the space
    /// taken by the custom title bar (and any other decoration) that sits OUTSIDE the terminal item.
    /// Window-level operations (snap-to-grid, min/base size, the initial size correction) describe the
    /// WINDOW, so they add this back to terminal-derived sizes; dropping it makes the window lose the
    /// title-bar height a little more on every frame. Rounded to whole pixels and clamped to >= 0 so
    /// every caller uses the SAME rounding (previously one path kept a double + ceil while others
    /// lround'd, which could disagree by a pixel at fractional DPI). Returns a zero offset when no
    /// window is attached.
    /// @return The (width, height) chrome offset in virtual pixels.
    [[nodiscard]] QSize chromeSize() const noexcept;

    /// Updates all window size constraints: minimum size, base size, and size increment.
    /// Configures the window manager to constrain user resizes to exact cell boundaries.
    void updateSizeConstraints();

    // Updates the recommended size in (virtual pixels) based on:
    // - the grid cell size (based on the current font size and DPI),
    // - configured window margins, and
    // - the terminal's screen size.
    void updateImplicitSize();

    void statsSummary();
    void doResize(crispy::size size);

    [[nodiscard]] vtrasterizer::GridMetrics gridMetrics() const noexcept { return _renderer->gridMetrics(); }

    /// Flags the screen as dirty.
    ///
    /// @returns boolean indicating whether the screen was clean before and made dirty (true), false
    /// otherwise.
    bool setScreenDirty()
    {
#if defined(CONTOUR_PERF_STATS)
        stats_.updatesSinceRendering++;
#endif
        return _state.touch();
    }

    // private data fields
    //
    std::string _profileName;
    std::string _programPath;
    TerminalSession* _session = nullptr;
    /// The session manager this display is registered with, cached the first time the display learns
    /// of one (focus-in / setSession). Used by ~TerminalDisplay to evict this display from the
    /// manager's per-display bookkeeping even when the session has already been detached (a closed
    /// split pane is destroyed session-less), which a _session-routed call could not reach.
    TerminalSessionManager* _manager = nullptr;
    std::chrono::steady_clock::time_point _startTime;
    std::chrono::steady_clock::time_point _initialResizeDeadline {};
    double _lastVirtualWidth {};
    double _lastVirtualHeight {};
    // Implicit (configured) size snapshot taken in createRenderer(). The stale-
    // geometry correction in sizeChanged() only applies when the implicit size has
    // since changed (i.e. a DPR correction actually happened) — otherwise the saved
    // _lastVirtual* size is legitimate (e.g. a tiling WM's tile) and must be kept.
    double _initialImplicitWidth {};
    double _initialImplicitHeight {};
    text::DPI _lastFontDPI;
#if !defined(__APPLE__) && !defined(_WIN32)
    mutable std::optional<double> _lastReportedContentScale;
#endif
    std::unique_ptr<vtrasterizer::Renderer> _renderer;
    bool _renderingPressure = false;
    display::OpenGLRenderer* _renderTarget = nullptr;
    bool _maximizedState = false;
    bool _titleBarVisible = true; ///< Whether the native window frame is shown (show_title_bar);
                                  ///< (re)initialized from the profile. false => frameless + custom CSD.
    bool _snapPending = false;    ///< Guards against redundant snap-to-grid post() calls.
    bool _sessionChanged = false;
    // update() timer used to animate the blinking cursor.
    QTimer _updateTimer;

    // {{{ Auto-scroll state for drag-selection outside window
    QTimer _autoScrollTimer;
    AutoScrollInfo _autoScrollState;
    // }}}

    RenderStateManager _state;
    bool _doDumpState = false;
    std::optional<std::variant<std::filesystem::path, std::monostate>> _saveScreenshot { std::nullopt };

    QFileSystemWatcher _filesystemWatcher;

    vtbackend::LineCount _lastHistoryLineCount = vtbackend::LineCount(0);

    // ======================================================================

#if defined(CONTOUR_PERF_STATS)
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
