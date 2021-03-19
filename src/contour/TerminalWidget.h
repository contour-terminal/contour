/**
 * This file is part of the "contour" project
 *   Copyright (c) 2019-2020 Christian Parpart <christian@parpart.family>
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
#include <contour/FileChangeWatcher.h>
#include <terminal/Color.h>
#include <terminal/Metrics.h>
#include <terminal_view/TerminalView.h>

#include <QtCore/QPoint>
#include <QtCore/QTimer>
#include <QtGui/QOpenGLExtraFunctions>
#include <QtGui/QVector4D>
#include <QtWidgets/QOpenGLWidget>
#include <QtWidgets/QMainWindow>
#include <QtWidgets/QSystemTrayIcon>
#include <QtWidgets/QScrollBar>

#include <atomic>
#include <fstream>
#include <memory>
#include <optional>
#include <vector>

namespace contour {

// It currently just handles one terminal inside, but ideally later it can handle
// multiple terminals in tabbed views as well tiled.
class TerminalWidget :
    public QOpenGLWidget,
    public terminal::view::TerminalView::Events,
    protected QOpenGLExtraFunctions
{
    Q_OBJECT

  public:
    TerminalWidget(config::Config _config,
                   bool _liveConfig,
                   std::string _profileName,
                   std::string _programPath);

    ~TerminalWidget() override;

    static QSurfaceFormat surfaceFormat();

    int pointsToPixels(text::font_size _fontSize) const noexcept;

    void initializeGL() override;
    void resizeGL(int _width, int _height) override;
    void paintGL() override;

    QSize minimumSizeHint() const override;
    QSize sizeHint() const override;

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

    /// Posts given function from terminal thread into the GUI thread.
    void post(std::function<void()> _fn);

    /// Applies given profile, potentially setting/resetting terminal configuration.
    void activateProfile(const std::string& _newProfileName);

    /// Applies given profile, potentially setting/resetting terminal configuration.
    void activateProfile(std::string const& _name, config::TerminalProfile _newProfile);

    terminal::view::TerminalView* view() const noexcept { return terminalView_.get(); }
    terminal::ScreenType screenType() const noexcept { return currentScreenType_; }

  Q_SIGNALS:
    void terminalBufferChanged(TerminalWidget*, terminal::ScreenType);
    void profileChanged(TerminalWidget*);
    void screenUpdated(TerminalWidget*);
    void viewportChanged(TerminalWidget*);
    void terminated(TerminalWidget*);

  public Q_SLOTS:
    void onFrameSwapped();

    void onScrollBarValueChanged(int _value);

  private:
    void createView();
    bool executeAction(actions::Action const& _action);
    bool executeAllActions(std::vector<actions::Action> const& _actions);
    bool executeInput(terminal::MouseEvent const& event);
    void followHyperlink(terminal::HyperlinkInfo const& _hyperlink);
    void scrollToBottomAndRedraw();

    bool fullscreen() const;
    void toggleFullscreen();

    bool setFontSize(text::font_size _fontSize);
    void spawnNewTerminal(std::string const& _profileName);

    void onScreenBufferChanged(terminal::ScreenType _type);

    float contentScale() const;

    bool resetConfig();
    bool reloadConfigWithProfile(std::string const& _profileName);
    bool reloadConfig(config::Config _newConfig, std::string const& _profileName);

    void onConfigReload(FileChangeWatcher::Event /*_event*/);

    void blinkingCursorUpdate();

    void setDefaultCursor();
    void updateCursor();

  private:
    void bell() override;
    void bufferChanged(terminal::ScreenType) override;
    void screenUpdated() override;
    void requestCaptureBuffer(int _absoluteStartLine, int _lineCount) override;
    void setFontDef(terminal::FontDef const& _fontDef) override;
    void copyToClipboard(std::string_view const& _data) override;
    void dumpState() override;
    void doDumpState();
    void notify(std::string_view const& /*_title*/, std::string_view const& /*_body*/) override;
    void reply(std::string_view const& /*_reply*/) override;
    void onClosed() override;
    void onSelectionComplete() override;
    void resizeWindow(int /*_width*/, int /*_height*/, bool /*_unitInPixels*/) override;
    void setWindowTitle(std::string_view const& /*_title*/) override;
    void setTerminalProfile(std::string const& _configProfileName) override;

    bool requestPermission(config::Permission _allowedByConfig, std::string_view _topicText);

  signals:
    void showNotification(QString const& _title, QString const& _body);
    void setBackgroundBlur(bool _enable);

  private:
    /// Declares the screen-dirtiness-vs-rendering state.
    enum class State {
        /// No screen updates and no rendering currently in progress.
        CleanIdle,

        /// Screen updates pending and no rendering currently in progress.
        DirtyIdle,

        /// No screen updates and rendering currently in progress.
        CleanPainting,

        /// Screen updates pending and rendering currently in progress.
        DirtyPainting
    };

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

    /// Flags the screen as dirty.
    ///
    /// @returns boolean indicating whether the screen was clean before and made dirty (true), false otherwise.
    bool setScreenDirty()
    {
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
                        return true;
                    break;
                case State::DirtyIdle:
                case State::DirtyPainting:
                    return false;
            }
        }
    }

    void statsSummary();
    void doResize(crispy::Size _size);
    void setSize(crispy::Size _size);

    config::TerminalProfile const& profile() const { return profile_; }
    config::TerminalProfile& profile() { return profile_; }

    terminal::renderer::GridMetrics const& gridMetrics() const noexcept { return terminalView_->gridMetrics(); }

    // private data fields
    //
    std::chrono::steady_clock::time_point now_;
    config::Config config_;
    std::string profileName_;
    config::TerminalProfile profile_;
    std::string programPath_;
    terminal::renderer::FontDescriptions fonts_;
    std::unique_ptr<terminal::view::TerminalView> terminalView_;
    std::unique_ptr<terminal::renderer::RenderTarget> renderTarget_;
    std::optional<FileChangeWatcher> configFileChangeWatcher_;
    QTimer updateTimer_;                            // update() timer used to animate the blinking cursor.
    std::mutex screenUpdateLock_;
    bool renderingPressure_ = false;
    bool maximizedState_ = false;
    struct Stats {
        std::atomic<uint64_t> updatesSinceRendering = 0;
        std::atomic<uint64_t> consecutiveRenderCount = 0;
    };
    Stats stats_;
#if defined(CONTOUR_VT_METRICS)
    terminal::Metrics terminalMetrics_{};
#endif

    struct {
        std::optional<bool> changeFont;
        std::map<std::string, bool> mapping;
    } rememberedPermissions_;

    // render state cache
    struct {
        terminal::RGBAColor backgroundColor{};
    } renderStateCache_;

    terminal::ScreenType currentScreenType_ = terminal::ScreenType::Main;
};

} // namespace contour
