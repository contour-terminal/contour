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
#include <contour/Audio.h>
#include <contour/Config.h>

#include <vtbackend/Terminal.h>

#include <vtrasterizer/Renderer.h>

#include <crispy/point.h>

#include <QtCore/QFileSystemWatcher>

#include <functional>
#include <thread>

namespace contour
{

namespace display
{
    class TerminalWidget;
}

class ContourGuiApp;

/**
 * Manages a single terminal session (Client, Terminal, Display)
 *
 * This class is designed to be working in:
 * - graphical displays (OpenGL, software rasterized)
 * - text based displays (think of TMUX client)
 * - headless-mode (think of TMUX server)
 */
class TerminalSession: public QObject, public terminal::Terminal::Events
{
    Q_OBJECT

  public:
    /**
     * Constructs a single terminal session.
     *
     * @param _pty a PTY object (can be process, networked, mockup, ...)
     * @param _display fronend display to render the terminal.
     */
    TerminalSession(std::unique_ptr<terminal::Pty> _pty, ContourGuiApp& _app);
    ~TerminalSession() override;

    /// Starts the VT background thread.
    void start();

    /// Initiates termination of this session, regardless of the underlying terminal state.
    void terminate();

    config::Config const& config() const noexcept { return config_; }
    config::TerminalProfile const& profile() const noexcept { return profile_; }

    double contentScale() const noexcept { return contentScale_; }
    void setContentScale(double value) noexcept { contentScale_ = value; }

    terminal::Pty& pty() noexcept { return terminal_.device(); }
    terminal::Terminal& terminal() noexcept { return terminal_; }
    terminal::Terminal const& terminal() const noexcept { return terminal_; }
    terminal::ScreenType currentScreenType() const noexcept { return currentScreenType_; }

    display::TerminalWidget* display() noexcept { return display_; }
    display::TerminalWidget const* display() const noexcept { return display_; }

    void attachDisplay(display::TerminalWidget& display);

    // Terminal::Events
    //
    void requestCaptureBuffer(terminal::LineCount lineCount, bool logical) override;
    void bell() override;
    void bufferChanged(terminal::ScreenType) override;
    void renderBufferUpdated() override;
    void screenUpdated() override;
    terminal::FontDef getFontDef() override;
    void setFontDef(terminal::FontDef const& _fontSpec) override;
    void copyToClipboard(std::string_view _data) override;
    void inspect() override;
    void notify(std::string_view _title, std::string_view _body) override;
    void onClosed() override;
    void pasteFromClipboard(unsigned count, bool strip) override;
    void onSelectionCompleted() override;
    void requestWindowResize(terminal::LineCount, terminal::ColumnCount) override;
    void requestWindowResize(terminal::Width, terminal::Height) override;
    void setWindowTitle(std::string_view _title) override;
    void setTerminalProfile(std::string const& _configProfileName) override;
    void discardImage(terminal::Image const&) override;
    void inputModeChanged(terminal::ViMode mode) override;
    void updateHighlights() override;
    void playSound(terminal::Sequence::Parameters const& params_) override;
    void cursorPositionChanged() override;

    // Input Events
    using Timestamp = std::chrono::steady_clock::time_point;
    void sendKeyPressEvent(terminal::Key _key, terminal::Modifier _modifier, Timestamp _now);
    void sendCharPressEvent(char32_t _value, terminal::Modifier _modifier, Timestamp _now);

    void sendMousePressEvent(terminal::Modifier _modifier,
                             terminal::MouseButton _button,
                             terminal::PixelCoordinate _pixelPosition,
                             Timestamp _now);
    void sendMouseMoveEvent(terminal::Modifier _modifier,
                            terminal::CellLocation _pos,
                            terminal::PixelCoordinate _pixelPosition,
                            Timestamp _now);
    void sendMouseReleaseEvent(terminal::Modifier _modifier,
                               terminal::MouseButton _button,
                               terminal::PixelCoordinate _pixelPosition,
                               Timestamp _now);

    void sendFocusInEvent();
    void sendFocusOutEvent();

    // Actions
    bool operator()(actions::CancelSelection);
    bool operator()(actions::ChangeProfile const&);
    bool operator()(actions::ClearHistoryAndReset);
    bool operator()(actions::CopyPreviousMarkRange);
    bool operator()(actions::CopySelection);
    bool operator()(actions::CreateDebugDump);
    bool operator()(actions::DecreaseFontSize);
    bool operator()(actions::DecreaseOpacity);
    bool operator()(actions::FollowHyperlink);
    bool operator()(actions::FocusNextSearchMatch);
    bool operator()(actions::FocusPreviousSearchMatch);
    bool operator()(actions::IncreaseFontSize);
    bool operator()(actions::IncreaseOpacity);
    bool operator()(actions::NewTerminal const&);
    bool operator()(actions::NoSearchHighlight);
    bool operator()(actions::OpenConfiguration);
    bool operator()(actions::OpenFileManager);
    bool operator()(actions::PasteClipboard);
    bool operator()(actions::PasteSelection);
    bool operator()(actions::Quit);
    bool operator()(actions::ReloadConfig const&);
    bool operator()(actions::ResetConfig);
    bool operator()(actions::ResetFontSize);
    bool operator()(actions::ScreenshotVT);
    bool operator()(actions::ScrollDown);
    bool operator()(actions::ScrollMarkDown);
    bool operator()(actions::ScrollMarkUp);
    bool operator()(actions::ScrollOneDown);
    bool operator()(actions::ScrollOneUp);
    bool operator()(actions::ScrollPageDown);
    bool operator()(actions::ScrollPageUp);
    bool operator()(actions::ScrollToBottom);
    bool operator()(actions::ScrollToTop);
    bool operator()(actions::ScrollUp);
    bool operator()(actions::SearchReverse);
    bool operator()(actions::SendChars const& _event);
    bool operator()(actions::ToggleAllKeyMaps);
    bool operator()(actions::ToggleFullscreen);
    bool operator()(actions::ToggleInputProtection);
    bool operator()(actions::ToggleStatusLine);
    bool operator()(actions::ToggleTitleBar);
    bool operator()(actions::TraceBreakAtEmptyQueue);
    bool operator()(actions::TraceEnter);
    bool operator()(actions::TraceLeave);
    bool operator()(actions::TraceStep);
    bool operator()(actions::ViNormalMode);
    bool operator()(actions::WriteScreen const& _event);

    void scheduleRedraw();

    ContourGuiApp& app() noexcept { return app_; }

    std::chrono::steady_clock::time_point startTime() const noexcept { return startTime_; }

    float uptime() const noexcept
    {
        using namespace std::chrono;
        auto const now = steady_clock::now();
        auto const uptimeMsecs = duration_cast<milliseconds>(now - startTime_).count();
        auto const uptimeSecs = static_cast<float>(uptimeMsecs) / 1000.0f;
        return uptimeSecs;
    }

  signals:
    void sessionClosed(TerminalSession&);

  public slots:
    void onConfigReload();
    void onHighlightUpdate();

  private:
    // helpers
    bool reloadConfig(config::Config _newConfig, std::string const& _profileName);
    int executeAllActions(std::vector<actions::Action> const& _actions);
    bool executeAction(actions::Action const& _action);
    void spawnNewTerminal(std::string const& _profileName);
    void activateProfile(std::string const& _newProfileName);
    bool reloadConfigWithProfile(std::string const& _profileName);
    bool resetConfig();
    void followHyperlink(terminal::HyperlinkInfo const& _hyperlink);
    bool requestPermission(config::Permission _allowedByConfig, std::string_view _topicText);
    void setFontSize(text::font_size _size);
    void setDefaultCursor();
    void configureTerminal();
    void configureCursor(config::CursorConfig const& cursorConfig);
    void configureDisplay();
    uint8_t matchModeFlags() const;
    void flushInput();
    void mainLoop();

    // private data
    //
    std::chrono::steady_clock::time_point startTime_;
    config::Config config_;
    std::string profileName_;
    config::TerminalProfile profile_;
    double contentScale_ = 1.0;
    ContourGuiApp& app_;

    terminal::Terminal terminal_;
    bool terminatedAndWaitingForKeyPress_ = false;
    display::TerminalWidget* display_ = nullptr;

    std::unique_ptr<QFileSystemWatcher> configFileChangeWatcher_;

    bool terminating_ = false;
    std::thread::id mainLoopThreadID_ {};
    std::unique_ptr<std::thread> screenUpdateThread_;

    // state vars
    //
    terminal::ScreenType currentScreenType_ = terminal::ScreenType::Primary;
    terminal::CellLocation currentMousePosition_ = terminal::CellLocation {};
    bool allowKeyMappings_ = true;
    Audio audio;
    std::vector<int> musicalNotesBuffer_;
};

} // namespace contour
