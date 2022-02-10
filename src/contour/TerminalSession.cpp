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
#include <contour/ContourGuiApp.h>
#include <contour/TerminalSession.h>
#include <contour/helper.h>

#include <terminal/MatchModes.h>
#include <terminal/Terminal.h>
#include <terminal/pty/Pty.h>
#include <terminal/pty/PtyProcess.h>

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
#include <QtWidgets/QApplication>
#include <QtWidgets/QMessageBox>

#include <algorithm>
#include <fstream>

#include <QtNetwork/QHostInfo>

#if defined(CONTOUR_BLUR_PLATFORM_KWIN)
    #include <KWindowEffects>
#endif

#if !defined(_MSC_VER)
    #include <signal.h>
    #include <unistd.h>
#endif

#if defined(_MSC_VER)
    #define __PRETTY_FUNCTION__ __FUNCDNAME__
#endif

using std::chrono::steady_clock;
using namespace std;
using namespace terminal;

namespace contour
{

namespace
{
    string unhandledExceptionMessage(string_view const& where, exception const& e)
    {
        return fmt::format("{}: Unhandled exception caught ({}). {}", where, typeid(e).name(), e.what());
    }

} // namespace

TerminalSession::TerminalSession(unique_ptr<Pty> _pty,
                                 std::chrono::seconds _earlyExitThreshold,
                                 config::Config _config,
                                 bool _liveConfig,
                                 string _profileName,
                                 string _programPath,
                                 ContourGuiApp& _app,
                                 unique_ptr<TerminalDisplay> _display,
                                 std::function<void()> _displayInitialized,
                                 std::function<void()> _onExit):
    startTime_ { steady_clock::now() },
    earlyExitThreshold_ { _earlyExitThreshold },
    config_ { move(_config) },
    profileName_ { move(_profileName) },
    profile_ { *config_.profile(profileName_) },
    programPath_ { move(_programPath) },
    app_ { _app },
    displayInitialized_ { move(_displayInitialized) },
    onExit_ { move(_onExit) },
    pty_ { move(_pty) },
    terminal_ { *pty_,
                config_.ptyReadBufferSize,
                *this,
                config_.profile(profileName_)->maxHistoryLineCount,
                config_.profile(profileName_)->copyLastMarkRangeOffset,
                config_.profile(profileName_)->cursorBlinkInterval,
                steady_clock::now(),
                config_.wordDelimiters,              // TODO: move to profile!
                config_.bypassMouseProtocolModifier, // TODO: you too
                config_.maxImageSize,
                config_.maxImageColorRegisters,
                config_.sixelCursorConformance,
                profile_.colors,
                _display ? _display->refreshRate() : 50.0,
                config_.reflowOnResize },
    display_ { move(_display) }
{
    if (_liveConfig)
    {
        LOGSTORE(SessionLog)
        ("Enable live configuration reloading of file {}.", config_.backingFilePath.generic_string());
        configFileChangeWatcher_.emplace(config_.backingFilePath,
                                         [this](FileChangeWatcher::Event event) { onConfigReload(event); });
    }

    sanitizeConfig(_config);
    profile_ = *config_.profile(profileName_); // XXX do it again. but we've to be more efficient here
    configureTerminal();
}

TerminalSession::~TerminalSession()
{
    (void)
        display_.release(); // TODO: due to Qt, this is currently not owned by us. That's sad, or is it not?
}

void TerminalSession::setDisplay(unique_ptr<TerminalDisplay> _display)
{
    LOGSTORE(SessionLog)("Assigning display.");
    display_ = move(_display);

    // XXX find better way (dpi)
    sanitizeConfig(config_);
    profile_ = *config_.profile(profileName_); // XXX do it again. but we've to be more efficient here
}

void TerminalSession::displayInitialized()
{
    // NB: Inform connected TTY and local Screen instance about initial cell pixel size.
    auto const pixels = display_->cellSize() * terminal_.pageSize();
    // auto const pixels =
    //     ImageSize { display_->cellSize().width * boxed_cast<Width>(terminal_.pageSize().columns),
    //                 display_->cellSize().height * boxed_cast<Height>(terminal_.pageSize().lines) };
    terminal_.resizeScreen(terminal_.pageSize(), pixels);

    configureDisplay();

    if (displayInitialized_)
        displayInitialized_();
}

void TerminalSession::start()
{
    terminal().start();
}

void TerminalSession::terminate()
{
    if (!display_)
        return;

    display_->closeDisplay();
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
    display_->post([this, _type]() { display_->bufferChanged(_type); });
}

void TerminalSession::screenUpdated()
{
    if (profile_.autoScrollOnUpdate && terminal().viewport().scrolled())
        terminal().viewport().scrollToBottom();

    if (terminal().hasInput())
        display_->post(bind(&TerminalSession::flushInput, this));

    scheduleRedraw();
}

void TerminalSession::flushInput()
{
    terminal().flushInput();
    if (terminal().hasInput())
        display_->post(bind(&TerminalSession::flushInput, this));
}

void TerminalSession::renderBufferUpdated()
{
    if (!display_)
        return;

    display_->renderBufferUpdated();
}

void TerminalSession::requestCaptureBuffer(int _absoluteStartLine, int _lineCount)
{
    display_->post([this, _absoluteStartLine, _lineCount]() {
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
    display_->post([this, spec = terminal::FontDef(_fontDef)]() {
        if (!display_->requestPermission(profile_.permissions.changeFont, "changing font"))
            return;

        auto const& currentFonts = profile_.fonts;
        terminal::renderer::FontDescriptions newFonts = currentFonts;

        if (spec.size != 0.0)
            newFonts.size = text::font_size { spec.size };

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

void TerminalSession::inspect()
{
    if (display_)
        display_->inspect();

    // Deferred termination? Then close display now.
    if (terminal_.device().isClosed() && !app_.dumpStateAtExit().has_value())
        display_->closeDisplay();
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
    auto const diff = std::chrono::duration_cast<std::chrono::seconds>(now - startTime_);

    if (auto const* pty = dynamic_cast<terminal::PtyProcess const*>(&terminal_.device()))
    {
        auto const exitStatus = pty->process().checkStatus();
        if (exitStatus)
            LOGSTORE(SessionLog)
        ("Process terminated after {} seconds with exit status {}.", diff.count(), *exitStatus);
        else LOGSTORE(SessionLog)("Process terminated after {} seconds.", diff.count());
    }
    else
        LOGSTORE(SessionLog)("Process terminated after {} seconds.", diff.count());

    if (onExit_)
        onExit_();

    if (diff < earlyExitThreshold_)
    {
        // auto const w = terminal_.pageSize().columns.as<int>();
        auto constexpr SGR = "\033[1;38:2::255:255:255m\033[48:2::255:0:0m"sv;
        auto constexpr EL = "\033[K"sv;
        auto constexpr TextLines = array<string_view, 2> { "Shell terminated too quickly.",
                                                           "The window will not be closed automatically." };
        for (auto const text: TextLines)
            terminal_.writeToScreen(fmt::format("\r\n{}{}{}", SGR, EL, text));
        terminal_.writeToScreen("\r\n");
        terminatedAndWaitingForKeyPress_ = true;
        return;
    }

    if (app_.dumpStateAtExit().has_value())
        inspect();
    else if (display_)
        display_->closeDisplay();
}

void TerminalSession::onSelectionCompleted()
{
    switch (config_.onMouseSelection)
    {
    case config::SelectionAction::CopyToSelectionClipboard:
        if (QClipboard* clipboard = QGuiApplication::clipboard();
            clipboard != nullptr && clipboard->supportsSelection())
        {
            string const text = terminal().extractSelectionText();
            clipboard->setText(QString::fromUtf8(text.c_str(), static_cast<int>(text.size())),
                               QClipboard::Selection);
        }
        break;
    case config::SelectionAction::CopyToClipboard:
        if (QClipboard* clipboard = QGuiApplication::clipboard(); clipboard != nullptr)
        {
            string const text = terminal().extractSelectionText();
            clipboard->setText(QString::fromUtf8(text.c_str(), static_cast<int>(text.size())),
                               QClipboard::Clipboard);
        }
        break;
    case config::SelectionAction::Nothing: break;
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

    display_->post([this, terminalTitle = string(_title)]() { display_->setWindowTitle(terminalTitle); });
}

void TerminalSession::setTerminalProfile(string const& _configProfileName)
{
    if (!display_)
        return;

    display_->post([this, name = string(_configProfileName)]() { activateProfile(name); });
}

void TerminalSession::discardImage(terminal::Image const& _image)
{
    if (!display_)
        return;

    display_->discardImage(_image);
}

// }}}
// {{{ Input Events
void TerminalSession::sendKeyPressEvent(Key _key, Modifier _modifier, Timestamp _now)
{
    LOGSTORE(InputLog)("{} {}", _modifier, _key);

    if (terminatedAndWaitingForKeyPress_)
    {
        display_->closeDisplay();
        return;
    }

    display_->setMouseCursorShape(MouseCursorShape::Hidden);

    if (auto const* actions =
            config::apply(config_.inputMappings.keyMappings, _key, _modifier, matchModeFlags()))
        executeAllActions(*actions);
    else
        terminal().sendKeyPressEvent(_key, _modifier, _now);
}

void TerminalSession::sendCharPressEvent(char32_t _value, Modifier _modifier, Timestamp _now)
{
    LOGSTORE(InputLog)("{} {}", _modifier, static_cast<uint32_t>(_value));
    assert(display_ != nullptr);

    if (terminatedAndWaitingForKeyPress_)
    {
        display_->closeDisplay();
        return;
    }

    display_->setMouseCursorShape(MouseCursorShape::Hidden);

    if (auto const* actions =
            config::apply(config_.inputMappings.charMappings, _value, _modifier, matchModeFlags()))
        executeAllActions(*actions);
    else
        terminal().sendCharPressEvent(_value, _modifier, _now); // TODO: get rid of Event{} struct here, too!
}

void TerminalSession::sendMousePressEvent(Modifier _modifier,
                                          MouseButton _button,
                                          MousePixelPosition _pixelPosition,
                                          Timestamp _now)
{
    // LOGSTORE(InputLog)("sendMousePressEvent: {} {} at {}", _button, _modifier, currentMousePosition_);

    // First try to pass the mouse event to the application, as it might have requested that.
    if (terminal().sendMousePressEvent(_modifier, _button, _pixelPosition, _now))
    {
        scheduleRedraw();
        return;
    }

    if (auto const* actions =
            config::apply(config_.inputMappings.mouseMappings, _button, _modifier, matchModeFlags()))
    {
        if (executeAllActions(*actions))
            return;
    }

    if (_button != MouseButton::Left)
        return;
    if (!terminal_.handleMouseSelection(_modifier, _now))
        return;
    scheduleRedraw();
}

void TerminalSession::sendMouseMoveEvent(terminal::Modifier _modifier,
                                         terminal::CellLocation _pos,
                                         terminal::MousePixelPosition _pixelPosition,
                                         Timestamp _now)
{
    // NB: This translation depends on the display's margin, so maybe
    //     the display should provide the translation?

    auto const handled = terminal().sendMouseMoveEvent(_modifier, _pos, _pixelPosition, _now);

    if (_pos == currentMousePosition_)
        return;

    bool const mouseHoveringHyperlink = terminal().isMouseHoveringHyperlink();
    currentMousePosition_ = _pos;
    if (mouseHoveringHyperlink)
        display_->setMouseCursorShape(MouseCursorShape::PointingHand);
    else
        setDefaultCursor();

    // TODO: enter this if only if: `&& only if selection has changed!`
    if (mouseHoveringHyperlink || handled || terminal().isSelectionInProgress())
    {
        terminal().breakLoopAndRefreshRenderBuffer();
        scheduleRedraw();
    }
}

void TerminalSession::sendMouseReleaseEvent(Modifier _modifier,
                                            MouseButton _button,
                                            MousePixelPosition _pixelPosition,
                                            Timestamp _now)
{
    terminal().sendMouseReleaseEvent(_modifier, _button, _pixelPosition, _now);
    scheduleRedraw();
}

void TerminalSession::sendFocusInEvent()
{
    // as per Qt-documentation, some platform implementations reset the cursor when leaving the
    // window, so we have to re-apply our desired cursor in focusInEvent().
    setDefaultCursor();

    terminal().sendFocusInEvent();

    display_->setBlurBehind(profile().backgroundBlur);
    scheduleRedraw();
}

void TerminalSession::sendFocusOutEvent()
{
    // TODO maybe paint with "faint" colors
    terminal().sendFocusOutEvent();

    scheduleRedraw();
}

// }}}
// {{{ Actions
bool TerminalSession::operator()(actions::CancelSelection)
{
    terminal_.clearSelection();
    return true;
}

bool TerminalSession::operator()(actions::ChangeProfile const& _action)
{
    LOGSTORE(SessionLog)("Changing profile to: {}", _action.name);
    if (_action.name == profileName_)
        return true;

    activateProfile(_action.name);
    return true;
}

bool TerminalSession::operator()(actions::ClearHistoryAndReset)
{
    LOGSTORE(SessionLog)("Clearing history and perform terminal hard reset");

    auto const pageSize = terminal_.pageSize();
    auto const pixelSize = display_->pixelSize();

    terminal_.resetHard();
    auto const tmpPageSize = PageSize { pageSize.lines, pageSize.columns + ColumnCount(1) };
    terminal_.resizeScreen(tmpPageSize, pixelSize);
    this_thread::yield();
    terminal_.resizeScreen(pageSize, pixelSize);
    return true;
}

bool TerminalSession::operator()(actions::CopyPreviousMarkRange)
{
    copyToClipboard(terminal().extractLastMarkRange());
    return true;
}

bool TerminalSession::operator()(actions::CopySelection)
{
    copyToClipboard(terminal().extractSelectionText());
    return true;
}

bool TerminalSession::operator()(actions::DecreaseFontSize)
{
    auto constexpr OnePt = text::font_size { 1.0 };
    setFontSize(profile().fonts.size - OnePt);
    // auto const currentFontSize = view().renderer().fontDescriptions().size;
    // auto const newFontSize = currentFontSize - OnePt;
    // setFontSize(newFontSize);
    return true;
}

bool TerminalSession::operator()(actions::DecreaseOpacity)
{
    if (static_cast<uint8_t>(profile_.backgroundOpacity) == 0)
        return true;

    --profile_.backgroundOpacity;
    display_->setBackgroundOpacity(profile_.backgroundOpacity);
    return true;
}

bool TerminalSession::operator()(actions::FollowHyperlink)
{
    auto const _l = scoped_lock { terminal() };
    auto const currentMousePosition = terminal().currentMousePosition();
    auto const currentMousePositionRel =
        terminal::CellLocation { currentMousePosition.line
                                     + terminal().viewport().scrollOffset().as<LineOffset>(),
                                 currentMousePosition.column };
    if (terminal().screen().contains(currentMousePosition))
    {
        if (auto hyperlink = terminal().screen().hyperlinkAt(currentMousePositionRel))
        {
            followHyperlink(*hyperlink);
            return true;
        }
    }
    return false;
}

bool TerminalSession::operator()(actions::IncreaseFontSize)
{
    auto constexpr OnePt = text::font_size { 1.0 };
    // auto const currentFontSize = view().renderer().fontDescriptions().size;
    // auto const newFontSize = currentFontSize + OnePt;
    // setFontSize(newFontSize);
    setFontSize(profile().fonts.size + OnePt);
    return true;
}

bool TerminalSession::operator()(actions::IncreaseOpacity)
{
    if (static_cast<uint8_t>(profile_.backgroundOpacity) >= 255)
        return true;

    ++profile_.backgroundOpacity;
    display_->setBackgroundOpacity(profile_.backgroundOpacity);
    return true;
}

bool TerminalSession::operator()(actions::NewTerminal const& _action)
{
    spawnNewTerminal(_action.profileName.value_or(profileName_));
    return true;
}

bool TerminalSession::operator()(actions::OpenConfiguration)
{
    if (!QDesktopServices::openUrl(QUrl(QString::fromUtf8(config_.backingFilePath.string().c_str()))))
        errorlog()("Could not open configuration file \"{}\".", config_.backingFilePath.generic_string());

    return true;
}

bool TerminalSession::operator()(actions::OpenFileManager)
{
    auto const _l = scoped_lock { terminal() };
    auto const& cwd = terminal().screen().currentWorkingDirectory();
    if (!QDesktopServices::openUrl(QUrl(QString::fromUtf8(cwd.c_str()))))
        errorlog()("Could not open file \"{}\".", cwd);

    return true;
}

bool TerminalSession::operator()(actions::PasteClipboard)
{
    if (QClipboard* clipboard = QGuiApplication::clipboard(); clipboard != nullptr)
    {
        string const text = clipboard->text(QClipboard::Clipboard).toUtf8().toStdString();
        terminal().sendPaste(string_view { text });
    }

    return true;
}

bool TerminalSession::operator()(actions::PasteSelection)
{
    if (QClipboard* clipboard = QGuiApplication::clipboard(); clipboard != nullptr)
    {
        string const text = clipboard->text(QClipboard::Selection).toUtf8().toStdString();
        terminal().sendPaste(string_view { text });
    }

    return true;
}

bool TerminalSession::operator()(actions::Quit)
{
    // TODO: later warn here when more then one terminal view is open
    terminal().device().close();
    exit(EXIT_SUCCESS);
}

bool TerminalSession::operator()(actions::ReloadConfig const& _action)
{
    if (_action.profileName.has_value())
        reloadConfigWithProfile(_action.profileName.value());
    else
        reloadConfigWithProfile(profileName_);

    return true;
}

bool TerminalSession::operator()(actions::ResetConfig)
{
    resetConfig();
    return true;
}

bool TerminalSession::operator()(actions::ResetFontSize)
{
    setFontSize(config_.profile(profileName_)->fonts.size);
    return true;
}

bool TerminalSession::operator()(actions::ScreenshotVT)
{
    auto _l = lock_guard { terminal() };
    auto const screenshot = terminal().screen().screenshot();
    ofstream ofs { "screenshot.vt", ios::trunc | ios::binary };
    ofs << screenshot;
    return true;
}

bool TerminalSession::operator()(actions::ScrollDown)
{
    terminal().viewport().scrollDown(profile_.historyScrollMultiplier);
    return true;
}

bool TerminalSession::operator()(actions::ScrollMarkDown)
{
    terminal().viewport().scrollMarkDown();
    return true;
}

bool TerminalSession::operator()(actions::ScrollMarkUp)
{
    terminal().viewport().scrollMarkUp();
    return true;
}

bool TerminalSession::operator()(actions::ScrollOneDown)
{
    terminal().viewport().scrollDown(LineCount(1));
    return true;
}

bool TerminalSession::operator()(actions::ScrollOneUp)
{
    terminal().viewport().scrollUp(LineCount(1));
    return true;
}

bool TerminalSession::operator()(actions::ScrollPageDown)
{
    auto const stepSize = terminal().pageSize().lines / LineCount(2);
    terminal().viewport().scrollDown(stepSize);
    return true;
}

bool TerminalSession::operator()(actions::ScrollPageUp)
{
    auto const stepSize = terminal().pageSize().lines / LineCount(2);
    terminal().viewport().scrollUp(stepSize);
    return true;
}

bool TerminalSession::operator()(actions::ScrollToBottom)
{
    terminal().viewport().scrollToBottom();
    return true;
}

bool TerminalSession::operator()(actions::ScrollToTop)
{
    terminal().viewport().scrollToTop();
    return true;
}

bool TerminalSession::operator()(actions::ScrollUp)
{
    terminal().viewport().scrollUp(profile_.historyScrollMultiplier);
    return true;
}

bool TerminalSession::operator()(actions::SendChars const& _event)
{
    auto const now = steady_clock::now();

    for (auto const ch: _event.chars)
    {
        terminal().sendCharPressEvent(static_cast<char32_t>(ch), terminal::Modifier::None, now);
    }
    return true;
}

bool TerminalSession::operator()(actions::ToggleAllKeyMaps)
{
    allowKeyMappings_ = !allowKeyMappings_;
    LOGSTORE(InputLog)("{} key mappings.", allowKeyMappings_ ? "Enabling" : "Disabling");
    return true;
}

bool TerminalSession::operator()(actions::ToggleFullscreen)
{
    if (display_)
        display_->toggleFullScreen();
    return true;
}

bool TerminalSession::operator()(actions::WriteScreen const& _event)
{
    terminal().writeToScreen(_event.chars);
    return true;
}
// }}}
// {{{ implementation helpers
void TerminalSession::setDefaultCursor()
{
    using Type = terminal::ScreenType;
    switch (terminal().screen().bufferType())
    {
    case Type::Main: display_->setMouseCursorShape(MouseCursorShape::IBeam); break;
    case Type::Alternate: display_->setMouseCursorShape(MouseCursorShape::Arrow); break;
    }
}

void TerminalSession::sanitizeConfig(config::Config& _config)
{
    if (!display_)
        return;

    auto const fontDPI = display_->fontDPI(); // profile.fonts.dpiScale is applied already
    for (config::TerminalProfile& profile: _config.profiles | ::ranges::views::values)
        if (!profile.fonts.dpi.x || !profile.fonts.dpi.y)
            profile.fonts.dpi = fontDPI;
}

bool TerminalSession::reloadConfig(config::Config _newConfig, string const& _profileName)
{
    LOGSTORE(SessionLog)
    ("Reloading configuration from {} with profile {}", _newConfig.backingFilePath.string(), _profileName);

    sanitizeConfig(_newConfig);

    config_ = move(_newConfig);
    activateProfile(_profileName);

    return true;
}

int TerminalSession::executeAllActions(std::vector<actions::Action> const& _actions)
{
    if (allowKeyMappings_)
    {
        int executionCount = 0;
        for (actions::Action const& action: _actions)
            if (executeAction(action))
                ++executionCount;
        scheduleRedraw();
        return executionCount;
    }

    auto const containsToggleKeybind = [](std::vector<actions::Action> const& _actions) {
        return std::any_of(_actions.begin(), _actions.end(), [](actions::Action const& action) {
            return holds_alternative<actions::ToggleAllKeyMaps>(action);
        });
    };

    if (containsToggleKeybind(_actions))
    {
        bool const ex = executeAction(actions::ToggleAllKeyMaps {});
        scheduleRedraw();
        return ex;
    }

    LOGSTORE(InputLog)("Key mappings are currently disabled via ToggleAllKeyMaps input mapping action.");
    return 0;
}

// Executes given action @p _action.
//
// The return value indicates whether or not this action did apply or not.
// For example a FollowHyperlink only applies when there is a hyperlink
// at the current cursor position to follow,
// however, a ScrollToTop applies regardless of the current viewport
// scrolling position.
bool TerminalSession::executeAction(actions::Action const& _action)
{
    LOGSTORE(SessionLog)("executeAction: {}", _action);
    return visit(*this, _action);
}

void TerminalSession::spawnNewTerminal(string const& _profileName)
{
    auto const wd = [this]() -> string {
#if defined(__APPLE__)
        if (auto const* ptyProcess = dynamic_cast<PtyProcess const*>(pty_.get()))
        {
            return ptyProcess->process().workingDirectory(&ptyProcess->pty());
        }
#else
        auto const _l = scoped_lock { terminal_ };
        return terminal_.screen().currentWorkingDirectory();
#endif
        return "."s;
    }();

    if (config_.spawnNewProcess)
    {
        ::contour::spawnNewTerminal(programPath_, config_.backingFilePath.generic_string(), _profileName, wd);
    }
    else
    {
        auto config = config_;
        config.profile(profileName_)->shell.workingDirectory = FileSystem::path(wd);
        app_.newWindow(config);
    }
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
    auto const _l = scoped_lock { terminal_ };
    LOGSTORE(SessionLog)("Configuring terminal.");
    auto& screen = terminal_.screen();

    terminal_.setWordDelimiters(config_.wordDelimiters);
    terminal_.setMouseProtocolBypassModifier(config_.bypassMouseProtocolModifier);
    terminal_.setMouseBlockSelectionModifier(config_.mouseBlockSelectionModifier);
    terminal_.setLastMarkRangeOffset(profile_.copyLastMarkRangeOffset);

    LOGSTORE(SessionLog)("Setting terminal ID to {}.", profile_.terminalId);
    screen.setTerminalId(profile_.terminalId);
    screen.setSixelCursorConformance(config_.sixelCursorConformance);
    terminal_.setMaxImageColorRegisters(config_.maxImageColorRegisters);
    screen.setMaxImageSize(config_.maxImageSize);
    LOGSTORE(SessionLog)
    ("maxImageSize={}, sixelScrolling={}", config_.maxImageSize, config_.sixelScrolling ? "yes" : "no");
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
    display_->setBlurBehind(profile_.backgroundBlur);

    display_->setBackgroundImage(profile_.colors.backgroundImage);

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
    display_->setPageSize(pageSize);
    display_->setFonts(profile_.fonts);
    // TODO: maybe update margin after this call?

    display_->setHyperlinkDecoration(profile_.hyperlinkDecoration.normal, profile_.hyperlinkDecoration.hover);

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

    if (terminal_.selectionAvailable())
        flags |= static_cast<uint8_t>(MatchModes::Flag::Select);

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
    auto newConfig = config::Config {};
    auto configFailures = int { 0 };

    try
    {
        loadConfigFromFile(newConfig, config_.backingFilePath.string());
    }
    catch (exception const& e)
    {
        // TODO: logger_.error(e.what());
        errorlog()("Configuration failure. {}", unhandledExceptionMessage(__PRETTY_FUNCTION__, e));
        ++configFailures;
    }

    if (!newConfig.profile(_profileName))
    {
        errorlog()(fmt::format("Currently active profile with name '{}' gone.", _profileName));
        ++configFailures;
    }

    if (configFailures)
    {
        errorlog()("Failed to load configuration.");
        return false;
    }

    return reloadConfig(std::move(newConfig), _profileName);
}

bool TerminalSession::resetConfig()
{
    auto const ec = config::createDefaultConfig(config_.backingFilePath);
    if (ec)
    {
        errorlog()("Failed to load default config at {}; ({}) {}",
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
    auto const isLocal =
        _hyperlink.isLocal() && _hyperlink.host() == QHostInfo::localHostName().toStdString();
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
    display_->post([this]() { reloadConfigWithProfile(profileName_); });

    // TODO: needed still?
    // if (setScreenDirty())
    //     update();
}

// }}}

} // namespace contour
