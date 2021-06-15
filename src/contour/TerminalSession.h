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
                    config::Config _config,
                    bool _liveconfig,
                    std::string _profileName,
                    std::string _programPath,
                    std::unique_ptr<TerminalDisplay> _display,
                    std::function<void()> _displayInitialized);
    ~TerminalSession();

    void start();

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
    void resizeWindow(int _width, int _height, bool _unitInPixels) override;
    void setWindowTitle(std::string_view _title) override;
    void setTerminalProfile(std::string const& _configProfileName) override;
    void discardImage(terminal::Image const&) override;

    // Input Events
    using Timestamp = std::chrono::steady_clock::time_point;
    void sendKeyPressEvent(terminal::KeyInputEvent const& _event, Timestamp _now);
    void sendCharPressEvent(terminal::CharInputEvent const& _event, Timestamp _now);
    void sendMousePressEvent(terminal::MousePressEvent const& _event, Timestamp _now);
    void sendMouseMoveEvent(terminal::MouseMoveEvent const& _event, Timestamp _now);
    void sendMouseReleaseEvent(terminal::MouseReleaseEvent const& _event, Timestamp _now);
    void sendFocusInEvent();
    void sendFocusOutEvent();

    // Actions
    void operator()(actions::ChangeProfile const&);
    void operator()(actions::CopyPreviousMarkRange);
    void operator()(actions::CopySelection);
    void operator()(actions::DecreaseFontSize);
    void operator()(actions::DecreaseOpacity);
    void operator()(actions::FollowHyperlink);
    void operator()(actions::IncreaseFontSize);
    void operator()(actions::IncreaseOpacity);
    void operator()(actions::NewTerminal const&);
    void operator()(actions::OpenConfiguration);
    void operator()(actions::OpenFileManager);
    void operator()(actions::PasteClipboard);
    void operator()(actions::PasteSelection);
    void operator()(actions::Quit);
    void operator()(actions::ReloadConfig const&);
    void operator()(actions::ResetConfig);
    void operator()(actions::ResetFontSize);
    void operator()(actions::ScreenshotVT);
    void operator()(actions::ScrollDown);
    void operator()(actions::ScrollMarkDown);
    void operator()(actions::ScrollMarkUp);
    void operator()(actions::ScrollOneDown);
    void operator()(actions::ScrollOneUp);
    void operator()(actions::ScrollPageDown);
    void operator()(actions::ScrollPageUp);
    void operator()(actions::ScrollToBottom);
    void operator()(actions::ScrollToTop);
    void operator()(actions::ScrollUp);
    void operator()(actions::SendChars const& _event);
    void operator()(actions::ToggleAllKeyMaps);
    void operator()(actions::ToggleFullscreen);
    void operator()(actions::WriteScreen const& _event);

    void scheduleRedraw()
    {
        if (display_)
            display_->scheduleRedraw();
    }

  private:
    // helpers
    void sanitizeConfig(config::Config& _config);
    bool reloadConfig(config::Config _newConfig, std::string const& _profileName);
    void executeAllActions(std::vector<actions::Action> const& _actions);
    void executeAction(actions::Action const& _action);
    void spawnNewTerminal(std::string const& _profileName);
    void activateProfile(std::string const& _newProfileName);
    bool reloadConfigWithProfile(std::string const& _profileName);
    bool resetConfig();
    void followHyperlink(terminal::HyperlinkInfo const& _hyperlink);
    bool requestPermission(config::Permission _allowedByConfig, std::string_view _topicText);
    void setFontSize(text::font_size _size);
    void onConfigReload(FileChangeWatcher::Event _event);
    void setDefaultCursor();
    config::TerminalProfile& profile() noexcept { return profile_; }
    void configureTerminal();
    void configureDisplay();

    // private data
    //
    config::Config config_;
    std::string profileName_;
    config::TerminalProfile profile_;
    std::string programPath_;
    std::function<void()> displayInitialized_;

    std::unique_ptr<terminal::Pty> pty_;
    terminal::Terminal terminal_;
    std::unique_ptr<TerminalDisplay> display_;

    std::optional<FileChangeWatcher> configFileChangeWatcher_;

    // state vars
    //
    terminal::ScreenType currentScreenType_ = terminal::ScreenType::Main;
    bool allowKeyMappings_ = true;
};

}
