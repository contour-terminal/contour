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
#include <QtQuick/QQuickItem>

#include <QtQml>
#include <atomic>
#include <fstream>
#include <memory>
#include <optional>
#include <vector>

#include <qqml.h>

namespace contour::display
{

class OpenGLRenderer;

// It currently just handles one terminal inside, but ideally later it can handle
// multiple terminals in tabbed views as well tiled.
class TerminalDisplay: public QQuickItem
{
    Q_OBJECT
    Q_PROPERTY(TerminalSession* session READ getSessionHelper WRITE setSession NOTIFY sessionChanged)
    Q_PROPERTY(QString profile READ profileName WRITE setProfileName NOTIFY profileNameChanged)
    Q_PROPERTY(QString title READ title NOTIFY titleChanged)
#if QT_VERSION >= QT_VERSION_CHECK(5, 15, 0)
    QML_ELEMENT
#endif

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

    // NB: Use TerminalSession.attachDisplay, that one is calling this here.
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
    void setHyperlinkDecoration(vtrasterizer::Decorator normal, vtrasterizer::Decorator hover);

    // terminal events
    void scheduleRedraw();
    void renderBufferUpdated();
    void onSelectionCompleted();
    void bufferChanged(vtbackend::ScreenType);
    void discardImage(vtbackend::Image const&);
    // }}}

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

  private:
    // helper methods
    //
    void doDumpStateInternal();
    void createRenderer();
    [[nodiscard]] QMatrix4x4 createModelMatrix() const;
    void configureScreenHooks();
    void watchKdeDpiSetting();
    [[nodiscard]] float uptime() const noexcept;

    [[nodiscard]] vtbackend::PageSize pageSize() const
    {
        assert(_renderer);
        return pageSizeForPixels(pixelSize(), _renderer->gridMetrics().cellSize);
    }

    void updateMinimumSize();
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
    text::DPI _lastFontDPI;
    std::unique_ptr<vtrasterizer::Renderer> _renderer;
    bool _renderingPressure = false;
    display::OpenGLRenderer* _renderTarget = nullptr;
    bool _maximizedState = false;

    // update() timer used to animate the blinking cursor.
    QTimer _updateTimer;

    RenderStateManager _state;
    bool _doDumpState = false;

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
