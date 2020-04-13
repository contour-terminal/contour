#pragma once

#include <contour/Actions.h>
#include <contour/Config.h>
#include <contour/FileChangeWatcher.h>
#include <terminal_view/TerminalView.h>

#include <QOpenGLWindow>
#include <QOpenGLFunctions>
#include <QTimer>

#include <atomic>
#include <fstream>
#include <memory>

namespace contour {

// XXX Maybe just now a main window and maybe later just a TerminalWindow.
//
// It currently just handles one terminal inside, but ideally later it can handle
// multiple terminals in tabbed views as well tiled.
class TerminalWindow :
    public QOpenGLWindow,
    protected QOpenGLFunctions
{
    Q_OBJECT

  public:
    TerminalWindow(config::Config _config, std::string _profileName, std::string _programPath);
    ~TerminalWindow() override;

    void initializeGL() override;
    void paintGL() override;

    void resizeEvent(QResizeEvent* _event) override;

    void keyPressEvent(QKeyEvent* _keyEvent) override;
    void wheelEvent(QWheelEvent* _wheelEvent) override;
    void mousePressEvent(QMouseEvent* _mousePressEvent) override;
    void mouseReleaseEvent(QMouseEvent* _mouseReleaseEvent) override;
    void mouseMoveEvent(QMouseEvent* _mouseMoveEvent) override;

    void focusInEvent(QFocusEvent* _event) override;
    void focusOutEvent(QFocusEvent* _event) override;

    bool event(QEvent* _event) override;

    void post(std::function<void()> _fn);

    /// Applies given profile, potentially setting/resetting terminal configuration.
    void setProfile(config::TerminalProfile _newProfile);

  public Q_SLOTS:
    void onFrameSwapped();
    void onScreenChanged(QScreen* _screen);

  private:
    void executeAction(actions::Action const& _action);
    void executeInput(terminal::MouseEvent const& event);

    bool fullscreen() const;
    void toggleFullScreen();

    bool setFontSize(unsigned _fontSize);
    void onSelectionComplete();
    std::string extractSelectionText();
    void spawnNewTerminal(std::string const& _profileName);

    void onScreenBufferChanged(terminal::ScreenBuffer::Type _type);
    void onBell();

    float contentScale() const;

    bool enableBackgroundBlur(bool _enable);
    bool reloadConfigValues();

    void onScreenUpdate();
    void onWindowTitleChanged();
    void onDoResize(unsigned _width, unsigned _height, bool _inPixels);
    void onConfigReload(FileChangeWatcher::Event /*_event*/);
    void onTerminalClosed();

    void blinkingCursorUpdate();

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

    config::TerminalProfile const& profile() const { return profile_; }
    config::TerminalProfile& profile() { return profile_; }

    std::chrono::steady_clock::time_point now_;
    config::Config config_;
    std::string profileName_;
    config::TerminalProfile profile_;
    std::string programPath_;
    std::ofstream loggingSink_;
    LoggingSink logger_;
    terminal::view::FontManager fontManager_;
    std::reference_wrapper<terminal::view::Font> regularFont_;
    std::unique_ptr<terminal::view::TerminalView> terminalView_;
    FileChangeWatcher configFileChangeWatcher_;
    std::mutex queuedCallsLock_;
    std::deque<std::function<void()>> queuedCalls_;
    QTimer updateTimer_;                            // update() timer used to animate the blinking cursor.
    std::mutex screenUpdateLock_;
    struct Stats {
        std::atomic<uint64_t> updatesSinceRendering = 0;
        std::atomic<uint64_t> consecutiveRenderCount = 0;
    };
    Stats stats_;
};

} // namespace contour
