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
#include <QtMultimedia/QMediaPlayer>
#include <QtQml/QtQml>
#include <QtQuick/QQuickItem>

#include <filesystem>
#include <memory>
#include <optional>
#include <variant>
#if defined(CONTOUR_PERF_STATS)
    #include <atomic>
#endif

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
    void notify(std::string_view /*_title*/, std::string_view /*_body*/);
    void resizeWindow(vtbackend::LineCount, vtbackend::ColumnCount);
    void resizeWindow(vtbackend::Width, vtbackend::Height);
    void setFonts(vtrasterizer::FontDescriptions fontDescriptions);
    bool setFontSize(text::font_size newFontSize);
    bool setPageSize(vtbackend::PageSize newPageSize);
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
        return pageSizeForPixels(availablePixels,
                                 _renderer->gridMetrics().cellSize,
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

    [[nodiscard]] vtrasterizer::GridMetrics const& gridMetrics() const noexcept
    {
        return _renderer->gridMetrics();
    }

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
    std::chrono::steady_clock::time_point _startTime;
    std::chrono::steady_clock::time_point _initialResizeDeadline {};
    text::DPI _lastFontDPI;
#if !defined(__APPLE__) && !defined(_WIN32)
    mutable std::optional<double> _lastReportedContentScale;
#endif
    std::unique_ptr<vtrasterizer::Renderer> _renderer;
    bool _renderingPressure = false;
    display::OpenGLRenderer* _renderTarget = nullptr;
    bool _maximizedState = false;
    bool _sessionChanged = false;
    // update() timer used to animate the blinking cursor.
    QTimer _updateTimer;

    RenderStateManager _state;
    bool _doDumpState = false;
    std::optional<std::variant<std::filesystem::path, std::monostate>> _saveScreenshot { std::nullopt };

    QFileSystemWatcher _filesystemWatcher;
    QMediaPlayer _mediaPlayer;

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
