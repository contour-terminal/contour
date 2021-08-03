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
#include <contour/TerminalDisplay.h>
#include <contour/TerminalSession.h>
#include <contour/helper.h>

#include <terminal/Color.h>
#include <terminal/Metrics.h>
#include <terminal/primitives.h>
#include <terminal_renderer/Renderer.h>

#include <QtCore/QPoint>
#include <QtCore/QTimer>
#include <QtGui/QOpenGLExtraFunctions>
#include <QtGui/QVector4D>
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
    #include <QtOpenGLWidgets/QOpenGLWidget>
#else
    #include <QtWidgets/QOpenGLWidget>
#endif
#include <QtWidgets/QMainWindow>
#include <QtWidgets/QSystemTrayIcon>
#include <QtWidgets/QScrollBar>

#include <atomic>
#include <fstream>
#include <memory>
#include <optional>
#include <vector>

namespace contour::opengl {

// It currently just handles one terminal inside, but ideally later it can handle
// multiple terminals in tabbed views as well tiled.
class TerminalWidget :
    public QOpenGLWidget,
    public TerminalDisplay,
    private QOpenGLExtraFunctions
{
    Q_OBJECT

public:
    TerminalWidget(config::TerminalProfile const& _profile,
                   TerminalSession& _session,
                   std::function<void()> _adaptSize,
                   std::function<void(bool)> _enableBackgroundBlur);

    ~TerminalWidget() override;

    // {{{ OpenGL rendering handling
    static QSurfaceFormat surfaceFormat();
    QSize minimumSizeHint() const override;
    QSize sizeHint() const override;
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
    void post(std::function<void()> _fn) override;

    // Attributes
    double refreshRate() const override;
    crispy::Point screenDPI() const override;
    bool isFullScreen() const override;
    terminal::ImageSize pixelSize() const override;
    terminal::ImageSize cellSize() const override;

    // (user requested) actions
    bool requestPermission(config::Permission _allowedByConfig, std::string_view _topicText) override;
    terminal::FontDef getFontDef() override;
    void bell() override;
    void copyToClipboard(std::string_view /*_data*/) override;
    void dumpState() override;
    void notify(std::string_view /*_title*/, std::string_view /*_body*/) override;
    void resizeWindow(terminal::LineCount, terminal::ColumnCount) override;
    void resizeWindow(terminal::Width, terminal::Height) override;
    void setFonts(terminal::renderer::FontDescriptions _fontDescriptions) override;
    bool setFontSize(text::font_size _size) override;
    bool setScreenSize(terminal::PageSize _newScreenSize) override;
    void setMouseCursorShape(MouseCursorShape _shape) override;
    void setTerminalProfile(config::TerminalProfile _profile) override;
    void setWindowTitle(std::string_view /*_title*/) override;
    void setWindowFullScreen() override;
    void setWindowMaximized() override;
    void setWindowNormal() override;
    void setBackgroundBlur(bool _enable) override;
    void toggleFullScreen() override;
    void setHyperlinkDecoration(terminal::renderer::Decorator _normal,
                                terminal::renderer::Decorator _hover) override;
    void setBackgroundOpacity(terminal::Opacity _opacity) override;

    // terminal events
    void scheduleRedraw() override;
    void renderBufferUpdated() override;
    void onClosed() override;
    void onSelectionCompleted() override;
    void bufferChanged(terminal::ScreenType) override;
    void discardImage(terminal::Image const&) override;
    // }}}

    /// Declares the screen-dirtiness-vs-rendering state.
    enum class State {
        CleanIdle,      //!< No screen updates and no rendering currently in progress.
        DirtyIdle,      //!< Screen updates pending and no rendering currently in progress.
        CleanPainting,  //!< No screen updates and rendering currently in progress.
        DirtyPainting   //!< Screen updates pending and rendering currently in progress.
    };

  public Q_SLOTS:
    void onFrameSwapped();
    void onScrollBarValueChanged(int _value);

  signals:
    void terminalBufferChanged(terminal::ScreenType);
    void terminalBufferUpdated();
    void terminated();
    void showNotification(QString const& _title, QString const& _body);

  private:
    // helper methods
    //
    config::TerminalProfile const& profile() const noexcept { return session_.profile(); }
    terminal::Terminal& terminal() noexcept { return session_.terminal(); }
    terminal::PageSize screenSize() const { return screenSizeForPixels(pixelSize(), renderer_.gridMetrics()); }
    void assertInitialized();
    double contentScale() const;
    void blinkingCursorUpdate();
    void updateMinimumSize();

    void statsSummary();
    void doResize(crispy::Size _size);
    terminal::renderer::GridMetrics const& gridMetrics() const noexcept { return renderer_.gridMetrics(); }

    /// Flags the screen as dirty.
    ///
    /// @returns boolean indicating whether the screen was clean before and made dirty (true), false otherwise.
    bool setScreenDirty()
    {
        //(still needed?) terminalView_->terminal().forceRender();
#if defined(CONTOUR_PERF_STATS)
        stats_.updatesSinceRendering++;
#endif
        for (;;)
        {
            auto state = state_.load();
            switch (state)
            {
                case State::CleanIdle:
                    if (state_.compare_exchange_strong(state, State::DirtyIdle))
                        return true;
                    break;
                case State::CleanPainting:
                    if (state_.compare_exchange_strong(state, State::DirtyPainting))
                        return false;
                    break;
                case State::DirtyIdle:
                case State::DirtyPainting:
                    return false;
            }
        }
    }

    // private data fields
    //
    TerminalSession& session_;
    std::function<void()> adaptSize_;
    std::function<void(bool)> enableBackgroundBlur_;
    terminal::renderer::Renderer renderer_;
    std::atomic<bool> initialized_ = false;
    bool renderingPressure_ = false;
    std::unique_ptr<terminal::renderer::RenderTarget> renderTarget_;
    PermissionCache rememberedPermissions_{};
    bool maximizedState_ = false;

    // update() timer used to animate the blinking cursor.
    QTimer updateTimer_;

    /// Defines the current screen-dirtiness-vs-rendering state.
    ///
    /// This is primarily updated by two independant threads, the rendering thread and the I/O
    /// thread.
    /// The rendering thread constantly marks the rendering state CleanPainting whenever it is about
    /// to render and, depending on whether new screen changes happened, in the frameSwapped()
    /// callback either DirtyPainting and continues to rerender or CleanIdle if no changes came in
    /// since last render.
    ///
    /// The I/O thread constantly marks the state dirty whenever new data has arrived,
    /// either DirtyIdle if no painting is currently in progress, DirtyPainting otherwise.
    std::atomic<State> state_ = State::CleanIdle;

    // ======================================================================

#if defined(CONTOUR_PERF_STATS)
    struct Stats {
        std::atomic<uint64_t> updatesSinceRendering = 0;
        std::atomic<uint64_t> consecutiveRenderCount = 0;
    };
    Stats stats_;
    std::atomic<uint64_t> renderCount_ = 0;
#endif
};

} // namespace contour

namespace fmt {
    template <>
    struct formatter<contour::opengl::TerminalWidget::State> {
        using State = contour::opengl::TerminalWidget::State;
        template <typename ParseContext>
        constexpr auto parse(ParseContext& ctx)
        {
            return ctx.begin();
        }
        template <typename FormatContext>
        auto format(State state, FormatContext& ctx)
        {
            switch (state)
            {
                case State::CleanIdle:
                    return format_to(ctx.out(), "clean-idle");
                case State::CleanPainting:
                    return format_to(ctx.out(), "clean-painting");
                case State::DirtyIdle:
                    return format_to(ctx.out(), "dirty-idle");
                case State::DirtyPainting:
                    return format_to(ctx.out(), "dirty-painting");
            }
            return format_to(ctx.out(), "Invalid");
        }
    };
}
