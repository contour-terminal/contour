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
#include <QtCore/QTimer>
#include <QtGui/QOpenGLExtraFunctions>
#include <QtGui/QVector4D>
#include <QtQml/QtQml>
#include <QtQuick/QQuickItem>

#include <filesystem>
#include <memory>
#include <optional>
#include <variant>
#if defined(CONTOUR_PERF_STATS)
    #include <atomic>
#endif

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

  public Q_SLOTS:
    void onAutoScrollTick();
    void onSceneGrapheInitialized();
    void onBeforeSynchronize();
    void onBeforeRendering();
    void paint();

    void handleWindowChanged(QQuickWindow* newWindow);
    void sizeChanged();
    void cleanup();

    void onAfterRendering();
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
    bool _snapPending = false; ///< Guards against redundant snap-to-grid post() calls.
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
