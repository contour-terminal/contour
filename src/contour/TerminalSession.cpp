// SPDX-License-Identifier: Apache-2.0
#include <contour/Actions.h>
#include <contour/ContourGuiApp.h>
#include <contour/ExternalLauncher.h>
#include <contour/TerminalSession.h>
#include <contour/display/TerminalAccessible.h>
#include <contour/display/TerminalDisplay.h>
#include <contour/helper.h>

#include <vtbackend/HintModeHandler.h>
#include <vtbackend/MatchModes.h>
#include <vtbackend/Terminal.h>
#include <vtbackend/ViCommands.h>

#include <vtpty/Process.h>
#include <vtpty/Pty.h>
#include <vtpty/SshSession.h>

#include <crispy/StackTrace.h>
#include <crispy/assert.h>
#include <crispy/utils.h>

#include <QtCore/QDebug>
#include <QtCore/QFileInfo>
#include <QtCore/QMetaObject>
#include <QtCore/QMimeData>
#include <QtCore/QPointer>
#include <QtCore/QStandardPaths>
#include <QtCore/QTimer>
#include <QtCore/QUrl>
#include <QtGui/QClipboard>
#include <QtGui/QGuiApplication>
#include <QtGui/QKeyEvent>
#include <QtGui/QScreen>
#include <QtGui/QWindow>
#include <QtNetwork/QHostInfo>

#include <algorithm>
#include <array>
#include <filesystem>
#include <format>
#include <fstream>
#include <limits>
#include <ranges>
#include <regex>

#if defined(__OpenBSD__)
    #include <pthread_np.h>
    #define pthread_setname_np pthread_set_name_np
#elif !defined(_WIN32)
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
        return std::format("{}: Unhandled exception caught ({}). {}", where, typeid(e).name(), e.what());
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
                                                 ColorPreference colorPreference,
                                                 std::optional<vtbackend::PageSize> initialPageSize)
    {
        auto settings = vtbackend::Settings {};

        // A new tab/split inherits the live window's running grid; only a brand-new window falls back to
        // the profile's configured terminalSize. Applied here so the terminal is BORN at the right size,
        // not just corrected once a display attaches (which never happens for a background tab).
        settings.pageSize = initialPageSize.value_or(profile.terminalSize.value());
        settings.ptyBufferObjectSize = config.ptyBufferObjectSize.value();
        settings.ptyReadBufferSize = config.ptyReadBufferSize.value();
        settings.maxHistoryLineCount = profile.history.value().maxHistoryLineCount;
        settings.mouseWheelScrollMultiplier = profile.history.value().historyScrollMultiplier;
        settings.autoScrollOnUpdate = profile.history.value().autoScrollOnUpdate;
        settings.copyLastMarkRangeOffset = profile.copyLastMarkRangeOffset.value();
        settings.cursorBlinkInterval = profile.modeInsert.value().cursor.cursorBlinkInterval;
        settings.cursorShape = profile.modeInsert.value().cursor.cursorShape;
        settings.cursorDisplay = profile.modeInsert.value().cursor.cursorDisplay;
        settings.blinkStyle = profile.blinkStyle.value();
        settings.screenTransitionStyle = profile.screenTransitionStyle.value();
        settings.screenTransitionDuration = profile.screenTransitionDuration.value();
        settings.cursorMotionAnimationDuration = profile.cursorMotionAnimationDuration.value();
        settings.smoothLineScrolling = profile.smoothLineScrolling.value();
        settings.smoothScrolling = profile.smoothScrolling.value();
        settings.momentumScrolling = profile.momentumScrolling.value();
        settings.wordDelimiters = unicode::from_utf8(config.wordDelimiters.value());
        settings.mouseProtocolBypassModifiers = config.bypassMouseProtocolModifiers.value();
        settings.maxImageRegisterCount = config.images.value().maxImageColorRegisters;
        settings.goodImageProtocol = config.images.value().goodImageProtocol;
        settings.statusDisplayType = profile.statusLine.value().initialType;
        settings.statusDisplayPosition = profile.statusLine.value().position;
        settings.indicatorStatusLine.left = profile.statusLine.value().indicator.left;
        settings.indicatorStatusLine.middle = profile.statusLine.value().indicator.middle;
        settings.indicatorStatusLine.right = profile.statusLine.value().indicator.right;
        settings.tabNamingMode = [&]() {
            // try to find Tab section in one of the status line segments

            std::string segment;
            if (profile.statusLine.value().indicator.left.find("Tabs") != std::string::npos)
            {
                segment = profile.statusLine.value().indicator.left;
            }
            else if (profile.statusLine.value().indicator.middle.find("Tabs") != std::string::npos)
            {
                segment = profile.statusLine.value().indicator.middle;
            }
            else if (profile.statusLine.value().indicator.right.find("Tabs") != std::string::npos)
            {
                segment = profile.statusLine.value().indicator.right;
            }

            // check if indexing is defined
            if (segment.find("Indexing=") != std::string::npos)
            {
                // cut the string after indexing=
                std::string indexing = segment.substr(segment.find("Indexing=") + 9);
                // cut right part of the string
                indexing = indexing.substr(0, indexing.find(','));
                indexing = indexing.substr(0, indexing.find('}'));

                std::ranges::transform(
                    indexing, indexing.begin(), [](unsigned char c) { return std::tolower(c); });

                if (indexing == "title")
                {
                    return vtbackend::TabsNamingMode::Title;
                }
            }
            return vtbackend::TabsNamingMode::Indexing;
        }();

        settings.syncWindowTitleWithHostWritableStatusDisplay =
            profile.statusLine.value().syncWindowTitleWithHostWritableStatusDisplay;
        if (auto const* p = preferredColorPalette(profile.colors.value(), colorPreference))
            settings.colorPalette = *p;
        settings.primaryScreen.allowReflowOnResize = config.reflowOnResize.value();
        settings.highlightDoubleClickedWord = profile.highlightDoubleClickedWord.value();
        settings.highlightTimeout = profile.highlightTimeout.value();
        settings.frozenModes = profile.frozenModes.value();
        settings.graphemeClustering = config.graphemeClustering.value();

        return settings;
    }

    int createSessionId()
    {
        static int nextSessionId = 1;
        return nextSessionId++;
    }

    /// Resolves the profile a session should run under: the explicitly requested @p requested when it
    /// is non-empty and present in @p config, otherwise the application default @p appDefault. A
    /// requested-but-unknown name never aborts (unlike Config::profile()'s assert) — it falls back to
    /// the default, matching activateProfile()'s runtime tolerance for a removed profile.
    std::string resolveProfileName(config::Config const& config,
                                   std::string const& requested,
                                   std::string const& appDefault)
    {
        if (!requested.empty() && config.findProfile(requested) != nullptr)
            return requested;
        return appDefault;
    }

    class ExitWatcherThread: public QThread
    {
      public:
        ExitWatcherThread(TerminalSession& session): _session { session } {}

        void run() override
        {
            sessionLog()("ExitWatcherThread: Started.");
            _session.terminal().device().waitForClosed();
            sessionLog()("ExitWatcherThread: Terminal device closed.");
            postToObject(&_session, [&]() { _session.onClosed(); });
        }

      private:
        TerminalSession& _session;
    };

} // namespace

TerminalSession::TerminalSession(TerminalSessionManager* manager,
                                 unique_ptr<vtpty::Pty> pty,
                                 ContourGuiApp& app,
                                 std::string profileName,
                                 std::optional<vtbackend::PageSize> initialPageSize,
                                 std::optional<vtpty::Process::ExecInfo> launchedCommand):
    _manager { manager },
    _id { createSessionId() },
    _startTime { steady_clock::now() },
    _config { app.config() },
    _profileName { resolveProfileName(_config, profileName, app.profileName()) },
    _profileOverride { profileName.empty() ? std::optional<std::string> {} : std::optional { profileName } },
    _launchedCommand { std::move(launchedCommand) },
    _profile { *_config.profile(_profileName) },
    _app { app },
    _currentColorPreference { app.colorPreference() },
    _accumulatedPixelScroll {},
    _accumulatedAngleScroll {},
    _terminal { *this,
                std::move(pty),
                createSettingsFromConfig(_config, _profile, _currentColorPreference, initialPageSize),
                std::chrono::steady_clock::now() },
    _exitWatcherThread { std::make_unique<ExitWatcherThread>(*this) }
{
    if (app.liveConfig())
    {
        sessionLog()("Enable live configuration reloading of file {}.", _config.configFile.generic_string());
        _configFileChangeWatcher = make_unique<QFileSystemWatcher>();
        _configFileChangeWatcher->addPath(QString::fromStdString(_config.configFile.generic_string()));
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
    if (_exitWatcherThread->isRunning())
        _exitWatcherThread->terminate();
    if (_screenUpdateThread)
        _screenUpdateThread->join();
}

void TerminalSession::detachDisplay(display::TerminalDisplay& display)
{
    sessionLog()("Detaching display from session.");
    Require(_display == &display);
    _display = nullptr;
}

display::ForcedFontDpiProvider* TerminalSession::forcedFontDpiProvider() noexcept
{
    return _app.forcedFontDpiProvider();
}

void TerminalSession::attachDisplay(display::TerminalDisplay& newDisplay)
{
    sessionLog()("Attaching session to display {}x{}.", newDisplay.width(), newDisplay.height());

    // Enforce the one-session-one-display invariant: if a different display is still attached (e.g.
    // the hidden single-pane view after the active tab was split, which is only made invisible — not
    // destroyed — by QML), tell it to release this session first. Otherwise both displays would
    // believe they own the session, _display would silently flip to whichever attached last, and the
    // stale display's later detachDisplay() would trip the _display == &display precondition.
    if (_display != nullptr && _display != &newDisplay)
    {
        // One session, one display: release the old display's hold. Session->display ownership lives on
        // the pane tree now, so there is no manager-side per-display map to keep in sync.
        _display->releaseSession();
    }

    // We're being called by newDisplay!
    _display = &newDisplay;

    {
        // NB: Inform connected TTY and local Screen instance about initial cell pixel size.
        auto const l = scoped_lock { _terminal };
        _terminal.resizeScreen(_terminal.totalPageSize(),
                               _display->reportedPixelSize(_terminal.totalPageSize()));
        // refreshRate() dereferences window()->screen(); pre-window (see below) the posted
        // configureDisplay() sets it once the window exists.
        if (_display->window() != nullptr)
            _terminal.setRefreshRate(_display->refreshRate());
    }

    // configureDisplay() only runs from createRenderer(), i.e. once for the first session, so the
    // ceiling has to be (re)derived here too. See updateImageCanvasCeiling() for the null-checks.
    updateImageCanvasCeiling();

    {
        auto const _ = std::scoped_lock { _onClosedMutex };
        if (_onClosedHandled)
            _display->closeDisplay();
    }

    // A window-title / tab-name change that arrived while no display was attached was dropped by
    // refreshGuiTabInfoForStatusLine() (it only posts when _display is set). Now that a display is
    // attached, refresh the indicator status-line tab label so it reflects the current name rather than
    // a stale one.
    refreshGuiTabInfoForStatusLine();

    // Similarly, terminal replies (e.g. answers to VT capability queries that shells like fish
    // send right at startup) that were generated while no display was attached were queued in
    // the input generator but never flushed: screenUpdated() only posts flushInput() when
    // _display is set, and the shell may be blocked waiting for the reply, producing no further
    // PTY output that would trigger another flush. Without this, the shell hangs until the next
    // focus/input event finally flushes the queue (observed as a multi-second startup stall).
    if (_terminal.hasInput())
        _display->post(bind(&TerminalSession::flushInput, this));

    scheduleRedraw();
}

void TerminalSession::scheduleRedraw()
{
    _terminal.markScreenDirty();
    // Don't refresh GUI tab info here: reached from the parser thread while _stateMutex is held (ESU path),
    // so _manager->update() would re-lock that non-recursive mutex. It only changes on GUI-thread events.
    if (_display)
        _display->scheduleRedraw();
}

void TerminalSession::start()
{
    // ensure that we start only once
    if (!_screenUpdateThread)
    {
        sessionLog()("Starting terminal session.");
        _terminal.device().start();
        _screenUpdateThread = make_unique<std::thread>(bind(&TerminalSession::mainLoop, this));
        _exitWatcherThread->start(QThread::LowPriority);
    }
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
}

void TerminalSession::terminate()
{
    // Closing the PTY device is the display-independent teardown trigger on BOTH paths: it makes
    // ExitWatcherThread's waitForClosed() return and post onClosed() onto this session's thread, which
    // fires sessionClosed -> TerminalSessionManager::removeSession. Routing the display case through
    // closeDisplay() alone was not enough: closeDisplay() only emits terminated(), whose QML handler
    // closes the tab only when canCloseWindow() holds (false while a multi-tab window still has more
    // sessions than displays), so closing the *active* tab of a multi-tab window never reached
    // removeSession and leaked the session plus its shell process. Idempotent: a second close on an
    // already-closed device is a no-op (matching onClosed()'s own guard).
    sessionLog()("Terminated. Closing PTY device{}.", _display ? " and display" : "");
    // Deliberate close: set BEFORE closing the device so onClosed() (which the exit watcher fires as
    // soon as it observes the close) skips the early-exit notice and emits sessionClosed as usual —
    // the notice is only for shells that die on their own right after startup, and taking it here
    // would leave the tab unpruned, waiting for a key press the user never intended to give.
    _terminationRequested = true;
    if (!_terminal.device().isClosed())
        _terminal.device().close();
    else
    {
        // The device is already closed: if onClosed() already showed the early-exit notice, this
        // deliberate close is the acknowledgement — prune the pane now, mirroring sendKeyEvent()'s
        // acknowledge path; otherwise closing a notice-showing tab/pane would be a silent no-op.
        // _onClosedMutex serializes with an in-flight onClosed() so the notice cannot be armed after
        // the check.
        auto const armed = [this]() {
            auto const _ = std::scoped_lock { _onClosedMutex };
            auto const value = _terminatedAndWaitingForKeyPress;
            _terminatedAndWaitingForKeyPress = false;
            return value;
        }();
        if (armed)
            emit sessionClosed(*this);
    }

    // If a display is attached, also let the GUI tear its view down. Not the session-removal trigger
    // (that is the device().close() above); this just releases the display-side resources. Re-read
    // _display: the sessionClosed prune above may have torn the display down with the pane.
    if (_display)
        _display->closeDisplay();
}

// {{{ Events implementations
void TerminalSession::bell()
{
    emit onBell(_profile.bell.value().volume);

    if (_profile.bell.value().alert)
        emit onAlert();
}

void TerminalSession::bufferChanged(vtbackend::ScreenType type)
{
    if (!_display)
        return;

    _currentScreenType = type;
    emit isScrollbarVisibleChanged();
    // Re-check _display at dispatch: a tab switch or split collapse may have detached it (via
    // TerminalDisplay::setSession -> detachDisplay) between this post and the GUI thread running it.
    _display->post([this, type]() {
        if (_display)
            _display->bufferChanged(type);
    });
}

void TerminalSession::screenUpdated()
{
    if (!_display)
        return;

    if (_profile.history.value().autoScrollOnUpdate && terminal().viewport().scrolled()
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
        case GuardedRole::BigPaste: applyPendingPaste(allow, remember); break;
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
                    case GuardedRole::BigPaste: emit requestPermissionForPasteLargeFile(); break;
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
    if (auto const* colorPalette = preferredColorPalette(_profile.colors.value(), preference))
    {
        _terminal.resetColorPalette(*colorPalette);

        emit backgroundColorChanged();
    }
}

void TerminalSession::requestCaptureBuffer(LineCount lines, bool logical)
{
    if (!_display)
        return;

    _pendingBufferCapture = CaptureBufferRequest { .lines = lines, .logical = logical };

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

void TerminalSession::requestShowHostWritableStatusLine()
{
    if (_display)
        _display->post([this]() {
            requestPermission(_profile.permissions.value().displayHostWritableStatusLine,
                              GuardedRole::ShowHostWritableStatusLine);
        });
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
    _terminal.setSyncWindowTitleWithHostWritableStatusDisplay(false);
}

vtbackend::FontDef TerminalSession::getFontDef()
{
    // A display-less session (a background pane during a split/tab rebind) has no renderer to read
    // live font metrics from; answer the VT font query from the profile's configured fonts instead
    // of dereferencing a null display.
    if (!_display)
    {
        auto const& fonts = _profile.fonts.value();
        return vtbackend::FontDef { .size = fonts.size.pt,
                                    .regular = fonts.regular.toPattern(),
                                    .bold = fonts.bold.toPattern(),
                                    .italic = fonts.italic.toPattern(),
                                    .boldItalic = fonts.boldItalic.toPattern(),
                                    .emoji = fonts.emoji.toPattern() };
    }
    return _display->getFontDef();
}

void TerminalSession::setFontDef(vtbackend::FontDef const& fontDef)
{
    if (!_display)
        return;

    _pendingFontChange = fontDef;

    _display->post(
        [this]() { requestPermission(_profile.permissions.value().changeFont, GuardedRole::ChangeFont); });
}

void TerminalSession::applyPendingFontChange(bool allow, bool remember)
{
    if (remember)
        _rememberedPermissions[GuardedRole::ChangeFont] = allow;

    if (!_pendingFontChange)
        return;

    auto const& currentFonts = _profile.fonts.value();
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

void TerminalSession::setPointerShape(std::string_view cssName)
{
    // OSC 22 speaks CSS pointer names; the display speaks its own enum. The mapping is the whole
    // binding between the two, and only names vtbackend advertises as supported can arrive here.
    auto const shape = [cssName]() -> std::optional<MouseCursorShape> {
        if (cssName == "text")
            return MouseCursorShape::IBeam;
        if (cssName == "pointer")
            return MouseCursorShape::PointingHand;
        if (cssName == "default")
            return MouseCursorShape::Arrow;
        if (cssName == "none")
            return MouseCursorShape::Hidden;
        return std::nullopt;
    }();

    if (!shape || !_display)
        return;

    // The event arrives on the parser thread; the cursor belongs to the GUI thread.
    postToObject(_display, [display = _display, shape = *shape]() { display->setMouseCursorShape(shape); });
}

void TerminalSession::copyToClipboard(std::string_view data)
{
    if (!_display)
        return;

    _display->post([data = string(data)]() { display::TerminalDisplay::copyToClipboard(data); });
}

void TerminalSession::openDocument(std::string_view fileOrUrl)
{
    sessionLog()("openDocument: {}\n", fileOrUrl);
    auto const text = QString::fromUtf8(fileOrUrl.data(), static_cast<int>(fileOrUrl.size()));
    auto url = QUrl(text);

    // A single-letter scheme is a Windows drive letter (e.g. "C:\path"), not a real URL scheme —
    // QUrl otherwise parses "C:" as the scheme and the path never resolves as a local file. Treat
    // such a value (and a genuinely scheme-less one) as a filesystem path.
    if (url.scheme().isEmpty() || url.scheme().size() == 1)
    {
        auto const fileInfo = QFileInfo(text);
        if (fileInfo.exists())
            url = QUrl::fromLocalFile(fileInfo.absoluteFilePath());
    }

    if (!_app.externalLauncher().openUrl(url))
        errorLog()("Could not open document \"{}\".", fileOrUrl);
}

void TerminalSession::inspect()
{
    if (_display)
        _display->inspect();

    // Deferred termination? Then close display now.
    if (_terminal.device().isClosed() && !_app.dumpStateAtExit().has_value())
    {
        sessionLog()("Terminal device is closed. Notify session manager.");
        _manager->currentSessionIsTerminated();
        //_display->closeDisplay(); // TODO MOVE LOGIC
    }
}

void TerminalSession::notify(string_view title, string_view content)
{
#if defined(__linux__)
    auto notification = vtbackend::DesktopNotification {};
    notification.title = std::string(title);
    notification.body = std::string(content);
    _desktopNotifier.notify(notification);
#else
    emit showNotification(QString::fromUtf8(title.data(), static_cast<int>(title.size())),
                          QString::fromUtf8(content.data(), static_cast<int>(content.size())));
#endif
}

void TerminalSession::showDesktopNotification(vtbackend::DesktopNotification const& notification)
{
#if defined(__linux__)
    _desktopNotifier.notify(notification);

    // Connect close event reporting if requested.
    if (notification.closeEventRequested)
    {
        auto const identifier = notification.identifier;
        QObject::connect(
            &_desktopNotifier,
            &FreeDesktopNotifier::notificationClosed,
            this,
            [this, identifier](QString const& closedId, uint /*reason*/) {
                if (closedId.toStdString() == identifier)
                {
                    _terminal.reply("\033]99;i={}:p=close;\033\\", identifier);
                    _terminal.desktopNotificationManager().removeActiveNotification(identifier);
                }
            },
            Qt::SingleShotConnection);
    }

    auto const identifier = notification.identifier;

    // Connect activation reporting if requested.
    if (notification.reportOnActivation)
    {
        QObject::connect(
            &_desktopNotifier,
            &FreeDesktopNotifier::actionInvoked,
            this,
            [this, identifier](QString const& activatedId) {
                if (activatedId.toStdString() == identifier)
                    _terminal.reply("\033]99;i={}:p=activated;\033\\", identifier);
            },
            Qt::SingleShotConnection);
    }

    // Focus terminal on activation if requested.
    if (notification.focusOnActivation)
    {
        QObject::connect(
            &_desktopNotifier,
            &FreeDesktopNotifier::actionInvoked,
            this,
            [this, identifier](QString const& activatedId) {
                if (activatedId.toStdString() == identifier)
                    focusTerminalWindow();
            },
            Qt::SingleShotConnection);
    }
#else
    // On non-Linux platforms, fall back to the simple notification mechanism.
    emit showNotification(QString::fromStdString(notification.title),
                          QString::fromStdString(notification.body));
#endif
}

void TerminalSession::discardDesktopNotification(std::string_view identifier)
{
#if defined(__linux__)
    _desktopNotifier.close(std::string(identifier));
#else
    (void) identifier;
#endif
}

void TerminalSession::focusTerminalWindow()
{
    if (_display)
    {
        QMetaObject::invokeMethod(
            _display,
            [display = _display]() {
                if (auto* window = display->window())
                {
                    window->raise();
                    window->requestActivate();
                }
            },
            Qt::QueuedConnection);
    }
}

void TerminalSession::onClosed()
{
    auto const _ = std::scoped_lock { _onClosedMutex };
    sessionLog()("Terminal device closed (thread {})", crispy::threadName());

    if (!_terminal.device().isClosed())
        _terminal.device().close();

    auto const now = steady_clock::now();
    auto const diff = std::chrono::duration_cast<std::chrono::seconds>(now - _startTime);

    if (auto* localProcess = dynamic_cast<vtpty::Process*>(&_terminal.device()))
    {
        auto const exitStatus = localProcess->checkStatus();
        if (exitStatus)
            sessionLog()(
                "Process terminated after {} seconds with exit status {}.", diff.count(), *exitStatus);
        else
            sessionLog()("Process terminated after {} seconds.", diff.count());
    }
#if defined(VTPTY_LIBSSH2)
    else if (auto* sshSession = dynamic_cast<vtpty::SshSession*>(&_terminal.device()))
    {
        auto const exitStatus = sshSession->exitStatus();
        if (exitStatus)
            sessionLog()(
                "Process terminated after {} seconds with exit status {}.", diff.count(), *exitStatus);
        else
            sessionLog()("Process terminated after {} seconds.", diff.count());
    }
#endif
    else
        sessionLog()("Process terminated after {} seconds.", diff.count());

    if (diff < _app.earlyExitThreshold() && !_terminationRequested)
    {
        // Deliberately do NOT emit sessionClosed here: removeSession() would prune this pane from the
        // model and tear down its QML item, destroying the very screen the message below is shown on
        // (and leaving an empty window behind that nothing can close). The pane stays fully alive until
        // the acknowledging key press (see sendKeyEvent/sendCharEvent), which prunes and closes then.
        auto constexpr SGR = "\033[1;38:2::255:255:255m\033[48:2::255:0:0m"sv;
        auto constexpr EL = "\033[K"sv;
        auto constexpr TextLines = array<string_view, 2> { "Shell terminated too quickly.",
                                                           "The window will not be closed automatically." };
        for (auto const text: TextLines)
            _terminal.writeToScreen(std::format("\r\n{}{}{}", SGR, EL, text));
        _terminal.writeToScreen("\r\n");
        _terminatedAndWaitingForKeyPress = true;
        return;
    }

    auto isClosedAlready = _onClosedHandled.load();
    if (isClosedAlready || !_onClosedHandled.compare_exchange_weak(isClosedAlready, true))
    {
        sessionLog()("onClosed called: thread {}, display {}", crispy::threadName(), _display ? "yes" : "no");
        if (_display)
            _display->closeDisplay();
        return;
    }

    // The at-exit state dump MUST run before the model prune: sessionClosed -> removeSession tears
    // this pane's display down, and the dump (screen + renderer inspection + screenshot) needs a live
    // display. inspect() drives the display's own bounded frame pump and terminates the session once
    // the deferred readback has delivered, so the prune happens on that second close.
    if (_app.dumpStateAtExit().has_value())
    {
        inspect();
        return;
    }

    // Prune this pane from the model FIRST (sessionClosed -> removeSession -> closePane collapses the
    // split / removes the tab), THEN emit terminated() on the display: TerminalPane.onTerminated closes
    // the OS window only when canCloseWindow() sees no remaining pane sessions, which requires this
    // session to be gone already (the order canCloseWindow() documents). Re-check _display after the
    // emit: the model prune may have torn the display down with the pane.
    emit sessionClosed(*this);

    if (_display)
    {
        sessionLog()("Terminal device is closed. Notify manager and close the pane display.");
        _manager->currentSessionIsTerminated();
        _display->closeDisplay();
    }
    else
        sessionLog()("Terminal device is closed. But no display available (yet).");
}

void TerminalSession::pasteFromClipboard(unsigned count, bool strip)
{
    if (QClipboard* clipboard = QGuiApplication::clipboard(); clipboard != nullptr)
    {
        // mimeData() returns nullptr when the clipboard is empty or unavailable (e.g. on the
        // offscreen platform, or a headless session with nothing ever copied). Guard it: an empty
        // clipboard is a paste no-op, not a crash. The format logging only runs when data exists.
        if (QMimeData const* md = clipboard->mimeData(); md != nullptr)
        {
            sessionLog()("pasteFromClipboard: mime data contains {} formats.", md->formats().size());
            for (int i = 0; i < md->formats().size(); ++i)
                sessionLog()("pasteFromClipboard[{}]: {}\n", i, md->formats().at(i).toStdString());
        }

        auto const text = clipboard->text(QClipboard::Clipboard);

        // 1 MB hard limit
        if (text.size() > 1024 * 1024)
        {
            sessionLog()("Clipboard contains huge text. Ignoring.");
            // A display-less session (background pane, headless test) has nowhere to toast the
            // rejection; the paste is ignored either way.
            if (_display)
                _display->post([this]() {
                    emit showNotification("Screenshot", QString::fromStdString("Paste is too big"));
                });
            return;
        }
        // 512 KB soft limit to ask user for permission
        if (text.size() > 1024 * 512)
        {
            _pendingBigPaste = clipboard;
            emit requestPermissionForPasteLargeFile();
            sessionLog()("Clipboard contains huge text. Requesting permission.");
            return;
        }

        string const strippedText = strip_if(normalize_crlf(clipboard->text(QClipboard::Clipboard)), strip);
        sessionLog()("Size of text: {}", strippedText.size());
        if (strippedText.empty())
            sessionLog()("Clipboard does not contain text.");
        else if (count == 1)
            terminal().sendPaste(string_view { strippedText });
        else
        {
            string fullPaste;
            for (unsigned i = 0; i < count; ++i)
                fullPaste += strippedText;
            terminal().sendPaste(string_view { fullPaste });
        }
    }
    else
        sessionLog()("Could not access clipboard.");
}

void TerminalSession::applyPendingPaste(bool allow, bool remember)
{
    sessionLog()("applyPendingPaste: allow={}, remember={}", allow, remember);
    if (remember)
        _rememberedPermissions[GuardedRole::BigPaste] = allow;

    if (!_pendingBigPaste)
        return;

    if (!allow)
    {
        _pendingBigPaste = std::nullopt;
        return;
    }

    auto* clipboard = _pendingBigPaste.value();
    auto text = clipboard->text(QClipboard::Clipboard);
    terminal().sendPaste(string_view { text.toStdString() });
}

void TerminalSession::onSelectionCompleted()
{
    switch (_config.onMouseSelection.value())
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

    sessionLog()("Application request to resize window: {}x{} cells", columns, lines);
    // Re-check _display at dispatch: pane rebinding may detach it before the GUI thread runs this.
    _display->post([this, lines, columns]() {
        if (_display)
            _display->resizeWindow(lines, columns);
    });
}

void TerminalSession::resizeTerminalToDisplaySize()
{
    // Re-check _display at dispatch: pane rebinding may detach it before the GUI thread runs this.
    if (_display)
        _display->post([this]() {
            if (_display)
                _display->resizeTerminalToDisplaySize();
        });
}

void TerminalSession::requestWindowResize(Width width, Height height)
{
    if (!_display)
        return;

    sessionLog()("Application request to resize window: {}x{} pixels", width, height);
    // Re-check _display at dispatch: pane rebinding may detach it before the GUI thread runs this.
    _display->post([this, width, height]() {
        if (_display)
            _display->resizeWindow(width, height);
    });
}

void TerminalSession::addToAccumulatedScroll(crispy::point pixelDelta,
                                             crispy::point angleDelta,
                                             vtbackend::ScrollPhase phase) noexcept
{
    // Drop incidental sideways drift before it can accumulate into a whole column step. Filtering here
    // rather than at the binding lookup keeps it in ONE place and fixes the mouse-reporting path too: an
    // application receiving phantom horizontal wheel reports during a vertical scroll is equally wrong.
    if (!_horizontalWheelGesture.acceptsHorizontal(pixelDelta, angleDelta, phase))
    {
        pixelDelta.x = 0;
        angleDelta.x = 0;
    }

    if (angleDelta && !pixelDelta)
        _accumulatedPixelScroll = {};
    else
        _accumulatedPixelScroll += pixelDelta;

    _accumulatedAngleScroll += angleDelta;
}

std::tuple<LineOffset, ColumnOffset> TerminalSession::consumeScroll() noexcept
{
    if (_accumulatedPixelScroll)
    {
        auto const pixelStepSize = crispy::point {
            .x = _display->cellSize().width.as<int>(),
            .y = _display->cellSize().height.as<int>(),
        };
        auto const pixelSteps = _accumulatedPixelScroll / pixelStepSize;

        if (pixelSteps)
        {
            _accumulatedPixelScroll -= pixelSteps * pixelStepSize;
            _accumulatedAngleScroll = {};

            return {
                LineOffset::cast_from(pixelSteps.y),
                ColumnOffset::cast_from(pixelSteps.x),
            };
        }
    }

    auto const angleStepSize = double { 8 * 5 };
    auto const angleSteps = _accumulatedAngleScroll / angleStepSize;

    _accumulatedAngleScroll -= angleSteps * angleStepSize;
    _accumulatedPixelScroll = {};

    // One step per notch; do NOT apply historyScrollMultiplier here (the ScrollUp/Down actions
    // and the backend's mouseWheelScrollMultiplier own it) or the alt-screen wheel double-counts.
    return {
        LineOffset::cast_from(angleSteps.y),
        ColumnOffset::cast_from(angleSteps.x),
    };
}

QString TerminalSession::title() const
{
    // Bound to main.qml's window `title:`, so Qt re-evaluates this on the GUI thread whenever the title
    // changes — concurrently with the parser thread's OSC 0/2 writer (setWindowTitle assigns _windowTitle
    // under _stateMutex). Read the locked copy via resolvedWindowTitle() rather than the lock-free
    // windowTitle() reference, which would tear (or use-after-free on a string reallocation) against that
    // writer. Native tabs/splits make these GUI-thread title reads far more frequent.
    auto const windowTitle = resolvedWindowTitle();
#if !defined(NDEBUG)
    return QString::fromStdString(windowTitle + " - Contour (DEBUG)");
#else
    return QString::fromStdString(windowTitle + " - Contour");
#endif
}

void TerminalSession::refreshGuiTabInfoForStatusLine()
{
    // setWindowTitle()/setTabName() are invoked on the parser thread while _stateMutex is held (ESU
    // path), so we must not call _manager->update() directly: it rebuilds the tab info from every
    // session and would re-lock that non-recursive mutex (the deadlock scheduleRedraw() was changed to
    // avoid). Post the refresh to the GUI thread instead, where it runs free of the parser-thread lock.
    //
    // Post through THIS session (a QObject on the GUI thread), NOT through _display: a background
    // (unfocused) tab's session has no display attached — the display follows the active tab — so
    // gating on _display would skip the tab-strip label refresh for every unfocused tab, freezing its
    // title until it was next focused. The tab-strip update is a pure model change and does not need a
    // display. postToObject targets `this`, so Qt auto-cancels the queued call if the session is
    // destroyed first (e.g. closing a tab while its shell still emits an OSC title change), avoiding a
    // use-after-free on _manager.
    postToObject(this, [this]() {
        _manager->update();                               // indicator status line
        _manager->refreshTabForSession(modelSessionId()); // GUI tab strip label
    });
}

void TerminalSession::setWindowTitle(string_view title)
{
    emit titleChanged(QString::fromUtf8(title.data(), static_cast<int>(title.size())));

    // In TabsNamingMode::Title the indicator status-line tab name derives from the window title, so a
    // runtime title change must refresh the status-line tab info (no longer done by scheduleRedraw()).
    refreshGuiTabInfoForStatusLine();
}

void TerminalSession::setTabName(string_view name)
{
    // A tab-name change (via the tab-naming escape sequence) feeds the indicator status-line tab label;
    // refresh it on the GUI thread since scheduleRedraw() no longer does (see
    // refreshGuiTabInfoForStatusLine).
    (void) name;
    refreshGuiTabInfoForStatusLine();
}

void TerminalSession::setWindowFrameColor(vtbackend::RGBColor color)
{
    // DECAC item 2: the application assigned a window-frame color, which maps to this session's tab
    // background. This is invoked on the parser thread while Terminal::_stateMutex is held, so it must
    // NOT touch _manager->model() directly (SessionModel mutation drives the GUI model and
    // updateStatusLine() would re-lock the non-recursive mutex — see refreshGuiTabInfoForStatusLine).
    // Post the mutation to the GUI thread; postToObject targets `this`, so Qt auto-cancels the queued
    // call if the session is destroyed first, avoiding a use-after-free on _manager.
    postToObject(this,
                 [this, color, id = modelSessionId()]() { _manager->setTabColorForSession(id, color); });
}

void TerminalSession::resetWindowFrameColor()
{
    // DECAC item 2 with no colors, or a hard reset (RIS): clear the tab color. Same threading
    // constraint as setWindowFrameColor() above.
    postToObject(this, [this, id = modelSessionId()]() { _manager->resetTabColorForSession(id); });
}

void TerminalSession::setTerminalProfile(string const& configProfileName)
{
    if (!_display)
        return;

    // OSC-driven profile switch (application explicitly requested it): fit the window to the new
    // profile's terminal_size, as with the {ChangeProfile} keybinding.
    _display->post([this, name = string(configProfileName)]() {
        activateProfile(name, ProfileWindowSizePolicy::Apply);
    });
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
        case ViMode::Insert: configureCursor(_profile.modeInsert.value().cursor); break;
        case ViMode::Normal: configureCursor(_profile.modeNormal.value().cursor); break;
        case ViMode::Visual:
        case ViMode::VisualLine:
        case ViMode::VisualBlock: configureCursor(_profile.modeVisual.value().cursor); break;
        case ViMode::Hint: configureCursor(_profile.modeNormal.value().cursor); break;
    }
}

void TerminalSession::onScrollOffsetChanged(vtbackend::ScrollOffset value)
{
    emit scrollOffsetChanged(unbox(value));
}
// }}}
// {{{ Input Events

void handleAction(auto const& actions, auto eventType, auto callback)
{
    if (eventType == KeyboardEventType::Press)
        callback(*actions);
    else if (eventType == KeyboardEventType::Repeat)
    {
        // Drop actions that must not fire on auto-repeat (e.g. CloseTab/ClosePane/CreateNewTab). The
        // overwhelmingly common held keys (characters, arrows) bind only repeatable actions, so avoid
        // allocating a filtered copy unless there is actually a non-repeatable action to drop.
        if (std::ranges::none_of(*actions, actions::isNonRepeatable))
            callback(*actions);
        else
            callback(actions::filterRepeatableActions(*actions));
    }
}

void TerminalSession::sendKeyEvent(Key key,
                                   KeyboardModifiers modifiers,
                                   KeyboardEventType eventType,
                                   Timestamp now)
{
    inputLog()("Key {} event received: {} {}", eventType, modifiers, key);

    if (_terminatedAndWaitingForKeyPress && eventType == KeyboardEventType::Press)
    {
        sessionLog()("Terminated and waiting for key press. Closing pane.");
        // Prune-then-terminate, mirroring onClosed()'s normal path: remove this pane from the model
        // first so canCloseWindow() can approve closing the window, then let the QML side act on
        // terminated(). _display re-checked: the prune may tear the display down with the pane.
        emit sessionClosed(*this);
        if (_display)
            _display->closeDisplay();
        return;
    }

    // Guarded like sendCharEvent: a display-less session (background pane, headless test) has no
    // mouse cursor to hide.
    if (_profile.mouse.value().hideWhileTyping && _display != nullptr)
        _display->setMouseCursorShape(MouseCursorShape::Hidden);

    if (eventType != KeyboardEventType::Release)
    {
        // Key bindings match on the chord: a latched lock key must not change which shortcut fires.
        if (auto const* actions = config::apply(
                _config.inputMappings.value().keyMappings, key, modifiers.chord, matchModeFlags()))
        {
            auto executionCount = 0;
            handleAction(actions, eventType, [&](auto const& actions) {
                executionCount = executeAllActions(actions);
            });
            if (executionCount > 0)
                return;
        }
    }
    terminal().sendKeyEvent(key, modifiers, eventType, now);
}

void TerminalSession::sendCharEvent(char32_t value,
                                    uint32_t physicalKey,
                                    KeyboardModifiers modifiers,
                                    KeyboardEventType eventType,
                                    Timestamp now)
{
    inputLog()("Character {} event received: {} '{}'",
               eventType,
               modifiers,
               crispy::escape(unicode::convert_to<char>(value)));

    // The early-exit-notice acknowledge must run whether or not a display is attached, exactly like
    // sendKeyEvent above: the notice can be showing on a background/display-less pane, and a
    // character key must dismiss it too (previously this was nested under `if (_display)`, so a
    // character press never closed a display-less notice while a key press did — an inconsistency).
    if (_terminatedAndWaitingForKeyPress && eventType == KeyboardEventType::Press)
    {
        sessionLog()("Terminated and waiting for key press. Closing pane.");
        // Prune-then-terminate; see sendKeyEvent() for the rationale.
        emit sessionClosed(*this);
        if (_display)
            _display->closeDisplay();
        return;
    }

    if (_profile.mouse.value().hideWhileTyping && _display != nullptr)
        _display->setMouseCursorShape(MouseCursorShape::Hidden);

    if (eventType != KeyboardEventType::Release)
    {
        // Find a char binding for this key (ignored while editing the search prompt).
        auto const& charMappings = _config.inputMappings.value().charMappings;
        auto const flags = matchModeFlags();
        auto const* actions = config::apply(charMappings, value, modifiers.chord, flags);

        // A shortcut written with the base key label (e.g. `Ctrl+Shift+,`) is stored under the base
        // character, but Qt delivers a Shift+punctuation chord as the *shifted* symbol ('<' here). When
        // the direct lookup misses and Shift is held, retry under the un-shifted base so the binding
        // fires as the user intended — letters already match (their codepoint is shift-invariant).
        if (actions == nullptr && modifiers.chord.test(vtbackend::Modifier::Shift))
            if (auto const base = unshiftedCodepoint(value); base != value)
                actions = config::apply(charMappings, base, modifiers.chord, flags);

        if (actions != nullptr && !_terminal.inputHandler().isEditingSearch())
        {
            auto executionCount = 0;
            handleAction(actions, eventType, [&](auto const& actions) {
                executionCount = executeAllActions(actions);
            });
            if (executionCount > 0)
                return;
        }
    }
    terminal().sendCharEvent(value, physicalKey, modifiers, eventType, now);
}

void TerminalSession::sendMousePressEvent(Modifiers modifiers,
                                          MouseButton button,
                                          PixelCoordinate pixelPosition)
{
    auto const uiHandledHint = false;
    inputLog()("Mouse press received: {} {}\n", modifiers, button);

    terminal().tick(steady_clock::now());

    if (crispy::locked(_terminal, [&]() {
            return _terminal.sendMousePressEvent(modifiers, button, pixelPosition, uiHandledHint);
        }))
        return;

    auto const sanitizedModifier = modifiers.contains(_config.bypassMouseProtocolModifiers.value())
                                       ? modifiers.without(_config.bypassMouseProtocolModifiers.value())
                                       : modifiers;

    if (auto const* actions = config::apply(
            _config.inputMappings.value().mouseMappings, button, sanitizedModifier, matchModeFlags()))
    {
        executeAllActions(*actions);
        return;
    }

    // A horizontal notch reaching this point is about to become a DISCRETE navigation step (switching a
    // tab), and the scroll quantization that produced it counts one step per cell width — so a single
    // trackpad flick arrives here a dozen times over. Allow one per gesture.
    //
    // Only here, never earlier: an application that asked for the mouse consumed the press above and
    // still receives every one of them, because horizontal scrolling inside an application IS continuous.
    if ((button == MouseButton::WheelLeft || button == MouseButton::WheelRight)
        && !_horizontalWheelGesture.consumeNavigationStep())
        return;

    // The user's mappings did not claim this button, so fall back to the built-in ones. They are consulted
    // second on purpose: an explicit binding in the user's config always wins (see
    // builtinFallbackMouseMappings for why a plain default could not reach an existing user at all).
    if (auto const* actions =
            config::applyBuiltinFallback(_config, button, sanitizedModifier, matchModeFlags()))
        executeAllActions(*actions);
}

bool TerminalSession::applyFallbackMouseBinding(MouseButton button)
{
    auto const noModifiers = Modifiers { vtbackend::Modifier::None };
    auto const* actions = config::applyBuiltinFallback(_config, button, noModifiers, matchModeFlags());
    if (actions == nullptr)
        return false;

    executeAllActions(*actions);
    return true;
}

void TerminalSession::sendMouseMoveEvent(vtbackend::Modifiers modifiers,
                                         vtbackend::CellLocation pos,
                                         vtbackend::PixelCoordinate pixelPosition)
{
    // NB: This translation depends on the display's margin, so maybe
    //     the display should provide the translation?

    if (!(pos < terminal().pageSize()))
        return;

    terminal().tick(steady_clock::now());

    auto constexpr UiHandledHint = false;
    crispy::locked(_terminal,
                   [&]() { _terminal.sendMouseMoveEvent(modifiers, pos, pixelPosition, UiHandledHint); });

    // The cursor shape lives on the display; a display-less session (background pane, headless
    // test) has no cursor to change.
    if (pos != _currentMousePosition && _display != nullptr)
    {
        // Change cursor shape only when changing grid cell.
        _currentMousePosition = pos;
        if (terminal().isMouseHoveringHyperlink()
            || (modifiers.contains(vtbackend::Modifier::Control) && terminal().localPathAtMousePosition()))
            _display->setMouseCursorShape(MouseCursorShape::PointingHand);
        else
            setDefaultCursor();
    }
}

void TerminalSession::sendMouseReleaseEvent(Modifiers modifiers,
                                            MouseButton button,
                                            PixelCoordinate pixelPosition)
{
    terminal().tick(steady_clock::now());

    crispy::locked(_terminal, [&]() {
        auto const uiHandledHint = false;
        _terminal.sendMouseReleaseEvent(modifiers, button, pixelPosition, uiHandledHint);
    });
    scheduleRedraw();
}

void TerminalSession::performAutoScroll(int direction, vtbackend::LineCount lineCount)
{
    terminal().tick(steady_clock::now());
    crispy::locked(_terminal, [&]() { _terminal.performAutoScroll(direction, lineCount); });
}

void TerminalSession::sendFocusInEvent()
{
    // as per Qt-documentation, some platform implementations reset the cursor when leaving the
    // window, so we have to re-apply our desired cursor in focusInEvent().
    setDefaultCursor();

    terminal().sendFocusInEvent();

    if (_display)
        _display->setBlurBehind(_profile.background.value().blur);

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
    if (!_audio)
        _audio = std::make_unique<Audio>();

    auto range = params.range();
    _musicalNotesBuffer.clear();
    _musicalNotesBuffer.insert(_musicalNotesBuffer.begin(), range.begin() + 2, range.end());
    emit _audio->play(params.at(0), params.at(1), _musicalNotesBuffer);
}

void TerminalSession::cursorPositionChanged()
{
    QGuiApplication::inputMethod()->update(Qt::ImCursorRectangle);

    // Kept, not replaced, by the accessibility path below: on Windows, Magnifier frequently follows the
    // IME rectangle rather than a UIA text range.

    // Nobody is listening: the whole path costs one relaxed atomic load. Read through our own flag rather
    // than QAccessible::isActive(), which reads a Qt-internal static with no memory ordering — and this
    // runs on the TERMINAL thread.
    if (!display::TerminalAccessible::isActive())
        return;

    // This fires once per frame AND twice a second from the cursor blink (the render buffer's cursor is
    // simply absent while blinked off), so collapse repeats to at most one pending post.
    if (_caretUpdatePending.test_and_set(std::memory_order_acq_rel))
        return;

    // NOTHING about the terminal is read here. refreshRenderBuffer() reaches this callback with the state
    // mutex ALREADY HELD on one of its two paths, and that mutex is a plain non-recursive std::mutex — so
    // reading terminal state at this point would self-deadlock on one path and not the other, which is
    // the worst kind of hang to diagnose. The decision is made on the GUI thread instead.
    if (auto* display = _display)
        display->post([this]() {
            _caretUpdatePending.clear(std::memory_order_release);
            if (auto* target = _display)
                target->reportAccessibleCaret();
        });
}
// }}}
// {{{ Actions
bool TerminalSession::operator()(actions::CancelSelection)
{
    if (!_terminal.selectionAvailable())
        return false;
    _terminal.clearSelection();
    return true;
}

bool TerminalSession::operator()(actions::ChangeProfile const& action)
{
    sessionLog()("Changing profile to: {}", action.name);
    if (action.name == _profileName)
        return true;

    // Explicit user action: fit the window to the new profile's terminal_size.
    activateProfile(action.name, ProfileWindowSizePolicy::Apply);
    return true;
}

bool TerminalSession::operator()(actions::ClearHistoryAndReset)
{
    sessionLog()("Clearing history and perform terminal hard reset");

    // Locked, for the reason spelled out at operator()(SoftReset) below.
    crispy::locked(_terminal, [&]() { terminal().hardReset(); });
    return true;
}

bool TerminalSession::operator()(actions::CopyPreviousMarkRange)
{
    crispy::locked(_terminal, [&]() { copyToClipboard(terminal().extractLastMarkRange()); });
    return true;
}

bool TerminalSession::operator()(actions::SelectAll)
{
    crispy::locked(_terminal, [&]() { terminal().selectAll(); });
    return true;
}

bool TerminalSession::operator()(actions::OpenContextMenu)
{
    // Not while a left-drag selection is in flight. The popup takes the mouse grab, so the button-release
    // that would end that drag never reaches the display: the selection would go on extending with every
    // later hover, and the auto-scroll timer would go on firing, with no button held down at all.
    if (crispy::locked(_terminal, [&]() { return terminal().leftMouseButtonPressed(); }))
        return false;

    _manager->openContextMenu(this);
    return true;
}

ContextMenuState TerminalSession::contextMenuState()
{
    auto profileNames = std::vector<std::string> {};
    for (auto const& name: _config.profiles.value() | std::views::keys)
        profileNames.push_back(name);
    // `profiles` is an unordered_map: without this the submenu would shuffle its rows between two opens.
    std::ranges::sort(profileNames);

    // mimeData()->hasText(), not text(): the latter is a synchronous round-trip to whichever process owns
    // the clipboard, and it drags the ENTIRE payload across — a 5 MB log, a huge listing — only for
    // .isEmpty() to throw it away. On the GUI thread, inside a mouse-press handler. This is the very cost
    // pasteFromClipboard() is written to avoid (it refuses above 1 MB and asks above 512 KB); asking for
    // the available formats instead answers the same question without transferring a byte of content.
    auto const* clipboard = QGuiApplication::clipboard();
    auto const* mimeData = clipboard != nullptr ? clipboard->mimeData(QClipboard::Clipboard) : nullptr;
    auto const clipboardHasText = mimeData != nullptr && mimeData->hasText();

    // This machine's host name, so the working-directory row can be grayed out for a remote (SSH) cwd
    // whose path the local file manager cannot open. Read outside the terminal lock (it is not grid state).
    auto const localHost = QHostInfo::localHostName().toStdString();

    // One lock for the whole snapshot. The parser thread mutates the grid concurrently, so a menu that
    // asked the terminal a fresh question per row would be reading a moving target — and a QML binding
    // that reached into the terminal on its own schedule would be a plain data race.
    return crispy::locked(_terminal, [&]() {
        auto const block = terminal().lastCommandBlock();
        auto const hyperlink = terminal().tryGetHoveringHyperlink();

        return ContextMenuState {
            .hasSelection = terminal().selectionAvailable(),
            .clipboardHasText = clipboardHasText,
            // An empty block is no block. A shell that emits OSC 133;D at its very first prompt (tcsh
            // cannot guard against that in an alias) reports a command that never ran, with no text to
            // copy — offering the user three rows that would all yield nothing.
            .hasLastCommand = block.has_value() && !(block->prompt.empty() && block->output.empty()),
            // Only a working directory on this host can be opened by the local file manager. OSC 7 gives a
            // file://HOST/PATH; a remote (SSH) cwd resolves to nullopt and grays the "Open Current Folder".
            .hasLocalWorkingDirectory =
                vtbackend::localWorkingDirectory(terminal().currentWorkingDirectory(), localHost).has_value(),
            // Left for the window to fill in: whether this tab holds more than one pane is not something
            // a session knows about itself.
            .hasSplits = false,
            .inputProtected = !terminal().allowInput(),
            // Taken now, while the pointer is still on the cell the user clicked. The rows built from this
            // carry the URI with them, because by the time one is picked the pointer has moved to the menu.
            .hyperlinkUnderCursor = hyperlink ? hyperlink->uri : std::string {},
            .activeProfile = profileName(),
            .profileNames = std::move(profileNames),
        };
    });
}

bool TerminalSession::operator()(actions::SoftReset)
{
    sessionLog()("Performing terminal soft reset");

    // A soft reset is far from a handful of flags: it re-establishes the margins and drops the status
    // display, which RESIZES the main page. Run bare on the GUI thread, that grid resize races the parser
    // thread's line writes — a corrupted screen at best, a heap-buffer-overflow at worst.
    //
    // Taking the lock cannot deadlock: DECSTR (CSI ! p) already reaches Terminal::softReset() from the
    // parser thread, from inside writeToScreen()'s own _stateMutex hold, so every callback the reset makes
    // is exercised under this very lock every time an application asks for one. This path simply joins it.
    crispy::locked(_terminal, [&]() { terminal().softReset(); });
    return true;
}

bool TerminalSession::operator()(actions::CopyLastCommandPrompt)
{
    return copyLastCommandBlock(vtbackend::CommandBlockPart::Prompt);
}

bool TerminalSession::operator()(actions::CopyLastCommandOutput)
{
    return copyLastCommandBlock(vtbackend::CommandBlockPart::Output);
}

bool TerminalSession::operator()(actions::CopyLastCommandBlock)
{
    return copyLastCommandBlock(vtbackend::CommandBlockPart::PromptAndOutput);
}

bool TerminalSession::copyLastCommandBlock(vtbackend::CommandBlockPart part)
{
    auto const block = crispy::locked(_terminal, [&]() { return terminal().lastCommandBlock(); });
    if (!block)
        return false;

    // Copying nothing is not a copy — it is the destruction of whatever the user had on the clipboard.
    // `cd /tmp` prints not one character, and its block's Output is empty; so is the Prompt of a block
    // whose prompt line has already scrolled out of the history. QClipboard::setText("") would replace the
    // URL the user copied a minute ago with nothing at all, and the menu row would look like it did
    // nothing. Refusing leaves the clipboard alone, and tells the caller the row had nothing to give.
    auto const text = vtbackend::textOf(*block, part);
    if (text.empty())
        return false;

    copyToClipboard(text);
    return true;
}

bool TerminalSession::operator()(actions::CopyHyperlink const& action)
{
    // A URI the caller pinned wins: the context menu captured the link the user right-clicked, and asking
    // the terminal again now would answer about wherever the pointer has since wandered. A key binding
    // pins nothing, and for it "the link under the cursor" is exactly the right question.
    if (!action.uri.empty())
    {
        copyToClipboard(action.uri);
        return true;
    }

    auto const l = scoped_lock { terminal() };
    auto const hyperlink = terminal().tryGetHoveringHyperlink();
    if (!hyperlink)
        return false;

    copyToClipboard(hyperlink->uri);
    return true;
}

bool TerminalSession::operator()(actions::CopySelection copySelection)
{

    switch (copySelection.format)
    {
        case actions::CopyFormat::Text:
            // Copy the selection in pure text, plus whitespaces and newline.
            crispy::locked(_terminal, [&]() { copyToClipboard(terminal().extractSelectionText()); });
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

bool TerminalSession::operator()(actions::CreateSelection const& customSelector)
{
    _terminal.triggerWordWiseSelectionWithCustomDelimiters(customSelector.delimiters);
    return true;
}

bool TerminalSession::operator()(actions::DecreaseFontSize)
{
    auto constexpr OnePt = text::font_size { 1.0 };
    setFontSize(profile().fonts.value().size - OnePt);

    emit fontSizeChanged();
    // auto const currentFontSize = view().renderer().fontDescriptions().size;
    // auto const newFontSize = currentFontSize - OnePt;
    // setFontSize(newFontSize);
    return true;
}

bool TerminalSession::operator()(actions::DecreaseOpacity)
{
    if (static_cast<uint8_t>(_profile.background.value().opacity) == 0)
        return true;

    --_profile.background.value().opacity;

    emit opacityChanged();

    // Also emit backgroundColorChanged() because the background color is
    // semi-transparent and thus the opacity change affects the background color.
    emit backgroundColorChanged();

    return true;
}

bool TerminalSession::operator()(actions::FocusNextSearchMatch)
{
    auto const nextPosition = _terminal.searchNextMatch(_terminal.normalModeCursorPosition());
    if (!nextPosition)
        return false;
    _terminal.moveNormalModeCursorTo(nextPosition.value());
    _terminal.viewport().makeVisibleWithinSafeArea(nextPosition->line);
    // TODO why didn't the makeVisibleWithinSafeArea() call from inside jumpToNextMatch not work?
    return true;
}

bool TerminalSession::operator()(actions::FocusPreviousSearchMatch)
{
    auto const nextPosition = _terminal.searchPrevMatch(_terminal.normalModeCursorPosition());
    if (!nextPosition)
        return false;
    _terminal.moveNormalModeCursorTo(nextPosition.value());
    _terminal.viewport().makeVisibleWithinSafeArea(nextPosition->line);
    // TODO why didn't the makeVisibleWithinSafeArea() call from inside jumpToPreviousMatch not work?
    return true;
}

bool TerminalSession::operator()(actions::FollowHyperlink const& action)
{
    // Pinned by the caller (the context menu, which captured the link the user right-clicked) beats asking
    // the terminal where the pointer happens to rest now. See operator()(CopyHyperlink) above.
    if (!action.uri.empty())
    {
        followHyperlink(vtbackend::HyperlinkInfo { .userId = {}, .uri = action.uri });
        return true;
    }

    auto const l = scoped_lock { terminal() };
    if (auto const hyperlink = terminal().tryGetHoveringHyperlink())
    {
        followHyperlink(*hyperlink);
        return true;
    }
    if (auto const path = terminal().localPathAtMousePosition())
    {
        openDocument(*path);
        return true;
    }
    return false;
}

bool TerminalSession::operator()(actions::HintMode const& action)
{
    sessionLog()("Activating hint mode with patterns: '{}', action: {}", action.patterns, action.hintAction);

    // Start with builtin patterns.
    auto patterns = vtbackend::HintModeHandler::builtinPatterns();

    // Merge user-configured patterns: override builtins with same name, append new ones.
    for (auto const& userPattern: profile().hintPatterns.value())
    {
        try
        {
            auto compiled = vtbackend::HintPattern {
                .name = userPattern.name,
                .regex = std::regex(userPattern.regex,
                                    std::regex_constants::ECMAScript | std::regex_constants::optimize),
                .validator = {},
                .transformer = {},
            };
            auto const it =
                std::ranges::find_if(patterns, [&](auto const& p) { return p.name == userPattern.name; });
            if (it != patterns.end())
                *it = std::move(compiled); // Override builtin with same name.
            else
                patterns.push_back(std::move(compiled)); // Append new user pattern.
        }
        catch (std::regex_error const& e)
        {
            sessionLog()("Skipping hint pattern '{}': invalid regex '{}': {}",
                         userPattern.name,
                         userPattern.regex,
                         e.what());
        }
    }

    // Filter by requested pattern name(s) if specified.
    if (!action.patterns.empty() && action.patterns != "all")
    {
        auto const requestedNames = crispy::split(std::string_view(action.patterns), '|');
        auto const nameMatches = [&](auto const& p) {
            return std::ranges::find(requestedNames, std::string_view(p.name)) != requestedNames.end();
        };

        if (std::ranges::any_of(patterns, nameMatches))
        {
            std::erase_if(patterns, [&](auto const& p) { return !nameMatches(p); });
            sessionLog()("Filtered to {} hint pattern(s) matching '{}'", patterns.size(), action.patterns);
        }
        else
        {
            sessionLog()("No hint patterns matched '{}', falling back to all patterns", action.patterns);
        }
    }

    auto const hintAction = action.hintAction;
    crispy::locked(terminal(), [&]() { terminal().activateHintMode(patterns, hintAction); });
    return true;
}

bool TerminalSession::operator()(actions::IncreaseFontSize)
{
    auto constexpr OnePt = text::font_size { 1.0 };
    // auto const currentFontSize = view().renderer().fontDescriptions().size;
    // auto const newFontSize = currentFontSize + OnePt;
    // setFontSize(newFontSize);

    emit fontSizeChanged();
    setFontSize(profile().fonts.value().size + OnePt);
    return true;
}

bool TerminalSession::operator()(actions::IncreaseOpacity)
{
    if (static_cast<uint8_t>(_profile.background.value().opacity) >= std::numeric_limits<uint8_t>::max())
        return true;
    ++_profile.background.value().opacity;

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

bool TerminalSession::operator()(actions::OpenConfiguration event)
{
    // By default open the in-app settings page over this session's window (routed through the manager
    // like every window-scoped op). The explicit `in_editor: true` opt-in instead opens the raw
    // configuration file in the OS's external editor — the historical behavior.
    if (!event.inEditor)
    {
        _manager->openSettings(/*acting*/ this);
        return true;
    }

    if (!_app.externalLauncher().openUrl(QUrl(QString::fromUtf8(_config.configFile.string().c_str()))))
        errorLog()("Could not open configuration file \"{}\".", _config.configFile.generic_string());

    return true;
}

bool TerminalSession::operator()(actions::OpenFileManager)
{
    // OSC 7 advertises the cwd as a file://HOST/PATH URL. Hand the raw URL to the file manager and its
    // host authority is read as a network share ("//fedora/home/..."), which does not exist locally.
    // Resolve it to a plain local path first, and only when it is on THIS host — a remote (SSH) cwd is
    // not openable here (its menu row is grayed out, but a keybinding could still reach this handler).
    auto const localHost = QHostInfo::localHostName().toStdString();
    auto localPath = std::optional<std::string> {};
    auto cwd = std::string {};
    {
        auto const l = scoped_lock { terminal() };
        cwd = terminal().currentWorkingDirectory();
        localPath = vtbackend::localWorkingDirectory(cwd, localHost);
    }

    if (!localPath)
    {
        // A remote (SSH) or otherwise non-local cwd cannot be opened in this host's file manager. The
        // context-menu row for this is grayed out, but a keybinding can still reach this handler — so
        // report why nothing happened rather than silently swallowing the request.
        errorLog()("Cannot open file manager: working directory \"{}\" is not on the local host.", cwd);
        return true;
    }

    if (!_app.externalLauncher().openUrl(QUrl::fromLocalFile(QString::fromStdString(*localPath))))
        errorLog()("Could not open folder \"{}\".", *localPath);

    return true;
}

bool TerminalSession::operator()(actions::OpenSelection)
{
    crispy::locked(_terminal, [&]() {
        (void) _app.externalLauncher().openUrl(
            QUrl(QString::fromUtf8(terminal().extractSelectionText().c_str())));
    });
    return true;
}

bool TerminalSession::operator()(actions::PasteClipboard paste)
{
    pasteFromClipboard(1, paste.strip);
    return true;
}

bool TerminalSession::operator()(actions::PasteSelection paste)
{
    if (QClipboard* clipboard = QGuiApplication::clipboard(); clipboard != nullptr)
    {
        string const text = normalize_crlf(clipboard->text(QClipboard::Selection));
        if (paste.evaluateInShell)
            terminal().sendRawInput(string_view { text + "\n" });
        else
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
        setFontSize(profile->fonts.value().size);
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

bool TerminalSession::operator()(actions::SaveScreenshot)
{
    auto savePath =
        app().dumpStateAtExit().value_or(crispy::app::instance()->localStateDir())
        / fs::path(std::format("contour-screenshot-{:%Y-%m-%d-%H-%M-%S}.png", chrono::system_clock::now()));

    _display->setScreenshotOutput(savePath);
    auto message = std::format("Saving screenshot to {}", savePath.string());
    sessionLog()(message);

    _display->post(
        [this, message]() { emit showNotification("Screenshot", QString::fromStdString(message)); });
    return true;
}

bool TerminalSession::operator()(actions::CopyScreenshot)
{
    _display->setScreenshotOutput(std::monostate {});
    auto message = std::format("Saving screenshot to clipboard");
    sessionLog()(message);

    _display->post(
        [this, message]() { emit showNotification("Screenshot", QString::fromStdString(message)); });

    return true;
}

void TerminalSession::smoothScrollUp(vtbackend::LineCount lineCount)
{
    if (terminal().settings().smoothScrolling)
    {
        auto const cellHeight = static_cast<float>(terminal().cellPixelSize().height.as<int>());
        auto const pixels = static_cast<float>(*lineCount) * cellHeight;
        if (terminal().applySmoothScrollPixelDelta(pixels) == vtbackend::SmoothScrollResult::Applied)
            return;
    }
    terminal().viewport().scrollUp(lineCount);
}

void TerminalSession::smoothScrollDown(vtbackend::LineCount lineCount)
{
    if (terminal().settings().smoothScrolling)
    {
        auto const cellHeight = static_cast<float>(terminal().cellPixelSize().height.as<int>());
        auto const pixels = -static_cast<float>(*lineCount) * cellHeight;
        if (terminal().applySmoothScrollPixelDelta(pixels) == vtbackend::SmoothScrollResult::Applied)
            return;
    }
    terminal().viewport().scrollDown(lineCount);
}

bool TerminalSession::operator()(actions::ScrollDown)
{
    smoothScrollDown(vtbackend::LineCount(*_profile.history.value().historyScrollMultiplier));
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
    smoothScrollDown(LineCount(1));
    return true;
}

bool TerminalSession::operator()(actions::ScrollOneUp)
{
    smoothScrollUp(LineCount(1));
    return true;
}

bool TerminalSession::operator()(actions::ScrollPageDown)
{
    auto const stepSize = terminal().pageSize().lines / LineCount(2);
    smoothScrollDown(stepSize);
    return true;
}

bool TerminalSession::operator()(actions::ScrollPageUp)
{
    auto const stepSize = terminal().pageSize().lines / LineCount(2);
    smoothScrollUp(stepSize);
    return true;
}

bool TerminalSession::operator()(actions::ScrollToBottom)
{
    // Snap immediately for ScrollToTop/Bottom (animating large distances is impractical).
    terminal().resetSmoothScroll();
    terminal().viewport().scrollToBottom();
    return true;
}

bool TerminalSession::operator()(actions::ScrollToTop)
{
    // Snap immediately for ScrollToTop/Bottom (animating large distances is impractical).
    terminal().resetSmoothScroll();
    terminal().viewport().scrollToTop();
    return true;
}

bool TerminalSession::operator()(actions::ScrollUp)
{
    smoothScrollUp(vtbackend::LineCount(*_profile.history.value().historyScrollMultiplier));
    return true;
}

bool TerminalSession::operator()(actions::SearchReverse)
{
    terminal().inputHandler().startSearchExternally();

    return true;
}

bool TerminalSession::operator()(actions::SendChars const& event)
{
    // auto const now = steady_clock::now();
    // for (auto const ch: event.chars)
    //     terminal().sendCharPressEvent(static_cast<char32_t>(ch), vtbackend::Modifiers::None, now);
    terminal().sendRawInput(event.chars);
    return true;
}

bool TerminalSession::operator()(actions::ToggleAllKeyMaps)
{
    _allowKeyMappings = !_allowKeyMappings;
    inputLog()("{} key mappings.", _allowKeyMappings ? "Enabling" : "Disabling");

    if (!_allowKeyMappings)
    {
        terminal().setStatusLineDefinition(
            parseStatusLineDefinition("{}", "{Text:text=key bindings disabled,Left= « ,Right= » }", "{}"));
        terminal().setStatusDisplay(StatusDisplayType::Indicator);
    }
    else
        terminal().resetStatusLineDefinition();
    return true;
}

bool TerminalSession::operator()(actions::ToggleFullscreen)
{
    if (_display)
        _display->toggleFullScreen();
    return true;
}

bool TerminalSession::operator()(actions::ToggleInputMethodHandling)
{
    if (_display)
        _display->toggleInputMethodEditorHandling();
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
    if (terminal().statusDisplayType() != StatusDisplayType::Indicator)
        terminal().setStatusDisplay(StatusDisplayType::Indicator);
    else
        terminal().setStatusDisplay(StatusDisplayType::None);

    // `savedStatusDisplayType` holds only a value if the application has been overriding
    // the status display type. But the user now actively requests a given type,
    // so make sure restoring will not destroy the user's desire.
    if (terminal().savedStatusDisplayType())
        terminal().setSavedStatusDisplayType(terminal().statusDisplayType());

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

bool TerminalSession::operator()(actions::CreateNewTab)
{
    _manager->createNewTab(this);
    return true;
}

bool TerminalSession::operator()(actions::CloseTab)
{
    _manager->closeTab(this);
    return true;
}

bool TerminalSession::operator()(actions::MoveTabTo event)
{
    _manager->moveTabTo(event.position, this);
    return true;
}

bool TerminalSession::operator()(actions::MoveTabToLeft)
{
    _manager->moveTabToLeft(this);
    return true;
}

bool TerminalSession::operator()(actions::MoveTabToRight)
{
    _manager->moveTabToRight(this);
    return true;
}

bool TerminalSession::operator()(actions::SwitchToTab const& event)
{
    _manager->switchToTab(event.position, this);
    return true;
}

bool TerminalSession::operator()(actions::SwitchToPreviousTab)
{
    _manager->switchToPreviousTab(this);
    return true;
}

bool TerminalSession::operator()(actions::SwitchToTabLeft)
{
    _manager->switchToTabLeft(this);
    return true;
}

bool TerminalSession::operator()(actions::SwitchToTabRight)
{
    _manager->switchToTabRight(this);
    return true;
}

bool TerminalSession::operator()(actions::OpenCommandPalette)
{
    // Open the GUI-native command palette over this session's window. Routed through the manager like
    // every other window-scoped op so it targets the window this session is actually in.
    _manager->openCommandPalette(/*acting*/ this);
    return true;
}

bool TerminalSession::operator()(actions::SetTabTitle)
{
    // Open the GUI-native inline tab-title editor for the active tab. Routed through the manager
    // like every other tab op so it targets this session's window.
    _manager->beginTabTitleEdit(/*acting*/ this);
    return true;
}

bool TerminalSession::operator()(actions::SetTabColor const& action)
{
    // Naming no color means "let me pick one": open the same flyout the tab's context menu opens.
    // Naming one means the user already decided, so skip the popup and apply it.
    if (action.color.has_value())
        _manager->setActiveTabColor(*action.color, /*acting*/ this);
    else
        _manager->beginTabColorPick(/*acting*/ this);
    return true;
}

bool TerminalSession::operator()(actions::ResetTabColor)
{
    _manager->resetActiveTabColor(/*acting*/ this);
    return true;
}

bool TerminalSession::operator()(actions::SplitVertical)
{
    _manager->splitActivePane(/*vertical*/ true, /*acting*/ this);
    return true;
}

bool TerminalSession::operator()(actions::SplitHorizontal)
{
    _manager->splitActivePane(/*vertical*/ false, /*acting*/ this);
    return true;
}

bool TerminalSession::operator()(actions::ClosePane)
{
    _manager->closeActivePane(/*acting*/ this);
    return true;
}

bool TerminalSession::operator()(actions::FocusPaneLeft)
{
    _manager->focusPane(vtmux::FocusDirection::Left, /*acting*/ this);
    return true;
}

bool TerminalSession::operator()(actions::FocusPaneRight)
{
    _manager->focusPane(vtmux::FocusDirection::Right, /*acting*/ this);
    return true;
}

bool TerminalSession::operator()(actions::FocusPaneUp)
{
    _manager->focusPane(vtmux::FocusDirection::Up, /*acting*/ this);
    return true;
}

bool TerminalSession::operator()(actions::FocusPaneDown)
{
    _manager->focusPane(vtmux::FocusDirection::Down, /*acting*/ this);
    return true;
}

namespace
{
    /// Translates an action-layer Direction (transport-agnostic) into the model's FocusDirection.
    /// @param direction The direction the user requested.
    /// @return The corresponding vtmux::FocusDirection.
    [[nodiscard]] constexpr vtmux::FocusDirection toFocusDirection(actions::Direction direction) noexcept
    {
        switch (direction)
        {
            case actions::Direction::Left: return vtmux::FocusDirection::Left;
            case actions::Direction::Right: return vtmux::FocusDirection::Right;
            case actions::Direction::Up: return vtmux::FocusDirection::Up;
            case actions::Direction::Down: return vtmux::FocusDirection::Down;
        }
        return vtmux::FocusDirection::Left;
    }
} // namespace

bool TerminalSession::operator()(actions::SwapPaneLeft)
{
    _manager->swapPane(vtmux::FocusDirection::Left, /*acting*/ this);
    return true;
}

bool TerminalSession::operator()(actions::SwapPaneRight)
{
    _manager->swapPane(vtmux::FocusDirection::Right, /*acting*/ this);
    return true;
}

bool TerminalSession::operator()(actions::SwapPaneUp)
{
    _manager->swapPane(vtmux::FocusDirection::Up, /*acting*/ this);
    return true;
}

bool TerminalSession::operator()(actions::SwapPaneDown)
{
    _manager->swapPane(vtmux::FocusDirection::Down, /*acting*/ this);
    return true;
}

bool TerminalSession::operator()(actions::MovePaneLeft)
{
    _manager->movePane(vtmux::FocusDirection::Left, /*acting*/ this);
    return true;
}

bool TerminalSession::operator()(actions::MovePaneRight)
{
    _manager->movePane(vtmux::FocusDirection::Right, /*acting*/ this);
    return true;
}

bool TerminalSession::operator()(actions::MovePaneUp)
{
    _manager->movePane(vtmux::FocusDirection::Up, /*acting*/ this);
    return true;
}

bool TerminalSession::operator()(actions::MovePaneDown)
{
    _manager->movePane(vtmux::FocusDirection::Down, /*acting*/ this);
    return true;
}

bool TerminalSession::operator()(actions::ToggleSplitOrientation)
{
    _manager->toggleActivePaneOrientation(/*acting*/ this);
    return true;
}

bool TerminalSession::operator()(actions::TogglePaneZoom)
{
    _manager->toggleActivePaneZoom(/*acting*/ this);
    return true;
}

bool TerminalSession::operator()(actions::ResizePane const& action)
{
    // percent is a whole-number step; convert to the (0, 1) ratio fraction the model nudges by.
    _manager->resizeActivePane(
        toFocusDirection(action.direction), static_cast<double>(action.percent) / 100.0, /*acting*/ this);
    return true;
}

bool TerminalSession::operator()(actions::LaunchLayout const& event)
{
    _manager->launchLayout(event.name, this);
    return true;
}

bool TerminalSession::operator()(actions::SaveLayout const& event)
{
    // Naming no layout means "ask me for one": open the save-as prompt (the mirror of a colorless
    // SetTabColor opening the color picker). Naming one means the user already decided, so save directly.
    if (event.name.empty())
        _manager->beginSaveLayoutPrompt(/*acting*/ this);
    else
        _manager->saveLayout(event.name, this);
    return true;
}

// }}}
// {{{ implementation helpers
void TerminalSession::setDefaultCursor()
{
    if (!_display)
        return;

    using Type = vtbackend::ScreenType;
    switch (_terminal.screenType())
    {
        case Type::Primary: _display->setMouseCursorShape(MouseCursorShape::IBeam); break;
        case Type::Alternate: _display->setMouseCursorShape(MouseCursorShape::Arrow); break;
    }
}

bool TerminalSession::reloadConfig(config::Config newConfig, string const& profileName)
{
    // clang-format off
    sessionLog()("Reloading configuration from {} with profile {}",
                 newConfig.configFile.string(), profileName);
    // clang-format on

    _config = std::move(newConfig);
    // Passive config-file reload: keep the user's live window size (default Preserve policy). Only an
    // explicit profile switch re-fits the window to the profile's terminal_size.
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
        return std::ranges::any_of(actions, [](actions::Action const& action) {
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

std::string TerminalSession::workingDirectory() const
{
#if !defined(_WIN32)
    if (auto const* ptyProcess = dynamic_cast<vtpty::Process const*>(&_terminal.device()))
        return ptyProcess->workingDirectory();
#else
    // On Windows the CWD is only known via OSC 7, which reports it as a file:// URL. Passing that raw
    // URL to CreateProcess() as a new tab/split's working directory fails with ERROR_DIRECTORY and
    // crashes the app, so extract the filesystem path first (fix from master's OSC-7 crash fix).
    std::string cwdUrl;
    {
        auto const _l = scoped_lock { _terminal };
        cwdUrl = _terminal.currentWorkingDirectory();
    }
    if (!cwdUrl.empty())
        if (auto path = vtbackend::extractPathFromFileUrl(cwdUrl); !path.empty())
        {
            // The path is only usable to CreateProcess() if it exists on THIS machine. An SSH session
            // advertises its *remote* cwd over OSC 7 (e.g. "file://remotehost/home/user" -> "/home/user"),
            // which does not exist locally; handing it to a new local tab/split's CreateProcess() fails
            // and Process::start() throws, and — being reached from a QML `session:` binding write inside
            // a Qt event handler — that exception aborts the whole process. Only inherit a directory that
            // actually exists locally; otherwise fall through to the "." sentinel (inherit our own cwd).
            std::error_code ec;
            if (std::filesystem::is_directory(fs::path(path), ec))
                return path;
        }
#endif
    return "."s;
}

std::string TerminalSession::displayWorkingDirectory() const
{
    // OSC 7 first: it is the shell speaking, so it tracks a `cd` made inside a full-screen application
    // and it is the only source that can be right for a remote session. Reported as a file:// URL.
    auto cwdUrl = std::string {};
    {
        auto const lock = scoped_lock { _terminal };
        cwdUrl = _terminal.currentWorkingDirectory();
    }
    if (!cwdUrl.empty())
        if (auto path = vtbackend::extractPathFromFileUrl(cwdUrl); !path.empty())
            return path;

    // Nothing reported (no shell integration, or not yet): fall back to where the session was started.
    // Unlike workingDirectory() this is NOT filtered for local existence — a path worth SHOWING need not
    // be one a child could be spawned in.
#if !defined(_WIN32)
    if (auto const* ptyProcess = dynamic_cast<vtpty::Process const*>(&_terminal.device()))
        return ptyProcess->workingDirectory();
#endif

    return {};
}

void TerminalSession::spawnNewTerminal(string const& profileName)
{
    auto const wd = workingDirectory();

    if (_config.spawnNewProcess.value())
    {
        sessionLog()("spawning new process");
        auto const command = ::contour::buildSpawnTerminalCommand(
            _app.programPath(), _config.configFile.generic_string(), profileName, wd);
        _app.externalLauncher().runDetached(command.program, command.arguments);
    }
    else
    {
        sessionLog()("spawning new in-process window");
        _app.config().profile(_profileName)->shell.value().workingDirectory = fs::path(wd);
        // The new window mints its own WindowController + first tab in main.qml's Component.onCompleted,
        // so no session-staging handshake is needed.
        // A window spawned from an existing one should open on that window's screen (the best
        // pre-show DPR predictor); the new window's bindWindow() consumes it.
        _app.newWindow(_display != nullptr && _display->window() != nullptr ? _display->window()->screen()
                                                                            : nullptr);
    }
}

void TerminalSession::emitProfileDerivedPropertiesChanged()
{
    // One row per Q_PROPERTY that is read out of the profile — directly (opacity, dim_unfocused,
    // scrollbar.*) or out of the color palette that configureTerminal() re-seeds from it (the background
    // image/blur/color). Adding a profile-derived property means adding a row here, rather than
    // remembering to hand-write another `emit` at every site that swaps the profile.
    //
    // These signals carry no C++ slots: they exist purely so the QML bindings in TerminalPane.qml and
    // SessionChrome.qml re-read the now-current profile. Emitting one whose value did not actually change
    // therefore costs a binding re-evaluation and nothing else.
    static constexpr auto Notifiers = std::array {
        &TerminalSession::opacityChanged,
        &TerminalSession::dimUnfocusedChanged,
        &TerminalSession::pathToBackgroundChanged,
        &TerminalSession::opacityBackgroundChanged,
        &TerminalSession::isImageBackgroundChanged,
        &TerminalSession::isBlurBackgroundChanged,
        &TerminalSession::backgroundColorChanged,
        &TerminalSession::isScrollbarRightChanged,
        &TerminalSession::isScrollbarVisibleChanged,
    };

    for (auto const notify: Notifiers)
        (this->*notify)();
}

void TerminalSession::activateProfile(string const& newProfileName, ProfileWindowSizePolicy windowSizePolicy)
{
    // findProfile() (not profile()): the name comes from runtime input (a keybinding or the
    // {ChangeProfile} action) and may reference a profile the user removed. profile() asserts on a
    // miss — a precondition for callers with a proven-present name — and would abort the app here.
    auto* newProfile = _config.findProfile(newProfileName);
    if (!newProfile)
    {
        sessionLog()("Cannot change profile. No such profile: '{}'.", newProfileName);
        return;
    }

    sessionLog()("Changing profile to {}.", newProfileName);
    _profileName = newProfileName;
    _profile = *newProfile;
    configureTerminal();

    // _profile was just replaced, so every QML binding reading a profile-derived property is now stale.
    emitProfileDerivedPropertiesChanged();

    // An EXPLICIT profile switch (keybinding / OSC request) may change the configured grid
    // (terminal_size); ask the window to fit it — a content-driven grid->window request through the
    // controller choke point (refused when fullscreen/maximized; a WM refusal leaves the reflowed
    // grid). Posted like the sibling display calls; the inner _display re-check covers a teardown
    // between schedule and dispatch.
    //
    // A passive config-file RELOAD must NOT resize: the window size is the user's live authority once
    // the window is mapped, so re-fitting to terminal_size on every save would silently discard an
    // interactively-set size (and, with one file-watcher per pane, fire N competing resizes at once).
    // Hence the policy gate — the resize is tied to the intent of the activation, not the activation.
    if (_display != nullptr && windowSizePolicy == ProfileWindowSizePolicy::Apply)
    {
        auto const configuredSize = _profile.terminalSize.value();
        _display->post([this, configuredSize]() {
            if (_display != nullptr)
                _display->resizeWindow(configuredSize.lines, configuredSize.columns);
        });
    }

    // The tab-label template lives in the profile, so a reload may change every tab's label. This runs
    // on the GUI thread (config-reload path), so refresh the tab strip directly. Guarded because a
    // session may be configured before it is attached to a manager.
    if (_manager != nullptr)
        _manager->refreshAllTabTitles();
}

void TerminalSession::configureTerminal()
{
    auto const l = scoped_lock { _terminal };
    sessionLog()("Configuring terminal.");

    _terminal.setWordDelimiters(_config.wordDelimiters.value());
    _terminal.setExtendedWordDelimiters(_config.extendedWordDelimiters.value());
    _terminal.setMouseProtocolBypassModifiers(_config.bypassMouseProtocolModifiers.value());
    _terminal.setMouseBlockSelectionModifiers(_config.mouseBlockSelectionModifiers.value());
    _terminal.setLastMarkRangeOffset(_profile.copyLastMarkRangeOffset.value());

    sessionLog()("Setting terminal ID to {}.", _profile.terminalId.value());
    _terminal.setTerminalId(_profile.terminalId.value());
    _terminal.setMaxSixelColorRegisters(_config.images.value().maxImageColorRegisters);
    // NB: the image canvas ceiling is deliberately NOT touched here. It is monitor-derived, and
    // configureTerminal() runs on every config reload and profile switch -- paths that
    // configureDisplay() is not on. Re-deriving it needs a window; assigning the config value here
    // is what used to reset the canvas to 0x0 and make sixel images vanish until the next resize.
    _terminal.setMode(vtbackend::DECMode::NoSixelScrolling, !_config.images.value().sixelScrolling);
    _terminal.settings().goodImageProtocol = _config.images.value().goodImageProtocol;
    _terminal.setStatusDisplay(_profile.statusLine.value().initialType);
    sessionLog()("imageCanvasCeiling={}, sixelScrolling={}, goodImageProtocol={}",
                 _terminal.imageCanvasCeiling(),
                 _config.images.value().sixelScrolling,
                 _config.images.value().goodImageProtocol);

    // XXX
    // if (!terminalView.renderer().renderTargetAvailable())
    //     return;

    configureCursor(_profile.modeInsert.value().cursor);
    updateColorPreference(_app.colorPreference());
    _terminal.setMaxHistoryLineCount(_profile.history.value().maxHistoryLineCount);
    _terminal.setMouseWheelScrollMultiplier(_profile.history.value().historyScrollMultiplier);
    _terminal.settings().autoScrollOnUpdate = _profile.history.value().autoScrollOnUpdate;
    _terminal.setHighlightTimeout(_profile.highlightTimeout.value());
    _terminal.viewport().setScrollOff(_profile.modalCursorScrollOff.value());
    _terminal.inputHandler().setSearchModeSwitch(_profile.searchModeSwitch.value());
    _terminal.settings().isInsertAfterYank = _profile.insertAfterYank.value();
    _terminal.settings().blinkStyle = _profile.blinkStyle.value();
    _terminal.settings().screenTransitionStyle = _profile.screenTransitionStyle.value();
    _terminal.settings().screenTransitionDuration = _profile.screenTransitionDuration.value();
    _terminal.settings().cursorMotionAnimationDuration = _profile.cursorMotionAnimationDuration.value();
    _terminal.settings().smoothLineScrolling = _profile.smoothLineScrolling.value();
    _terminal.settings().smoothScrolling = _profile.smoothScrolling.value();
    _terminal.settings().momentumScrolling = _profile.momentumScrolling.value();
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

void TerminalSession::updateImageCanvasCeiling()
{
    // The cap is the monitor, not the window: a window resize cannot change what an image may be,
    // so only attach and monitor-change need to re-derive it.
    if (!_display || _display->window() == nullptr || _display->window()->screen() == nullptr)
        return;

    auto const screenSize = _display->window()->screen()->size();
    auto const devicePixels =
        geometry::availableDevicePixels(screenSize.width(), screenSize.height(), _display->contentScale());

    // In the unit every other pixel report uses: XTSMGRAPHICS answers this ceiling alongside a canvas
    // size the application reads in reported pixels, and it sizes an image against both. Leaving the
    // ceiling in device pixels on a scaled display would let it through an image the reported grid has
    // no room for -- one that then overflows the page and scrolls the screen instead of fitting it.
    auto const ceiling = geometry::reportedPixels(devicePixels, _display->reportedPixelScale());

    auto const _ = std::scoped_lock { _terminal };
    _terminal.setImageCanvasCeiling(ceiling);
}

void TerminalSession::configureDisplay()
{
    if (!_display)
        return;

    // This runs as a POSTED call (createRenderer defers it to the GUI loop), so it can dispatch after
    // this pane was already torn out of its window — window() returns null while the display object is
    // still alive (the same independent-teardown hazard the render-thread slots guard against). A
    // detached display has nothing to configure; dereferencing window()->screen() would crash.
    if (_display->window() == nullptr)
        return;

    // Same hazard, other resource: the scene graph can be INVALIDATED between the post and this
    // dispatch (X11/XWayland does so whenever the window is unexposed or its surface is recreated),
    // tearing the render target down while display and window stay alive. setFonts() below requires a
    // live render target by contract. Bailing out is the designed re-entry path, not a skip: every
    // render-target (re)creation re-posts configureDisplay() (see TerminalDisplay::createRenderer),
    // so configuration re-runs once rendering is possible again.
    if (!_display->hasRenderTarget())
        return;

    sessionLog()("Configuring display.");
    _display->setBlurBehind(_profile.background.value().blur);

    updateImageCanvasCeiling();

    // NB: The profile's window show-mode (maximized/fullscreen/normal) is deliberately NOT applied
    // here. Window-state authority belongs solely to WindowController::showInitial() — which maps
    // every window (fresh or tab-transplant receiver) directly into the profile's state on open —
    // and to the explicit user actions (toggleMaximized/toggleFullScreen). configureDisplay() runs
    // on EVERY renderer (re)creation: the first pane, but also each split leaf's fresh
    // TerminalDisplay and any scene-graph re-creation after the window loses and regains its surface.
    // Re-asserting the profile's (default: non-maximized) state on those would drop the user's live
    // maximized/fullscreen state — the "window leaves maximized when I split" regression. Splitting is
    // a pure content-area operation; the QWindow's geometry and state must not change.

    _terminal.setRefreshRate(_display->refreshRate());
    _display->setFonts(_profile.fonts.value());
    resizeTerminalToDisplaySize();

    _display->setHyperlinkDecoration(_profile.hyperlinkDecoration.value().normal,
                                     _profile.hyperlinkDecoration.value().hover);

    // Re-emit the current title to the freshly-attached display. This runs on the GUI thread, so read
    // it via resolvedWindowTitle() (locked copy) rather than the lock-free windowTitle() reference,
    // which could tear against a concurrent parser-thread title write.
    setWindowTitle(_terminal.resolvedWindowTitle());
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

    if (!_terminal.search().pattern.empty())
        flags |= static_cast<uint8_t>(MatchModes::Flag::Search);

    if (_terminal.executionMode() != ExecutionMode::Normal)
        flags |= static_cast<uint8_t>(MatchModes::Flag::Trace);

    return flags;
}

void TerminalSession::setFontSize(text::font_size size)
{
    // No display (a background tab/split pane whose display was detached on the last tab switch):
    // there is no renderer to reconfigure, so persist the requested size to the profile directly. It
    // is applied when a display re-attaches (setSession seeds the renderer from profile().fonts). This
    // guards the IncreaseFontSize/DecreaseFontSize/ResetFontSize keybindings, which a background pane
    // can receive — the same null-_display crash class as the other guarded action paths.
    if (_display == nullptr)
    {
        _profile.fonts.value().size = size;
        return;
    }

    // _display->setFontSize() stages the change and applies it synchronously (applyStagedFontReconfigNow),
    // then returns whether the rendered font actually became @p size: false if the size was out of range
    // (not even staged) or if the render-thread apply failed and was swallowed (font-load/atlas error,
    // previous font kept). Only persist the size to the profile when it returns true — recording a size
    // the renderer never loaded would diverge the profile from the rendered font and make a later
    // increase/decrease step chain from the wrong base.
    if (!_display->setFontSize(size))
        return;

    _profile.fonts.value().size = size;
}

bool TerminalSession::reloadConfigWithProfile(string const& profileName)
{
    auto newConfig = config::Config {};
    auto configFailures = int { 0 };

    try
    {
        loadConfigFromFile(newConfig, _config.configFile.string());
    }
    catch (exception const& e)
    {
        // TODO: _logger.error(e.what());
        errorLog()("Configuration failure. {}", unhandledExceptionMessage(__PRETTY_FUNCTION__, e));
        ++configFailures;
    }

    if (!newConfig.profile(profileName))
    {
        errorLog()(std::format("Currently active profile with name '{}' gone.", profileName));
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
    auto const ec = config::createDefaultConfig(_config.configFile);
    if (ec)
    {
        errorLog()("Failed to load default config at {}; ({}) {}",
                   _config.configFile.string(),
                   ec.category().name(),
                   ec.message());
        return false;
    }

    config::Config const defaultConfig;
    try
    {
        config::loadConfigFromFile(_config.configFile);
    }
    catch (exception const& e)
    {
        sessionLog()("Failed to load default config: {}", e.what());
    }

    return reloadConfig(defaultConfig, defaultConfig.defaultProfileName.value());
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
        args.append(QString::fromStdString(_config.configFile.string()));
        args.append(QString::fromUtf8(hyperlink.path().data(), static_cast<int>(hyperlink.path().size())));
        _app.externalLauncher().execute(QString::fromStdString(_app.programPath()), args);
    }
    else if (isLocal && fileInfo.isFile() && editorEnv && *editorEnv)
    {
        QStringList args;
        args.append("config");
        args.append(QString::fromStdString(_config.configFile.string()));
        args.append(QString::fromStdString(editorEnv));
        args.append(QString::fromUtf8(hyperlink.path().data(), static_cast<int>(hyperlink.path().size())));
        _app.externalLauncher().execute(QString::fromStdString(_app.programPath()), args);
    }
    else if (isLocal)
        (void) _app.externalLauncher().openUrl(QUrl(hyperlink.uri.c_str()));
    else
        (void) _app.externalLauncher().openUrl(QUrl(QString::fromUtf8(hyperlink.uri.c_str())));
}

void TerminalSession::onConfigReload()
{
    // reloadAllSessions() fans this out to EVERY session, including background tabs/split panes whose
    // display was detached on the last tab switch (_display == nullptr) — the same null-_display crash
    // class the action handlers (e.g. setFontSize) guard against. With a display, hop onto its (GUI)
    // thread as before. With none, there is no render thread to marshal onto, and reloadConfigWithProfile
    // is itself display-safe (activateProfile guards its one _display use), so run it directly rather than
    // skip it — otherwise a background tab would keep serving the pre-reload config until it is
    // reactivated.
    if (_display != nullptr)
        _display->post([this]() { reloadConfigWithProfile(_profileName); });
    else
        reloadConfigWithProfile(_profileName);

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
