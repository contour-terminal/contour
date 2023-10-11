// SPDX-License-Identifier: Apache-2.0
#include <contour/ContourGuiApp.h>
#include <contour/TerminalSession.h>
#include <contour/display/TerminalWidget.h>
#include <contour/helper.h>

#include <vtbackend/MatchModes.h>
#include <vtbackend/Terminal.h>
#include <vtbackend/ViCommands.h>

#include <vtpty/Process.h>
#include <vtpty/Pty.h>

#include <crispy/StackTrace.h>
#include <crispy/assert.h>

#include <QtCore/QDebug>
#include <QtCore/QFileInfo>
#include <QtCore/QMetaObject>
#include <QtCore/QMimeData>
#include <QtCore/QProcess>
#include <QtCore/QStandardPaths>
#include <QtCore/QTimer>
#include <QtGui/QClipboard>
#include <QtGui/QDesktopServices>
#include <QtGui/QGuiApplication>
#include <QtGui/QKeyEvent>
#include <QtGui/QScreen>
#include <QtGui/QWindow>
#include <QtNetwork/QHostInfo>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <limits>

#if !defined(_WIN32)
    #include <pthread.h>
#endif

#if !defined(_MSC_VER)
    #include <csignal>

    #include <unistd.h>
#endif

#if defined(_MSC_VER)
    #define __PRETTY_FUNCTION__ __FUNCDNAME__
#endif

using std::chrono::steady_clock;
using namespace std;
using namespace vtbackend;

namespace fs = std::filesystem;

namespace contour
{

namespace
{
    string unhandledExceptionMessage(string_view const& where, exception const& e)
    {
        return fmt::format("{}: Unhandled exception caught ({}). {}", where, typeid(e).name(), e.what());
    }

    void setThreadName(char const* name)
    {
#if defined(__APPLE__)
        pthread_setname_np(name);
#elif !defined(_WIN32)
        pthread_setname_np(pthread_self(), name);
#endif
    }

    ColorPalette const* preferredColorPalette(config::ColorConfig const& config,
                                              vtbackend::ColorPreference preference)
    {
        if (auto const* dualColorConfig = std::get_if<config::DualColorConfig>(&config))
        {
            switch (preference)
            {
                case vtbackend::ColorPreference::Dark: return &dualColorConfig->darkMode;
                case vtbackend::ColorPreference::Light: return &dualColorConfig->lightMode;
            }
        }
        else if (auto const* simpleColorConfig = std::get_if<config::SimpleColorConfig>(&config))
            return &simpleColorConfig->colors;

        errorLog()("preferredColorPalette: Unknown color config type.");
        return nullptr;
    }

    string normalize_crlf(QString&& text)
    {
#if !defined(_WIN32)
        return text.replace("\r\n", "\n").replace("\r", "\n").toUtf8().toStdString();
#else
        return text.toUtf8().toStdString();
#endif
    }

    string strip_if(string input, bool shouldStrip)
    {
        if (!shouldStrip)
            return input;

        string output = input;

        do
        {
            std::swap(input, output);
            output = crispy::replace(input, "  ", " ");
            output = crispy::replace(output, "\t", " ");
            output = crispy::replace(output, "\n", " ");
        } while (input != output);

        return output;
    }

    vtbackend::Settings createSettingsFromConfig(config::Config const& config,
                                                 config::TerminalProfile const& profile,
                                                 ColorPreference colorPreference)
    {
        auto settings = vtbackend::Settings {};

        settings.ptyBufferObjectSize = config.ptyBufferObjectSize;
        settings.ptyReadBufferSize = config.ptyReadBufferSize;
        settings.maxHistoryLineCount = profile.maxHistoryLineCount;
        settings.copyLastMarkRangeOffset = profile.copyLastMarkRangeOffset;
        settings.cursorBlinkInterval = profile.inputModes.insert.cursor.cursorBlinkInterval;
        settings.smoothLineScrolling = profile.smoothLineScrolling;
        settings.wordDelimiters = unicode::from_utf8(config.wordDelimiters);
        settings.mouseProtocolBypassModifier = config.bypassMouseProtocolModifier;
        settings.maxImageSize = config.maxImageSize;
        settings.maxImageRegisterCount = config.maxImageColorRegisters;
        settings.statusDisplayType = profile.initialStatusDisplayType;
        settings.statusDisplayPosition = profile.statusDisplayPosition;
        settings.syncWindowTitleWithHostWritableStatusDisplay =
            profile.syncWindowTitleWithHostWritableStatusDisplay;
        if (auto const* p = preferredColorPalette(profile.colors, colorPreference))
            settings.colorPalette = *p;
        settings.refreshRate = profile.refreshRate;
        settings.primaryScreen.allowReflowOnResize = config.reflowOnResize;
        settings.highlightDoubleClickedWord = profile.highlightDoubleClickedWord;
        settings.highlightTimeout = profile.highlightTimeout;
        settings.frozenModes = profile.frozenModes;

        return settings;
    }

    int createSessionId()
    {
        static int nextSessionId = 1;
        return nextSessionId++;
    }

} // namespace

TerminalSession::TerminalSession(unique_ptr<vtpty::Pty> pty, ContourGuiApp& app):
    _id { createSessionId() },
    _startTime { steady_clock::now() },
    _config { app.config() },
    _profileName { app.profileName() },
    _profile { *_config.profile(_profileName) },
    _app { app },
    _currentColorPreference { app.colorPreference() },
    _terminal { *this,
                std::move(pty),
                createSettingsFromConfig(_config, _profile, _currentColorPreference),
                std::chrono::steady_clock::now() }
{
    if (app.liveConfig())
    {
        sessionLog()("Enable live configuration reloading of file {}.",
                     _config.backingFilePath.generic_string());
        _configFileChangeWatcher = make_unique<QFileSystemWatcher>();
        _configFileChangeWatcher->addPath(QString::fromStdString(_config.backingFilePath.generic_string()));
        connect(_configFileChangeWatcher.get(),
                SIGNAL(fileChanged(const QString&)),
                this,
                SLOT(onConfigReload()));
    }
    _musicalNotesBuffer.reserve(16);
    _profile = *_config.profile(_profileName); // XXX do it again. but we've to be more efficient here
    configureTerminal();
}

TerminalSession::~TerminalSession()
{
    sessionLog()("Destroying terminal session.");
    _terminating = true;
    _terminal.device().wakeupReader();
    if (_screenUpdateThread)
        _screenUpdateThread->join();
}

void TerminalSession::detachDisplay(display::TerminalWidget& display)
{
    sessionLog()("Detaching display from session.");
    Require(_display == &display);
    _display = nullptr;
}

void TerminalSession::attachDisplay(display::TerminalWidget& newDisplay)
{
    sessionLog()("Attaching display.");
    // newDisplay.setSession(*this); // NB: we're being called by newDisplay!
    _display = &newDisplay;

    setContentScale(newDisplay.contentScale());

    // NB: Inform connected TTY and local Screen instance about initial cell pixel size.
    auto const pixels = _display->cellSize() * _terminal.pageSize();
    // auto const pixels =
    //     ImageSize { _display->cellSize().width * boxed_cast<Width>(_terminal.pageSize().columns),
    //                 _display->cellSize().height * boxed_cast<Height>(_terminal.pageSize().lines) };
    auto const l = scoped_lock { _terminal };
    _terminal.resizeScreen(_terminal.pageSize(), pixels);
    _terminal.setRefreshRate(_display->refreshRate());
}

void TerminalSession::scheduleRedraw()
{
    _terminal.markScreenDirty();
    if (_display)
        _display->scheduleRedraw();
}

void TerminalSession::start()
{
    _terminal.device().start();
    _screenUpdateThread = make_unique<std::thread>(bind(&TerminalSession::mainLoop, this));
}

void TerminalSession::mainLoop()
{
    setThreadName("Terminal.Loop");

    _mainLoopThreadID = this_thread::get_id();

    sessionLog()("Starting main loop with thread id {}", [&]() {
        stringstream sstr;
        sstr << _mainLoopThreadID;
        return sstr.str();
    }());

    while (!_terminating)
    {
        if (!_terminal.processInputOnce())
            break;
    }

    sessionLog()("Event loop terminating (PTY {}).", _terminal.device().isClosed() ? "closed" : "open");
    onClosed();
}

void TerminalSession::terminate()
{
    if (!_display)
        return;

    _display->closeDisplay();
}

// {{{ Events implementations
void TerminalSession::bell()
{
    emit onBell();

    if (_profile.bell.alert)
        emit onAlert();
}

void TerminalSession::bufferChanged(vtbackend::ScreenType type)
{
    if (!_display)
        return;

    _currentScreenType = type;
    emit isScrollbarVisibleChanged();
    _display->post([this, type]() { _display->bufferChanged(type); });
}

void TerminalSession::screenUpdated()
{
    if (!_display)
        return;

    if (_profile.autoScrollOnUpdate && terminal().viewport().scrolled()
        && terminal().inputHandler().mode() == ViMode::Insert)
        terminal().viewport().scrollToBottom();

    if (terminal().hasInput())
        _display->post(bind(&TerminalSession::flushInput, this));

    if (_lastHistoryLineCount != _terminal.currentScreen().historyLineCount())
    {
        _lastHistoryLineCount = _terminal.currentScreen().historyLineCount();
        emit historyLineCountChanged(unbox(_lastHistoryLineCount));
    }

    scheduleRedraw();
}

void TerminalSession::flushInput()
{
    terminal().flushInput();
    if (terminal().hasInput() && _display)
        _display->post(bind(&TerminalSession::flushInput, this));
}

void TerminalSession::renderBufferUpdated()
{
    if (!_display)
        return;

    _display->renderBufferUpdated();
}

void TerminalSession::executeRole(GuardedRole role, bool allow, bool remember)
{
    switch (role)
    {
        case GuardedRole::CaptureBuffer: executePendingBufferCapture(allow, remember); break;
        case GuardedRole::ChangeFont: applyPendingFontChange(allow, remember); break;
        case GuardedRole::ShowHostWritableStatusLine:
            executeShowHostWritableStatusLine(allow, remember);
            break;
    }
}

void TerminalSession::requestPermission(config::Permission allowedByConfig, GuardedRole role)
{
    switch (allowedByConfig)
    {
        case config::Permission::Allow:
            sessionLog()("Permission for {} allowed by configuration.", role);
            executeRole(role, true, false);
            return;
        case config::Permission::Deny:
            sessionLog()("Permission for {} denied by configuration.", role);
            executeRole(role, false, false);
            return;
        case config::Permission::Ask: {
            if (auto const i = _rememberedPermissions.find(role); i != _rememberedPermissions.end())
            {
                executeRole(role, i->second, false);
                if (!i->second)
                    sessionLog()("Permission for {} denied by user for this session.", role);
                else
                    sessionLog()("Permission for {} allowed by user for this session.", role);
            }
            else
            {
                sessionLog()("Permission for {} requires asking user.", role);
                switch (role)
                {
                        // clang-format off
                    case GuardedRole::ChangeFont: emit requestPermissionForFontChange(); break;
                    case GuardedRole::CaptureBuffer: emit requestPermissionForBufferCapture(); break;
                    case GuardedRole::ShowHostWritableStatusLine: emit requestPermissionForShowHostWritableStatusLine(); break;
                        // clang-format on
                }
            }
            break;
        }
    }
}

void TerminalSession::updateColorPreference(vtbackend::ColorPreference preference)
{
    if (preference == _currentColorPreference)
        return;

    _currentColorPreference = preference;
    if (auto const* colorPalette = preferredColorPalette(_profile.colors, preference))
    {
        _terminal.resetColorPalette(*colorPalette);

        emit backgroundColorChanged();
    }
}

void TerminalSession::requestCaptureBuffer(LineCount lines, bool logical)
{
    if (!_display)
        return;

    _pendingBufferCapture = CaptureBufferRequest { lines, logical };

    emit requestPermissionForBufferCapture();
    // _display->post(
    //     [this]() { requestPermission(_profile.permissions.captureBuffer, GuardedRole::CaptureBuffer); });
}

void TerminalSession::executePendingBufferCapture(bool allow, bool remember)
{
    if (remember)
        _rememberedPermissions[GuardedRole::CaptureBuffer] = allow;

    if (!_pendingBufferCapture)
        return;

    auto const capture = _pendingBufferCapture.value();
    _pendingBufferCapture.reset();

    if (!allow)
        return;

    _terminal.primaryScreen().captureBuffer(capture.lines, capture.logical);

    displayLog()("requestCaptureBuffer: Finished. Waking up I/O thread.");
    flushInput();
}

void TerminalSession::executeShowHostWritableStatusLine(bool allow, bool remember)
{
    if (remember)
        _rememberedPermissions[GuardedRole::ShowHostWritableStatusLine] = allow;

    if (!allow)
        return;

    _terminal.setStatusDisplay(vtbackend::StatusDisplayType::HostWritable);
    displayLog()("requestCaptureBuffer: Finished. Waking up I/O thread.");
    flushInput();
    _terminal.state().syncWindowTitleWithHostWritableStatusDisplay = false;
}

vtbackend::FontDef TerminalSession::getFontDef()
{
    return _display->getFontDef();
}

void TerminalSession::setFontDef(vtbackend::FontDef const& fontDef)
{
    if (!_display)
        return;

    _pendingFontChange = fontDef;

    _display->post([this]() { requestPermission(_profile.permissions.changeFont, GuardedRole::ChangeFont); });
}

void TerminalSession::applyPendingFontChange(bool allow, bool remember)
{
    if (remember)
        _rememberedPermissions[GuardedRole::ChangeFont] = allow;

    if (!_pendingFontChange)
        return;

    auto const& currentFonts = _profile.fonts;
    vtrasterizer::FontDescriptions newFonts = currentFonts;

    auto const spec = std::move(_pendingFontChange.value());
    _pendingFontChange.reset();

    if (!allow)
        return;

    if (spec.size != 0.0)
        newFonts.size = text::font_size { spec.size };

    if (!spec.regular.empty())
        newFonts.regular = text::font_description::parse(spec.regular);

    auto const styledFont = [&](string_view font) -> text::font_description {
        // if a styled font is "auto" then infer froom regular font"
        if (font == "auto"sv)
            return currentFonts.regular;
        else
            return text::font_description::parse(font);
    };

    if (!spec.bold.empty())
        newFonts.bold = styledFont(spec.bold);

    if (!spec.italic.empty())
        newFonts.italic = styledFont(spec.italic);

    if (!spec.boldItalic.empty())
        newFonts.boldItalic = styledFont(spec.boldItalic);

    if (!spec.emoji.empty() && spec.emoji != "auto"sv)
        newFonts.emoji = text::font_description::parse(spec.emoji);

    _display->setFonts(newFonts);
}

void TerminalSession::copyToClipboard(std::string_view data)
{
    if (!_display)
        return;

    _display->post([this, data = string(data)]() { _display->copyToClipboard(data); });
}

void TerminalSession::openDocument(std::string_view fileOrUrl)
{
    auto stripped = strip_if(string(fileOrUrl), true);
    fmt::print("TerminalSession.openDocument: {}\n", fileOrUrl);
    QDesktopServices::openUrl(QUrl(QString::fromStdString(std::string(fileOrUrl))));
}

void TerminalSession::inspect()
{
    if (_display)
        _display->inspect();

    // Deferred termination? Then close display now.
    if (_terminal.device().isClosed() && !_app.dumpStateAtExit().has_value())
        _display->closeDisplay();
}

void TerminalSession::notify(string_view title, string_view content)
{
    emit showNotification(QString::fromUtf8(title.data(), static_cast<int>(title.size())),
                          QString::fromUtf8(content.data(), static_cast<int>(content.size())));
}

void TerminalSession::onClosed()
{
    auto const now = steady_clock::now();
    auto const diff = std::chrono::duration_cast<std::chrono::seconds>(now - _startTime);

    if (auto* localProcess = dynamic_cast<vtpty::Process*>(&_terminal.device()))
    {
        localProcess->close();
        auto const exitStatus = localProcess->checkStatus();
        if (exitStatus)
            sessionLog()(
                "Process terminated after {} seconds with exit status {}.", diff.count(), *exitStatus);
        else
            sessionLog()("Process terminated after {} seconds.", diff.count());
    }
    else
        sessionLog()("Process terminated after {} seconds.", diff.count());

    emit sessionClosed(*this);

    if (diff < _app.earlyExitThreshold())
    {
        // auto const w = _terminal.pageSize().columns.as<int>();
        auto constexpr SGR = "\033[1;38:2::255:255:255m\033[48:2::255:0:0m"sv;
        auto constexpr EL = "\033[K"sv;
        auto constexpr TextLines = array<string_view, 2> { "Shell terminated too quickly.",
                                                           "The window will not be closed automatically." };
        for (auto const text: TextLines)
            _terminal.writeToScreen(fmt::format("\r\n{}{}{}", SGR, EL, text));
        _terminal.writeToScreen("\r\n");
        _terminatedAndWaitingForKeyPress = true;
        return;
    }

    if (_app.dumpStateAtExit().has_value())
        inspect();
    else if (_display)
        _display->closeDisplay();
}

void TerminalSession::pasteFromClipboard(unsigned count, bool strip)
{
    if (QClipboard* clipboard = QGuiApplication::clipboard(); clipboard != nullptr)
    {
        QMimeData const* md = clipboard->mimeData();
        sessionLog()("pasteFromClipboard: mime data contains {} formats.", md->formats().size());
        for (int i = 0; i < md->formats().size(); ++i)
            sessionLog()("pasteFromClipboard[{}]: {}\n", i, md->formats().at(i).toStdString());
        string const text = strip_if(normalize_crlf(clipboard->text(QClipboard::Clipboard)), strip);
        if (text.empty())
            sessionLog()("Clipboard does not contain text.");
        else if (count == 1)
            terminal().sendPaste(string_view { text });
        else
        {
            string fullPaste;
            for (unsigned i = 0; i < count; ++i)
                fullPaste += text;
            terminal().sendPaste(string_view { fullPaste });
        }
    }
    else
        sessionLog()("Could not access clipboard.");
}

void TerminalSession::onSelectionCompleted()
{
    switch (_config.onMouseSelection)
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

void TerminalSession::requestWindowResize(LineCount lines, ColumnCount columns)
{
    if (!_display)
        return;

    sessionLog()("Application request to resize window: {}x{}", columns, lines);
    _display->post([this, lines, columns]() { _display->resizeWindow(lines, columns); });
}

void TerminalSession::requestWindowResize(QJSValue w, QJSValue h)
{
    requestWindowResize(Width(w.toInt()), Height(h.toInt()));
}

void TerminalSession::requestWindowResize(Width width, Height height)
{
    if (!_display)
        return;

    sessionLog()("Application request to resize window: {}x{} px", width, height);
    _display->post([this, width, height]() { _display->resizeWindow(width, height); });
}

void TerminalSession::setWindowTitle(string_view title)
{
    emit titleChanged(QString::fromUtf8(title.data(), static_cast<int>(title.size())));
}

void TerminalSession::setTerminalProfile(string const& configProfileName)
{
    if (!_display)
        return;

    _display->post([this, name = string(configProfileName)]() { activateProfile(name); });
}

void TerminalSession::discardImage(vtbackend::Image const& image)
{
    if (!_display)
        return;

    _display->discardImage(image);
}

void TerminalSession::inputModeChanged(vtbackend::ViMode mode)
{
    using vtbackend::ViMode;
    switch (mode)
    {
        case ViMode::Insert: configureCursor(_profile.inputModes.insert.cursor); break;
        case ViMode::Normal: configureCursor(_profile.inputModes.normal.cursor); break;
        case ViMode::Visual:
        case ViMode::VisualLine:
        case ViMode::VisualBlock: configureCursor(_profile.inputModes.visual.cursor); break;
    }
}

// }}}
// {{{ Input Events
void TerminalSession::sendKeyEvent(Key key, Modifier modifier, KeyboardEventType eventType, Timestamp now)
{
    inputLog()("Key press event received: {} {}", modifier, key);

    if (_terminatedAndWaitingForKeyPress)
    {
        _display->closeDisplay();
        return;
    }

    if (_profile.mouseHideWhileTyping)
        _display->setMouseCursorShape(MouseCursorShape::Hidden);

    if (eventType != KeyboardEventType::Release)
    {
        if (auto const* actions =
                config::apply(_config.inputMappings.keyMappings, key, modifier, matchModeFlags()))
        {
            executeAllActions(*actions);
            return;
        }
    }
    terminal().sendKeyEvent(key, modifier, eventType, now);
}

void TerminalSession::sendCharEvent(
    char32_t value, uint32_t physicalKey, Modifier modifier, KeyboardEventType eventType, Timestamp now)
{
    inputLog()("Character {} event received: {} {}",
               eventType,
               modifier,
               crispy::escape(unicode::convert_to<char>(value)));

    assert(_display != nullptr);

    if (_terminatedAndWaitingForKeyPress)
    {
        _display->closeDisplay();
        return;
    }

    if (_profile.mouseHideWhileTyping)
        _display->setMouseCursorShape(MouseCursorShape::Hidden);

    if (eventType != KeyboardEventType::Release)
    {
        if (auto const* actions =
                config::apply(_config.inputMappings.charMappings, value, modifier, matchModeFlags()))
        {
            executeAllActions(*actions);
            return;
        }
    }
    terminal().sendCharEvent(value, physicalKey, modifier, eventType, now);
}

void TerminalSession::sendMousePressEvent(Modifier modifier,
                                          MouseButton button,
                                          PixelCoordinate pixelPosition)
{
    auto const uiHandledHint = false;
    inputLog()("Mouse press received: {} {}\n", modifier, button);

    terminal().tick(steady_clock::now());

    if (terminal().sendMousePressEvent(modifier, button, pixelPosition, uiHandledHint))
        return;

    auto const sanitizedModifier = modifier.contains(_config.bypassMouseProtocolModifier)
                                       ? modifier.without(_config.bypassMouseProtocolModifier)
                                       : modifier;

    if (auto const* actions =
            config::apply(_config.inputMappings.mouseMappings, button, sanitizedModifier, matchModeFlags()))
        executeAllActions(*actions);
}

void TerminalSession::sendMouseMoveEvent(vtbackend::Modifier modifier,
                                         vtbackend::CellLocation pos,
                                         vtbackend::PixelCoordinate pixelPosition)
{
    // NB: This translation depends on the display's margin, so maybe
    //     the display should provide the translation?

    if (!(pos < terminal().pageSize()))
        return;

    terminal().tick(steady_clock::now());

    auto constexpr UiHandledHint = false;
    terminal().sendMouseMoveEvent(modifier, pos, pixelPosition, UiHandledHint);

    if (pos != _currentMousePosition)
    {
        // Change cursor shape only when changing grid cell.
        _currentMousePosition = pos;
        if (terminal().isMouseHoveringHyperlink())
            _display->setMouseCursorShape(MouseCursorShape::PointingHand);
        else
            setDefaultCursor();
    }
}

void TerminalSession::sendMouseReleaseEvent(Modifier modifier,
                                            MouseButton button,
                                            PixelCoordinate pixelPosition)
{
    terminal().tick(steady_clock::now());

    auto const uiHandledHint = false;
    terminal().sendMouseReleaseEvent(modifier, button, pixelPosition, uiHandledHint);
    scheduleRedraw();
}

void TerminalSession::sendFocusInEvent()
{
    // as per Qt-documentation, some platform implementations reset the cursor when leaving the
    // window, so we have to re-apply our desired cursor in focusInEvent().
    setDefaultCursor();

    terminal().sendFocusInEvent();

    if (_display)
        _display->setBlurBehind(_profile.backgroundBlur);

    scheduleRedraw();
}

void TerminalSession::sendFocusOutEvent()
{
    // TODO maybe paint with "faint" colors
    terminal().sendFocusOutEvent();

    scheduleRedraw();
}

void TerminalSession::updateHighlights()
{
    QTimer::singleShot(terminal().highlightTimeout(), this, SLOT(onHighlightUpdate()));
}

void TerminalSession::onHighlightUpdate()
{
    _terminal.resetHighlight();
}

void TerminalSession::playSound(vtbackend::Sequence::Parameters const& params)
{
    auto range = params.range();
    _musicalNotesBuffer.clear();
    _musicalNotesBuffer.insert(_musicalNotesBuffer.begin(), range.begin() + 2, range.end());
    emit _audio.play(params.at(0), params.at(1), _musicalNotesBuffer);
}

void TerminalSession::cursorPositionChanged()
{
    QGuiApplication::inputMethod()->update(Qt::ImCursorRectangle);
}
// }}}
// {{{ Actions
bool TerminalSession::operator()(actions::CancelSelection)
{
    _terminal.clearSelection();
    return true;
}

bool TerminalSession::operator()(actions::ChangeProfile const& action)
{
    sessionLog()("Changing profile to: {}", action.name);
    if (action.name == _profileName)
        return true;

    activateProfile(action.name);
    return true;
}

bool TerminalSession::operator()(actions::ClearHistoryAndReset)
{
    sessionLog()("Clearing history and perform terminal hard reset");

    _terminal.hardReset();
    _terminal.forceRedraw([]() { this_thread::yield(); });
    return true;
}

bool TerminalSession::operator()(actions::CopyPreviousMarkRange)
{
    copyToClipboard(terminal().extractLastMarkRange());
    return true;
}

bool TerminalSession::operator()(actions::CopySelection copySelection)
{
    switch (copySelection.format)
    {
        case actions::CopyFormat::Text:
            // Copy the selection in pure text, plus whitespaces and newline.
            copyToClipboard(terminal().extractSelectionText());
            break;
        case actions::CopyFormat::HTML:
            // TODO: This requires walking through each selected cell and construct HTML+CSS for it.
        case actions::CopyFormat::VT:
            // TODO: Construct VT escape sequences.
        case actions::CopyFormat::PNG:
            // TODO: Copy to clipboard as rendered PNG for the selected area.
            errorLog()("CopySelection format {} is not yet supported.", copySelection.format);
            return false;
    }
    return true;
}

bool TerminalSession::operator()(actions::CreateDebugDump)
{
    _terminal.inspect();
    return true;
}

bool TerminalSession::operator()(actions::DecreaseFontSize)
{
    auto constexpr OnePt = text::font_size { 1.0 };
    setFontSize(profile().fonts.size - OnePt);

    emit fontSizeChanged();
    // auto const currentFontSize = view().renderer().fontDescriptions().size;
    // auto const newFontSize = currentFontSize - OnePt;
    // setFontSize(newFontSize);
    return true;
}

bool TerminalSession::operator()(actions::DecreaseOpacity)
{
    if (static_cast<uint8_t>(_profile.backgroundOpacity) == 0)
        return true;

    --_profile.backgroundOpacity;

    emit opacityChanged();

    // Also emit backgroundColorChanged() because the background color is
    // semi-transparent and thus the opacity change affects the background color.
    emit backgroundColorChanged();

    return true;
}

bool TerminalSession::operator()(actions::FocusNextSearchMatch)
{
    if (_terminal.state().viCommands.jumpToNextMatch(1))
        _terminal.viewport().makeVisibleWithinSafeArea(_terminal.state().viCommands.cursorPosition.line);
    // TODO why didn't the makeVisibleWithinSafeArea() call from inside jumpToNextMatch not work?
    return true;
}

bool TerminalSession::operator()(actions::FocusPreviousSearchMatch)
{
    if (_terminal.state().viCommands.jumpToPreviousMatch(1))
        _terminal.viewport().makeVisibleWithinSafeArea(_terminal.state().viCommands.cursorPosition.line);
    // TODO why didn't the makeVisibleWithinSafeArea() call from inside jumpToPreviousMatch not work?
    return true;
}

bool TerminalSession::operator()(actions::FollowHyperlink)
{
    auto const l = scoped_lock { terminal() };
    if (auto const hyperlink = terminal().tryGetHoveringHyperlink())
    {
        followHyperlink(*hyperlink);
        return true;
    }
    return false;
}

bool TerminalSession::operator()(actions::IncreaseFontSize)
{
    auto constexpr OnePt = text::font_size { 1.0 };
    // auto const currentFontSize = view().renderer().fontDescriptions().size;
    // auto const newFontSize = currentFontSize + OnePt;
    // setFontSize(newFontSize);

    emit fontSizeChanged();
    setFontSize(profile().fonts.size + OnePt);
    return true;
}

bool TerminalSession::operator()(actions::IncreaseOpacity)
{
    if (static_cast<uint8_t>(_profile.backgroundOpacity) >= std::numeric_limits<uint8_t>::max())
        return true;
    ++_profile.backgroundOpacity;

    emit opacityChanged();

    // Also emit backgroundColorChanged() because the background color is
    // semi-transparent and thus the opacity change affects the background color.
    emit backgroundColorChanged();

    return true;
}

bool TerminalSession::operator()(actions::NewTerminal const& action)
{
    spawnNewTerminal(action.profileName.value_or(_profileName));
    return true;
}

bool TerminalSession::operator()(actions::NoSearchHighlight)
{
    _terminal.clearSearch();
    return true;
}

bool TerminalSession::operator()(actions::OpenConfiguration)
{
    if (!QDesktopServices::openUrl(QUrl(QString::fromUtf8(_config.backingFilePath.string().c_str()))))
        errorLog()("Could not open configuration file \"{}\".", _config.backingFilePath.generic_string());

    return true;
}

bool TerminalSession::operator()(actions::OpenFileManager)
{
    auto const l = scoped_lock { terminal() };
    auto const& cwd = terminal().currentWorkingDirectory();
    if (!QDesktopServices::openUrl(QUrl(QString::fromUtf8(cwd.c_str()))))
        errorLog()("Could not open file \"{}\".", cwd);

    return true;
}

bool TerminalSession::operator()(actions::OpenSelection)
{
    QDesktopServices::openUrl(QUrl(QString::fromUtf8(terminal().extractSelectionText().c_str())));
    return true;
}

bool TerminalSession::operator()(actions::PasteClipboard paste)
{
    pasteFromClipboard(1, paste.strip);
    return true;
}

bool TerminalSession::operator()(actions::PasteSelection)
{
    if (QClipboard* clipboard = QGuiApplication::clipboard(); clipboard != nullptr)
    {
        string const text = normalize_crlf(clipboard->text(QClipboard::Selection));
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

bool TerminalSession::operator()(actions::ReloadConfig const& action)
{
    if (action.profileName.has_value())
        reloadConfigWithProfile(action.profileName.value());
    else
        reloadConfigWithProfile(_profileName);

    return true;
}

bool TerminalSession::operator()(actions::ResetConfig)
{
    resetConfig();
    return true;
}

bool TerminalSession::operator()(actions::ResetFontSize)
{
    if (config::TerminalProfile const* profile = _config.profile(_profileName))
        setFontSize(profile->fonts.size);
    return true;
}

bool TerminalSession::operator()(actions::ScreenshotVT)
{
    auto l = lock_guard { terminal() };
    auto const screenshot = terminal().isPrimaryScreen() ? terminal().primaryScreen().screenshot()
                                                         : terminal().alternateScreen().screenshot();
    ofstream ofs { "screenshot.vt", ios::trunc | ios::binary };
    ofs << screenshot;
    return true;
}

bool TerminalSession::operator()(actions::ScrollDown)
{
    terminal().viewport().scrollDown(_profile.historyScrollMultiplier);
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
    terminal().viewport().scrollUp(_profile.historyScrollMultiplier);
    return true;
}

bool TerminalSession::operator()(actions::SearchReverse)
{
    _terminal.inputHandler().startSearchExternally();

    return true;
}

bool TerminalSession::operator()(actions::SendChars const& event)
{
    // auto const now = steady_clock::now();
    // for (auto const ch: event.chars)
    //     terminal().sendCharPressEvent(static_cast<char32_t>(ch), vtbackend::Modifier::None, now);
    terminal().sendRawInput(event.chars);
    return true;
}

bool TerminalSession::operator()(actions::ToggleAllKeyMaps)
{
    _allowKeyMappings = !_allowKeyMappings;
    inputLog()("{} key mappings.", _allowKeyMappings ? "Enabling" : "Disabling");
    return true;
}

bool TerminalSession::operator()(actions::ToggleFullscreen)
{
    if (_display)
        _display->toggleFullScreen();
    return true;
}

bool TerminalSession::operator()(actions::ToggleInputProtection)
{
    terminal().setAllowInput(!terminal().allowInput());
    return true;
}

bool TerminalSession::operator()(actions::ToggleStatusLine)
{
    auto const l = scoped_lock { _terminal };
    if (terminal().state().statusDisplayType != StatusDisplayType::Indicator)
        terminal().setStatusDisplay(StatusDisplayType::Indicator);
    else
        terminal().setStatusDisplay(StatusDisplayType::None);

    // `savedStatusDisplayType` holds only a value if the application has been overriding
    // the status display type. But the user now actively requests a given type,
    // so make sure restoring will not destroy the user's desire.
    if (terminal().state().savedStatusDisplayType)
        terminal().state().savedStatusDisplayType = terminal().state().statusDisplayType;

    return true;
}

bool TerminalSession::operator()(actions::ToggleTitleBar)
{
    if (_display)
        _display->toggleTitleBar();
    return true;
}

// {{{ Trace debug mode
bool TerminalSession::operator()(actions::TraceBreakAtEmptyQueue)
{
    _terminal.setExecutionMode(ExecutionMode::BreakAtEmptyQueue);
    return true;
}

bool TerminalSession::operator()(actions::TraceEnter)
{
    _terminal.setExecutionMode(ExecutionMode::Waiting);
    return true;
}

bool TerminalSession::operator()(actions::TraceLeave)
{
    _terminal.setExecutionMode(ExecutionMode::Normal);
    return true;
}

bool TerminalSession::operator()(actions::TraceStep)
{
    _terminal.setExecutionMode(ExecutionMode::SingleStep);
    return true;
}
// }}}

bool TerminalSession::operator()(actions::ViNormalMode)
{
    if (terminal().inputHandler().mode() == ViMode::Insert)
        terminal().inputHandler().setMode(ViMode::Normal);
    else if (terminal().inputHandler().mode() == ViMode::Normal)
        terminal().inputHandler().setMode(ViMode::Insert);
    return true;
}

bool TerminalSession::operator()(actions::WriteScreen const& event)
{
    terminal().writeToScreen(event.chars);
    return true;
}
// }}}
// {{{ implementation helpers
void TerminalSession::setDefaultCursor()
{
    if (!_display)
        return;

    using Type = vtbackend::ScreenType;
    switch (terminal().screenType())
    {
        case Type::Primary: _display->setMouseCursorShape(MouseCursorShape::IBeam); break;
        case Type::Alternate: _display->setMouseCursorShape(MouseCursorShape::Arrow); break;
    }
}

bool TerminalSession::reloadConfig(config::Config newConfig, string const& profileName)
{
    // clang-format off
    sessionLog()("Reloading configuration from {} with profile {}",
                 newConfig.backingFilePath.string(), profileName);
    // clang-format on

    _config = std::move(newConfig);
    activateProfile(profileName);

    return true;
}

int TerminalSession::executeAllActions(std::vector<actions::Action> const& actions)
{
    if (_allowKeyMappings)
    {
        int executionCount = 0;
        for (actions::Action const& action: actions)
            if (executeAction(action))
                ++executionCount;
        scheduleRedraw();
        return executionCount;
    }

    auto const containsToggleKeybind = [](std::vector<actions::Action> const& actions) {
        return std::any_of(actions.begin(), actions.end(), [](actions::Action const& action) {
            return holds_alternative<actions::ToggleAllKeyMaps>(action);
        });
    };

    if (containsToggleKeybind(actions))
    {
        bool const ex = executeAction(actions::ToggleAllKeyMaps {});
        scheduleRedraw();
        return ex;
    }

    inputLog()("Key mappings are currently disabled via ToggleAllKeyMaps input mapping action.");
    return 0;
}

// Executes given action @p action.
//
// The return value indicates whether or not this action did apply or not.
// For example a FollowHyperlink only applies when there is a hyperlink
// at the current cursor position to follow,
// however, a ScrollToTop applies regardless of the current viewport
// scrolling position.
bool TerminalSession::executeAction(actions::Action const& action)
{
    sessionLog()("executeAction: {}", action);
    return visit(*this, action);
}

void TerminalSession::spawnNewTerminal(string const& profileName)
{
    auto const wd = [this]() -> string {
#if !defined(_WIN32)
        if (auto const* ptyProcess = dynamic_cast<vtpty::Process const*>(&_terminal.device()))
            return ptyProcess->workingDirectory();
#else
        auto const _l = scoped_lock { _terminal };
        return _terminal.currentWorkingDirectory();
#endif
        return "."s;
    }();

    if (_config.spawnNewProcess)
    {
        sessionLog()("spawning new process");
        ::contour::spawnNewTerminal(
            _app.programPath(), _config.backingFilePath.generic_string(), profileName, wd);
    }
    else
    {
        sessionLog()("spawning new in-process window");
        _app.config().profile(_profileName)->shell.workingDirectory = fs::path(wd);
        _app.newWindow();
    }
}

void TerminalSession::activateProfile(string const& newProfileName)
{
    auto* newProfile = _config.profile(newProfileName);
    if (!newProfile)
    {
        sessionLog()("Cannot change profile. No such profile: '{}'.", newProfileName);
        return;
    }

    sessionLog()("Changing profile to {}.", newProfileName);
    _profileName = newProfileName;
    _profile = *newProfile;
    configureTerminal();
    configureDisplay();
}

void TerminalSession::configureTerminal()
{
    auto const l = scoped_lock { _terminal };
    sessionLog()("Configuring terminal.");

    _terminal.setWordDelimiters(_config.wordDelimiters);
    _terminal.setMouseProtocolBypassModifier(_config.bypassMouseProtocolModifier);
    _terminal.setMouseBlockSelectionModifier(_config.mouseBlockSelectionModifier);
    _terminal.setLastMarkRangeOffset(_profile.copyLastMarkRangeOffset);

    sessionLog()("Setting terminal ID to {}.", _profile.terminalId);
    _terminal.setTerminalId(_profile.terminalId);
    _terminal.setMaxImageColorRegisters(_config.maxImageColorRegisters);
    _terminal.setMaxImageSize(_config.maxImageSize);
    _terminal.setMode(vtbackend::DECMode::NoSixelScrolling, !_config.sixelScrolling);
    _terminal.setStatusDisplay(_profile.initialStatusDisplayType);
    sessionLog()("maxImageSize={}, sixelScrolling={}", _config.maxImageSize, _config.sixelScrolling);

    // XXX
    // if (!terminalView.renderer().renderTargetAvailable())
    //     return;

    configureCursor(_profile.inputModes.insert.cursor);
    updateColorPreference(_app.colorPreference());
    _terminal.setMaxHistoryLineCount(_profile.maxHistoryLineCount);
    _terminal.setHighlightTimeout(_profile.highlightTimeout);
    _terminal.viewport().setScrollOff(_profile.modalCursorScrollOff);
}

void TerminalSession::configureCursor(config::CursorConfig const& cursorConfig)
{
    _terminal.setCursorBlinkingInterval(cursorConfig.cursorBlinkInterval);
    _terminal.setCursorDisplay(cursorConfig.cursorDisplay);
    _terminal.setCursorShape(cursorConfig.cursorShape);

    // Force a redraw of the screen
    // to ensure the correct cursor shape is displayed.
    scheduleRedraw();
}

void TerminalSession::configureDisplay()
{
    if (!_display)
        return;

    sessionLog()("Configuring display.");
    _display->setBlurBehind(_profile.backgroundBlur);

    {
        auto const dpr = _display->contentScale();
        auto const qActualScreenSize = _display->window()->screen()->size() * dpr;
        auto const actualScreenSize = ImageSize { Width::cast_from(qActualScreenSize.width()),
                                                  Height::cast_from(qActualScreenSize.height()) };
        _terminal.setMaxImageSize(actualScreenSize, actualScreenSize);
    }

    if (_profile.maximized)
        _display->setWindowMaximized();
    else
        _display->setWindowNormal();

    if (_profile.fullscreen != _display->isFullScreen())
        _display->toggleFullScreen();

    _terminal.setRefreshRate(_display->refreshRate());
    auto const pageSize = PageSize {
        LineCount(unbox<int>(_display->pixelSize().height) / unbox<int>(_display->cellSize().height)),
        ColumnCount(unbox<int>(_display->pixelSize().width) / unbox<int>(_display->cellSize().width)),
    };
    _display->setPageSize(pageSize);
    _display->setFonts(_profile.fonts);
    // TODO: maybe update margin after this call?

    _display->setHyperlinkDecoration(_profile.hyperlinkDecoration.normal, _profile.hyperlinkDecoration.hover);

    setWindowTitle(_terminal.windowTitle());
}

uint8_t TerminalSession::matchModeFlags() const
{
    uint8_t flags = 0;

    if (_terminal.isAlternateScreen())
        flags |= static_cast<uint8_t>(MatchModes::Flag::AlternateScreen);

    if (_terminal.applicationCursorKeys())
        flags |= static_cast<uint8_t>(MatchModes::Flag::AppCursor);

    if (_terminal.applicationKeypad())
        flags |= static_cast<uint8_t>(MatchModes::Flag::AppKeypad);

    if (_terminal.selectionAvailable())
        flags |= static_cast<uint8_t>(MatchModes::Flag::Select);

    if (_terminal.inputHandler().mode() == ViMode::Insert)
        flags |= static_cast<uint8_t>(MatchModes::Flag::Insert);

    if (!_terminal.state().searchMode.pattern.empty())
        flags |= static_cast<uint8_t>(MatchModes::Flag::Search);

    if (_terminal.executionMode() != ExecutionMode::Normal)
        flags |= static_cast<uint8_t>(MatchModes::Flag::Trace);

    return flags;
}

void TerminalSession::setFontSize(text::font_size size)
{
    if (!_display->setFontSize(size))
        return;

    _profile.fonts.size = size;
}

bool TerminalSession::reloadConfigWithProfile(string const& profileName)
{
    auto newConfig = config::Config {};
    auto configFailures = int { 0 };

    try
    {
        loadConfigFromFile(newConfig, _config.backingFilePath.string());
    }
    catch (exception const& e)
    {
        // TODO: _logger.error(e.what());
        errorLog()("Configuration failure. {}", unhandledExceptionMessage(__PRETTY_FUNCTION__, e));
        ++configFailures;
    }

    if (!newConfig.profile(profileName))
    {
        errorLog()(fmt::format("Currently active profile with name '{}' gone.", profileName));
        ++configFailures;
    }

    if (configFailures)
    {
        errorLog()("Failed to load configuration.");
        return false;
    }

    return reloadConfig(std::move(newConfig), profileName);
}

bool TerminalSession::resetConfig()
{
    auto const ec = config::createDefaultConfig(_config.backingFilePath);
    if (ec)
    {
        errorLog()("Failed to load default config at {}; ({}) {}",
                   _config.backingFilePath.string(),
                   ec.category().name(),
                   ec.message());
        return false;
    }

    config::Config defaultConfig;
    try
    {
        config::loadConfigFromFile(_config.backingFilePath);
    }
    catch (exception const& e)
    {
        sessionLog()("Failed to load default config: {}", e.what());
    }

    return reloadConfig(defaultConfig, defaultConfig.defaultProfileName);
}

void TerminalSession::followHyperlink(vtbackend::HyperlinkInfo const& hyperlink)
{
    auto const fileInfo = QFileInfo(QString::fromStdString(string(hyperlink.path())));
    auto const isLocal = hyperlink.isLocal() && hyperlink.host() == QHostInfo::localHostName().toStdString();
    auto const* const editorEnv = getenv("EDITOR");

    if (isLocal && fileInfo.isFile() && fileInfo.isExecutable())
    {
        QStringList args;
        args.append("config");
        args.append(QString::fromStdString(_config.backingFilePath.string()));
        args.append(QString::fromUtf8(hyperlink.path().data(), static_cast<int>(hyperlink.path().size())));
        QProcess::execute(QString::fromStdString(_app.programPath()), args);
    }
    else if (isLocal && fileInfo.isFile() && editorEnv && *editorEnv)
    {
        QStringList args;
        args.append("config");
        args.append(QString::fromStdString(_config.backingFilePath.string()));
        args.append(QString::fromStdString(editorEnv));
        args.append(QString::fromUtf8(hyperlink.path().data(), static_cast<int>(hyperlink.path().size())));
        QProcess::execute(QString::fromStdString(_app.programPath()), args);
    }
    else if (isLocal)
        QDesktopServices::openUrl(QUrl(hyperlink.uri.c_str()));
    else
        QDesktopServices::openUrl(QString::fromUtf8(hyperlink.uri.c_str()));
}

void TerminalSession::onConfigReload()
{
    _display->post([this]() { reloadConfigWithProfile(_profileName); });

    // TODO: needed still?
    // if (setScreenDirty())
    //     update();

    if (_configFileChangeWatcher)
        connect(_configFileChangeWatcher.get(),
                SIGNAL(fileChanged(const QString&)),
                this,
                SLOT(onConfigReload()));
}

// }}}
// {{{ QAbstractItemModel impl
QModelIndex TerminalSession::index(int row, int column, const QModelIndex& parent) const
{
    Require(row == 0);
    Require(column == 0);
    // NOTE: if at all, we could expose session attribs like session id, session type
    // (local process), ...?
    crispy::ignore_unused(parent);
    return createIndex(row, column, nullptr);
}

QModelIndex TerminalSession::parent(const QModelIndex& child) const
{
    crispy::ignore_unused(child);
    return QModelIndex();
}

int TerminalSession::rowCount(const QModelIndex& parent) const
{
    crispy::ignore_unused(parent);
    return 1;
}

int TerminalSession::columnCount(const QModelIndex& parent) const
{
    crispy::ignore_unused(parent);
    return 1;
}

QVariant TerminalSession::data(const QModelIndex& index, int role) const
{
    crispy::ignore_unused(index, role);
    Require(index.row() == 0);
    Require(index.column() == 0);

    return QVariant(_id);
}

bool TerminalSession::setData(const QModelIndex& index, const QVariant& value, int role)
{
    // NB: Session-Id is read-only.
    crispy::ignore_unused(index, value, role);
    return false;
}
// }}}

} // namespace contour
