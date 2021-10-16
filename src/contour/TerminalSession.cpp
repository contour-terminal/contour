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
#include <contour/TerminalSession.h>
#include <contour/helper.h>

#include <terminal/MatchModes.h>
#include <terminal/Terminal.h>
#include <terminal/pty/Pty.h>

#include <crispy/StackTrace.h>

#include <range/v3/all.hpp>

#include <QtCore/QDebug>
#include <QtCore/QFileInfo>
#include <QtCore/QMetaObject>
#include <QtCore/QProcess>
#include <QtCore/QStandardPaths>
#include <QtCore/QTimer>
#include <QtGui/QClipboard>
#include <QtGui/QDesktopServices>
#include <QtGui/QKeyEvent>
#include <QtGui/QScreen>
#include <QtGui/QWindow>
#include <QtNetwork/QHostInfo>
#include <QtWidgets/QApplication>
#include <QtWidgets/QMessageBox>

#include <fstream>

#if defined(CONTOUR_BLUR_PLATFORM_KWIN)
#include <KWindowEffects>
#endif

#if !defined(_MSC_VER)
#include <unistd.h>
#include <signal.h>
#endif

#if defined(_MSC_VER)
#define __PRETTY_FUNCTION__ __FUNCDNAME__
#endif

using std::chrono::steady_clock;
using namespace std;
using namespace terminal;

namespace contour {

namespace // {{{ helper
{
    constexpr crispy::Point scale(crispy::Point p, double s)
    {
        return crispy::Point{
            static_cast<int>(static_cast<double>(p.x) * s),
            static_cast<int>(static_cast<double>(p.y) * s)
        };
    }

    string unhandledExceptionMessage(string_view const& where, exception const& e)
    {
        return fmt::format("{}: Unhandled exception caught ({}). {}", where, typeid(e).name(), e.what());
    }

} //  }}}

TerminalSession::TerminalSession(unique_ptr<Pty> _pty,
                                 config::Config _config,
                                 bool _liveConfig,
                                 string _profileName,
                                 string _programPath,
                                 unique_ptr<TerminalDisplay> _display,
                                 std::function<void()> _displayInitialized):
    startTime_{ steady_clock::now() },
    config_{ move(_config) },
    profileName_{ move(_profileName) },
    profile_{ *config_.profile(profileName_) },
    programPath_{ move(_programPath ) },
    displayInitialized_{ move(_displayInitialized) },
    pty_{ move(_pty) },
    terminal_{
        *pty_,
        config_.ptyReadBufferSize,
        *this,
        config_.profile(profileName_)->maxHistoryLineCount,
        {}, // TODO: that's actually dead param (_cursorBlinkInterval,)
        steady_clock::now(),
        config_.wordDelimiters, // TODO: move to profile!
        config_.bypassMouseProtocolModifier, // TODO: you too
        config_.maxImageSize,
        config_.maxImageColorRegisters,
        config_.sixelCursorConformance,
        profile_.colors,
        _display ? _display->refreshRate() : 50.0
    },
    display_{move(_display)}
{
    if (_liveConfig)
    {
        LOGSTORE(SessionLog)("Enable live configuration reloading of file {}.",
                             config_.backingFilePath.generic_string());
        configFileChangeWatcher_.emplace(config_.backingFilePath,
                                         [this](FileChangeWatcher::Event event) { onConfigReload(event); });
    }

    sanitizeConfig(_config);
    profile_ = *config_.profile(profileName_); // XXX do it again. but we've to be more efficient here
    configureTerminal();
}

TerminalSession::~TerminalSession()
{
    (void) display_.release(); // TODO: due to Qt, this is currently not owned by us. That's sad, or is it not?
}

void TerminalSession::setDisplay(unique_ptr<TerminalDisplay> _display)
{
    LOGSTORE(SessionLog)("Assigning display.");
    display_ = move(_display);

    // XXX find better way (dpi)
    sanitizeConfig(config_);
    profile_ = *config_.profile(profileName_); // XXX do it again. but we've to be more efficient here

    // NB: Inform connected TTY and local Screen instance about initial cell pixel size.
    auto const pixels = ImageSize{
        display_->cellSize().width * boxed_cast<Width>(terminal_.screenSize().columns),
        display_->cellSize().height * boxed_cast<Height>(terminal_.screenSize().lines)
    };
    terminal_.resizeScreen(terminal_.screenSize(), pixels);
}

void TerminalSession::displayInitialized()
{
    configureDisplay();

    if (displayInitialized_)
        displayInitialized_();
}

void TerminalSession::start()
{
    terminal().start();
}

// {{{ Events implementations
void TerminalSession::bell()
{
    LOGSTORE(SessionLog)("TODO: Beep!");
    QApplication::beep();
    // QApplication::beep() requires Qt Widgets dependency. doesn't suound good.
    // so maybe just a visual bell then? That would require additional OpenGL/shader work then though.
}

void TerminalSession::bufferChanged(terminal::ScreenType _type)
{
    display_->post([this, _type] ()
    {
        display_->bufferChanged(_type);
    });
}

void TerminalSession::screenUpdated()
{
#if defined(CONTOUR_VT_METRICS)
    // TODO
    // for (auto const& command : _commands)
    //     terminalMetrics_(command);
#endif
    if (profile_.autoScrollOnUpdate && terminal().viewport().scrolled())
        terminal().viewport().scrollToBottom();

    scheduleRedraw();
}

void TerminalSession::renderBufferUpdated()
{
    if (!display_)
        return;

    display_->renderBufferUpdated();
}

void TerminalSession::requestCaptureBuffer(int _absoluteStartLine, int _lineCount)
{
    display_->post([this, _absoluteStartLine, _lineCount]()
    {
        if (display_->requestPermission(profile_.permissions.captureBuffer, "capture screen buffer"))
        {
            terminal_.screen().captureBuffer(_absoluteStartLine, _lineCount);
        }
    });
}

terminal::FontDef TerminalSession::getFontDef()
{
    return display_->getFontDef();
}

void TerminalSession::setFontDef(terminal::FontDef const& _fontDef)
{
    display_->post([this, spec = terminal::FontDef(_fontDef)]()
    {
        if (!display_->requestPermission(profile_.permissions.changeFont, "changing font"))
            return;

        auto const& currentFonts = profile_.fonts;
        terminal::renderer::FontDescriptions newFonts = currentFonts;

        if (spec.size != 0.0)
            newFonts.size = text::font_size{ spec.size };

        if (!spec.regular.empty())
            newFonts.regular = text::font_description::parse(spec.regular);

        auto const styledFont = [&](string_view _font) -> text::font_description {
            // if a styled font is "auto" then infer froom regular font"
            if (_font == "auto"sv)
                return currentFonts.regular;
            else
                return text::font_description::parse(_font);
        };

        if (!spec.bold.empty())
            newFonts.bold = styledFont(spec.bold);

        if (!spec.italic.empty())
            newFonts.italic = styledFont(spec.italic);

        if (!spec.boldItalic.empty())
            newFonts.boldItalic = styledFont(spec.boldItalic);

        if (!spec.emoji.empty() && spec.emoji != "auto"sv)
            newFonts.emoji = text::font_description::parse(spec.emoji);

        display_->setFonts(newFonts);
    });
}

void TerminalSession::copyToClipboard(std::string_view _data)
{
    if (!display_)
        return;

    display_->post([this, data = string(_data)]() { display_->copyToClipboard(data); });
}

void TerminalSession::dumpState()
{
    if (!display_)
        return;

    display_->post([this]() { display_->dumpState(); });
}


void TerminalSession::notify(string_view _title, string_view _content)
{
    if (!display_)
        return;

    display_->notify(_title, _content);
}

void TerminalSession::onClosed()
{
    auto const now = steady_clock::now();
    auto const diff = now - startTime_;

    if (diff < std::chrono::seconds(5))
    {
        //auto const w = terminal_.screenSize().columns.as<int>();
        auto constexpr SGR = "\e[1;38:2::255:255:255m\e[48:2::255:0:0m"sv;
        auto constexpr EL = "\e[K"sv;
        auto constexpr TextLines = array<string_view, 2>{
            "Shell terminated too quickly.",
            "The window will not be closed automatically."
        };
        for (auto const text: TextLines)
            terminal_.writeToScreen(fmt::format("\r\n{}{}{}", SGR, EL, text));
        terminal_.writeToScreen("\r\n");
        terminatedAndWaitingForKeyPress_ = true;
        return;
    }

    if (!display_)
        return;

    display_->closeDisplay();
}

void TerminalSession::onSelectionCompleted()
{
    if (QClipboard* clipboard = QGuiApplication::clipboard(); clipboard != nullptr)
    {
        string const text = terminal().extractSelectionText();
        clipboard->setText(QString::fromUtf8(text.c_str(), static_cast<int>(text.size())), QClipboard::Selection);
    }
}

void TerminalSession::resizeWindow(LineCount _lines, ColumnCount _columns)
{
    if (!display_)
        return;

    LOGSTORE(SessionLog)("Application request to resize window: {}x{} px", _columns, _lines);
    display_->post([this, _lines, _columns]() { display_->resizeWindow(_lines, _columns); });
}

void TerminalSession::resizeWindow(Width _width, Height _height)
{
    if (!display_)
        return;

    LOGSTORE(SessionLog)("Application request to resize window: {}x{} px", _width, _height);
    display_->post([this, _width, _height]() { display_->resizeWindow(_width, _height); });
}

void TerminalSession::setWindowTitle(string_view _title)
{
    if (!display_)
        return;

    display_->post([this, terminalTitle = string(_title)]() {
        display_->setWindowTitle(terminalTitle);
    });
}

void TerminalSession::setTerminalProfile(string const& _configProfileName)
{
    if (!display_)
        return;

    display_->post([this, name = string(_configProfileName)]() {
        activateProfile(name);
    });
}

void TerminalSession::discardImage(terminal::Image const& _image)
{
    display_->discardImage(_image);
}

// }}}
// {{{ Input Events
void TerminalSession::sendKeyPressEvent(Key _key,
                                        Modifier _modifier,
                                        Timestamp _now)
{
    LOGSTORE(InputLog)("{} {}", _modifier, _key);

    if (terminatedAndWaitingForKeyPress_)
    {
        display_->closeDisplay();
        return;
    }

    display_->setMouseCursorShape(MouseCursorShape::Hidden);

    if (auto const* actions = config::apply(config_.inputMappings.keyMappings,
                                            _key,
                                            _modifier,
                                            matchModeFlags()))
        executeAllActions(*actions);
    else
        terminal().sendKeyPressEvent(_key, _modifier, _now);
}

void TerminalSession::sendCharPressEvent(char32_t _value, Modifier _modifier, Timestamp _now)
{
    LOGSTORE(InputLog)("{} {}", _modifier, _value);
    assert(display_ != nullptr);

    if (terminatedAndWaitingForKeyPress_)
    {
        display_->closeDisplay();
        return;
    }

    display_->setMouseCursorShape(MouseCursorShape::Hidden);

    if (auto const* actions = config::apply(config_.inputMappings.charMappings,
                                            _value,
                                            _modifier,
                                            matchModeFlags()))
        executeAllActions(*actions);
    else
        terminal().sendCharPressEvent(_value, _modifier, _now); // TODO: get rid of Event{} struct here, too!
}

void TerminalSession::sendMousePressEvent(MouseButton _button, Modifier _modifier, Timestamp _now)
{
    // debuglog(MouseInputTag).write("sendMousePressEvent: {}", _event);

    // First try to pass the mouse event to the application, as it might have requested that.
    if (terminal().sendMousePressEvent(_button, _modifier, _now))
    {
        scheduleRedraw();
        return;
    }

    if (auto const* actions = config::apply(config_.inputMappings.mouseMappings,
                                            _button,
                                            _modifier,
                                            matchModeFlags()))
    {
        executeAllActions(*actions);
        return;
    }

    if (_button != MouseButton::Left)
        return;
    if (!terminal_.handleMouseSelection(_modifier, _now))
        return;
    scheduleRedraw();
}

void TerminalSession::sendMouseMoveEvent(terminal::Coordinate _pos, terminal::Modifier _modifier, Timestamp _now)
{
    if (_pos == currentMousePosition_)
        return;

    currentMousePosition_ = _pos;

    auto const handled = terminal().sendMouseMoveEvent(_pos, _modifier, _now);

    bool const mouseHoveringHyperlink = terminal().isMouseHoveringHyperlink();
    if (mouseHoveringHyperlink)
        display_->setMouseCursorShape(MouseCursorShape::PointingHand);
    else
        setDefaultCursor();

    if (mouseHoveringHyperlink || handled || terminal().isSelectionInProgress()) // && only if selection has changed!
    {
        terminal().breakLoopAndRefreshRenderBuffer();
        scheduleRedraw();
    }
}

void TerminalSession::sendMouseReleaseEvent(MouseButton _button, Modifier _modifier, Timestamp _now)
{
    terminal().sendMouseReleaseEvent(_button, _modifier, _now);
    scheduleRedraw();
}

void TerminalSession::sendFocusInEvent()
{
    // as per Qt-documentation, some platform implementations reset the cursor when leaving the
    // window, so we have to re-apply our desired cursor in focusInEvent().
    setDefaultCursor();

    terminal().screen().setFocus(true);
    terminal().sendFocusInEvent();

    display_->setBackgroundBlur(profile().backgroundBlur);
    scheduleRedraw();
}

void TerminalSession::sendFocusOutEvent()
{
    // TODO maybe paint with "faint" colors
    terminal().screen().setFocus(false);
    terminal().sendFocusOutEvent();

    scheduleRedraw();
}

// }}}
// {{{ Actions
void TerminalSession::operator()(actions::ChangeProfile const& _action)
{
    LOGSTORE(SessionLog)("Changing profile to: {}", _action.name);
    if (_action.name == profileName_)
        return;

    activateProfile(_action.name);
}

void TerminalSession::operator()(actions::ClearHistoryAndReset)
{
    LOGSTORE(SessionLog)("Clearing history and perform terminal hard reset");

    auto const screenSize = terminal_.screenSize();
    auto const pixelSize = display_->pixelSize();

    terminal_.screen().resetHard();
    auto const tmpScreenSize = PageSize{
        screenSize.lines,
        screenSize.columns + ColumnCount(1)
    };
    terminal_.resizeScreen(tmpScreenSize, pixelSize);
    this_thread::yield();
    terminal_.resizeScreen(screenSize, pixelSize);
}

void TerminalSession::operator()(actions::CopyPreviousMarkRange)
{
    copyToClipboard(terminal().extractLastMarkRange());
}

void TerminalSession::operator()(actions::CopySelection)
{
    copyToClipboard(terminal().extractSelectionText());
}

void TerminalSession::operator()(actions::DecreaseFontSize)
{
    auto constexpr OnePt = text::font_size{ 1.0 };
    setFontSize(profile().fonts.size - OnePt);
    // auto const currentFontSize = view().renderer().fontDescriptions().size;
    // auto const newFontSize = currentFontSize - OnePt;
    // setFontSize(newFontSize);
}

void TerminalSession::operator()(actions::DecreaseOpacity)
{
    if (static_cast<uint8_t>(profile_.backgroundOpacity) == 0)
        return;

    --profile_.backgroundOpacity;
    display_->setBackgroundOpacity(profile_.backgroundOpacity);
}

void TerminalSession::operator()(actions::FollowHyperlink)
{
    #if defined(LIBTERMINAL_HYPERLINKS)
    auto const _l = scoped_lock{terminal()};
    auto const currentMousePosition = terminal().currentMousePosition();
    auto const currentMousePositionRel = terminal::Coordinate{
        currentMousePosition.row - terminal().viewport().relativeScrollOffset().as<int>(),
        currentMousePosition.column
    };
    if (terminal().screen().contains(currentMousePosition))
    {
        if (auto hyperlink = terminal().screen().at(currentMousePositionRel).hyperlink(); hyperlink != nullptr)
        {
            followHyperlink(*hyperlink);
            return;
        }
    }
    #endif
}

void TerminalSession::operator()(actions::IncreaseFontSize)
{
    auto constexpr OnePt = text::font_size{ 1.0 };
    // auto const currentFontSize = view().renderer().fontDescriptions().size;
    // auto const newFontSize = currentFontSize + OnePt;
    // setFontSize(newFontSize);
    setFontSize(profile().fonts.size + OnePt);
}

void TerminalSession::operator()(actions::IncreaseOpacity)
{
    if (static_cast<uint8_t>(profile_.backgroundOpacity) >= 255)
        return;

    ++profile_.backgroundOpacity;
    display_->setBackgroundOpacity(profile_.backgroundOpacity);
}

void TerminalSession::operator()(actions::NewTerminal const& _action)
{
    spawnNewTerminal(_action.profileName.value_or(profileName_));
}

void TerminalSession::operator()(actions::OpenConfiguration)
{
    if (!QDesktopServices::openUrl(QUrl(QString::fromUtf8(config_.backingFilePath.string().c_str()))))
        cerr << "Could not open configuration file \"" << config_.backingFilePath << "\"" << endl;
}

void TerminalSession::operator()(actions::OpenFileManager)
{
    auto const _l = scoped_lock{terminal()};
    auto const& cwd = terminal().screen().currentWorkingDirectory();
    if (!QDesktopServices::openUrl(QUrl(QString::fromUtf8(cwd.c_str()))))
        cerr << "Could not open file \"" << cwd << "\"" << endl;
}

void TerminalSession::operator()(actions::PasteClipboard)
{
    if (QClipboard* clipboard = QGuiApplication::clipboard(); clipboard != nullptr)
    {
        string const text = clipboard->text(QClipboard::Clipboard).toUtf8().toStdString();
        terminal().sendPaste(string_view{text});
    }
}

void TerminalSession::operator()(actions::PasteSelection)
{
    if (QClipboard* clipboard = QGuiApplication::clipboard(); clipboard != nullptr)
    {
        string const text = clipboard->text(QClipboard::Selection).toUtf8().toStdString();
        terminal().sendPaste(string_view{text});
    }
}

void TerminalSession::operator()(actions::Quit)
{
    //TODO: later warn here when more then one terminal view is open
    terminal().device().close();
    exit(EXIT_SUCCESS);
}

void TerminalSession::operator()(actions::ReloadConfig const& _action)
{
    if (_action.profileName.has_value())
        reloadConfigWithProfile(_action.profileName.value());
    else
        reloadConfigWithProfile(profileName_);
}

void TerminalSession::operator()(actions::ResetConfig)
{
    resetConfig();
}

void TerminalSession::operator()(actions::ResetFontSize)
{
    setFontSize(config_.profile(profileName_)->fonts.size);
}

void TerminalSession::operator()(actions::ScreenshotVT)
{
    auto _l = lock_guard{ terminal() };
    auto const screenshot = terminal().screen().screenshot();
    ofstream ofs{ "screenshot.vt", ios::trunc | ios::binary };
    ofs << screenshot;
}

void TerminalSession::operator()(actions::ScrollDown)
{
    terminal().viewport().scrollDown(profile_.historyScrollMultiplier);
}

void TerminalSession::operator()(actions::ScrollMarkDown)
{
    terminal().viewport().scrollMarkDown();
}

void TerminalSession::operator()(actions::ScrollMarkUp)
{
    terminal().viewport().scrollMarkUp();
}

void TerminalSession::operator()(actions::ScrollOneDown)
{
    terminal().viewport().scrollDown(LineCount(1));
}

void TerminalSession::operator()(actions::ScrollOneUp)
{
    terminal().viewport().scrollUp(LineCount(1));
}

void TerminalSession::operator()(actions::ScrollPageDown)
{
    auto const stepSize = terminal().screenSize().lines / LineCount(2);
    terminal().viewport().scrollDown(stepSize);
}

void TerminalSession::operator()(actions::ScrollPageUp)
{
    auto const stepSize = terminal().screenSize().lines / LineCount(2);
    terminal().viewport().scrollUp(stepSize);
}

void TerminalSession::operator()(actions::ScrollToBottom)
{
    terminal().viewport().scrollToBottom();
}

void TerminalSession::operator()(actions::ScrollToTop)
{
    terminal().viewport().scrollToTop();
}

void TerminalSession::operator()(actions::ScrollUp)
{
    terminal().viewport().scrollUp(profile_.historyScrollMultiplier);
}

void TerminalSession::operator()(actions::SendChars const& _event)
{
    auto const now = steady_clock::now();

    for (auto const ch: _event.chars)
    {
        terminal().sendCharPressEvent(static_cast<char32_t>(ch),
                                      terminal::Modifier::None,
                                      now);
    }
}

void TerminalSession::operator()(actions::ToggleAllKeyMaps)
{
    allowKeyMappings_ = !allowKeyMappings_;
    LOGSTORE(InputLog)(
        "{} key mappings.",
        allowKeyMappings_ ? "Enabling" : "Disabling"
    );
}

void TerminalSession::operator()(actions::ToggleFullscreen)
{
    if (display_)
        display_->toggleFullScreen();
}

void TerminalSession::operator()(actions::WriteScreen const& _event)
{
    terminal().writeToScreen(_event.chars);
}

// }}}
// {{{ implementation helpers
void TerminalSession::setDefaultCursor()
{
    using Type = terminal::ScreenType;
    switch (terminal().screen().bufferType())
    {
        case Type::Main:
            display_->setMouseCursorShape(MouseCursorShape::IBeam);
            break;
        case Type::Alternate:
            display_->setMouseCursorShape(MouseCursorShape::Arrow);
            break;
    }
}

void TerminalSession::sanitizeConfig(config::Config& _config)
{
    if (!display_)
        return;

    auto const dpi = display_->screenDPI();
    for (config::TerminalProfile& profile: _config.profiles | ranges::views::values)
        if (!profile.fonts.dpi.x || !profile.fonts.dpi.y)
            profile.fonts.dpi = scale(dpi, profile.fonts.dpiScale);
}

bool TerminalSession::reloadConfig(config::Config _newConfig, string const& _profileName)
{
    LOGSTORE(SessionLog)("Reloading configuration from {} with profile {}",
                         _newConfig.backingFilePath.string(),
                         _profileName);

    sanitizeConfig(_newConfig);

    config_ = move(_newConfig);
    activateProfile(_profileName);

    return true;
}

void TerminalSession::executeAllActions(std::vector<actions::Action> const& _actions)
{
    if (allowKeyMappings_)
    {
        for (actions::Action const& action : _actions)
            executeAction(action);
        scheduleRedraw();
        return;
    }

    auto const containsToggleKeybind = [](std::vector<actions::Action> const& _actions)
    {
        return std::any_of(
            _actions.begin(),
            _actions.end(),
            [](actions::Action const& action) {
                return holds_alternative<actions::ToggleAllKeyMaps>(action);
            }
        );
    };

    if (containsToggleKeybind(_actions))
    {
        executeAction(actions::ToggleAllKeyMaps{});
        scheduleRedraw();
        return;
    }

    LOGSTORE(InputLog)("Key mappings are currently disabled via ToggleAllKeyMaps input mapping action.");
}

void TerminalSession::executeAction(actions::Action const& _action)
{
    LOGSTORE(SessionLog)("executeAction: {}", _action);
    visit(*this, _action);
}

void TerminalSession::spawnNewTerminal(string const& _profileName)
{
    ::contour::spawnNewTerminal(
        programPath_,
        config_.backingFilePath.generic_string(),
        _profileName,
        [this]() -> string {
            auto const _l = scoped_lock{terminal_};
            return terminal_.screen().currentWorkingDirectory();
        }()
    );
}

void TerminalSession::activateProfile(string const& _newProfileName)
{
    auto newProfile = config_.profile(_newProfileName);
    if (!newProfile)
    {
        LOGSTORE(SessionLog)("Cannot change profile. No such profile: '{}'.", _newProfileName);
        return;
    }

    LOGSTORE(SessionLog)("Changing profile to {}.", _newProfileName);
    profileName_ = _newProfileName;
    profile_ = *newProfile;
    configureTerminal();
    configureDisplay();
}

void TerminalSession::configureTerminal()
{
    auto const _l = scoped_lock{terminal_};
    LOGSTORE(SessionLog)("Configuring terminal.");
    terminal::Screen& screen = terminal_.screen();

    terminal_.setWordDelimiters(config_.wordDelimiters);
    terminal_.setMouseProtocolBypassModifier(config_.bypassMouseProtocolModifier);

    LOGSTORE(SessionLog)("Setting terminal ID to {}.", profile_.terminalId);
    screen.setTerminalId(profile_.terminalId);
    screen.setRespondToTCapQuery(config_.experimentalFeatures.count("tcap"));
    screen.setSixelCursorConformance(config_.sixelCursorConformance);
    screen.setMaxImageColorRegisters(config_.maxImageColorRegisters);
    screen.setMaxImageSize(config_.maxImageSize);
    LOGSTORE(SessionLog)("maxImageSize={}, sixelScrolling={}",
            config_.maxImageSize, config_.sixelScrolling ? "yes" : "no");
    screen.setMode(terminal::DECMode::SixelScrolling, config_.sixelScrolling);

    // XXX
    // if (!_terminalView.renderer().renderTargetAvailable())
    //     return;

    screen.setMaxHistoryLineCount(profile_.maxHistoryLineCount);
    terminal_.setCursorBlinkingInterval(profile_.cursorBlinkInterval);
    terminal_.setCursorDisplay(profile_.cursorDisplay);
    terminal_.setCursorShape(profile_.cursorShape);
    terminal_.screen().colorPalette() = profile_.colors;
    terminal_.screen().defaultColorPalette() = profile_.colors;
}

void TerminalSession::configureDisplay()
{
    if (!display_)
        return;

    LOGSTORE(SessionLog)("Configuring display.");
    display_->setBackgroundBlur(profile_.backgroundBlur);

    if (profile_.maximized)
        display_->setWindowMaximized();
    else
        display_->setWindowNormal();

    if (profile_.fullscreen != display_->isFullScreen())
        display_->toggleFullScreen();

    terminal_.setRefreshRate(display_->refreshRate());
    auto const pageSize = PageSize {
        LineCount(*display_->pixelSize().height / *display_->cellSize().height),
        ColumnCount(*display_->pixelSize().width / *display_->cellSize().width),
    };
    display_->setScreenSize(pageSize);
    display_->setFonts(profile_.fonts);
    // TODO: maybe update margin after this call?

    display_->setHyperlinkDecoration(profile_.hyperlinkDecoration.normal,
                                     profile_.hyperlinkDecoration.hover);

    display_->setWindowTitle(terminal_.screen().windowTitle());

}

uint8_t TerminalSession::matchModeFlags() const
{
    uint8_t flags = 0;

    if (terminal_.screen().isAlternateScreen())
        flags |= static_cast<uint8_t>(MatchModes::Flag::AlternateScreen);

    if (terminal_.applicationCursorKeys())
        flags |= static_cast<uint8_t>(MatchModes::Flag::AppCursor);

    if (terminal_.applicationKeypad())
        flags |= static_cast<uint8_t>(MatchModes::Flag::AppKeypad);

    return flags;
}

void TerminalSession::setFontSize(text::font_size _size)
{
    if (!display_->setFontSize(_size))
        return;

    profile_.fonts.size = _size;
}

bool TerminalSession::reloadConfigWithProfile(string const& _profileName)
{
    auto newConfig = config::Config{};
    auto configFailures = int{0};
    auto const configLogger = [&](string const& _msg)
    {
        cerr << "Configuration failure. " << _msg << '\n';
        ++configFailures;
    };

    try
    {
        loadConfigFromFile(newConfig, config_.backingFilePath.string());
    }
    catch (exception const& e)
    {
        //TODO: logger_.error(e.what());
        configLogger(unhandledExceptionMessage(__PRETTY_FUNCTION__, e));
    }

    if (!newConfig.profile(_profileName))
        configLogger(fmt::format("Currently active profile with name '{}' gone.", _profileName));

    if (configFailures)
    {
        cerr << "Failed to load configuration.\n";
        return false;
    }

    return reloadConfig(std::move(newConfig), _profileName);
}

bool TerminalSession::resetConfig()
{
    auto const ec = config::createDefaultConfig(config_.backingFilePath);
    if (ec)
    {
        cerr << fmt::format("Failed to load default config at {}; ({}) {}\n",
                            config_.backingFilePath.string(),
                            ec.category().name(),
                            ec.message());
        return false;
    }

    config::Config defaultConfig;
    try
    {
        config::loadConfigFromFile(config_.backingFilePath);
    }
    catch (exception const& e)
    {
        LOGSTORE(SessionLog)("Failed to load default config: {}", e.what());
    }

    return reloadConfig(defaultConfig, defaultConfig.defaultProfileName);
}

void TerminalSession::followHyperlink(terminal::HyperlinkInfo const& _hyperlink)
{
    auto const fileInfo = QFileInfo(QString::fromStdString(string(_hyperlink.path())));
    auto const isLocal = _hyperlink.isLocal() && _hyperlink.host() == QHostInfo::localHostName().toStdString();
    auto const editorEnv = getenv("EDITOR");

    if (isLocal && fileInfo.isFile() && fileInfo.isExecutable())
    {
        QStringList args;
        args.append("config");
        args.append(QString::fromStdString(config_.backingFilePath.string()));
        args.append(QString::fromUtf8(_hyperlink.path().data(), static_cast<int>(_hyperlink.path().size())));
        QProcess::execute(QString::fromStdString(programPath_), args);
    }
    else if (isLocal && fileInfo.isFile() && editorEnv && *editorEnv)
    {
        QStringList args;
        args.append("config");
        args.append(QString::fromStdString(config_.backingFilePath.string()));
        args.append(QString::fromStdString(editorEnv));
        args.append(QString::fromUtf8(_hyperlink.path().data(), static_cast<int>(_hyperlink.path().size())));
        QProcess::execute(QString::fromStdString(programPath_), args);
    }
    else if (isLocal)
        QDesktopServices::openUrl(QUrl::fromLocalFile(QString::fromUtf8(string(_hyperlink.path()).c_str())));
    else
        QDesktopServices::openUrl(QString::fromUtf8(_hyperlink.uri.c_str()));
}

bool TerminalSession::requestPermission(config::Permission _allowedByConfig, string_view _topicText)
{
    return display_->requestPermission(_allowedByConfig, _topicText);
}

void TerminalSession::onConfigReload(FileChangeWatcher::Event /*_event*/)
{
    display_->post([this]() {
        reloadConfigWithProfile(profileName_);
    });

    // TODO: needed still?
    // if (setScreenDirty())
    //     update();
}

// }}}

} // end namespace
