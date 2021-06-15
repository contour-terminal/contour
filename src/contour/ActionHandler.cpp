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
#include <contour/ActionHandler.h>
#include <contour/helper.h>

#include <terminal/Metrics.h>
#include <terminal/pty/Pty.h>
#if defined(_MSC_VER)
#include <terminal/pty/ConPty.h>
#else
#include <terminal/pty/UnixPty.h>
#endif

#include <crispy/debuglog.h>

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

#if defined(CONTOUR_BLUR_PLATFORM_KWIN)
#include <KWindowEffects>
#endif

#include <variant>
#include <fstream>

using namespace std;
using std::chrono::steady_clock;
using crispy::Size;

namespace contour::actions
{

namespace // {{{ helper
{
    inline char const* signalName(int _signo)
    {
#if defined(__unix__) || defined(__APPLE__)
        return strsignal(_signo);
#else
        return "unknown";
#endif
    }

    constexpr crispy::Point scale(crispy::Point p, double s)
    {
        return crispy::Point{
            static_cast<int>(static_cast<double>(p.x) * s),
            static_cast<int>(static_cast<double>(p.y) * s)
        };
    }

    double sanitizeRefreshRate(double _userValue, double _systemValue) noexcept
    {
        if (1.0 < _userValue && _userValue < _systemValue)
            return _userValue;
        else
            return _systemValue;
    }

    std::string unhandledExceptionMessage(string_view const& where, exception const& e)
    {
        return fmt::format("{}: Unhandled exception caught ({}). {}", where, typeid(e).name(), e.what());
    }

    // void reportUnhandledException(string_view const& where, exception const& e)
    // {
    //     debuglog(WidgetTag).write("{}", unhandledExceptionMessage(where, e));
    //     cerr << unhandledExceptionMessage(where, e) << endl;
    // }

    QScreen* screenOf(QWidget& _widget)
    {
#if QT_VERSION >= QT_VERSION_CHECK(5, 14, 0)
        return _widget.screen();
#elif QT_VERSION >= QT_VERSION_CHECK(5, 10, 0)
        #warning "Using alternative implementation of screenOf() for Qt >= 5.10.0"
        if (auto topLevel = _widget.window())
        {
            if (auto screenByPos = QGuiApplication::screenAt(topLevel->geometry().center()))
                return screenByPos;
        }
        return QGuiApplication::primaryScreen();
#else
        #warning "Using alternative implementation of screenOf() for Qt < 5.10.0"
        return QGuiApplication::primaryScreen();
#endif
    }
} // }}}

ActionHandler::ActionHandler(config::Config _config,
                             string _profileName,
                             string _programPath,
                             bool _liveConfig,
                             crispy::Point _dpi,
                             function<void()> _updateDisplay,
                             function<void(terminal::ScreenType)> _bufferTypeChanged,
                             function<void(bool)> _setBackgroundBlur,
                             function<void()> _profileChanged,
                             function<void(string_view, string_view)> _showNotification
):
    config_{ move(_config) },
    profileName_{ move(_profileName) },
    profile_{ *config_.profile(profileName_) },
    programPath_{ std::move(_programPath) },
    terminalView_{make_unique<terminal::view::TerminalView>(
        steady_clock::now(),
        config_.ptyReadBufferSize,
        *this,
        config_.profile().maxHistoryLineCount,
        config_.wordDelimiters,
        config_.bypassMouseProtocolModifier,
        scale(_dpi, config_.profile().fonts.dpiScale),
        config_.profile().fonts,
        config_.profile().cursorShape,
        config_.profile().cursorDisplay,
        config_.profile().cursorBlinkInterval,
        config_.profile().colors,
        config_.profile().backgroundOpacity,
        config_.profile().hyperlinkDecoration.normal,
        config_.profile().hyperlinkDecoration.hover,
#if defined(_MSC_VER)
        make_unique<terminal::ConPty>(config_.profile().terminalSize),
#else
        make_unique<terminal::UnixPty>(config_.profile().terminalSize),
#endif
        config_.profile().shell,
        sanitizeRefreshRate(
            config_.profile().refreshRate,
            static_cast<double>(screenOf(widget()) ? screenOf(widget())->refreshRate() : 30.0)
        )
    )},
    updateDisplay_{ std::move(_updateDisplay) },
    terminalBufferChanged_{ std::move(_bufferTypeChanged) },
    setBackgroundBlur_{ std::move(_setBackgroundBlur) },
    profileChanged_{ std::move(_profileChanged) },
    showNotification_{ std::move(_showNotification) },
    allowKeyMappings_{ true },
    maximizedState_{ false }
{
    if (_liveConfig)
    {
        debuglog(WidgetTag).write("Enable live configuration reloading of file {}.",
                                  config().backingFilePath.generic_string());
        configFileChangeWatcher_.emplace(config().backingFilePath,
                                         [this](FileChangeWatcher::Event event) { onConfigReload(event); });
    }
}

void ActionHandler::displayInitialized()
{
    displayInitialized_ = true;;
    activateProfile(profileName());
}

void ActionHandler::setWidget(QWidget& _newTerminalWidget)
{
    terminalWidget_ = _newTerminalWidget;
}

// {{{ backend handlers
void ActionHandler::operator()(ChangeProfile const& _action)
{
    if (_action.name != profileName_)
    {
        activateProfile(_action.name);
    }
}

void ActionHandler::copyToClipboard(string_view _text)
{
    if (QClipboard* clipboard = QGuiApplication::clipboard(); clipboard != nullptr)
        clipboard->setText(QString::fromUtf8(_text.data(), static_cast<int>(_text.size())));
}

void ActionHandler::operator()(CopyPreviousMarkRange)
{
    copyToClipboard(terminal().extractLastMarkRange());
}

void ActionHandler::operator()(CopySelection)
{
    copyToClipboard(terminal().extractSelectionText());
}

void ActionHandler::operator()(DecreaseFontSize)
{
    auto constexpr OnePt = text::font_size{ 1.0 };
    setFontSize(profile().fonts.size - OnePt);
    // auto const currentFontSize = view().renderer().fontDescriptions().size;
    // auto const newFontSize = currentFontSize - OnePt;
    // setFontSize(newFontSize);
}

void ActionHandler::operator()(DecreaseOpacity)
{
    if (static_cast<uint8_t>(profile_.backgroundOpacity) == 0)
        return;

    --profile_.backgroundOpacity;
    view().setBackgroundOpacity(profile_.backgroundOpacity);
}

void ActionHandler::operator()(FollowHyperlink)
{
    auto const _l = scoped_lock{terminal()};
    auto const currentMousePosition = terminal().currentMousePosition();
    auto const currentMousePositionRel = terminal::Coordinate{
        currentMousePosition.row - terminal().viewport().relativeScrollOffset(),
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
}

void ActionHandler::operator()(IncreaseFontSize)
{
    auto constexpr OnePt = text::font_size{ 1.0 };
    // auto const currentFontSize = view().renderer().fontDescriptions().size;
    // auto const newFontSize = currentFontSize + OnePt;
    // setFontSize(newFontSize);
    setFontSize(profile().fonts.size + OnePt);
}

void ActionHandler::operator()(IncreaseOpacity)
{
    if (static_cast<uint8_t>(profile_.backgroundOpacity) >= 255)
        return;

    ++profile_.backgroundOpacity;
    view().setBackgroundOpacity(profile_.backgroundOpacity);
}

void ActionHandler::operator()(NewTerminal const& _action)
{
    spawnNewTerminal(_action.profileName.value_or(profileName_));
}

void ActionHandler::operator()(OpenConfiguration)
{
    if (!QDesktopServices::openUrl(QUrl(QString::fromUtf8(config().backingFilePath.string().c_str()))))
        cerr << "Could not open configuration file \"" << config().backingFilePath << "\"" << endl;
}

void ActionHandler::operator()(OpenFileManager)
{
    auto const _l = scoped_lock{terminal()};
    auto const& cwd = terminal().screen().currentWorkingDirectory();
    if (!QDesktopServices::openUrl(QUrl(QString::fromUtf8(cwd.c_str()))))
        cerr << "Could not open file \"" << cwd << "\"" << endl;
}

void ActionHandler::operator()(PasteClipboard)
{
    if (QClipboard* clipboard = QGuiApplication::clipboard(); clipboard != nullptr)
    {
        string const text = clipboard->text(QClipboard::Clipboard).toUtf8().toStdString();
        terminal().sendPaste(string_view{text});
    }
}

void ActionHandler::operator()(PasteSelection)
{
    if (QClipboard* clipboard = QGuiApplication::clipboard(); clipboard != nullptr)
    {
        string const text = clipboard->text(QClipboard::Selection).toUtf8().toStdString();
        terminal().sendPaste(string_view{text});
    }
}

void ActionHandler::operator()(Quit)
{
    //TODO: later warn here when more then one terminal view is open
    terminal().device().close();
    exit(EXIT_SUCCESS);
}

void ActionHandler::operator()(ReloadConfig const& _action)
{
    if (_action.profileName.has_value())
        reloadConfigWithProfile(_action.profileName.value());
    else
        reloadConfigWithProfile(profileName_);
}

void ActionHandler::operator()(ResetConfig)
{
    resetConfig();
}

void ActionHandler::operator()(ResetFontSize)
{
    setFontSize(config_.profile(profileName_)->fonts.size);
}

void ActionHandler::operator()(ScreenshotVT)
{
    auto _l = lock_guard{ terminal() };
    auto const screenshot = terminal().screen().screenshot();
    ofstream ofs{ "screenshot.vt", ios::trunc | ios::binary };
    ofs << screenshot;
}

void ActionHandler::operator()(ScrollDown)
{
    terminal().viewport().scrollDown(profile_.historyScrollMultiplier);
}

void ActionHandler::operator()(ScrollMarkDown)
{
    terminal().viewport().scrollMarkDown();
}

void ActionHandler::operator()(ScrollMarkUp)
{
    terminal().viewport().scrollMarkUp();
}

void ActionHandler::operator()(ScrollOneDown)
{
    terminal().viewport().scrollDown(1);
}

void ActionHandler::operator()(ScrollOneUp)
{
    terminal().viewport().scrollUp(1);
}

void ActionHandler::operator()(ScrollPageDown)
{
    auto const terminalSize = terminal().screenSize();
    terminal().viewport().scrollDown(terminalSize.height / 2);
}

void ActionHandler::operator()(ScrollPageUp)
{
    auto const terminalSize = terminal().screenSize();
    terminal().viewport().scrollUp(terminalSize.height / 2);
}

void ActionHandler::operator()(ScrollToBottom)
{
    terminal().viewport().scrollToBottom();
}

void ActionHandler::operator()(ScrollToTop)
{
    terminal().viewport().scrollToTop();
}

void ActionHandler::operator()(ScrollUp)
{
    terminal().viewport().scrollUp(profile_.historyScrollMultiplier);
}

void ActionHandler::operator()(SendChars const& _event)
{
    for (auto const ch: _event.chars)
        terminal().send(
            terminal::CharInputEvent{
                static_cast<char32_t>(ch),
                terminal::Modifier::None
            },
            steady_clock::now()
        );
}

void ActionHandler::operator()(ToggleAllKeyMaps)
{
    allowKeyMappings_ = !allowKeyMappings_;
    debuglog(KeyboardTag).write(
        "{} key mappings.",
        allowKeyMappings_ ? "Enabling" : "Disabling"
    );
}

void ActionHandler::operator()(ToggleFullscreen)
{
    toggleFullscreen();
}

void ActionHandler::operator()(WriteScreen const& _event)
{
    terminal().writeToScreen(_event.chars);
}

// }}}
// {{{ Events implementations
void ActionHandler::bell()
{
    debuglog(WidgetTag).write("TODO: Beep!");
    QApplication::beep();
    // QApplication::beep() requires Qt Widgets dependency. doesn't suound good.
    // so maybe just a visual bell then? That would require additional OpenGL/shader work then though.
}

void ActionHandler::bufferChanged(terminal::ScreenType _type)
{
    currentScreenType_ = _type;
    post([this, _type] ()
    {
        setDefaultCursor();
        terminalBufferChanged_(_type);
    });
}

void ActionHandler::screenUpdated()
{
#if defined(CONTOUR_VT_METRICS)
    // TODO
    // for (auto const& command : _commands)
    //     terminalMetrics_(command);
#endif
    if (profile().autoScrollOnUpdate && terminal().viewport().scrolled())
        terminal().viewport().scrollToBottom();

    renderBufferUpdated();
}

void ActionHandler::renderBufferUpdated()
{
    // TODO: still needed?
    // if (setScreenDirty())
    //     update(); //QCoreApplication::postEvent(this, new QEvent(QEvent::UpdateRequest));
    updateDisplay_();
}

void ActionHandler::requestCaptureBuffer(int _absoluteStartLine, int _lineCount)
{
    post([this, _absoluteStartLine, _lineCount]() {
        if (requestPermission(profile().permissions.captureBuffer, "capture screen buffer"))
        {
            terminal().screen().captureBuffer(_absoluteStartLine, _lineCount);
        }
    });
}

void ActionHandler::setFontDef(terminal::FontDef const& _fontDef)
{
    post([this, spec = terminal::FontDef(_fontDef)]() {
        if (requestPermission(profile().permissions.changeFont, "changing font"))
        {
            auto const& currentFonts = view().renderer().fontDescriptions();
            terminal::renderer::FontDescriptions newFonts = currentFonts;

            if (spec.size != 0.0)
                newFonts.size = text::font_size{ spec.size };

            if (!spec.regular.empty())
                newFonts.regular = currentFonts.regular;

            auto const styledFont = [&](string_view _font) -> text::font_description {
                // if a styled font is "auto" then infer froom regular font"
                if (_font == "auto"sv)
                    return currentFonts.regular;
                else
                    return text::font_description::parse(_font);
            };

            if (!spec.bold.empty())
                fonts_.bold = styledFont(spec.bold);

            if (!spec.italic.empty())
                fonts_.italic = styledFont(spec.italic);

            if (!spec.boldItalic.empty())
                fonts_.boldItalic = styledFont(spec.boldItalic);

            if (!spec.emoji.empty() && spec.emoji != "auto"sv)
                fonts_.emoji = text::font_description::parse(spec.emoji);

            view().renderer().setFonts(newFonts);
        }
    });
}

void ActionHandler::dumpState()
{
    post([this]() { doDumpState(); });
}

void ActionHandler::notify(string_view _title, string_view _content)
{
    showNotification_(_title, _content);
}

void ActionHandler::onClosed()
{
    using terminal::Process;

    // TODO: silently quit instantly when window/terminal has been spawned already since N seconds.
    // This message should only be printed for "fast" terminal terminations.

    view().waitForProcessExit();
    terminal::Process::ExitStatus const ec = *view().process().checkStatus();
    if (holds_alternative<Process::SignalExit>(ec))
        terminal().writeToScreen(fmt::format("\r\nShell has terminated with signal {} ({}).",
                                             get<Process::SignalExit>(ec).signum,
                                             signalName(get<Process::SignalExit>(ec).signum)));
    else if (auto const normalExit = get<Process::NormalExit>(ec); normalExit.exitCode != EXIT_SUCCESS)
        terminal().writeToScreen(fmt::format("\r\nShell has terminated with exit code {}.",
                                                            normalExit.exitCode));
    else
        widget().close(); // TODO: call this only from within the GUI thread!
}

void ActionHandler::onSelectionComplete()
{
    if (QClipboard* clipboard = QGuiApplication::clipboard(); clipboard != nullptr)
    {
        string const text = terminal().extractSelectionText();
        clipboard->setText(QString::fromUtf8(text.c_str(), static_cast<int>(text.size())), QClipboard::Selection);
    }
}

void ActionHandler::resizeWindow(int _width, int _height, bool _inPixels)
{
    debuglog(WidgetTag).write("Application request to resize window: {}x{} {}", _width, _height, _inPixels ? "px" : "cells");

    if (widget().window()->isFullScreen())
    {
        cerr << "Application request to resize window in full screen mode denied." << endl;
    }
    else if (_inPixels)
    {
        auto const screenSize = widget().size();

        if (!_width)
            _width = screenSize.width();

        if (!_height)
            _height = screenSize.height();

        auto const width = _width / view().gridMetrics().cellSize.width;
        auto const height = _height / view().gridMetrics().cellSize.height;
        auto const newScreenSize = Size{width, height};
        post([this, newScreenSize]() { setSize(newScreenSize); });
    }
    else
    {
        if (!_width)
            _width = profile().terminalSize.width;

        if (!_height)
            _height = profile().terminalSize.height;

        auto const newScreenSize = Size{_width, _height};
        post([this, newScreenSize]() { setSize(newScreenSize); });
    }
}

void ActionHandler::setWindowTitle(string_view _title)
{
    post([this, terminalTitle = string(_title)]() {
        auto const title = terminalTitle.empty()
            ? "contour"s
            : fmt::format("{} - contour", terminalTitle);
        if (widget().window()->windowHandle())
            widget().window()->windowHandle()->setTitle(QString::fromUtf8(title.c_str()));
    });
}

void ActionHandler::setTerminalProfile(string const& _configProfileName)
{
    post([this, name = string(_configProfileName)]() {
        activateProfile(name);
    });
}

// }}}
// {{{ implementation helpers
void ActionHandler::post(function<void()> _fn)
{
    postToObject(&widget(), std::move(_fn));
}

bool ActionHandler::reloadConfig(config::Config _newConfig, string const& _profileName)
{
    debuglog(WidgetTag).write("Reloading configuration from {} with profile {}",
                              _newConfig.backingFilePath.string(),
                              _profileName);

    // sanitize/auto-fill refresh rates for profiles where it is set to 0 (auto)
    for (auto& profile: _newConfig.profiles)
    {
        profile.second.refreshRate = sanitizeRefreshRate(
            profile.second.refreshRate,
            static_cast<double>(screenOf(widget()) ? screenOf(widget())->refreshRate() : 30.0)
        );
    }

    configureTerminal(view(), _newConfig, _profileName);

    config_ = std::move(_newConfig);
    profileName_ = _profileName;

    return true;
}

void ActionHandler::spawnNewTerminal(string const& _profileName)
{
    ::contour::spawnNewTerminal(
        programPath_,
        config().backingFilePath.generic_string(),
        _profileName,
        [terminal = &view().terminal()]() -> string {
            auto const _l = scoped_lock{*terminal};
            return terminal->screen().currentWorkingDirectory();
        }()
    );
}

void ActionHandler::activateProfile(string const& _newProfileName)
{
    if (auto newProfile = config().profile(_newProfileName); newProfile)
    {
        debuglog(WidgetTag).write("Changing profile to '{}'.", _newProfileName);
        activateProfile(_newProfileName, *newProfile);
    }
    else
        debuglog(WidgetTag).write("Cannot change profile. No such profile: '{}'.", _newProfileName);
}

void ActionHandler::activateProfile(string const& _name, config::TerminalProfile newProfile)
{
    widget().setMinimumSize(view().cellWidth() * 3, view().cellHeight() * 2);

    if (newProfile.backgroundBlur != profile().backgroundBlur)
        setBackgroundBlur_(newProfile.backgroundBlur);

    if (newProfile.maximized)
        widget().window()->showMaximized();
    else
        widget().window()->showNormal();

    if (newProfile.fullscreen != widget().window()->isFullScreen())
        toggleFullscreen();

    profile_ = std::move(newProfile);
    profileName_ = _name;

    profileChanged_();
}

void ActionHandler::setFontSize(text::font_size _size)
{
    if (_size.pt < 5.) // Let's not be crazy.
        return;

    if (_size.pt > 200.)
        return;

    view().setFontSize(_size);
    profile_.fonts.size = _size;

    widget().setMinimumSize(view().cellWidth() * 3, view().cellHeight() * 2);
}

void ActionHandler::toggleFullscreen()
{
    if (widget().window()->isFullScreen())
    {
        widget().window()->showNormal();
        if (maximizedState_)
            widget().window()->showMaximized();
    }
    else
    {
        maximizedState_ = widget().window()->isMaximized();
        widget().window()->showFullScreen();
    }

    // if (window_.visibility() == QWindow::FullScreen)
    //     window_.setVisibility(QWindow::Windowed);
    // else
    //     window_.setVisibility(QWindow::FullScreen);
}

bool ActionHandler::reloadConfigWithProfile(string const& _profileName)
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
        loadConfigFromFile(newConfig, config().backingFilePath.string());

        for (config::TerminalProfile& profile: newConfig.profiles | ranges::views::values)
            profile.fonts.dpi = scale(
                crispy::Point{ widget().logicalDpiX(), widget().logicalDpiY() },
                profile.fonts.dpiScale
            );
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

bool ActionHandler::resetConfig()
{
    auto const ec = config::createDefaultConfig(config().backingFilePath);
    if (ec)
    {
        cerr << fmt::format("Failed to load default config at {}; ({}) {}\n",
                            config().backingFilePath.string(),
                            ec.category().name(),
                            ec.message());
        return false;
    }

    config::Config defaultConfig;
    try
    {
        config::loadConfigFromFile(config().backingFilePath);
    }
    catch (exception const& e)
    {
        debuglog(WidgetTag).write("Failed to load default config: {}", e.what());
    }

    return reloadConfig(defaultConfig, defaultConfig.defaultProfileName);
}

void ActionHandler::followHyperlink(terminal::HyperlinkInfo const& _hyperlink)
{
    auto const fileInfo = QFileInfo(QString::fromStdString(string(_hyperlink.path())));
    auto const isLocal = _hyperlink.isLocal() && _hyperlink.host() == QHostInfo::localHostName().toStdString();
    auto const editorEnv = getenv("EDITOR");

    if (isLocal && fileInfo.isFile() && fileInfo.isExecutable())
    {
        QStringList args;
        args.append("config");
        args.append(QString::fromStdString(config().backingFilePath.string()));
        args.append(QString::fromUtf8(_hyperlink.path().data(), static_cast<int>(_hyperlink.path().size())));
        QProcess::execute(QString::fromStdString(programPath_), args);
    }
    else if (isLocal && fileInfo.isFile() && editorEnv && *editorEnv)
    {
        QStringList args;
        args.append("config");
        args.append(QString::fromStdString(config().backingFilePath.string()));
        args.append(QString::fromStdString(editorEnv));
        args.append(QString::fromUtf8(_hyperlink.path().data(), static_cast<int>(_hyperlink.path().size())));
        QProcess::execute(QString::fromStdString(programPath_), args);
    }
    else if (isLocal)
        QDesktopServices::openUrl(QUrl::fromLocalFile(QString::fromUtf8(string(_hyperlink.path()).c_str())));
    else
        QDesktopServices::openUrl(QString::fromUtf8(_hyperlink.uri.c_str()));
}

bool ActionHandler::requestPermission(config::Permission _allowedByConfig, string_view _topicText)
{
    switch (_allowedByConfig)
    {
        case config::Permission::Allow:
            debuglog(WidgetTag).write("Permission for {} allowed by configuration.", _topicText);
            return true;
        case config::Permission::Deny:
            debuglog(WidgetTag).write("Permission for {} denied by configuration.", _topicText);
            return false;
        case config::Permission::Ask:
            break;
    }

    // Did we remember a last interactive question?
    if (auto const i = rememberedPermissions_.mapping.find(string(_topicText)); i != rememberedPermissions_.mapping.end())
        return i->second;

    debuglog(WidgetTag).write("Permission for {} requires asking user.", _topicText);

    auto const reply = QMessageBox::question(&widget(),
        fmt::format("{} requested", _topicText).c_str(),
        QString::fromStdString(fmt::format("The application has requested for {}. Do you allow this?", _topicText)),
        QMessageBox::StandardButton::Yes
            | QMessageBox::StandardButton::YesToAll
            | QMessageBox::StandardButton::No
            | QMessageBox::StandardButton::NoToAll,
        QMessageBox::StandardButton::NoButton
    );

    switch (reply)
    {
        case QMessageBox::StandardButton::NoToAll:
            rememberedPermissions_.mapping[string(_topicText)] = false;
            break;
        case QMessageBox::StandardButton::YesToAll:
            rememberedPermissions_.mapping[string(_topicText)] = true;
            [[fallthrough]];
        case QMessageBox::StandardButton::Yes:
            return true;
        default:
            break;
    }

    return false;
}

void ActionHandler::doDumpState()
{
    // makeCurrent(); TODO: OH that cannot be called here. is it sure to be current already as
    //                      we're called from main thread's paintGL?

    auto const tmpDir = FileSystem::path(QStandardPaths::writableLocation(QStandardPaths::TempLocation).toStdString());
    auto const targetDir = tmpDir / FileSystem::path("contour-debug");
    FileSystem::create_directories(targetDir);
    debuglog(WidgetTag).write("Dumping state into directory: {}", targetDir.generic_string());
    // TODO: The above should be done from the outside and the targetDir being passed into this call.
    // TODO: maybe zip this dir in the end.

    // TODO: use this file store for everything that needs to be dumped.
    terminal().screen().dumpState("Dump screen state.");
    view().renderer().dumpState(std::cout);

    enum class ImageBufferFormat { RGBA, RGB, Alpha };

    auto screenshotSaver = [](FileSystem::path const& _filename, ImageBufferFormat _format) {
        auto const [qImageFormat, elementCount] = [&]() -> tuple<QImage::Format, int> {
            switch (_format) {
                case ImageBufferFormat::RGBA: return tuple{QImage::Format_RGBA8888, 4};
                case ImageBufferFormat::RGB: return tuple{QImage::Format_RGB888, 3};
                case ImageBufferFormat::Alpha: return tuple{QImage::Format_Grayscale8, 1};
            }
            return tuple{QImage::Format_Grayscale8, 1};
        }();

        // That's a little workaround for MacOS/X's C++ Clang compiler.
        auto const theImageFormat = qImageFormat;
        auto const theElementCount = elementCount;

        return [_filename, theImageFormat, theElementCount](vector<uint8_t> const& _buffer, Size _size) {
            auto image = make_unique<QImage>(_size.width, _size.height, theImageFormat);
            // Vertically flip the image, because the coordinate system between OpenGL and desktop screens is inverse.
            crispy::for_each(
                // TODO: std::execution::seq,
                crispy::times(_size.height),
                [&_buffer, &image, theElementCount, _size](int i) {
                    uint8_t const* sourceLine = &_buffer.data()[i * _size.width * theElementCount];
                    copy(sourceLine, sourceLine + _size.width * theElementCount, image->scanLine(_size.height - i - 1));
                }
            );
            image->save(QString::fromStdString(_filename.generic_string()));
        };
    };

    auto const atlasScreenshotSaver = [&screenshotSaver, &targetDir](string const& _allocatorName,
                                                                     unsigned _instanceId,
                                                                     vector<uint8_t> const& _buffer,
                                                                     Size _size) {
        return [&screenshotSaver, &targetDir, &_buffer, _size, _allocatorName, _instanceId](ImageBufferFormat _format) {
            auto const formatText = [&]() {
                switch (_format) {
                    case ImageBufferFormat::RGBA: return "rgba"sv;
                    case ImageBufferFormat::RGB: return "rgb"sv;
                    case ImageBufferFormat::Alpha: return "alpha"sv;
                }
                return "unknown"sv;
            }();
            auto const fileName = targetDir / fmt::format("atlas-{}-{}-{}.png", _allocatorName, formatText, _instanceId);
            return screenshotSaver(fileName, _format)(_buffer, _size);
        };
    };

    terminal::renderer::RenderTarget& renderTarget = view().renderer().renderTarget();

    for (auto const* allocator: renderTarget.allAtlasAllocators())
    {
        for (auto const atlasID: allocator->activeAtlasTextures())
        {
            auto infoOpt = renderTarget.readAtlas(*allocator, atlasID);
            if (!infoOpt.has_value())
                continue;

            terminal::renderer::AtlasTextureInfo& info = infoOpt.value();
            auto const saveScreenshot = atlasScreenshotSaver(allocator->name(), atlasID.value, info.buffer, info.size);
            switch (info.format)
            {
                case terminal::renderer::atlas::Format::RGBA:
                    saveScreenshot(ImageBufferFormat::RGBA);
                    break;
                case terminal::renderer::atlas::Format::RGB:
                    saveScreenshot(ImageBufferFormat::RGB);
                    break;
                case terminal::renderer::atlas::Format::Red:
                    saveScreenshot(ImageBufferFormat::Alpha);
                    break;
            }
        }
    }

    renderTarget.scheduleScreenshot(screenshotSaver(targetDir / "screenshot.png", ImageBufferFormat::RGBA));
}

void ActionHandler::onConfigReload(FileChangeWatcher::Event /*_event*/)
{
    post([this]() { reloadConfigWithProfile(profileName_); });

    // TODO: needed still?
    // if (setScreenDirty())
    //     update();
}

void ActionHandler::setSize(Size _size)
{
    debuglog(WidgetTag).write("Calling setSize with {}", _size);

    profile().terminalSize = _size;
    view().setTerminalSize(profile().terminalSize);

    widget().updateGeometry();

    // TODO: needed?
    // if (setScreenDirty())
    //     update();
}

// }}}

} // end namespace
