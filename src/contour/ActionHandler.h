/**
 * This file is part of the "libterminal" project
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

#include <terminal_view/TerminalView.h>

#include <terminal/InputGenerator.h>
#include <terminal/Terminal.h>

#include <QtGui/QWindow>
#include <QtWidgets/QWidget>

#include <chrono>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace contour::actions {

// Find a way where we do not need to double-implement
// the actions handlers just for having various render frontends,
// such as: OpenGL, Vulkan, ..., Software.
// TODO: soon getting a new name, as it's a terminal without the render widget part.
//
// TerminalWidget --> TerminalDisplay
// ActionHandler  --> TerminalInstance
//
// The TerminalInstance owns <TerminalDisplay> and its <RenderTarget>,
// so it can be changed on the fly to a different
// render stack (Software, OpenGL, Vulkan, ...).
class ActionHandler:
    public terminal::view::TerminalView::Events
{
public:
    using Timestamp = std::chrono::steady_clock::time_point;

    ActionHandler(
        config::Config _config,
        std::string _profileName,
        std::string _programPath,
        bool _liveConfig,
        crispy::Point _dpi,
        std::function<void()> _updateDisplay,
        std::function<void(terminal::ScreenType)> _bufferTypeChanged,
        std::function<void(bool)> _setBackgroundBlur,
        std::function<void()> _profileChanged,
        std::function<void(std::string_view, std::string_view)> showNotification_
    );

    config::Config const& config() const noexcept { return config_; }
    config::Config& config() noexcept { return config_; }
    std::string const& profileName() const noexcept { return profileName_; }
    config::TerminalProfile const& profile() const noexcept { return profile_; }
    config::TerminalProfile& profile() noexcept { return profile_; }
    terminal::ScreenType currentScreenType() const noexcept { return currentScreenType_; }
    terminal::view::TerminalView& view() { return *terminalView_; }

    /// To be invoked by terminal display widget (e.g. OpenGL surface) when surface initialization is done.
    void displayInitialized();
    bool isDisplayInitialized() const noexcept { return displayInitialized_; }

    void setWidget(QWidget& _newTerminalWidget);
    void activateProfile(std::string const& _newProfileName);
    bool reloadConfig(config::Config _newConfig, std::string const& _profileName);
    void executeAllActions(std::vector<actions::Action> const& _actions);

    void keyPressEvent(terminal::KeyInputEvent const& _event);
    void charPressEvent(terminal::CharInputEvent const& _event);
    void mousePressEvent(terminal::MousePressEvent const& _event);
    void mouseMoveEvent(terminal::MouseMoveEvent const& _event);
    void mouseReleaseEvent(terminal::MouseReleaseEvent const& _event);
    void focusInEvent();
    void focusOutEvent();

    void operator()(ChangeProfile const&);
    void operator()(CopyPreviousMarkRange);
    void operator()(CopySelection);
    void operator()(DecreaseFontSize);
    void operator()(DecreaseOpacity);
    void operator()(FollowHyperlink);
    void operator()(IncreaseFontSize);
    void operator()(IncreaseOpacity);
    void operator()(NewTerminal const&);
    void operator()(OpenConfiguration);
    void operator()(OpenFileManager);
    void operator()(PasteClipboard);
    void operator()(PasteSelection);
    void operator()(Quit);
    void operator()(ReloadConfig const&);
    void operator()(ResetConfig);
    void operator()(ResetFontSize);
    void operator()(ScreenshotVT);
    void operator()(ScrollDown);
    void operator()(ScrollMarkDown);
    void operator()(ScrollMarkUp);
    void operator()(ScrollOneDown);
    void operator()(ScrollOneUp);
    void operator()(ScrollPageDown);
    void operator()(ScrollPageUp);
    void operator()(ScrollToBottom);
    void operator()(ScrollToTop);
    void operator()(ScrollUp);
    void operator()(SendChars const& _event);
    void operator()(ToggleAllKeyMaps);
    void operator()(ToggleFullscreen);
    void operator()(WriteScreen const& _event);

    // terminal::view::TerminalView::Events overrides
    //
    void bell() override;
    void bufferChanged(terminal::ScreenType) override;
    void screenUpdated() override;
    void renderBufferUpdated() override;
    void requestCaptureBuffer(int _absoluteStartLine, int _lineCount) override;
    void setFontDef(terminal::FontDef const& _fontDef) override;
    void copyToClipboard(std::string_view _text) override;
    void dumpState() override;
    void notify(std::string_view /*_title*/, std::string_view /*_body*/) override;
    void onClosed() override;
    void onSelectionComplete() override;
    void resizeWindow(int /*_width*/, int /*_height*/, bool /*_unitInPixels*/) override;
    void setWindowTitle(std::string_view /*_title*/) override;
    void setTerminalProfile(std::string const& _configProfileName) override;

private:
    QWidget& widget() const noexcept;
    QWidget& widget() noexcept;
    terminal::Terminal& terminal();
    config::InputMappings const& inputMappings() const noexcept { return config_.inputMappings; }

    /// Posts given function from terminal thread into the GUI thread.
    void post(std::function<void()> _fn);

    void executeAction(Action const& _action);
    void setFontSize(text::font_size _size);
    void toggleFullscreen();
    void setDefaultCursor();
    bool requestPermission(config::Permission _allowedByConfig, std::string_view _topicText);
    void doDumpState();
    void onConfigReload(FileChangeWatcher::Event /*_event*/);
    void setSize(crispy::Size _size);

    void activateProfile(std::string const& _name, config::TerminalProfile newProfile);
    void spawnNewTerminal(std::string const& _profileName);
    bool reloadConfigWithProfile(std::string const& _profileName);
    bool resetConfig();
    void followHyperlink(terminal::HyperlinkInfo const& _hyperlink);

    void updateDisplay() { if (displayInitialized_) updateDisplay_(); }

    // private fields
    //
    QWidget* terminalWidget_ = nullptr;

    config::Config config_;
    std::string profileName_;
    config::TerminalProfile profile_;
    std::string programPath_;
    terminal::renderer::FontDescriptions fonts_;
    std::unique_ptr<terminal::view::TerminalView> terminalView_;
    std::optional<FileChangeWatcher> configFileChangeWatcher_;

    std::function<void()> updateDisplay_;
    std::function<void(terminal::ScreenType)> terminalBufferChanged_;
    std::function<void(bool)> setBackgroundBlur_;
    std::function<void()> profileChanged_;
    std::function<void(std::string_view, std::string_view)> showNotification_;

    // state
    //
    bool displayInitialized_ = false;
    bool allowKeyMappings_;
    bool maximizedState_;
    terminal::ScreenType currentScreenType_ = terminal::ScreenType::Main;
    struct {
        std::optional<bool> changeFont;
        std::map<std::string, bool> mapping;
    } rememberedPermissions_;
};

} // namespace contour::actions
