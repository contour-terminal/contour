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
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
    #include <QtOpenGLWidgets/QOpenGLWidget>
#else
    #include <QtWidgets/QOpenGLWidget>
#endif
#include <QtMultimedia/QMediaPlayer>
#include <QtWidgets/QMainWindow>
#include <QtWidgets/QScrollBar>
#include <QtWidgets/QSystemTrayIcon>

#include <atomic>
#include <fstream>
#include <memory>
#include <optional>
#include <vector>

namespace contour::display
{

class OpenGLRenderer;

// It currently just handles one terminal inside, but ideally later it can handle
// multiple terminals in tabbed views as well tiled.
class TerminalWidget: public QOpenGLWidget, private QOpenGLExtraFunctions
{
    Q_OBJECT

  public:
    TerminalWidget();

    ~TerminalWidget() override;

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
    void setSession(TerminalSession& newSession);

    [[nodiscard]] TerminalSession& session() noexcept
    {
        assert(session_ != nullptr);
        return *session_;
    }

    [[nodiscard]] terminal::PageSize windowSize() const noexcept;

    // {{{ OpenGL rendering handling
    [[nodiscard]] QSize minimumSizeHint() const override;
    [[nodiscard]] QSize sizeHint() const override;
    void initializeGL() override;
    void resizeGL(int _width, int _height) override;
    void paintGL() override;
    // }}}

    // {{{ Input handling
    void keyPressEvent(QKeyEvent* _keyEvent) override;
    void wheelEvent(QWheelEvent* _wheelEvent) override;
    void mousePressEvent(QMouseEvent* _mousePressEvent) override;
    void mouseReleaseEvent(QMouseEvent* _mouseReleaseEvent) override;
    void mouseMoveEvent(QMouseEvent* _mouseMoveEvent) override;
    void focusInEvent(QFocusEvent* _event) override;
    void focusOutEvent(QFocusEvent* _event) override;
    void inputMethodEvent(QInputMethodEvent* _event) override;
    QVariant inputMethodQuery(Qt::InputMethodQuery _query) const override;
    bool event(QEvent* _event) override;
    // }}}

    // {{{ TerminalDisplay API
    void closeDisplay();
    void post(std::function<void()> _fn);

    // Attributes
    [[nodiscard]] terminal::RefreshRate refreshRate() const;
    text::DPI fontDPI() const noexcept;
    text::DPI logicalDPI() const noexcept;
    text::DPI physicalDPI() const noexcept;
    [[nodiscard]] bool isFullScreen() const;

    [[nodiscard]] terminal::ImageSize pixelSize() const;
    [[nodiscard]] terminal::ImageSize cellSize() const;

    // (user requested) actions
    bool requestPermission(config::Permission _allowedByConfig, std::string_view _topicText);
    terminal::FontDef getFontDef();
    void bell();
    void copyToClipboard(std::string_view /*_data*/);
    void inspect();
    void doDumpState();
    void notify(std::string_view /*_title*/, std::string_view /*_body*/);
    void resizeWindow(terminal::LineCount, terminal::ColumnCount);
    void resizeWindow(terminal::Width, terminal::Height);
    void setFonts(terminal::rasterizer::FontDescriptions _fontDescriptions);
    bool setFontSize(text::font_size _size);
    bool setPageSize(terminal::PageSize _newPageSize);
    void setMouseCursorShape(MouseCursorShape _shape);
    void setWindowTitle(std::string_view /*_title*/);
    void setWindowFullScreen();
    void setWindowMaximized();
    void setWindowNormal();
    void setBlurBehind(bool _enable);
    void setBackgroundImage(std::shared_ptr<terminal::BackgroundImage const> const& backgroundImage);
    void toggleFullScreen();
    void toggleTitleBar();
    void setHyperlinkDecoration(terminal::rasterizer::Decorator _normal,
                                terminal::rasterizer::Decorator _hover);
    void setBackgroundOpacity(terminal::Opacity _opacity);

    // terminal events
    void scheduleRedraw();
    void renderBufferUpdated();
    void onSelectionCompleted();
    void bufferChanged(terminal::ScreenType);
    void discardImage(terminal::Image const&);
    // }}}

    [[nodiscard]] double contentScale() const;

    void logDisplayTopInfo();
    void logDisplayInfo();

  public Q_SLOTS:
    void onFrameSwapped();
    void onScrollBarValueChanged(int _value);
    void onRefreshRateChanged();
    void applyFontDPI();
    void onScreenChanged();
    void onDpiConfigChanged();

  signals:
    void displayInitialized();
    void enableBlurBehind(bool);
    void profileNameChanged();
    void terminalBufferChanged(terminal::ScreenType);
    void terminalBufferUpdated();
    void terminated();
    void showNotification(QString const& _title, QString const& _body);

  private:
    // helper methods
    //
    void configureScreenHooks();
    void watchKdeDpiSetting();
    void initializeRenderer();
    [[nodiscard]] float uptime() const noexcept;

    [[nodiscard]] terminal::PageSize pageSize() const
    {
        return pageSizeForPixels(pixelSize(), renderer_->gridMetrics().cellSize);
    }

    void updateMinimumSize();

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
    std::unique_ptr<terminal::rasterizer::RenderTarget> renderTarget_;
    PermissionCache rememberedPermissions_ {};
    bool maximizedState_ = false;
    bool framelessWidget_ = false;

    // update() timer used to animate the blinking cursor.
    QTimer updateTimer_;

    RenderStateManager state_;

    QFileSystemWatcher filesystemWatcher_;
    QMediaPlayer mediaPlayer_;

#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
    QAudioOutput* audioOutput_ = nullptr;
#endif

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
