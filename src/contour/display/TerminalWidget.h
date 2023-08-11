/**
 * This file is part of the "contour" project
 *   Copyright (c) 2019-2021 Christian Parpart <christian@parpart.family>
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
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
class TerminalWidget: public QQuickItem
{
    Q_OBJECT
    Q_PROPERTY(TerminalSession* session READ getSessionHelper WRITE setSession NOTIFY sessionChanged)
    Q_PROPERTY(QString profile READ profileName WRITE setProfileName NOTIFY profileNameChanged)
    Q_PROPERTY(QString title READ title NOTIFY titleChanged)
#if QT_VERSION >= QT_VERSION_CHECK(5, 15, 0)
    QML_ELEMENT
#endif

    TerminalSession* getSessionHelper() { return session_; }

  public:
    explicit TerminalWidget(QQuickItem* parent = nullptr);
    ~TerminalWidget() override;

    // {{{ QML property helper
    QString title() const
    {
        if (session_)
            return session_->title();
        else
            return "No session";
    }
    // }}}

    [[nodiscard]] config::TerminalProfile const& profile() const noexcept
    {
        assert(session_ != nullptr);
        return session_->profile();
    }

    [[nodiscard]] terminal::Terminal const& terminal() const noexcept
    {
        assert(session_ != nullptr);
        return session_->terminal();
    }

    [[nodiscard]] terminal::Terminal& terminal() noexcept
    {
        assert(session_ != nullptr);
        return session_->terminal();
    }

    [[nodiscard]] bool hasSession() const noexcept { return session_ != nullptr; }

    // NB: Use TerminalSession.attachDisplay, that one is calling this here.
    void setSession(TerminalSession* newSession);

    [[nodiscard]] TerminalSession& session() noexcept
    {
        assert(session_ != nullptr);
        return *session_;
    }

    [[nodiscard]] terminal::PageSize windowSize() const noexcept;

    // {{{ Input handling
    void keyPressEvent(QKeyEvent* _keyEvent) override;
    void wheelEvent(QWheelEvent* _wheelEvent) override;
    void mousePressEvent(QMouseEvent* _mousePressEvent) override;
    void mouseReleaseEvent(QMouseEvent* _mouseReleaseEvent) override;
    void mouseMoveEvent(QMouseEvent* _mouseMoveEvent) override;
    void hoverMoveEvent(QHoverEvent* event) override;
    void focusInEvent(QFocusEvent* _event) override;
    void focusOutEvent(QFocusEvent* _event) override;
#if QT_CONFIG(im)
    void inputMethodEvent(QInputMethodEvent* _event) override;
    [[nodiscard]] QVariant inputMethodQuery(Qt::InputMethodQuery _query) const override;
#endif
    bool event(QEvent* _event) override;
    // }}}

    // {{{ TerminalDisplay API
    void closeDisplay();
    void post(std::function<void()> _fn);

    // Attributes
    [[nodiscard]] terminal::RefreshRate refreshRate() const;
    text::DPI fontDPI() const noexcept;
    [[nodiscard]] bool isFullScreen() const;

    [[nodiscard]] terminal::ImageSize pixelSize() const;
    [[nodiscard]] terminal::ImageSize cellSize() const;

    // (user requested) actions
    terminal::FontDef getFontDef();
    void copyToClipboard(std::string_view /*_data*/);
    void inspect();
    void notify(std::string_view /*_title*/, std::string_view /*_body*/);
    void resizeWindow(terminal::LineCount, terminal::ColumnCount);
    void resizeWindow(terminal::Width, terminal::Height);
    void setFonts(terminal::rasterizer::FontDescriptions _fontDescriptions);
    bool setFontSize(text::font_size _size);
    bool setPageSize(terminal::PageSize _newPageSize);
    void setMouseCursorShape(MouseCursorShape _shape);
    void setWindowFullScreen();
    void setWindowMaximized();
    void setWindowNormal();
    void setBlurBehind(bool _enable);
    void toggleFullScreen();
    void toggleTitleBar();
    void setHyperlinkDecoration(terminal::rasterizer::Decorator _normal,
                                terminal::rasterizer::Decorator _hover);

    // terminal events
    void scheduleRedraw();
    void renderBufferUpdated();
    void onSelectionCompleted();
    void bufferChanged(terminal::ScreenType);
    void discardImage(terminal::Image const&);
    // }}}

    [[nodiscard]] double contentScale() const;

    Q_INVOKABLE void logDisplayInfo();

    void releaseResources() override;

    QString profileName() const { return QString::fromStdString(profileName_); }
    void setProfileName(QString const& name) { profileName_ = name.toStdString(); }

  public Q_SLOTS:
    void onSceneGrapheInitialized();
    void onBeforeSynchronize();
    void onBeforeRendering();
    void paint();

    void handleWindowChanged(QQuickWindow* win);
    void sizeChanged();
    void cleanup();

    void onAfterRendering();
    void onScrollBarValueChanged(int _value);
    void onRefreshRateChanged();
    void applyFontDPI();
    void onScreenChanged();
    void onDpiConfigChanged();
    void doDumpState();

  signals:
    void profileNameChanged();
    void titleChanged(QString const&);
    void sessionChanged(TerminalSession*);
    void terminalBufferChanged(terminal::ScreenType);
    void terminated();
    void showNotification(QString const& _title, QString const& _body);

  public:
    Q_INVOKABLE [[nodiscard]] int pageLineCount() const noexcept
    {
        if (!session_)
            return 1;
        return unbox<int>(terminal().pageSize().lines);
    }

    Q_INVOKABLE [[nodiscard]] int historyLineCount() const noexcept
    {
        if (!session_)
            return 0;
        return unbox<int>(terminal().currentScreen().historyLineCount());
    }

  private:
    // helper methods
    //
    void createRenderer();
    [[nodiscard]] QMatrix4x4 createModelMatrix() const;
    void configureScreenHooks();
    void watchKdeDpiSetting();
    [[nodiscard]] float uptime() const noexcept;

    [[nodiscard]] terminal::PageSize pageSize() const
    {
        assert(renderer_);
        return pageSizeForPixels(pixelSize(), renderer_->gridMetrics().cellSize);
    }

    void updateSizeProperties();

    void statsSummary();
    void doResize(crispy::Size _size);

    [[nodiscard]] terminal::rasterizer::GridMetrics const& gridMetrics() const noexcept
    {
        return renderer_->gridMetrics();
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
        return state_.touch();
    }

    // private data fields
    //
    std::string profileName_;
    std::string programPath_;
    TerminalSession* session_ = nullptr;
    std::chrono::steady_clock::time_point startTime_;
    text::DPI lastFontDPI_;
    std::unique_ptr<terminal::rasterizer::Renderer> renderer_;
    bool renderingPressure_ = false;
    display::OpenGLRenderer* renderTarget_ = nullptr;
    bool maximizedState_ = false;

    // update() timer used to animate the blinking cursor.
    QTimer updateTimer_;

    RenderStateManager state_;

    QFileSystemWatcher filesystemWatcher_;
    QMediaPlayer mediaPlayer_;

    terminal::LineCount lastHistoryLineCount_ = terminal::LineCount(0);

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
