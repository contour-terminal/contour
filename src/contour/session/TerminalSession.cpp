// SPDX-License-Identifier: Apache-2.0
#include <contour/ContourGuiApp.hpp>
#include <contour/config/Actions.hpp>
#include <contour/display/TerminalDisplay.hpp>
#include <contour/geometry/CellRectangle.hpp>
#include <contour/input/KeyMapping.hpp>
#include <contour/input/Logging.hpp>
#include <contour/input/MouseMapping.hpp>
#include <contour/platform/Clipboard.hpp>
#include <contour/platform/ExternalLauncher.hpp>
#include <contour/platform/QtInvoke.hpp>
#include <contour/platform/SpeechSynthesizer.hpp>
#include <contour/session/Logging.hpp>
#include <contour/session/SpawnCommand.hpp>
#include <contour/session/TerminalSession.hpp>

#include <vtbackend/core/FileUrl.hpp>
#include <vtbackend/input/MatchModes.hpp>
#include <vtbackend/input/vi/HintModeHandler.hpp>
#include <vtbackend/input/vi/ViCommands.hpp>
#include <vtbackend/screen/Terminal.hpp>

#include <vtpty/Process.hpp>
#include <vtpty/Pty.hpp>
#include <vtpty/SshSession.hpp>

#include <crispy/Assert.hpp>
#include <crispy/StackTrace.hpp>
#include <crispy/Utils.hpp>

#include <QtCore/QDeadlineTimer>
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
#include <cstddef>
#include <filesystem>
#include <format>
#include <fstream>
#include <limits>
#include <ranges>
#include <regex>
#include <span>

#ifdef __OpenBSD__
    #include <pthread_np.h>
    #define pthread_setname_np pthread_set_name_np
#elifndef _WIN32
    #include <pthread.h>
#endif

#ifndef _MSC_VER
    #include <csignal>

    #include <unistd.h>
#endif

#ifdef _MSC_VER
    #define __PRETTY_FUNCTION__ __FUNCDNAME__
#endif

using std::chrono::steady_clock;
using namespace std;
using namespace vtbackend;

namespace fs = std::filesystem;

namespace contour::session
{

namespace
{

    /// This machine's /etc/machine-id, or empty where there is none.
    ///
    /// Read once at session construction rather than on every question: it cannot change while the
    /// process runs, and the callers that need it are on the GUI path. Absent on Windows and on a system
    /// without systemd, where the host-name comparison carries the locality test alone.
    [[nodiscard]] std::string readLocalMachineId()
    {
        auto file = std::ifstream { "/etc/machine-id" };
        if (!file)
            return {};
        auto value = std::string {};
        std::getline(file, value);
        // Trimmed, because trailing whitespace would make every comparison against a wire value fail.
        return std::string { crispy::trimRight(value) };
    }
    string unhandledExceptionMessage(string_view const& where, exception const& e)
    {
        return std::format("{}: Unhandled exception caught ({}). {}", where, typeid(e).name(), e.what());
    }

    void setThreadName(char const* name)
    {
#ifdef __APPLE__
        pthread_setname_np(name);
#elifndef _WIN32
        pthread_setname_np(pthread_self(), name);
#endif
    }

#ifdef __linux__
    /// Connects @p action to @p signal, running it on the first emission naming @p identifier and
    /// breaking the connection there.
    ///
    /// Deliberately NOT Qt::SingleShotConnection: that breaks the connection on the first EMISSION
    /// of the signal, whichever notification it was about -- so with two notifications outstanding,
    /// the one that closes (or is clicked) first silently retires the OTHER one's handler, and the
    /// report it was waiting for is never sent. Several notifications are live at once routinely:
    /// OSC 99 identifies them precisely so an application can have more than one.
    ///
    /// @param notifier   Whose signal to connect to.
    /// @param context    Whose lifetime the connection is bound to.
    /// @param signal     The signal, whose first parameter is the identifier emitted.
    /// @param identifier The notification identifier this connection is about.
    /// @param action     What to run, taking the signal's remaining parameters.
    template <typename... Args, typename Action>
    void connectOnceMatching(platform::Notifier* notifier,
                             TerminalSession* context,
                             void (platform::Notifier::*signal)(QString, Args...),
                             std::string identifier,
                             Action action)
    {
        // Held indirectly because the handler must be able to break its own connection, which does
        // not exist yet when the handler is built.
        auto const connection = std::make_shared<QMetaObject::Connection>();
        *connection =
            QObject::connect(notifier,
                             signal,
                             context,
                             [identifier = std::move(identifier), connection, action = std::move(action)](
                                 QString const& emitted, Args... args) {
                                 if (emitted.toStdString() != identifier)
                                     return;

                                 QObject::disconnect(*connection);
                                 action(args...);
                             });
    }
#endif

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

    string normalizeCrlf(QString text)
    {
#ifndef _WIN32
        return text.replace("\r\n", "\n").replace("\r", "\n").toUtf8().toStdString();
#else
        return text.toUtf8().toStdString();
#endif
    }

    string stripIf(string input, bool shouldStrip)
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
        // The emulation half comes from the shared table, so a GUI-hosted session and a
        // daemon-hosted one can never disagree on what the terminal IS; only the
        // presentation fields below are the GUI's own.
        auto settings = config::emulationSettings(config, profile);

        // A new tab/split inherits the live window's running grid; a brand-new window keeps the
        // profile's configured terminalSize, which the shared table already applied. Overridden here
        // so the terminal is BORN at the right size, not just corrected once a display attaches
        // (which never happens for a background tab).
        if (initialPageSize)
            settings.pageSize = *initialPageSize;

        // Focus is granted, never assumed: TerminalSessionManager::setFocusedSession is the sole
        // authority, and it only focus-OUTs the session that previously held focus. A session born
        // focused (a background tab, a non-active split pane, any tab of a window that does not own
        // focus) would therefore stay "focused" forever -- rendering an active cursor and withholding
        // the DECSET 1004 focus-out its application is due.
        settings.focused = false;
        settings.ptyBufferObjectSize = config.ptyBufferObjectSize.value();
        settings.ptyReadBufferSize = config.ptyReadBufferSize.value();
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
        settings.mouseProtocolBypassModifiers = config.bypassMouseProtocolModifiers.value();
        settings.statusDisplayType = profile.statusLine.value().initialType;
        settings.statusDisplayPosition = profile.statusLine.value().position;
        settings.indicatorStatusLine.left = profile.statusLine.value().indicator.left;
        settings.indicatorStatusLine.middle = profile.statusLine.value().indicator.middle;
        settings.indicatorStatusLine.right = profile.statusLine.value().indicator.right;
        settings.tabNamingMode = [&]() {
            // try to find Tab section in one of the status line segments

            std::string segment;
            if (profile.statusLine.value().indicator.left.contains("Tabs"))
            {
                segment = profile.statusLine.value().indicator.left;
            }
            else if (profile.statusLine.value().indicator.middle.contains("Tabs"))
            {
                segment = profile.statusLine.value().indicator.middle;
            }
            else if (profile.statusLine.value().indicator.right.contains("Tabs"))
            {
                segment = profile.statusLine.value().indicator.right;
            }

            // check if indexing is defined
            if (segment.contains("Indexing="))
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
        settings.highlightDoubleClickedWord = profile.highlightDoubleClickedWord.value();
        settings.highlightTimeout = profile.highlightTimeout.value();

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

      protected:
        void run() override
        {
            sessionLog()("ExitWatcherThread: Started.");
            _session.terminal().device().waitForClosed();
            sessionLog()("ExitWatcherThread: Terminal device closed.");
            platform::postToObject(&_session, [&]() { _session.onClosed(); });
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
                                 std::optional<vtpty::Process::ExecInfo> launchedCommand,
                                 std::unique_ptr<platform::Notifier> notifier):
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
    _localHostName { QHostInfo::localHostName().toStdString() },
    _localMachineId { readLocalMachineId() },
    _accumulatedPixelScroll {},
    _accumulatedAngleScroll {},
    _hyperlinkHover { _localHostName },
    _terminal { *this,
                app.processEnvironment(),
                std::move(pty),
                createSettingsFromConfig(_config, _profile, _currentColorPreference, initialPageSize),
                std::chrono::steady_clock::now() },
    _exitWatcherThread { std::make_unique<ExitWatcherThread>(*this) },
    // _config is declared before _desktopNotifier, so reading it here is well-defined. The delay is
    // fixed for the notifier's life: it decides how a backend that cannot observe a close behaves,
    // which is configuration, not state.
    _desktopNotifier { notifier ? std::move(notifier)
                                : platform::makeDesktopNotifier(_config.notificationCloseTimeout.value()) }
{
    if (app.liveConfig())
    {
        sessionLog()("Enable live configuration reloading of file {}.", _config.configFile.generic_string());
        _configFileChangeWatcher = make_unique<QFileSystemWatcher>();
        _configFileChangeWatcher->addPath(QString::fromStdString(_config.configFile.generic_string()));
        connect(_configFileChangeWatcher.get(),
                SIGNAL(fileChanged(QString const&)),
                this,
                SLOT(onConfigReload()));
    }
    _musicalNotesBuffer.reserve(16);

    // A tally walks the whole scrollback, so it must never run on the render path. Output that keeps
    // arriving only re-arms this; the tally runs once the terminal goes quiet for a beat. Explicit
    // actions (typing, stepping, switching case) re-tally immediately instead of waiting for it.
    _searchTallyTimer.setSingleShot(true);
    _searchTallyTimer.setInterval(std::chrono::milliseconds { 250 });
    connect(&_searchTallyTimer, &QTimer::timeout, this, [this]() {
        // Cleared before the tally, so output arriving DURING it still schedules the next one.
        _searchTallyPostPending.clear(std::memory_order_release);
        refreshSearchStatus();
    });

    _profile = *_config.profile(_profileName); // XXX do it again. but we've to be more efficient here
    configureTerminal();
}

TerminalSession::~TerminalSession()
{
    sessionLog()("Destroying terminal session.");
    _terminating = true;

    // ~TerminalDisplay detaches in this direction, but nothing did it in the other: a session destroyed
    // while a display still names it left that display's _session dangling — and dangling is non-null, so
    // the render-thread frame path's guards pass it straight through to terminal(). releaseSession()
    // fences that frame out first.
    //
    // DEFENSIVE: today's shutdown destroys the QML engine (and every display) before the session manager,
    // so production should reach here with _display already null. Made an invariant rather than an
    // assumption, since the guards cannot tell the difference. Pinned session-first in the tests.
    if (_display != nullptr)
        _display->releaseSession();

    // Close the device first: that is what makes ExitWatcherThread's waitForClosed() return, so the
    // watcher ends by itself. terminate() alone could not do this job. It only REQUESTS termination and
    // does not wait, so the QThread was destroyed while its thread was still running -- Qt says as much
    // ("QThread: Destroyed while thread is still running") and the live thread then reads a freed
    // QThread. It is also unsafe on its own terms: it cuts the thread at an arbitrary instruction, and
    // every waitForClosed() that blocks on a condition variable (SshSession's, the test mocks') holds
    // that device's mutex while it waits, which would then never be unlocked.
    if (!_terminal.device().isClosed())
        _terminal.device().close();
    _terminal.device().wakeupReader();

    // Bounded, because ConPty::waitForClosed() only notices the close on its next one-second poll.
    // terminate() stays as the last resort, and is now followed by the wait() it always needed.
    // wait() on a thread that was never started returns immediately, so the never-started case (a
    // session whose device failed to start) costs nothing.
    if (!_exitWatcherThread->wait(QDeadlineTimer { std::chrono::seconds { 3 } }))
    {
        _exitWatcherThread->terminate();
        _exitWatcherThread->wait();
    }

    if (_screenUpdateThread)
        _screenUpdateThread->join();
}

void TerminalSession::detachDisplay(DisplaySurface& display)
{
    sessionLog()("Detaching display from session.");
    Require(_display == &display);
    _display = nullptr;
}

display::ForcedFontDpiProvider* TerminalSession::forcedFontDpiProvider() noexcept
{
    return _app.forcedFontDpiProvider();
}

input::KeyboardLayout const& TerminalSession::keyboardLayout() const noexcept
{
    return _app.keyboardLayout();
}

void TerminalSession::attachDisplay(DisplaySurface& newDisplay)
{
    sessionLog()("Attaching session to display of {} device pixels.", newDisplay.pixelSize());

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

    // Start the freshly-attached display with clean cursor-move coalescing state. This heals the rare
    // case where the previous display was torn down after a cursorPositionChanged() post was queued but
    // before it drained (dropping the clear): without this, the stale-set flag would make every future
    // cursorPositionChanged() early-return, freezing IME rectangle tracking and the a11y caret.
    _cursorMovedPostPending.clear(std::memory_order_release);

    // A closed session has nothing to resize: its PTY is gone. Closing a pane makes QML re-run the
    // Loader binding for the surviving panes, which re-enters setSession() -> attachDisplay() while
    // onClosed() is still unwinding, so this path really is reached after the PTY has been closed.
    // Skipping the resize keeps a dead session from pushing a geometry change down to it; ConPty
    // additionally refuses the resize on its own side, since attachDisplay() is not the only caller.
    if (!isClosed())
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

    // And likewise the pointer shape: an `OSC 22` that arrived while no display was attached has
    // nowhere to go at the time, so the new display starts on its own default and the application's
    // shape is lost for the rest of the session. setDefaultCursor() applies the remembered shape if
    // there is one and falls back to the screen-type default if there is not, which is exactly what
    // a freshly attached display wants.
    setDefaultCursor();

    // And likewise the scrollbar's travel: screenUpdated() only publishes it while a display is
    // attached, so a pane that accumulated scrollback in the background carries whatever count was
    // current when its display went away -- zero, for one that never had one. Nothing else republishes
    // until the next screen update, which for an idle shell may be never, leaving the freshly bound
    // scrollbar sized for a history it cannot see.
    announceScrollableLineCount(
        crispy::locked(_terminal, [this] { return _terminal.viewport().scrollableLineCount(); }));

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
    if (_screenUpdateThread)
        return;

    sessionLog()("Starting terminal session.");

    // A device that will not start is an expected, recoverable outcome, and vtpty reports it as one.
    // The catch is the backstop for what a signature cannot cover — std::bad_alloc, a throw from
    // inside a platform call — because start() is reached from a Qt event handler (see
    // TerminalDisplay::setSession), and an exception escaping it abandons a half-constructed
    // display mid-mutation. That was the crash of issue #1711; nothing may leave this function.
    auto const started = [this]() -> vtpty::StartResult {
        try
        {
            return _terminal.device().start();
        }
        catch (std::exception const& e)
        {
            return std::unexpected(
                vtpty::StartFailure { .error = vtpty::StartError::SpawnFailed, .detail = e.what() });
        }
    }();

    if (!started)
    {
        reportDeviceStartFailure(started.error());
        return; // No device, so no read loop and no exit watcher to start.
    }

    // The device came up, but not necessarily as asked: the spawn may have had to drop the inherited
    // working directory or fall back to the login shell. Say so before the child's own output arrives.
    if (!started->diagnostic.empty())
    {
        sessionLog()("Device started with a diagnostic: {}", started->diagnostic);
        writeNotice(std::array { started->diagnostic });
    }

    _screenUpdateThread = make_unique<std::thread>(bind(&TerminalSession::mainLoop, this));
    _exitWatcherThread->start(QThread::LowPriority);
}

void TerminalSession::writeNotice(std::span<std::string const> lines)
{
    // Same highlighting as onClosed()'s early-exit notice, so every message this terminal speaks in
    // its own voice — rather than relaying the child's — looks the same to the user.
    auto constexpr SGR = "\033[1;38:2::255:255:255m\033[48:2::255:0:0m"sv;
    auto constexpr EL = "\033[K"sv;
    for (auto const& text: lines)
        _terminal.writeToScreen(std::format("\r\n{}{}{}", SGR, EL, text));
    _terminal.writeToScreen("\r\n");
}

void TerminalSession::reportDeviceStartFailure(vtpty::StartFailure const& failure)
{
    sessionLog()("Failed to start terminal device: {}", failure);

    // Where the user is looking, and carrying the platform's own reason rather than a bare "it
    // failed" — this is the parent-side equivalent of the diagnostic the POSIX child writes onto the
    // pty before it exits (see Process_unix.cpp).
    writeNotice(std::array {
        std::format("{}", failure), "The terminal could not be started."s, "Press any key to close."s });

    // Close the device so the session is coherently dead rather than idling on a device that never
    // came up, and arm the early-exit notice so the pane is dismissed by the paths that already
    // handle a shell that died right after starting: the acknowledging key press in sendKeyEvent(),
    // or terminate()'s already-closed branch when the user closes the tab instead. Without this the
    // pane would be a zombie — nothing to prune it, since the exit watcher never started.
    if (!_terminal.device().isClosed())
        _terminal.device().close();
    auto const _ = std::scoped_lock { _onClosedMutex };
    _terminatedAndWaitingForKeyPress = true;
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
void TerminalSession::announce(QString const& message, QAccessible::AnnouncementPoliteness politeness)
{
    if (!_config.accessibilityAnnouncements.value())
        return;
    _announcer->announce(message, politeness);
}

void TerminalSession::bell()
{
    emit onBell(_profile.bell.value().volume);

    if (_profile.bell.value().alert)
        emit onAlert();

    // A bell has no object whose state changed, so nothing reaches a screen reader unless it is said.
    // Runs on the TERMINAL thread, possibly with the state mutex held -- the announcer posts, and this
    // message is a literal, so nothing here reads the terminal.
    announce(QObject::tr("Bell"));
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

    // Emphatically NOT under Terminal's state mutex, however much the fold projection below would like
    // it: this callback is raised BOTH with that mutex held and without it. Terminal::setMode() ends a
    // synchronized update (DECRST 2026) by calling synchronizedOutput(), which refreshes the render
    // buffer on the already-locked path and then raises this -- mid-parse, on the parser thread, with
    // the mutex held. The mutex is a plain non-recursive std::mutex, so taking it here self-deadlocks on
    // the very sequence neovim, tmux and every other modern TUI ends each frame with. @see
    // Events::progressChanged for the same contract, stated.
    //
    // Not while the viewport is parked on something the user went looking for. The Vi-mode test used
    // to be the whole guard, and it worked only because starting a search forced Normal mode; the bar
    // deliberately switches no mode, so a search now runs with the mode still Insert. Both
    // Terminal::search()/searchReverse() and ViCommands::moveCursorTo() reveal their match and then
    // raise screenUpdated(), so without this the very next statement scrolled the viewport straight
    // back to the bottom -- no match living in the scrollback could be shown at all, and the ones that
    // could were yanked away by the next byte of output.
    //
    // Two ways to be parked, not one. The bar being open is the obvious one. The other is F3, which
    // the default bindings gate on the Search match mode -- "a pattern is set", which OUTLIVES the bar
    // (@see searchBarClosed) -- so stepping through matches with the bar shut is a supported flow that
    // runs in Insert mode with the bar's flag clear. @see _searchMatchRevealed for how it un-parks.
    auto const viewportScrolled = _terminal.viewport().scrolled();
    if (!viewportScrolled)
        _searchMatchRevealed.store(false, std::memory_order_relaxed);

    if (_profile.history.value().autoScrollOnUpdate && viewportScrolled
        && _terminal.inputHandler().mode() == ViMode::Insert
        && !_isSearchBarOpen.load(std::memory_order_relaxed)
        && !_searchMatchRevealed.load(std::memory_order_relaxed))
        _terminal.viewport().scrollToBottom();

    auto const scrollable = _terminal.viewport().scrollableLineCount();

    // New output can add or remove matches, so the find bar's count goes stale. Re-tallying walks the
    // whole scrollback, so it is neither done here nor once per frame: this only re-arms a timer on
    // the GUI thread, and only while the bar is open. Posted rather than called, for the reason
    // stated above -- this runs on the parser thread, sometimes with the state mutex held.
    if (_isSearchBarOpen.load(std::memory_order_relaxed)
        && !_searchTallyPostPending.test_and_set(std::memory_order_acq_rel))
    {
        QMetaObject::invokeMethod(
            this,
            // The flag is cleared where the timer FIRES, not here, so exactly one post is made per
            // 250 ms window. Clearing it here instead let every following screen update post again
            // while the timer was still running -- one queued call per PTY read chunk, which under
            // heavy output is thousands a second.
            [this]() {
                // Re-checked here: the bar can close between this post and its delivery, and starting
                // the timer then would fire a tally 250 ms later for a bar that is gone.
                if (_isSearchBarOpen.load(std::memory_order_relaxed))
                    _searchTallyTimer.start();
                else
                    _searchTallyPostPending.clear(std::memory_order_release);
            },
            Qt::QueuedConnection);
    }

    if (terminal().hasInput())
        _display->post(bind(&TerminalSession::flushInput, this));

    // The scrollable count rather than the history depth, so that folding a block -- which moves that
    // count without touching the depth -- resizes the scrollbar too.
    announceScrollableLineCount(scrollable);

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

config::Permission TerminalSession::configuredPermissionFor(GuardedRole role) const
{
    auto const& permissions = _profile.permissions.value();
    switch (role)
    {
        case GuardedRole::ChangeFont: return permissions.changeFont;
        case GuardedRole::CaptureBuffer: return permissions.captureBuffer;
        case GuardedRole::ShowHostWritableStatusLine: return permissions.displayHostWritableStatusLine;
        // BigPaste is gated on payload size, not on configuration; what the gate adds for it is the
        // answer the user asked to be remembered for the session.
        case GuardedRole::BigPaste: return config::Permission::Ask;
    }
    return config::Permission::Ask;
}

void TerminalSession::postPermissionRequest(GuardedRole role)
{
    if (!_display)
        return;

    // The hop to the GUI thread is load-bearing, not decoration. These requests arrive on the terminal
    // thread (Terminal::Events hooks, raised with the terminal's state mutex held), while the gate's
    // Allow branch runs the role's executor synchronously -- work that belongs on the GUI thread, like
    // every other guarded role's executor. The configured permission is read inside the lambda, on the
    // GUI thread, because a config reload may replace the profile under us.
    _display->post([this, role]() { requestPermission(configuredPermissionFor(role), role); });
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
    // A display-less session (a background split/tab pane) has no GUI queue to decide on, and the client
    // is owed an answer either way -- so refuse it here rather than dropping it silently.
    //
    // Deliberately WITHOUT scoped_lock { _terminal }: this is a Terminal::Events hook, raised from
    // parseFragmentChunked() while writeToScreen() holds the terminal's state mutex, and that mutex is a
    // plain non-recursive std::mutex -- taking it here self-deadlocks the parser thread. The explicit
    // flush is needed for the same reason the branch exists: screenUpdated() returns early without a
    // display, so nothing else would ever push this reply to the PTY.
    if (!_display)
    {
        _terminal.primaryScreen().replyCaptureBufferEnd();
        flushInput();
        return;
    }

    {
        auto const _ = std::scoped_lock { _pendingBufferCaptureMutex };
        _pendingBufferCaptures.emplace_back(CaptureBufferRequest { .lines = lines, .logical = logical });
    }

    postPermissionRequest(GuardedRole::CaptureBuffer);
}

void TerminalSession::executePendingBufferCapture(bool allow, bool remember)
{
    if (remember)
        _rememberedPermissions[GuardedRole::CaptureBuffer] = allow;

    // One verdict answers every request outstanding at this moment: the user was asked once, about
    // capturing this buffer, and each request still owes its own reply.
    auto requests = std::vector<CaptureBufferRequest> {};
    {
        auto const _ = std::scoped_lock { _pendingBufferCaptureMutex };
        requests = std::exchange(_pendingBufferCaptures, {});
    }

    if (requests.empty())
        return;

    // Both the grid walk and the reply touch terminal state from the GUI thread, so they take the state
    // mutex like every other GUI-thread access in this file. The parser thread held it while raising the
    // Events hook and released it with that chunk; by now it may well be inside the next one.
    auto const l = scoped_lock { _terminal };

    for (auto const& request: requests)
    {
        if (allow)
            _terminal.primaryScreen().captureBuffer(request.lines, request.logical);
        else
            // A refusal is still a reply. The protocol cannot express "no reply is coming", so a client
            // reading until the empty terminating chunk would otherwise block forever -- which is what
            // `capture_buffer: deny` would mean if it just returned here.
            _terminal.primaryScreen().replyCaptureBufferEnd();
    }

    sessionLog()("requestCaptureBuffer: Answered {} request(s) with {}. Waking up I/O thread.",
                 requests.size(),
                 allow ? "allow" : "deny");
    flushInput();
}

void TerminalSession::requestShowHostWritableStatusLine()
{
    postPermissionRequest(GuardedRole::ShowHostWritableStatusLine);
}

void TerminalSession::executeShowHostWritableStatusLine(bool allow, bool remember)
{
    if (remember)
        _rememberedPermissions[GuardedRole::ShowHostWritableStatusLine] = allow;

    if (!allow)
        return;

    _terminal.setStatusDisplay(vtbackend::StatusDisplayType::HostWritable);
    sessionLog()("Host-writable status line shown. Waking up I/O thread.");
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

    postPermissionRequest(GuardedRole::ChangeFont);
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
        newFonts.size = text::FontSize { spec.size };

    if (!spec.regular.empty())
        newFonts.regular = text::FontDescription::parse(spec.regular);

    auto const styledFont = [&](string_view font) -> text::FontDescription {
        // if a styled font is "auto" then infer froom regular font"
        if (font == "auto"sv)
            return currentFonts.regular;
        else
            return text::FontDescription::parse(font);
    };

    if (!spec.bold.empty())
        newFonts.bold = styledFont(spec.bold);

    if (!spec.italic.empty())
        newFonts.italic = styledFont(spec.italic);

    if (!spec.boldItalic.empty())
        newFonts.boldItalic = styledFont(spec.boldItalic);

    if (!spec.emoji.empty() && spec.emoji != "auto"sv)
        newFonts.emoji = text::FontDescription::parse(spec.emoji);

    // Persist as THIS session's own, the way setFontSize() does: setSession() and configureDisplay() both
    // re-seed from profile().fonts, so leaving the pre-OSC-50 value made the approved font revert on the
    // next scene-graph re-creation or tab rebind. Unlike setFontSize() this does not wait for the apply —
    // it may legitimately be deferred (see setFonts()), and the profile is what the deferral resumes from.
    _profile.fonts.value() = newFonts;

    // The answer can arrive long after the request, so the pane may be gone. Consuming the pending change
    // above is deliberate: an approval for a display that no longer exists is finished, not queued.
    if (_display == nullptr)
        return;

    _display->setFonts(newFonts);
}

void TerminalSession::setPointerShape(std::string_view cssName)
{
    // An EMPTY name means the terminal is back to its own default: the application has stopped
    // imposing a shape, by resetting it, by popping the stack empty, or through RIS. Forgetting the
    // remembered shape is what lets the screen-type defaults below apply again -- caching the name
    // the stack happens to hold at its bottom would pin the I-beam for the rest of the session and
    // the alternate screen would never get its arrow back.
    if (cssName.empty())
    {
        _applicationPointerShape = std::nullopt;
        if (_display)
            _display->post([this]() { setDefaultCursor(); });
        return;
    }

    // OSC 22 speaks CSS pointer names; the display speaks its own enum. The mapping is the whole
    // binding between the two, and only names vtbackend advertises as supported can arrive here.
    auto const shape = [cssName]() -> std::optional<input::MouseCursorShape> {
        if (cssName == "text")
            return input::MouseCursorShape::IBeam;
        if (cssName == "pointer")
            return input::MouseCursorShape::PointingHand;
        if (cssName == "default")
            return input::MouseCursorShape::Arrow;
        if (cssName == "none")
            return input::MouseCursorShape::Hidden;
        return std::nullopt;
    }();

    if (!shape)
        return;

    // Remember it before the display gets a look in: without a display attached there is nothing to
    // post to, and dropping the shape there is what lost an `OSC 22` sent to a backgrounded pane.
    // attachDisplay() applies whatever is remembered here once a display arrives. Returning early on
    // a null display -- as the reset path above is careful not to do -- would drop the shape in
    // precisely the case this remembering exists to serve.
    _applicationPointerShape = shape;

    // The event arrives on the parser thread; the cursor belongs to the GUI thread.
    if (_display)
        _display->post([display = _display, shape = *shape]() { display->setMouseCursorShape(shape); });
}

void TerminalSession::copyToClipboard(std::string_view data)
{
    if (!_display)
        return;

    _display->post([data = string(data)]() { platform::copyToClipboard(data); });
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

    openExternally(url, "document", fileOrUrl);
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
#ifdef __linux__
    auto notification = vtbackend::DesktopNotification {};
    notification.title = std::string(title);
    notification.body = std::string(content);
    _desktopNotifier->notify(notification);
#else
    emit showNotification(QString::fromUtf8(title.data(), static_cast<int>(title.size())),
                          QString::fromUtf8(content.data(), static_cast<int>(content.size())));
#endif
}

void TerminalSession::showDesktopNotification(vtbackend::DesktopNotification const& notification)
{
    // Said as well as shown: a desktop notification is a visual popup, and an assistive client that
    // does not read the desktop's own notifications would otherwise miss it entirely.
    announce(notification.title.empty()
                 ? QString::fromStdString(notification.body)
                 : QStringLiteral("%1: %2").arg(QString::fromStdString(notification.title),
                                                QString::fromStdString(notification.body)));

#ifdef __linux__
    _desktopNotifier->notify(notification);

    auto const identifier = notification.identifier;

    if (notification.closeEventRequested)
        connectOnceMatching(_desktopNotifier.get(),
                            this,
                            &platform::Notifier::notificationClosed,
                            identifier,
                            [this, identifier](uint /*reason*/, vtbackend::CloseReport report) {
                                _terminal.reply("\033]{}\033\\",
                                                vtbackend::buildOSC99CloseResponse(identifier, report));
                                _terminal.desktopNotificationManager().removeActiveNotification(identifier);
                            });

    if (notification.reportOnActivation)
        connectOnceMatching(
            _desktopNotifier.get(), this, &platform::Notifier::actionInvoked, identifier, [this, identifier] {
                _terminal.reply("\033]99;i={}:p=activated;\033\\", identifier);
            });

    if (notification.focusOnActivation)
        connectOnceMatching(
            _desktopNotifier.get(), this, &platform::Notifier::actionInvoked, identifier, [this] {
                focusTerminalWindow();
            });
#else
    // On non-Linux platforms, fall back to the simple notification mechanism.
    emit showNotification(QString::fromStdString(notification.title),
                          QString::fromStdString(notification.body));
#endif
}

void TerminalSession::discardDesktopNotification(std::string_view identifier)
{
    // No #ifdef: where there is nothing to withdraw a notification from, the notifier is a
    // NullNotifier and this is a no-op.
    _desktopNotifier->close(std::string(identifier));
}

void TerminalSession::focusTerminalWindow()
{
    if (_display)
    {
        _display->post([display = _display]() {
            if (auto* window = display->window())
            {
                window->raise();
                window->requestActivate();
            }
        });
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
#ifdef VTPTY_LIBSSH2
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

    // A lost daemon connection reaches us as a closed device, exactly like a shell exiting —
    // and for a pane whose session lives in the daemon, "the shell terminated" is simply false:
    // the shell is still running, on the other side of a socket that went away. Reported as an
    // early exit it also sends whoever reads it hunting through shell startup, which is where
    // this notice came from. Age is irrelevant here: a connection lost after hours is still
    // worth saying out loud, so this is checked before the early-exit window.
    if (_transportLost && !_terminationRequested)
    {
        sessionLog()("Daemon connection lost after {} seconds; the hosted shell is unaffected.",
                     diff.count());
        // Like the early-exit notice below, this deliberately does not emit sessionClosed: pruning
        // the pane would destroy the very screen the message is shown on. The pane stays alive until
        // the acknowledging key press.
        writeNotice(array { "Disconnected from the Contour daemon."s,
                            "Your sessions are still running — reattach with: contour client"s });
        _terminatedAndWaitingForKeyPress = true;
        return;
    }

    if (diff < _app.earlyExitThreshold() && !_terminationRequested)
    {
        // Deliberately do NOT emit sessionClosed here: removeSession() would prune this pane from the
        // model and tear down its QML item, destroying the very screen the message below is shown on
        // (and leaving an empty window behind that nothing can close). The pane stays fully alive until
        // the acknowledging key press (see sendKeyEvent/sendCharEvent), which prunes and closes then.
        writeNotice(
            array { "Shell terminated too quickly."s, "The window will not be closed automatically."s });
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

        // Read the clipboard ONCE: text() is a synchronous round-trip to whichever process owns the
        // clipboard and drags the entire payload across, so the guards, the immediate path and the
        // deferred one all work from this one copy.
        auto const text = clipboard->text(QClipboard::Clipboard);

        // 1 MB hard limit
        if (text.size() > static_cast<qsizetype>(1024 * 1024))
        {
            sessionLog()("Clipboard contains huge text. Ignoring.");
            // A display-less session (background pane, headless test) has nowhere to toast the
            // rejection; the paste is ignored either way.
            if (_display)
                _display->post(
                    [this]() { emit showNotification(tr("Paste"), tr("The pasted content is too large.")); });
            return;
        }

        // Normalize before the soft limit, not after: what the user is asked to approve, and what is
        // measured, must be the bytes that would actually reach the application.
        auto pasteText = stripIf(normalizeCrlf(text), strip);

        // 512 KB soft limit to ask user for permission
        if (pasteText.size() > static_cast<size_t>(1024 * 512))
        {
            _pendingBigPaste = BigPasteRequest { .text = std::move(pasteText), .count = count };
            sessionLog()("Clipboard contains huge text. Requesting permission.");
            // BigPaste is the one guarded role with no configuration field, so the gate resolves it as
            // Ask; what it adds over a bare emit is the remembered per-session answer, which only
            // requestPermission() ever reads. No post() here, unlike requestCaptureBuffer(): both entry
            // points (the PasteClipboard action and Vi's paste commands) are GUI-thread input paths, as
            // the QClipboard dereference above already requires.
            requestPermission(configuredPermissionFor(GuardedRole::BigPaste), GuardedRole::BigPaste);
            return;
        }

        sessionLog()("Size of text: {}", pasteText.size());
        if (pasteText.empty())
            sessionLog()("Clipboard does not contain text.");
        else
            sendPasteRepeatedly(pasteText, count);
    }
    else
        sessionLog()("Could not access clipboard.");
}

void TerminalSession::sendPasteRepeatedly(std::string const& text, unsigned count)
{
    if (count == 1)
    {
        terminal().sendPaste(string_view { text });
        return;
    }

    auto repeated = string {};
    repeated.reserve(text.size() * count);
    for ([[maybe_unused]] auto const _: std::views::iota(0U, count))
        repeated += text;
    terminal().sendPaste(string_view { repeated });
}

void TerminalSession::applyPendingPaste(bool allow, bool remember)
{
    sessionLog()("applyPendingPaste: allow={}, remember={}", allow, remember);
    if (remember)
        _rememberedPermissions[GuardedRole::BigPaste] = allow;

    if (!_pendingBigPaste)
        return;

    // Take the request out either way: a refused paste is finished, not still pending.
    auto const request = std::exchange(_pendingBigPaste, std::nullopt).value();

    if (!allow)
        return;

    // The stored text is the one that was measured and approved -- deliberately not a fresh read of the
    // clipboard, whose contents may have changed while the dialog was up. @see BigPasteRequest.
    sendPasteRepeatedly(request.text, request.count);
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

void TerminalSession::addToAccumulatedScroll(crispy::Point pixelDelta,
                                             crispy::Point angleDelta,
                                             vtbackend::ScrollPhase phase,
                                             bool platformInverted) noexcept
{
    // Drop incidental sideways drift before it can accumulate into a whole column step. Filtering here
    // rather than at the binding lookup keeps it in ONE place and fixes the mouse-reporting path too: an
    // application receiving phantom horizontal wheel reports during a vertical scroll is equally wrong.
    if (!_horizontalWheelGesture.acceptsHorizontal(pixelDelta, angleDelta, phase, platformInverted))
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
        auto const pixelStepSize = crispy::Point {
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
    // Bound to Main.qml's window `title:`, so Qt re-evaluates this on the GUI thread whenever the title
    // changes — concurrently with the parser thread's OSC 0/2 writer (setWindowTitle assigns _windowTitle
    // under _stateMutex). Read the locked copy via resolvedWindowTitle() rather than the lock-free
    // windowTitle() reference, which would tear (or use-after-free on a string reallocation) against that
    // writer. Native tabs/splits make these GUI-thread title reads far more frequent.
    auto const windowTitle = resolvedWindowTitle();
#ifndef NDEBUG
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
    platform::postToObject(this, [this]() {
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
    platform::postToObject(
        this, [this, color, id = modelSessionId()]() { _manager->setTabColorForSession(id, color); });
}

void TerminalSession::resetWindowFrameColor()
{
    // DECAC item 2 with no colors, or a hard reset (RIS): clear the tab color. Same threading
    // constraint as setWindowFrameColor() above.
    platform::postToObject(this, [this, id = modelSessionId()]() { _manager->resetTabColorForSession(id); });
}

void TerminalSession::progressChanged(vtbackend::Progress /*progress*/)
{
    // OSC 9;4. Same threading constraint as setWindowFrameColor() above: this runs on the parser
    // thread with Terminal::_stateMutex held, and the tab-strip refresh reads the terminal back
    // through resolvedProgress(), which takes that same non-recursive mutex. Post it to the GUI
    // thread; postToObject targets `this`, so Qt cancels the queued call if the session dies first.
    // The value itself is not carried across: the refresh reads it back from the terminal, so the
    // strip cannot paint a state older than the one in effect when the posted call finally runs.
    platform::postToObject(this,
                           [this, id = modelSessionId()]() { _manager->refreshTabProgressForSession(id); });
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

void TerminalSession::searchPromptRequested()
{
    // Reached with the terminal's state mutex HELD (SearchReverse takes it, and `/` arrives inside
    // sendCharEvent). A queued connection is therefore mandatory: the find bar reads terminal state as
    // it opens, and a direct call would re-enter that non-recursive mutex on this very thread.
    QMetaObject::invokeMethod(this, [this]() { emit searchBarRequested(); }, Qt::QueuedConnection);
}

// {{{ Find bar
void TerminalSession::searchBarOpened()
{
    // BEFORE the lock, not after it: moveNormalModeCursorTo() below reaches ViCommands::moveCursorTo(),
    // which reveals the cursor and then raises screenUpdated() -- and screenUpdated() consults this
    // very flag to decide whether it may scroll the viewport back to the bottom. Set afterwards, the
    // open sequence would be the one update that slipped past the guard it is turning on.
    _isSearchBarOpen.store(true, std::memory_order_relaxed);

    {
        auto const l = scoped_lock { _terminal };

        // The search runs from the NORMAL-mode cursor, which ViCommands only syncs to the real screen
        // cursor on an Insert -> Normal transition -- and this bar deliberately makes no such
        // transition. Without this, a search started from Insert mode begins wherever the vi cursor
        // was last left (usually the top of the page) and cannot see anything below it: the old
        // startSearchExternally() got the sync for free by switching mode, purely to reveal the
        // status line.
        //
        // Not when the place the last search left the cursor is still on screen. The bar is HANDED
        // OVER to whichever session the pane is showing (SearchBar.qml's _syncOpenState), so this
        // runs again on every tab switch -- and on a session sitting on a match the sync would throw
        // that place away: the vi cursor is what "3 of 27" counts up to, and what the focused
        // highlight is drawn from, so moving it to the bottom of the page turns that into
        // "27 matches" and (through ViCommands::moveCursorTo) scrolls the match off screen. Keyed on
        // what is VISIBLE rather than merely on a pattern being set, so a stale pattern the user has
        // long since scrolled away from still starts the next search from where they are looking.
        auto const restingOnAVisibleMatch =
            !_terminal.search().pattern.empty()
            && _terminal.viewport().isLineVisible(_terminal.normalModeCursorPosition().line);
        if (!restingOnAVisibleMatch && _terminal.inputHandler().mode() == vtbackend::ViMode::Insert)
            _terminal.moveNormalModeCursorTo(_terminal.currentScreen().cursor().position);
    }

    refreshSearchStatus();
}

void TerminalSession::searchBarClosed()
{
    // Only the bar's own visibility is dropped here -- NOT the pattern, which deliberately outlives
    // it so F3 keeps stepping and the matches stay lit. What this ends is the tallying: nothing
    // displays a count now, and walking the scrollback every 250 ms to compute one is the exact cost
    // the flag exists to avoid. Told by the bar rather than inferred, because only the bar knows.
    _isSearchBarOpen.store(false, std::memory_order_relaxed);
    _searchTallyTimer.stop();

    // Cleared BECAUSE the timer was just stopped: the flag is otherwise only cleared where the timer
    // fires, so stopping it mid-window would strand the flag set for the rest of the session -- and a
    // stranded flag means screenUpdated() never posts again, so re-opening the bar would show a count
    // that no longer follows new output. Same hazard attachDisplay() heals for the cursor-moved flag.
    _searchTallyPostPending.clear(std::memory_order_release);
}

void TerminalSession::searchCleared()
{
    // Nothing left to be parked ON. F3 is gated on the Search match mode, which is "a pattern is set",
    // so with the pattern gone there is no longer any way to step to another match -- and a flag left
    // standing here would keep the viewport off the bottom for the rest of the session, since the only
    // other thing that clears it is the user scrolling back down by hand. @see _searchMatchRevealed.
    _searchMatchRevealed.store(false, std::memory_order_relaxed);

    // Reached from the parser thread and from under Terminal's state mutex (Vi mode changes clear the
    // search mid-input), so both halves are posted rather than done here.
    if (auto* display = _display)
        display->scheduleRedraw();

    QMetaObject::invokeMethod(this, &TerminalSession::refreshSearchStatus, Qt::QueuedConnection);
}

void TerminalSession::setSearchPattern(QString const& pattern)
{
    {
        auto const l = scoped_lock { _terminal };

        auto text = pattern.toStdU32String();
        if (text.empty())
        {
            _terminal.clearSearch();
            _searchStatus = {};
        }
        else
        {
            // Two steps rather than searchReverse(text, pos): that overload short-circuits when the
            // term is unchanged and hands back the position it was given, which is indistinguishable
            // from a match. Here the result has to actually mean "found something".
            (void) _terminal.setNewSearchTerm(std::move(text), vtbackend::SearchOrigin::Typed);

            // Backwards from the cursor, which is what the terminal's search has always done and what
            // makes an incremental search land on the nearest match behind you rather than jumping to
            // the top of the scrollback on every keystroke.
            auto const match = _terminal.searchReverse(_terminal.normalModeCursorPosition());
            if (match)
            {
                // The cursor MUST follow the match, exactly as the vi `/` flow did. Three things read
                // it: the tally's ordinal (so "3 of 27" has a 3 at all), the renderer's choice of
                // searchHighlightFocused (so one match reads as the current one), and Enter, which
                // steps from wherever the cursor is. Dropping the location broke all three.
                _terminal.moveNormalModeCursorTo(*match);
            }

            // NOT tallied here. Counting walks the whole grid, and this runs on every keystroke, on
            // the GUI thread, with the terminal locked -- on a deep scrollback that is the difference
            // between a find bar and a stutter. What is knowable WITHOUT the walk goes in right away:
            // whether anything matched, which is what tints the field and enables the buttons. The
            // count follows on the debounce below.
            _searchStatus = describeSearchCounting(match ? MatchPresence::Some : MatchPresence::None);
        }

        _searchPatternText = pattern;
        _searchSummary = renderSearchSummary(_searchStatus);
    }

    emit searchStateChanged();
    scheduleSearchTally();
}

void TerminalSession::scheduleSearchTally()
{
    if (!_isSearchBarOpen.load(std::memory_order_relaxed))
        return;

    // start() RESTARTS a running timer, which is what makes this a debounce: a fast typist pays one
    // tally when they pause, not one per character. The output path deliberately does NOT restart it
    // (see screenUpdated), so a busy terminal still refreshes the count at a steady cadence.
    _searchTallyTimer.start();
}

void TerminalSession::cycleSearchCaseSensitivity()
{
    {
        auto const l = scoped_lock { _terminal };

        // Unconditional: nextSearchCase() never answers with the policy it was given, so
        // setSearchCaseSensitivity() cannot report "already in effect" here and testing it would only
        // suggest it could.
        (void) _terminal.setSearchCaseSensitivity(nextSearchCase(_terminal.search().caseSensitivity));

        // Only the RE-SEARCH is skipped without a pattern, never the refresh and the notify below: the
        // glyph and the tooltip changed regardless, and the bar has to be told.
        if (!_terminal.search().pattern.empty())
        {
            // Re-run in place: the old match may not be a match under the new policy.
            if (auto const match = _terminal.searchReverse(_terminal.normalModeCursorPosition()))
                _terminal.moveNormalModeCursorTo(*match);
        }

        refreshSearchStatusLocked();
    }
    emit searchStateChanged();
}

void TerminalSession::searchNext()
{
    stepSearch(SearchWrapEdge::Top);
}

void TerminalSession::searchPrevious()
{
    stepSearch(SearchWrapEdge::Bottom);
}

void TerminalSession::stepSearch(SearchWrapEdge edge)
{
    auto const before = [this]() {
        auto const l = scoped_lock { _terminal };
        return _terminal.normalModeCursorPosition();
    }();

    if (edge == SearchWrapEdge::Top)
        (void) (*this)(actions::FocusNextSearchMatch {});
    else
        (void) (*this)(actions::FocusPreviousSearchMatch {});

    // Wrapped on "did the cursor actually move", not on the action's return value: at the very first
    // cell of the scrollback searchPrevMatch() cannot step back any further, so it re-finds the match
    // the cursor already stands on and reports success -- which left the button live and inert, the
    // one thing MatchNavigation promises not to do.
    auto const after = [this]() {
        auto const l = scoped_lock { _terminal };
        return _terminal.normalModeCursorPosition();
    }();

    if (after == before)
        wrapSearchTo(edge);

    refreshSearchStatus();
}

void TerminalSession::wrapSearchTo(SearchWrapEdge edge)
{
    // The find bar wraps; Terminal::searchNextMatch does not, and must not -- vi's `n` stopping at
    // the end of the scrollback is its documented behaviour. So the wrap lives here, where "the last
    // match" means "start over" rather than "there is nothing more". Without it, the bar's own
    // MatchNavigation promise would be a lie at both ends of the list.
    auto const l = scoped_lock { _terminal };
    if (_terminal.search().pattern.empty())
        return;

    auto const& screen = _terminal.currentScreen();
    auto const from =
        edge == SearchWrapEdge::Top
            ? CellLocation { .line = -boxed_cast<vtbackend::LineOffset>(screen.historyLineCount()),
                             .column = vtbackend::ColumnOffset(0) }
            : CellLocation { .line = boxed_cast<vtbackend::LineOffset>(_terminal.pageSize().lines) - 1,
                             .column =
                                 boxed_cast<vtbackend::ColumnOffset>(_terminal.pageSize().columns) - 1 };

    auto const match = edge == SearchWrapEdge::Top ? _terminal.search(from) : _terminal.searchReverse(from);
    if (match)
        _terminal.moveNormalModeCursorTo(*match);
}

void TerminalSession::refreshSearchStatus()
{
    {
        auto const l = scoped_lock { _terminal };
        refreshSearchStatusLocked();
    }
    emit searchStateChanged();
}

QString TerminalSession::renderSearchSummary(SearchStatus const& status) const
{
    switch (status.outcome)
    {
        case SearchOutcome::Idle: return {};
        case SearchOutcome::NoMatch: return tr("No results");
        case SearchOutcome::Matched: break;
    }

    // The count is still being walked. Saying so beats both a stale number and a blank that fills in
    // a moment later looking like a glitch.
    if (status.readiness == CountReadiness::Counting)
        return tr("…");

    auto const capped = status.tally.exactness == vtbackend::TallyExactness::Capped;

    // Standing on a match: "3 of 27". SearchMatchTally documents when ordinal can be 0.
    if (status.tally.ordinal != 0)
        return capped ? tr("%1 of %2+").arg(status.tally.ordinal).arg(status.tally.total)
                      : tr("%1 of %2").arg(status.tally.ordinal).arg(status.tally.total);

    // Not on one, so there is no first number to report -- only how many there are. Through tr()'s
    // plural form rather than an English ternary, so a language with other plural rules can say it.
    return capped ? tr("%1+ matches").arg(status.tally.total)
                  : tr("%n match(es)", nullptr, static_cast<int>(status.tally.total));
}

QString TerminalSession::renderSearchCaseTooltip(vtbackend::SearchCaseSensitivity mode) const
{
    switch (mode)
    {
        case vtbackend::SearchCaseSensitivity::Smart:
            return tr("Match case: smart — exact only when the term has a capital");
        case vtbackend::SearchCaseSensitivity::Sensitive: return tr("Match case: on");
        case vtbackend::SearchCaseSensitivity::Insensitive: return tr("Match case: off");
    }
    crispy::unreachable();
}

void TerminalSession::refreshSearchStatusLocked()
{
    auto const& search = _terminal.search();
    _searchCase = describeSearchCase(search.caseSensitivity);
    _searchCaseTooltip = renderSearchCaseTooltip(search.caseSensitivity);
    // Cached rather than read through on demand: searchPattern() is a Q_PROPERTY whose NOTIFY is
    // searchStateChanged, so a binding woken by that signal reads it -- and if reading it took the
    // terminal lock, every emit made while holding that lock would deadlock. Caching it here makes
    // the whole property group lock-free to read, so the emit is safe wherever it happens.
    _searchPatternText = QString::fromStdU32String(search.pattern);

    // Skipped entirely while the bar is closed: nothing displays the answer, and walking the
    // scrollback to compute one nobody reads is the cost this guard exists to avoid. The LAST status
    // is left standing rather than replaced by an idle one, so nothing can publish a wrong label --
    // except when the pattern itself is gone, where keeping it IS the wrong label: the pattern above
    // has just been published as empty, and a leftover "3 of 27" beside it is a disagreement anyone
    // rebinding or re-opening the bar would see before the first tally lands.
    if (!_isSearchBarOpen.load(std::memory_order_relaxed))
    {
        if (search.pattern.empty())
        {
            _searchStatus = {};
            _searchSummary = renderSearchSummary(_searchStatus);
        }
        return;
    }

    _searchStatus = describeSearch(search.pattern,
                                   search.pattern.empty()
                                       ? vtbackend::SearchMatchTally {}
                                       : _terminal.tallySearchMatches(_terminal.normalModeCursorPosition()));
    _searchSummary = renderSearchSummary(_searchStatus);
}
// }}}

void TerminalSession::onScrollOffsetChanged(vtbackend::ScrollOffset value)
{
    // The hovered link is recomputed on mouse MOVEMENT only, so scrolling slides a different line under
    // a stationary pointer with nothing noticing. Hide rather than recompute: the terminal's own hover
    // state is stale for the same reason, so re-deriving from it here would only show a different wrong
    // answer. Browsers likewise drop a tooltip on scroll.
    clearHyperlinkHover();
    emit scrollOffsetChanged(unbox(value));
}
// }}}
// {{{ Input Events

static void handleAction(auto const& actions, auto eventType, auto callback)
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
    input::inputLog()("Key {} event received: {} {}", eventType, modifiers, key);

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
        _display->setMouseCursorShape(input::MouseCursorShape::Hidden);

    if (eventType != KeyboardEventType::Release)
    {
        // Runs @p actions (when there are any) through the repeat filter, reporting whether anything
        // actually ran. Shared by the two lookups below so they differ only in which table they consult.
        auto const runBinding = [&](auto const* actions) {
            if (actions == nullptr)
                return false;
            auto executionCount = 0;
            handleAction(actions, eventType, [&](auto const& actions) {
                executionCount = executeAllActions(actions);
            });
            return executionCount > 0;
        };

        // Key bindings match on the chord: a latched lock key must not change which shortcut fires.
        if (runBinding(config::apply(
                _config.inputMappings.value().keyMappings, key, modifiers.chord, matchModeFlags())))
            return;

        // The user's mappings did not claim this key, so fall back to the built-in ones. They are
        // consulted second on purpose: an explicit binding in the user's config always wins, because it
        // is found first (see builtinFallbackKeyMappings for why a plain default could not reach an
        // existing user at all).
        if (runBinding(config::applyBuiltinFallback(_config, key, modifiers.chord, matchModeFlags())))
            return;
    }
    terminal().sendKeyEvent(key, modifiers, eventType, now);
}

void TerminalSession::sendCharEvent(char32_t value,
                                    KeyIdentity keyIdentity,
                                    KeyboardModifiers modifiers,
                                    KeyboardEventType eventType,
                                    Timestamp now)
{
    input::inputLog()("Character {} event received: {} '{}'",
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
        _display->setMouseCursorShape(input::MouseCursorShape::Hidden);

    if (eventType != KeyboardEventType::Release)
    {
        // Find a char binding for this key (ignored while editing the search prompt).
        auto const& charMappings = _config.inputMappings.value().charMappings;
        auto const flags = matchModeFlags();

        // Bindings are stored folded (see YAMLConfigReader::parseKeyOrChar), because WHICH case a
        // letter arrives in is decided by the route contour::sendKeyEvent() took rather than by the
        // user — and on macOS the option-as-Alt branch even lets CapsLock decide, which would
        // otherwise let a latched lock key pick the shortcut. Match in the same folded form.
        // NB: only the *lookup* is folded; `value` itself is forwarded verbatim to the terminal
        // below, so what the application receives is unchanged.
        auto const folded = config::foldedBindingCodepoint(value);
        auto const* actions = config::apply(charMappings, folded, modifiers.chord, flags);

        // A shortcut written with the base key label (e.g. `Ctrl+Shift+,`) is stored under the base
        // character, but Qt delivers a Shift+punctuation chord as the *shifted* symbol ('<' here). When
        // the direct lookup misses and Shift is held, retry under the un-shifted base so the binding
        // fires as the user intended. The two normalizations are disjoint — unshiftedCodepoint()
        // rewrites only digits and punctuation, foldedBindingCodepoint() only ASCII letters — so they
        // compose in either order.
        if (actions == nullptr && modifiers.chord.test(vtbackend::Modifier::Shift))
            if (auto const base = input::unshiftedCodepoint(folded); base != folded)
                actions = config::apply(charMappings, base, modifiers.chord, flags);

        if (actions != nullptr)
        {
            auto executionCount = 0;
            handleAction(actions, eventType, [&](auto const& actions) {
                executionCount = executeAllActions(actions);
            });
            if (executionCount > 0)
                return;
        }
    }
    terminal().sendCharEvent(value, keyIdentity, modifiers, eventType, now);
}

void TerminalSession::sendMousePressEvent(Modifiers modifiers,
                                          MouseButton button,
                                          CellLocation pos,
                                          PixelCoordinate pixelPosition)
{
    auto const uiHandledHint = false;
    input::inputLog()("Mouse press received: {} {}\n", modifiers, button);

    terminal().tick(steady_clock::now());

    if (crispy::locked(_terminal, [&]() {
            return _terminal.sendMousePressEvent(modifiers, button, pos, pixelPosition, uiHandledHint);
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
    if (button == MouseButton::WheelLeft || button == MouseButton::WheelRight)
    {
        if (!_horizontalWheelGesture.consumeNavigationStep())
            return;

        // A swipe navigates the way the FINGERS went, a wheel tilt the way it says. Resolved here and
        // nowhere earlier: the same WheelLeft/WheelRight also feeds mouse REPORTING, where an
        // application doing its own horizontal scrolling must keep receiving the literal direction.
        //
        // Note this sits AFTER the user's own mouseMappings were consulted, so an explicitly bound
        // WheelLeft keeps meaning the button it names. That is deliberate: following the finger is a
        // property of the built-in tab-switching default, not of the button.
        button = input::horizontalNavigationButton(button == MouseButton::WheelRight,
                                                   _horizontalWheelGesture.usesNaturalDirection());
    }

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

    // The hovered link is read inside the lock the move already takes, rather than asked for again
    // afterwards. tryGetHoveringHyperlink() re-queries the screen and needs the same non-recursive
    // mutex, so a second acquisition here would be one more chance to deadlock, and a callback fired
    // from vtbackend's own hover-state update would run WITH the lock held and self-deadlock outright.
    auto const hoveredUri = crispy::locked(_terminal, [&]() -> std::string {
        _terminal.sendMouseMoveEvent(modifiers, pos, pixelPosition, UiHandledHint);
        auto const link = _terminal.tryGetHoveringHyperlink();
        return link ? link->uri : std::string {};
    });

    // The cursor shape lives on the display; a display-less session (background pane, headless
    // test) has no cursor to change.
    //
    // Normally only on a cell CHANGE, since that is the only thing that can change the answer -- but
    // the gutter changes the shape behind this decision's back, and leaving it lands the pointer on the
    // cell it came from. The flag is that invalidation: without it the gutter's pointing hand survived
    // over the grid until the pointer happened to cross into another cell.
    if ((pos != _currentMousePosition || _isPointerShapeStale) && _display != nullptr)
    {
        _isPointerShapeStale = false;
        _currentMousePosition = pos;
        if (terminal().isMouseHoveringHyperlink()
            || (modifiers.contains(vtbackend::Modifier::Control) && terminal().localPathAtMousePosition()))
            _display->setMouseCursorShape(input::MouseCursorShape::PointingHand);
        else
            setDefaultCursor();

        updateHyperlinkHover(hoveredUri, pos);
    }
}

ConsumedByGutter TerminalSession::sendGutterHoverEvent(std::optional<vtbackend::LineOffset> gridLine)
{
    // A display-less session has no cursor to change and no gutter to be over.
    if (_display == nullptr)
        return ConsumedByGutter::No;

    // The overwhelmingly common case -- the pointer is over the GRID, and with folding switched off
    // it is the only case. Answered before taking the terminal lock, because this runs on every
    // pointer motion and the ordinary move path is about to take that same lock itself.
    if (!gridLine)
    {
        clearGutterHover();
        return ConsumedByGutter::No;
    }

    _gutterHovered = true;
    auto const overFold = crispy::locked(_terminal, [&] {
        _terminal.setGutterHoverLine(gridLine);
        return _terminal.foldContaining(*gridLine).has_value();
    });

    // Over the gutter but beside a row no fold reaches: still not the grid, so the event is consumed
    // either way -- only the shape differs, saying whether there is anything here to click.
    if (overFold)
        _display->setMouseCursorShape(input::MouseCursorShape::PointingHand);
    else
        setDefaultCursor();

    // Leaving the grid ends any hyperlink hover the pointer left behind on its way out.
    clearHyperlinkHover();
    return ConsumedByGutter::Yes;
}

ConsumedByGutter TerminalSession::sendGutterPressEvent(std::optional<vtbackend::LineOffset> gridLine,
                                                       vtbackend::MouseButton button)
{
    // Only a left click acts on a fold; every other button belongs to the application, gutter or not.
    if (button != vtbackend::MouseButton::Left || !gridLine)
        return ConsumedByGutter::No;

    // Through the one gate every folding action passes: it takes the lock, honours the folding setting
    // and republishes the scrollable count, none of which a click on the column wants to restate.
    if (!withFolding([&](auto& terminal) { return terminal.toggleFoldContaining(*gridLine); }))
        return ConsumedByGutter::No;

    // Remembered here rather than by the caller, so the two halves of the handshake cannot drift: a
    // release reported for a press the child never saw leaves it holding a button down that was never
    // pressed.
    _gutterClickPending = true;
    return ConsumedByGutter::Yes;
}

ConsumedByGutter TerminalSession::sendGutterReleaseEvent()
{
    if (!_gutterClickPending)
        return ConsumedByGutter::No;

    _gutterClickPending = false;
    return ConsumedByGutter::Yes;
}

void TerminalSession::updateHyperlinkHover(std::string_view uri, vtbackend::CellLocation cell)
{
    if (!_config.hyperlinkHoverTooltip.value())
        return;

    auto constexpr MaxTooltipLength = size_t { 100 };
    auto const change = _hyperlinkHover.update(uri, cell, MaxTooltipLength);
    if (!change.changed)
        return;

    _hyperlinkTooltipText = QString::fromStdString(change.text);
    // The anchor is only meaningful while something is shown, and computing it needs a display.
    if (!change.text.empty() && _display != nullptr)
        _hyperlinkTooltipAnchor = geometry::cellRectangle(_display->gridMetrics().pageMargin,
                                                          _display->cellSize(),
                                                          change.anchor,
                                                          1,
                                                          _display->devicePixelRatio());
    emit hyperlinkHoverChanged();
}

void TerminalSession::clearGutterHover()
{
    // Nothing to clear, and -- the point of the flag -- nothing to lock for: this is the answer on
    // every pointer motion over the grid, which is nearly all of them.
    if (!_gutterHovered)
        return;

    _gutterHovered = false;

    // The gutter set the shape; only the grid can decide what replaces it, and it decides that per
    // CELL. Leaving the gutter usually lands on the cell the pointer left from, so say the shape is
    // stale rather than guess at it here -- the very next move then re-decides, hyperlink and all.
    _isPointerShapeStale = true;

    crispy::locked(_terminal, [&] { _terminal.setGutterHoverLine(std::nullopt); });
}

void TerminalSession::announceScrollableLineCount(vtbackend::LineCount scrollable)
{
    // One exchange rather than a load and a store: two threads can reach this at once (the parser
    // thread from screenUpdated(), the GUI thread from a folding action), and only the one that
    // actually moved the value should announce it.
    if (_lastHistoryLineCount.exchange(unbox(scrollable), std::memory_order_relaxed) == unbox(scrollable))
        return;

    emit historyLineCountChanged(unbox(scrollable));
}

void TerminalSession::onPointerLeft()
{
    clearHyperlinkHover();
    clearGutterHover();
    setDefaultCursor();
}

void TerminalSession::clearHyperlinkHover()
{
    if (!_hyperlinkHover.clear().changed)
        return;

    _hyperlinkTooltipText.clear();
    emit hyperlinkHoverChanged();
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
        _audio = std::make_unique<platform::Audio>();

    auto range = params.range();
    _musicalNotesBuffer.clear();
    _musicalNotesBuffer.insert(_musicalNotesBuffer.begin(), range.begin() + 2, range.end());
    emit _audio->play(params.at(0), params.at(1), _musicalNotesBuffer);
}

void TerminalSession::cursorPositionChanged()
{
    // NOTHING about the terminal is read here, and no Qt input-method call is made here either.
    // refreshRenderBuffer() reaches this callback on the terminal or render thread — with the state
    // mutex ALREADY HELD on one of its two paths, and that mutex is a plain non-recursive std::mutex.
    // QInputMethod::update() is not fire-and-forget: the Wayland backend synchronously re-queries
    // inputMethodQuery(), which reads the grid — from here that read would race the terminal thread
    // (or self-deadlock once it locks). Both the IME rectangle update and the accessibility caret
    // report therefore run on the GUI thread, via TerminalDisplay::reportCursorMoved().

    // With no display attached (the detach->attach gap while a tab moves to another window or a split
    // collapses) there is nothing to post to — and therefore nothing to coalesce. Snapshot and bail
    // BEFORE latching the flag: the flag is cleared only inside the post below, so latching it here
    // without scheduling that post would strand it set forever, and every later cursorPositionChanged()
    // would early-return for the session's life — silently killing IME rectangle tracking and the
    // accessibility caret. (attachDisplay() also clears the flag, healing the rarer case where the
    // display is torn down after the post is queued but before it drains.)
    auto* const display = _display;
    if (display == nullptr)
        return;

    // This fires once per frame AND twice a second from the cursor blink (the render buffer's cursor is
    // simply absent while blinked off), so collapse repeats to at most one pending post.
    if (_cursorMovedPostPending.test_and_set(std::memory_order_acq_rel))
        return;

    display->post([this]() {
        _cursorMovedPostPending.clear(std::memory_order_release);
        if (auto* target = _display)
            target->reportCursorMoved();
    });
}
// }}}
// {{{ Actions
bool TerminalSession::operator()(actions::CancelSelection)
{
    // Locked: the selection is torn down by the parser thread too (a buffer scroll re-extends or drops
    // it). See ViNormalMode for the rationale.
    auto const l = scoped_lock { _terminal };

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

command::ContextMenuState TerminalSession::contextMenuState()
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

    // One lock for the whole snapshot. The parser thread mutates the grid concurrently, so a menu that
    // asked the terminal a fresh question per row would be reading a moving target — and a QML binding
    // that reached into the terminal on its own schedule would be a plain data race.
    return crispy::locked(_terminal, [&]() {
        auto const block = terminal().lastCommandBlock();
        auto const hyperlink = terminal().tryGetHoveringHyperlink();

        // The grid line under the pointer, when a fold reaches it. _currentMousePosition is in MAIN-PAGE
        // rows -- which is the space translateScreenToGridLine() speaks, so it is fed in unadjusted --
        // and was last written by the move that necessarily preceded this right-click, so it still names
        // the clicked cell. Adding mainPageTopRow() here would reintroduce the off-by-a-status-line the
        // gutter hit-test already had (@see gutterLineAt).
        auto const foldLine = [&]() -> std::optional<vtbackend::LineOffset> {
            if (!_config.folding.value().enabled)
                return std::nullopt;
            auto const line = terminal().viewport().translateScreenToGridLine(_currentMousePosition.line);
            return terminal().foldContaining(line) ? std::optional { line } : std::nullopt;
        }();

        return command::ContextMenuState {
            .hasSelection = terminal().selectionAvailable(),
            .clipboardHasText = clipboardHasText,
            // An empty block is no block. A shell that emits OSC 133;D at its very first prompt (tcsh
            // cannot guard against that in an alias) reports a command that never ran, with no text to
            // copy — offering the user three rows that would all yield nothing.
            .hasLastCommand = block.has_value() && !(block->prompt.empty() && block->output.empty()),
            // Only a working directory on this host can be opened by the local file manager, and the row
            // and the action behind it go through ONE resolver so they cannot disagree. A remote OSC 7
            // cwd grays it out, and so does an OSC 3008 cwd behind a container boundary or from another
            // machine -- which would otherwise open the HOST's directory of the same name, a false
            // positive OSC 7 never had, because a context cwd carries no host authority to test.
            // The LOCKED resolver: this whole lambda runs inside crispy::locked() above, and the
            // terminal's mutex is not recursive.
            .hasLocalWorkingDirectory =
                resolveWorkingDirectoryLocked(vtbackend::CwdPurpose::OpenLocally).has_value(),
            // Left for the window to fill in: whether this tab holds more than one pane is not something
            // a session knows about itself.
            .hasSplits = false,
            .inputProtected = !terminal().allowInput(),
            // A property of the build and the machine, asked once here so the menu itself stays a pure
            // function of this snapshot.
            .canSpeak = _app.speechSynthesizer().available(),
            // Taken now, while the pointer is still on the cell the user clicked. The rows built from this
            // carry the URI with them, because by the time one is picked the pointer has moved to the menu.
            .hyperlinkUnderCursor = hyperlink ? hyperlink->uri : std::string {},
            // Taken now for the same reason the hyperlink is: the row acts on the line that was
            // clicked, and the pointer will have left it by the time the row is picked.
            .foldLine = foldLine,
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
    // Deliberately NOT locked: Terminal::inspect() only forwards to the display, which queues a flag
    // for the render thread to service on its next frame. No terminal state is read synchronously.
    _terminal.inspect();
    return true;
}

bool TerminalSession::operator()(actions::CreateSelection const& customSelector)
{
    // Locked: word-wise selection scans grid cells and installs a selector. See ViNormalMode.
    auto const l = scoped_lock { _terminal };

    _terminal.triggerWordWiseSelectionWithCustomDelimiters(customSelector.delimiters);
    return true;
}

bool TerminalSession::operator()(actions::DecreaseFontSize)
{
    auto constexpr OnePt = text::FontSize { 1.0 };
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
    // Locked: searchNextMatch() walks the grid and the Vi cursor moves with it. See ViNormalMode.
    auto const l = scoped_lock { _terminal };

    // Set BEFORE the step, not after it: searchNextMatch() reveals the match and raises
    // screenUpdated() from inside, so the guard has to already be up by then. @see _searchMatchRevealed.
    auto const wasRevealed = _searchMatchRevealed.exchange(true, std::memory_order_relaxed);

    auto const nextPosition = _terminal.searchNextMatch(_terminal.normalModeCursorPosition());
    if (!nextPosition)
    {
        // RESTORED, not cleared: this step revealed nothing, but a previous one may well have -- F3
        // at the last match is exactly that case, and storing false there would un-park the very match
        // the user is standing on, so the next byte of output scrolls it away.
        _searchMatchRevealed.store(wasRevealed, std::memory_order_relaxed);
        return false;
    }
    _terminal.moveNormalModeCursorTo(nextPosition.value());
    _terminal.viewport().makeVisibleWithinSafeArea(nextPosition->line);
    // TODO why didn't the makeVisibleWithinSafeArea() call from inside jumpToNextMatch not work?
    return true;
}

bool TerminalSession::operator()(actions::FocusPreviousSearchMatch)
{
    // Locked for the same reason as FocusNextSearchMatch above, and parked for the same reason.
    auto const l = scoped_lock { _terminal };

    auto const wasRevealed = _searchMatchRevealed.exchange(true, std::memory_order_relaxed);

    auto const nextPosition = _terminal.searchPrevMatch(_terminal.normalModeCursorPosition());
    if (!nextPosition)
    {
        // Restored rather than cleared, for the reason FocusNextSearchMatch above states.
        _searchMatchRevealed.store(wasRevealed, std::memory_order_relaxed);
        return false;
    }
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
    sessionLog()("Activating hint mode with patterns: '{}', action: {}, scope: {}",
                 action.patterns,
                 action.hintAction,
                 action.scope);

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

    auto request = vtbackend::HintModeRequest {
        .patterns = std::move(patterns),
        .action = action.hintAction,
        .scope = action.scope,
        .scrollbackLimit = profile().hintScrollbackLines.value(),
    };
    crispy::locked(terminal(), [&]() { terminal().activateHintMode(std::move(request)); });
    return true;
}

bool TerminalSession::operator()(actions::IncreaseFontSize)
{
    auto constexpr OnePt = text::FontSize { 1.0 };
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
    // Locked: clears the search term and the match set the parser thread re-derives on new output.
    auto const l = scoped_lock { _terminal };

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

    // fromLocalFile(), not QUrl(path): a bare filesystem path is not a URL. On POSIX it parses as a
    // scheme-less relative reference and on Windows `C:\...` parses with scheme "c", so the desktop
    // was handed something no handler could claim. The two sibling openers here already do this.
    openExternally(QUrl::fromLocalFile(QString::fromStdString(_config.configFile.string())),
                   "configuration file",
                   _config.configFile.generic_string());

    return true;
}

bool TerminalSession::operator()(actions::OpenFileManager)
{
    // OSC 7 advertises the cwd as a file://HOST/PATH URL. Hand the raw URL to the file manager and its
    // host authority is read as a network share ("//fedora/home/..."), which does not exist locally.
    // Resolve it to a plain local path first, and only when it is on THIS host — a remote (SSH) cwd is
    // not openable here (its menu row is grayed out, but a keybinding could still reach this handler).
    // The same resolver the menu row's enablement uses, so the two can never disagree about whether a
    // directory is openable.
    auto const resolved = resolveWorkingDirectory(vtbackend::CwdPurpose::OpenLocally);
    if (!resolved)
    {
        // A remote (SSH) cwd, or one behind a container/VM boundary, cannot be opened in this host's
        // file manager. The context-menu row is grayed out, but a keybinding can still reach this
        // handler — so report why nothing happened rather than silently swallowing the request.
        errorLog()("Cannot open file manager: the working directory is not on the local host.");
        return true;
    }

    openExternally(QUrl::fromLocalFile(QString::fromStdString(resolved->path)), "folder", resolved->path);

    return true;
}

bool TerminalSession::operator()(actions::OpenSelection)
{
    crispy::locked(_terminal, [&]() {
        auto const selection = terminal().extractSelectionText();
        openExternally(QUrl(QString::fromStdString(selection)), "selection", selection);
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
    if (QClipboard const* clipboard = QGuiApplication::clipboard(); clipboard != nullptr)
    {
        string const text = normalizeCrlf(clipboard->text(QClipboard::Selection));

        // Locked around the terminal calls only -- the clipboard read above is Qt's and must not run
        // with the terminal lock held. See ViNormalMode for the rationale.
        auto const l = scoped_lock { _terminal };

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
    // Deliberately NOT locked: closing the device is precisely the mechanism that breaks the parser
    // thread out of its blocking read. Serializing it behind that thread's own lock is what we are
    // trying to avoid.
    terminal().device().close();
    // Unwind the event loop rather than calling exit(): the PTY reader thread may still be
    // running, and exit() would run static destructors underneath it.
    QCoreApplication::exit(EXIT_SUCCESS);
    return true;
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
        app().dumpStateAtExit().value_or(crispy::App::instance()->localStateDir())
        / fs::path(std::format("contour-screenshot-{:%Y-%m-%d-%H-%M-%S}.png", chrono::system_clock::now()));

    _display->setScreenshotOutput(savePath);
    sessionLog()("Saving screenshot to {}", savePath.string());

    // The log line and the toast are deliberately separate strings: one is for a developer reading a
    // log in English, the other is shown to the user and therefore goes through tr().
    auto const notice = tr("Saving screenshot to %1").arg(QString::fromStdString(savePath.string()));
    _display->post([this, notice]() { emit showNotification(tr("Screenshot"), notice); });
    return true;
}

bool TerminalSession::operator()(actions::CopyScreenshot)
{
    _display->setScreenshotOutput(std::monostate {});
    sessionLog()("Saving screenshot to clipboard");

    auto const notice = tr("Screenshot copied to the clipboard.");
    _display->post([this, notice]() { emit showNotification(tr("Screenshot"), notice); });

    return true;
}

void TerminalSession::smoothScrollUp(vtbackend::LineCount lineCount)
{
    // Locked here rather than in each of the six scroll actions that funnel through this helper: the
    // viewport offset and the smooth-scroll state are read and advanced by the parser thread too (a
    // buffer scroll shifts the viewport), so a GUI-thread scroll must not interleave with it. See
    // ViNormalMode for the full GUI-vs-parser-thread rationale.
    auto const l = scoped_lock { _terminal };

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
    // Locked for the same reason as smoothScrollUp() above.
    auto const l = scoped_lock { _terminal };

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

namespace
{
    /// The grid line the folding actions act on: where the user is looking, which is the top of the
    /// viewport, not where the shell's cursor happens to be. ToggleFold is a viewport action.
    ///
    /// Not noexcept: that translation goes through the fold projection, which is built lazily and
    /// allocates.
    [[nodiscard]] vtbackend::LineOffset foldAnchorLine(vtbackend::Terminal const& terminal)
    {
        return terminal.viewport().topLine();
    }
} // namespace

bool TerminalSession::withFolding(std::invocable<vtbackend::Terminal&> auto&& action, FoldingGate gate)
{
    // Locked: the actions walk the grid's marks and mutate fold state the render pass reads. See
    // ScrollMarkDown.
    auto const [handled, scrollable] =
        crispy::locked(_terminal, [&]() -> std::pair<bool, vtbackend::LineCount> {
            // One gate for every folding action rather than one per handler: a seventh action then
            // cannot be live behind a disabled setting by forgetting to check.
            auto const ran =
                (gate == FoldingGate::Always || _config.folding.value().enabled) && action(terminal());

            // Read under the same lock that just moved it: folding a block takes its rows out of the
            // scrollable range without touching the history depth, and this is the only moment that is
            // visible -- the property reporting that range may neither compute it nor take this lock.
            return { ran, _terminal.viewport().scrollableLineCount() };
        });

    // Outside the lock: the QML binding this wakes runs synchronously on this thread and reaches back
    // into the session. Announcing a count that did not move is a no-op, so the disabled case needs no
    // second channel to say so.
    announceScrollableLineCount(scrollable);
    return handled;
}

bool TerminalSession::operator()(actions::CollapseAllFolds)
{
    return withFolding([](auto& terminal) { return terminal.collapseAllFolds(); });
}

bool TerminalSession::operator()(actions::CollapseLastFold)
{
    return withFolding([](auto& terminal) { return terminal.collapseLastFold(); });
}

bool TerminalSession::operator()(actions::ExpandAllFolds)
{
    // Ungated, but still through withFolding(): turning the feature off must let a user undo what they
    // folded while it was on rather than stranding the output behind a disabled setting -- and the
    // rows this hands BACK to the scrollable range have to be announced just as the ones a collapse
    // takes away are, or the scrollbar keeps the travel it had while they were hidden.
    return withFolding([](auto& terminal) { return terminal.expandAllFolds(); }, FoldingGate::Always);
}

bool TerminalSession::operator()(actions::ToggleFold)
{
    return withFolding(
        [](auto& terminal) { return terminal.toggleFoldContaining(foldAnchorLine(terminal)); });
}

bool TerminalSession::operator()(actions::ToggleFoldAt action)
{
    // The line comes from the context menu, which captured where the user right-clicked -- so this
    // acts on the block they were pointing at, not on whatever the viewport happens to start with.
    return withFolding([line = action.line](auto& terminal) {
        return terminal.toggleFoldContaining(vtbackend::LineOffset(line));
    });
}

bool TerminalSession::operator()(actions::ToggleLastFold)
{
    return withFolding([](auto& terminal) { return terminal.toggleLastFold(); });
}

bool TerminalSession::operator()(actions::ScrollMarkDown)
{
    // Locked: scans the grid for marks and moves the viewport. See ViNormalMode.
    auto const l = scoped_lock { _terminal };

    terminal().viewport().scrollMarkDown();
    return true;
}

bool TerminalSession::operator()(actions::ScrollMarkUp)
{
    // Locked for the same reason as ScrollMarkDown above.
    auto const l = scoped_lock { _terminal };

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
    // Locked: mutates the smooth-scroll state and the viewport. See ViNormalMode.
    auto const l = scoped_lock { _terminal };

    // Snap immediately for ScrollToTop/Bottom (animating large distances is impractical).
    terminal().resetSmoothScroll();
    terminal().viewport().scrollToBottom();
    return true;
}

bool TerminalSession::operator()(actions::ScrollToTop)
{
    // Locked for the same reason as ScrollToBottom above.
    auto const l = scoped_lock { _terminal };

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
    // Unlocked, deliberately: requestSearchPrompt() only posts, and the bar reads terminal state from
    // its own searchBarOpened() once the post is delivered -- by which time any lock taken here would
    // long since have been released. Taking one would contend with the parser thread and protect
    // nothing.
    //
    // No vi-mode switch either. The old prompt forced Normal mode only so the status line carrying it
    // became visible; the find bar floats over the terminal, so the user keeps the mode they were in.
    _terminal.requestSearchPrompt();

    return true;
}

bool TerminalSession::operator()(actions::SendChars const& event)
{
    // Locked: sendRawInput() appends to the input generator and flushes it, and the parser thread
    // appends to that same generator whenever a sequence replies. See ViNormalMode.
    auto const l = scoped_lock { _terminal };

    // auto const now = steady_clock::now();
    // for (auto const ch: event.chars)
    //     terminal().sendCharPressEvent(static_cast<char32_t>(ch), vtbackend::Modifiers::None, now);
    terminal().sendRawInput(event.chars);
    return true;
}

bool TerminalSession::operator()(actions::ToggleAllKeyMaps)
{
    _allowKeyMappings = !_allowKeyMappings;
    input::inputLog()("{} key mappings.", _allowKeyMappings ? "Enabling" : "Disabling");

    // Locked: setStatusDisplay() resizes the page and reflows the grid, exactly as ViNormalMode does.
    auto const l = scoped_lock { _terminal };

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
    // Locked around the terminal calls only; announce() below is Qt accessibility and must not run
    // with the terminal lock held. See ViNormalMode for the rationale.
    auto const allowInput = [&]() {
        auto const l = scoped_lock { _terminal };
        terminal().setAllowInput(!terminal().allowInput());
        return terminal().allowInput();
    }();

    // Assertive: this changes what typing does, so a client mid-sentence should be interrupted rather
    // than tell the user about it after they have already typed into a terminal that ignored them.
    announce(allowInput ? QObject::tr("Editable") : QObject::tr("Read-only"),
             QAccessible::AnnouncementPoliteness::Assertive);
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
// The four Trace* actions below are deliberately NOT locked: Terminal::setExecutionMode() carries its
// own _breakMutex and condition variable precisely so the GUI thread can steer the parser thread's
// loop from outside. Taking _stateMutex here would add nothing and only widen the critical section.
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
    // Locked for the same reason ToggleStatusLine above is: switching Vi mode is not a local state
    // flip. ViCommands::modeChanged() pushes (resp. pops) the indicator status display, and that
    // resizes the page -- Terminal::setStatusDisplay -> resizeScreen -> applyPageSizeToMainDisplay ->
    // Grid::resize, which reallocates and reflows the grid the parser thread is writing into under
    // this very lock. Unlocked, a keystroke landing mid-parse pulls the grid out from under a live
    // write; the result is a torn grid or a dangling line iterator, i.e. a segmentation fault or a
    // verifyState() abort -- and Require() is not compiled out in release builds. That is #1495,
    // which needed a remote tmux only because it keeps the parser thread busy enough to hit the
    // window nearly every time.
    auto const l = scoped_lock { _terminal };

    if (terminal().inputHandler().mode() == ViMode::Insert)
        terminal().inputHandler().setMode(ViMode::Normal);
    else if (terminal().inputHandler().mode() == ViMode::Normal)
        terminal().inputHandler().setMode(ViMode::Insert);
    return true;
}

bool TerminalSession::operator()(actions::WriteScreen const& event)
{
    // Deliberately NOT locked here: Terminal::writeToScreen() takes the terminal lock itself, and
    // _stateMutex is non-recursive -- wrapping this would self-deadlock.
    terminal().writeToScreen(event.chars);
    return true;
}

bool TerminalSession::operator()(actions::CreateNewTab const& action)
{
    _manager->createNewTab(this, action.profileName);
    return true;
}

bool TerminalSession::operator()(actions::SpeakSelection)
{
    // Bounded so a selected build log becomes a readable excerpt rather than minutes of speech with no
    // way to skip ahead.
    auto constexpr MaxSpokenChars = size_t { 4000 };

    if (!_app.speechSynthesizer().available())
        return false;

    // Locked around the extraction only: it reads the selection and the grid cells behind it, both of
    // which the parser thread mutates. The say() below is Qt and must not hold the lock.
    auto const text = [&]() {
        auto const l = scoped_lock { _terminal };
        return platform::speakableText(terminal().extractSelectionText(), MaxSpokenChars);
    }();
    if (text.empty())
        return false;

    _app.speechSynthesizer().say(text);
    return true;
}

bool TerminalSession::operator()(actions::StopSpeaking)
{
    _app.speechSynthesizer().stop();
    return true;
}

bool TerminalSession::operator()(actions::CloseAllTabs)
{
    // Deliberately NOT actions::Quit: that one calls exit() straight from a Qt slot, with no teardown
    // and no PTY reaping. Closing the window runs the tested teardown instead, and with a single window
    // open closing it IS quitting.
    _manager->closeAllTabs(this);
    return true;
}

bool TerminalSession::operator()(actions::SetTabBarVisibility action)
{
    if (_display)
        _display->setTabBarVisibility(action.mode);
    return true;
}

bool TerminalSession::operator()(actions::SetTabBarPosition action)
{
    if (_display)
        _display->setTabBarPosition(action.position);
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
    // The user's explicit "end this" affordance: Destroy, so a daemon-hosted session is closed on
    // the daemon too rather than left running with no view.
    _manager->closeActivePane(/*acting*/ this, SessionEnd::Destroy);
    return true;
}

bool TerminalSession::operator()(actions::FocusPaneLeft)
{
    _manager->focusPane(vtworkspace::FocusDirection::Left, /*acting*/ this);
    return true;
}

bool TerminalSession::operator()(actions::FocusPaneRight)
{
    _manager->focusPane(vtworkspace::FocusDirection::Right, /*acting*/ this);
    return true;
}

bool TerminalSession::operator()(actions::FocusPaneUp)
{
    _manager->focusPane(vtworkspace::FocusDirection::Up, /*acting*/ this);
    return true;
}

bool TerminalSession::operator()(actions::FocusPaneDown)
{
    _manager->focusPane(vtworkspace::FocusDirection::Down, /*acting*/ this);
    return true;
}

namespace
{
    /// Translates an action-layer Direction (transport-agnostic) into the model's FocusDirection.
    /// @param direction The direction the user requested.
    /// @return The corresponding vtworkspace::FocusDirection.
    [[nodiscard]] constexpr vtworkspace::FocusDirection toFocusDirection(
        actions::Direction direction) noexcept
    {
        switch (direction)
        {
            case actions::Direction::Left: return vtworkspace::FocusDirection::Left;
            case actions::Direction::Right: return vtworkspace::FocusDirection::Right;
            case actions::Direction::Up: return vtworkspace::FocusDirection::Up;
            case actions::Direction::Down: return vtworkspace::FocusDirection::Down;
        }
        return vtworkspace::FocusDirection::Left;
    }
} // namespace

bool TerminalSession::operator()(actions::SwapPaneLeft)
{
    _manager->swapPane(vtworkspace::FocusDirection::Left, /*acting*/ this);
    return true;
}

bool TerminalSession::operator()(actions::SwapPaneRight)
{
    _manager->swapPane(vtworkspace::FocusDirection::Right, /*acting*/ this);
    return true;
}

bool TerminalSession::operator()(actions::SwapPaneUp)
{
    _manager->swapPane(vtworkspace::FocusDirection::Up, /*acting*/ this);
    return true;
}

bool TerminalSession::operator()(actions::SwapPaneDown)
{
    _manager->swapPane(vtworkspace::FocusDirection::Down, /*acting*/ this);
    return true;
}

bool TerminalSession::operator()(actions::MovePaneLeft)
{
    _manager->movePane(vtworkspace::FocusDirection::Left, /*acting*/ this);
    return true;
}

bool TerminalSession::operator()(actions::MovePaneRight)
{
    _manager->movePane(vtworkspace::FocusDirection::Right, /*acting*/ this);
    return true;
}

bool TerminalSession::operator()(actions::MovePaneUp)
{
    _manager->movePane(vtworkspace::FocusDirection::Up, /*acting*/ this);
    return true;
}

bool TerminalSession::operator()(actions::MovePaneDown)
{
    _manager->movePane(vtworkspace::FocusDirection::Down, /*acting*/ this);
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

    // An `OSC 22` shape outranks the screen-type default: the application asked for it and nothing
    // has withdrawn it. This is reached from every mouse move and every focus change, so without the
    // check a requested shape survived exactly one mouse move before reverting -- which looks from
    // the outside like OSC 22 not working at all. RIS withdraws the shape by asking for the default
    // one, and it also returns to the primary screen, whose default this already is.
    if (auto const applicationShape = _applicationPointerShape.load())
    {
        _display->setMouseCursorShape(*applicationShape);
        return;
    }

    using Type = vtbackend::ScreenType;
    switch (_terminal.screenType())
    {
        case Type::Primary: _display->setMouseCursorShape(input::MouseCursorShape::IBeam); break;
        case Type::Alternate: _display->setMouseCursorShape(input::MouseCursorShape::Arrow); break;
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

    input::inputLog()("Key mappings are currently disabled via ToggleAllKeyMaps input mapping action.");
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

std::optional<vtbackend::ResolvedWorkingDirectory> TerminalSession::resolveWorkingDirectory(
    vtbackend::CwdPurpose purpose) const
{
    // Resolved AND returned under the lock: the resolver's intermediate views point into the ancestry,
    // while ResolvedWorkingDirectory owns its path, so nothing escaping here outlives the guard.
    auto const lock = scoped_lock { _terminal };
    return resolveWorkingDirectoryLocked(purpose);
}

std::optional<vtbackend::ResolvedWorkingDirectory> TerminalSession::resolveWorkingDirectoryLocked(
    vtbackend::CwdPurpose purpose) const
{
    auto const osc7 = _terminal.currentWorkingDirectory();
    return vtbackend::resolveWorkingDirectory(_terminal.contexts(), osc7, localIdentity(), purpose);
}

std::string TerminalSession::workingDirectory() const
{
#ifndef _WIN32
    // /proc FIRST on POSIX, and deliberately: it reads the pty child's own cwd, so it is always a real
    // path on THIS machine and can never hand fork+exec a path from another filesystem namespace. It
    // is a genuine trade rather than a free win -- it names the LOGIN shell, so it does not follow a
    // subshell the way an OSC 3008 cwd would -- and this side of it is the conservative one.
    //
    // An OSC 3008 cwd is consulted only where /proc has nothing to say (see the Windows branch
    // below), which is why the display path resolves separately.
    if (auto const* ptyProcess = dynamic_cast<vtpty::Process const*>(&_terminal.device()))
        return ptyProcess->workingDirectory();
#else
    // On Windows there is no /proc, so the advertised cwd is all there is: the OSC 3008 ancestry
    // first, then OSC 7. Both are filtered for locality by the resolver -- an OSC 3008 cwd behind a
    // container or from another machine, and an OSC 7 cwd naming a remote host, are both refused.
    if (auto const resolved = resolveWorkingDirectory(vtbackend::CwdPurpose::Spawn))
    {
        // Existence is checked on top of that, and the reason is a crash rather than a nicety: an SSH
        // session advertises its REMOTE cwd, and handing a path that does not exist here to a new
        // tab's CreateProcess() fails, makes Process::start() throw, and -- being reached from a QML
        // `session:` binding write inside a Qt event handler -- aborts the whole process.
        std::error_code ec;
        if (std::filesystem::is_directory(fs::path(resolved->path), ec))
            return resolved->path;
    }
#endif
    return "."s;
}

std::string TerminalSession::displayWorkingDirectory() const
{
    // The OSC 3008 ancestry first, then OSC 7: both are the shell speaking, so both track a `cd` made
    // inside a full-screen application, and both can be right for a session that is not on this
    // machine. NOT filtered for locality, unlike workingDirectory() above -- a container's /app is
    // exactly what the user is looking at, and a path worth SHOWING need not be one a child could be
    // spawned in.
    if (auto const resolved = resolveWorkingDirectory(vtbackend::CwdPurpose::Display))
        return resolved->path;

    // Nothing reported (no shell integration, or not yet): fall back to where the session was started.
    // Unlike workingDirectory() this is NOT filtered for local existence — a path worth SHOWING need not
    // be one a child could be spawned in.
#ifndef _WIN32
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
        auto const command = buildSpawnTerminalCommand(
            _app.programPath(), _config.configFile.generic_string(), profileName, wd, _localHostName);
        if (auto const spawned = _app.externalLauncher().runDetached(command.program, command.arguments);
            !spawned)
            errorLog()("Could not spawn \"{}\": {}.",
                       command.program.toStdString(),
                       platform::describe(spawned.error()));
    }
    else
    {
        // Attach mode: author the window on the daemon (B4); its new-window layout push spawns and
        // binds the matching OS window. A local factory returns false and we open an ordinary window.
        if (_app.requestRemoteWindow())
            return;
        sessionLog()("spawning new in-process window");
        _app.config().profile(_profileName)->shell.value().workingDirectory = fs::path(wd);
        // The new window mints its own WindowController + first tab in Main.qml's Component.onCompleted,
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
    _terminal.setHistoryLimits(_profile.history.value().limits());
    _terminal.setMouseWheelScrollMultiplier(_profile.history.value().historyScrollMultiplier);
    _terminal.settings().autoScrollOnUpdate = _profile.history.value().autoScrollOnUpdate;
    _terminal.setHighlightTimeout(_profile.highlightTimeout.value());
    _terminal.viewport().setScrollOff(_profile.modalCursorScrollOff.value());
    // The profile's policy, re-imposed on every profile application -- so a config reload or a
    // profile switch resets a policy the Aa button had pinned, the same way it resets every other
    // profile-derived setting here. The notify is what keeps the button honest about it: without one,
    // an open bar kept showing the lit glyph of a mode the terminal had already stopped using.
    // Already under this function's own lock (see its first statement) -- taking a second one here
    // self-deadlocks, because Terminal's state mutex is a plain non-recursive std::mutex.
    (void) _terminal.setSearchCaseSensitivity(_profile.searchCaseSensitivity.value());
    refreshSearchStatusLocked();

    // Posted rather than emitted: this function holds the lock for its whole body, and a QML binding
    // woken by this signal reads back into the session.
    QMetaObject::invokeMethod(this, [this]() { emit searchStateChanged(); }, Qt::QueuedConnection);
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
    auto const devicePixels = geometry::availableDevicePixels(
        screenSize.width(), screenSize.height(), _display->devicePixelRatio());

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

void TerminalSession::setFontSize(text::FontSize size)
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
    auto configFailures = 0;

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

void TerminalSession::openExternally(QUrl const& url, std::string_view what, std::string_view subject)
{
    if (auto const opened = _app.externalLauncher().openUrl(url); !opened)
        errorLog()("Could not open {} \"{}\": {}.", what, subject, platform::describe(opened.error()));
}

void TerminalSession::followHyperlink(vtbackend::HyperlinkInfo const& hyperlink)
{
    auto url = QUrl(QString::fromStdString(hyperlink.uri));

    // A file:// URL naming THIS machine is a local path, whichever way the application spelled the
    // authority (@see vtbackend::isLocalHost). isValid() keeps the openUrl() pass-through below for URIs
    // Qt cannot parse, such as the non-standard Windows form file://C:/dir.
    if (hyperlink.isLocal() && url.isValid()
        && vtbackend::isLocalHost(url.host().toStdString(), _localHostName))
    {
        // An authority that survives is read by the desktop's URL handler as a network location, so
        // file://host/tmp is looked up as //host/tmp and does not exist. Drop it, as OpenFileManager does.
        url.setHost(QString {});

        // Percent-escapes decoded, and the authority already gone: the path as the filesystem spells it.
        auto const localPath = url.toLocalFile();
        // qEnvironmentVariable() rather than getenv(): this runs on the GUI thread while the PTY and
        // render threads are live, and getenv() is not thread safe.
        auto const editorEnv = qEnvironmentVariable("EDITOR");
        auto const fileInfo = QFileInfo(localPath);

        // An executable file is run through `contour config <path>`; any other file is handed to $EDITOR
        // the same way. Anything else -- a directory, a missing path -- goes to the desktop below.
        if (fileInfo.isFile() && (fileInfo.isExecutable() || !editorEnv.isEmpty()))
        {
            auto args = QStringList { "config", QString::fromStdString(_config.configFile.string()) };
            if (!fileInfo.isExecutable())
                args << editorEnv;
            args << localPath;
            if (auto const ran =
                    _app.externalLauncher().execute(QString::fromStdString(_app.programPath()), args);
                !ran)
                errorLog()("Could not open \"{}\" in an editor: {}.",
                           localPath.toStdString(),
                           platform::describe(ran.error()));
            return;
        }
    }

    openExternally(url, "hyperlink", url.toString().toStdString());
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
                SIGNAL(fileChanged(QString const&)),
                this,
                SLOT(onConfigReload()));
}

// }}}
// {{{ QAbstractItemModel impl
QModelIndex TerminalSession::index(int row, int column, QModelIndex const& parent) const
{
    Require(row == 0);
    Require(column == 0);
    // NOTE: if at all, we could expose session attribs like session id, session type
    // (local process), ...?
    crispy::ignoreUnused(parent);
    return createIndex(row, column, nullptr);
}

QModelIndex TerminalSession::parent(QModelIndex const& child) const
{
    crispy::ignoreUnused(child);
    return {};
}

int TerminalSession::rowCount(QModelIndex const& parent) const
{
    crispy::ignoreUnused(parent);
    return 1;
}

int TerminalSession::columnCount(QModelIndex const& parent) const
{
    crispy::ignoreUnused(parent);
    return 1;
}

QVariant TerminalSession::data(QModelIndex const& index, int role) const
{
    crispy::ignoreUnused(index, role);
    Require(index.row() == 0);
    Require(index.column() == 0);

    return { _id };
}

bool TerminalSession::setData(QModelIndex const& index, QVariant const& value, int role)
{
    // NB: Session-Id is read-only.
    crispy::ignoreUnused(index, value, role);
    return false;
}
// }}}

} // namespace contour::session
