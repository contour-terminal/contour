#pragma once

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
    TerminalWindow(Config _config, std::string _programPath);
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

  public Q_SLOTS:
    void onFrameSwapped();
    void onScreenChanged(QScreen* _screen);

  private:
    void executeAction(Action const& _action);
    void executeInput(terminal::MouseEvent const& event);

    bool fullscreen() const;
    void toggleFullScreen();

    bool setFontSize(unsigned _fontSize, bool _resizeWindowIfNeeded);
    std::string getClipboardString();
    std::string extractSelectionText();
    void spawnNewTerminal();

    float contentScale() const;

    bool enableBackgroundBlur(bool _enable);
    bool reloadConfigValues();

    void onScreenUpdate();
    void onWindowTitleChanged();
    void onDoResize(unsigned _width, unsigned _height, bool _inPixels);
    void onConfigReload(FileChangeWatcher::Event /*_event*/);
    void onTerminalClosed();

    void connectAndUpdate();

  private:
    std::chrono::steady_clock::time_point now_;
    Config config_;
    std::string programPath_;
    std::ofstream loggingSink_;
    LoggingSink logger_;
    terminal::view::FontManager fontManager_;
    std::reference_wrapper<terminal::view::Font> regularFont_;
    std::unique_ptr<terminal::view::TerminalView> terminalView_;
    FileChangeWatcher configFileChangeWatcher_;
    std::mutex queuedCallsLock_;
    std::deque<std::function<void()>> queuedCalls_;
    QTimer updateTimer_;
    std::atomic<bool> screenDirty_ = true;
    std::atomic<bool> updating_ = false;
    struct Stats {
        std::atomic<uint64_t> updatesSinceRendering = 0;
        std::atomic<uint64_t> updatesSinceLastSwap = 0;
        std::atomic<uint64_t> currentRenderCount = 0;
    };
    Stats stats_;
};

} // namespace contour
