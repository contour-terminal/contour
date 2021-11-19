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

#include <contour/Controller.h>
#include <contour/Config.h>
#include <contour/TerminalDisplay.h>
#include <contour/FileChangeWatcher.h>

#include <terminal/Terminal.h>

#include <terminal_renderer/Renderer.h>

#include <crispy/point.h>

#include <functional>

namespace contour {

/**
 * Manages a single terminal session (Client, Terminal, Display)
 *
 * This class is designed to be working in:
 * - graphical displays (OpenGL, software rasterized)
 * - text based displays (think of TMUX client)
 * - headless-mode (think of TMUX server)
 */
class TerminalSession: public terminal::Terminal::Events
{
  public:
    /**
     * Constructs a single terminal session.
     *
     * @param _pty a PTY object (can be process, networked, mockup, ...)
     * @param _display fronend display to render the terminal.
     */
    TerminalSession(std::unique_ptr<terminal::Pty> _pty,
                    std::chrono::seconds _earlyExitThreshold,
                    config::Config _config,
                    bool _liveconfig,
                    std::string _profileName,
                    std::string _programPath,
                    Controller& _controller,
                    std::unique_ptr<TerminalDisplay> _display,
                    std::function<void()> _displayInitialized,
                    std::function<void()> _onExit);
    ~TerminalSession();

    /// Starts the VT background thread.
    void start();

    /// Initiates termination of this session, regardless of the underlying terminal state.
    void terminate();

    config::Config const& config() const noexcept { return config_; }
    config::TerminalProfile const& profile() const noexcept { return profile_; }

    terminal::Pty& pty() noexcept { return *pty_; }
    terminal::Terminal& terminal() noexcept { return terminal_; }
    terminal::Terminal const& terminal() const noexcept { return terminal_; }
    terminal::ScreenType currentScreenType() const noexcept { return currentScreenType_; }

    TerminalDisplay* display() noexcept { return display_.get(); }
    TerminalDisplay const* display() const noexcept { return display_.get(); }
    void setDisplay(std::unique_ptr<TerminalDisplay> _display);
    void displayInitialized();

    // Terminal::Events
    //
    void requestCaptureBuffer(int _absoluteStartLine, int _lineCount) override;
    void bell() override;
    void bufferChanged(terminal::ScreenType) override;
    void renderBufferUpdated() override;
    void screenUpdated() override;
    terminal::FontDef getFontDef() override;
    void setFontDef(terminal::FontDef const& _fontSpec) override;
    void copyToClipboard(std::string_view _data) override;
    void dumpState() override;
    void notify(std::string_view _title, std::string_view _body) override;
    void onClosed() override;
    void onSelectionCompleted() override;
    void resizeWindow(terminal::LineCount, terminal::ColumnCount) override;
    void resizeWindow(terminal::Width, terminal::Height) override;
    void setWindowTitle(std::string_view _title) override;
    void setTerminalProfile(std::string const& _configProfileName) override;
    void discardImage(terminal::Image const&) override;

    // Input Events
    using Timestamp = std::chrono::steady_clock::time_point;
    void sendKeyPressEvent(terminal::Key _key, terminal::Modifier _modifier, Timestamp _now);
    void sendCharPressEvent(char32_t _value, terminal::Modifier _modifier, Timestamp _now);
    void sendMousePressEvent(terminal::MouseButton _button, terminal::Modifier _modifier, Timestamp _now);
    void sendMouseMoveEvent(terminal::Coordinate _pos, terminal::Modifier _modifier, Timestamp _now);
    void sendMouseReleaseEvent(terminal::MouseButton _button, terminal::Modifier _modifier, Timestamp _now);
    void sendFocusInEvent();
    void sendFocusOutEvent();

    // Actions
    bool operator()(actions::CancelSelection);
    bool operator()(actions::ChangeProfile const&);
    bool operator()(actions::ClearHistoryAndReset);
    bool operator()(actions::CopyPreviousMarkRange);
    bool operator()(actions::CopySelection);
    bool operator()(actions::DecreaseFontSize);
    bool operator()(actions::DecreaseOpacity);
    bool operator()(actions::FollowHyperlink);
    bool operator()(actions::IncreaseFontSize);
    bool operator()(actions::IncreaseOpacity);
    bool operator()(actions::NewTerminal const&);
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
    bool operator()(actions::SendChars const& _event);
    bool operator()(actions::ToggleAllKeyMaps);
    bool operator()(actions::ToggleFullscreen);
    bool operator()(actions::WriteScreen const& _event);

    void scheduleRedraw()
    {
        terminal_.markScreenDirty();
        if (display_)
            display_->scheduleRedraw();
    }

    Controller& controller() noexcept { return controller_; }

  private:
    // helpers
    void sanitizeConfig(config::Config& _config);
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
    void onConfigReload(FileChangeWatcher::Event _event);
    void setDefaultCursor();
    void configureTerminal();
    void configureDisplay();
    uint8_t matchModeFlags() const;

    // private data
    //
    std::chrono::steady_clock::time_point startTime_;
    std::chrono::seconds earlyExitThreshold_;
    config::Config config_;
    std::string profileName_;
    config::TerminalProfile profile_;
    std::string programPath_;
    Controller& controller_;
    std::function<void()> displayInitialized_;
    std::function<void()> onExit_;

    std::unique_ptr<terminal::Pty> pty_;
    terminal::Terminal terminal_;
    bool terminatedAndWaitingForKeyPress_ = false;
    std::unique_ptr<TerminalDisplay> display_;

    std::optional<FileChangeWatcher> configFileChangeWatcher_;

    // state vars
    //
    terminal::ScreenType currentScreenType_ = terminal::ScreenType::Main;
    terminal::Coordinate currentMousePosition_ = terminal::Coordinate{};
    bool allowKeyMappings_ = true;
};

}
