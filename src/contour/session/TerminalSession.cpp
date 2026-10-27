// SPDX-License-Identifier: Apache-2.0
#include <contour/ContourGuiApp.hpp>
#include <contour/config/Actions.hpp>
#include <contour/config/FileOpenerTemplate.hpp>
#include <contour/display/TerminalDisplay.hpp>
#include <contour/geometry/CellRectangle.hpp>
#include <contour/input/KeyMapping.hpp>
#include <contour/input/Logging.hpp>
#include <contour/input/MouseMapping.hpp>
#include <contour/platform/Clipboard.hpp>
#include <contour/platform/ExternalLauncher.hpp>
#include <contour/platform/FinishNotification.hpp>
#include <contour/platform/NotificationOccasion.hpp>
#include <contour/platform/QtInvoke.hpp>
#include <contour/platform/QtPath.hpp>
#include <contour/platform/SpeechSynthesizer.hpp>
#include <contour/session/CommandBlockTooltip.hpp>
#include <contour/session/CommandOutputFiles.hpp>
#ifdef CONTOUR_EXTENSIONS
    #include <contour/config/ExtensionDeclarations.hpp>

    #include <vtextension/ExtensionConsent.hpp>
    #include <vtextension/SessionExtensions.hpp>
#endif
#include <contour/session/ExitCode.hpp>
#include <contour/session/HostedOperation.hpp>
#include <contour/session/LocalFileAction.hpp>
#include <contour/session/Logging.hpp>
#include <contour/session/ScreenshotFile.hpp>
#include <contour/session/SpawnCommand.hpp>
#include <contour/session/TerminalSession.hpp>
#include <contour/session/TerminalSessionManager.hpp>
#include <contour/window/TabAttention.hpp>

#include <vtbackend/core/FileUrl.hpp>
#include <vtbackend/core/WorkingDirectory.hpp>
#include <vtbackend/input/HeldModifiers.hpp>
#include <vtbackend/input/vi/HintModeHandler.hpp>
#include <vtbackend/input/vi/ViCommands.hpp>
#include <vtbackend/links/LinkTypes.hpp>
#include <vtbackend/links/PathProbe.hpp>
#include <vtbackend/links/PathResolution.hpp>
#include <vtbackend/screen/LinkDetector.hpp>
#include <vtbackend/screen/LinkPresentation.hpp>
#include <vtbackend/screen/PrintedLinks.hpp>
#include <vtbackend/screen/SearchQuery.hpp>
#include <vtbackend/screen/Terminal.hpp>
#include <vtbackend/shell/CommandLineSanitizer.hpp>
#include <vtbackend/shell/CommandLineText.hpp>

#include <vtpty/CommandLine.hpp>
#include <vtpty/Process.hpp>
#include <vtpty/Pty.hpp>
#include <vtpty/SshSession.hpp>

#include <crispy/StackTrace.hpp>

#include <core/Assert.hpp>
#include <core/Utf8Path.hpp>
#include <core/Utils.hpp>
#include <core/async/IExecutor.hpp>
#include <core/platform/PathUtils.hpp>

#include <libunicode/convert.h>

#include <QtCore/QDeadlineTimer>
#include <QtCore/QDebug>
#include <QtCore/QDir>
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

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <format>
#include <fstream>
#include <iterator>
#include <limits>
#include <ranges>
#include <regex>
#include <span>
#include <tuple>
#include <utility>

#include <tracy/Tracy.hpp>

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

    /// Names the calling thread `Terminal.Loop`, as `ps -T` and debuggers show it, where POSIX threads
    /// can be named; Windows names no thread here.
    void nameMainLoopThread()
    {
#ifdef __APPLE__
        pthread_setname_np("Terminal.Loop");
#elifndef _WIN32
        pthread_setname_np(pthread_self(), "Terminal.Loop");
#endif
    }

    /// Connects @p action to @p signal, running it on every emission naming @p identifier: the notifier
    /// reports on all of a session's notifications through the same signals.
    ///
    /// @param notifier   Whose signal to connect to.
    /// @param context    Whose lifetime the connection is bound to.
    /// @param signal     The signal, whose first parameter is the identifier emitted.
    /// @param identifier The notification identifier this connection is about.
    /// @param action     What to run, taking the signal's remaining parameters.
    /// @return The connection.
    template <typename... Args, typename Action>
    QMetaObject::Connection connectMatching(platform::Notifier* notifier,
                                            TerminalSession* context,
                                            void (platform::Notifier::*signal)(QString, Args...),
                                            std::string const& identifier,
                                            Action action)
    {
        return QObject::connect(notifier,
                                signal,
                                context,
                                [identifier = QString::fromStdString(identifier),
                                 action = std::move(action)](QString const& emitted, Args... args) {
                                    if (emitted == identifier)
                                        action(args...);
                                });
    }

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
                             std::string const& identifier,
                             Action action)
    {
        // Held indirectly because the handler must be able to break its own connection, which does
        // not exist yet when the handler is built.
        auto const connection = std::make_shared<QMetaObject::Connection>();
        *connection = connectMatching(
            notifier, context, signal, identifier, [connection, action = std::move(action)](Args... args) {
                QObject::disconnect(*connection);
                action(args...);
            });
    }

    string normalizeCrlf(QString const& text)
    {
#ifndef _WIN32
        // A copy shares the text's buffer until replace() writes to it.
        return QString { text }.replace("\r\n", "\n").replace("\r", "\n").toUtf8().toStdString();
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
            output = core::replace(input, "  ", " ");
            output = core::replace(output, "\t", " ");
            output = core::replace(output, "\n", " ");
        } while (input != output);

        return output;
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
            _session.deviceClosed();
            platform::postToObject(&_session, [&]() { _session.onClosed(); });
        }

      private:
        TerminalSession& _session;
    };

    /// How often the scrollbar's ticks may be rebuilt: one frame at 60 Hz. They summarise the whole
    /// scrollback, so recomputing them faster than a display can show them buys nothing.
    constexpr auto ScrollbarMarksFrameInterval = std::chrono::milliseconds { 16 };

    /// How often the program in front of a followed session's shell is asked for: each ask is one query of
    /// the process table, and a header or extension a second behind a program change is in time.
    constexpr auto ForegroundProcessPollInterval = std::chrono::seconds { 1 };

    /// @p ticks as QML reads them: one { position, kind, targetLine } map per tick, top first.
    /// @param ticks The bucketed ticks.
    /// @param trackPixels The track they were bucketed for; each position is a fraction of it.
    /// @return The list the scrollbarMarks property reports.
    [[nodiscard]] QVariantList toScrollbarMarkList(std::span<vtbackend::ScrollbarTick const> ticks,
                                                   int trackPixels)
    {
        auto list = QVariantList {};
        list.reserve(static_cast<qsizetype>(ticks.size()));
        for (auto const& tick: ticks)
            list.append(QVariantMap {
                { QStringLiteral("position"),
                  static_cast<double>(tick.pixel) / static_cast<double>(trackPixels) },
                { QStringLiteral("kind"), static_cast<int>(tick.kind) },
                { QStringLiteral("targetLine"), static_cast<qint64>(tick.targetStableId) },
            });
        return list;
    }

    /// Each mark kind's colour under @p palette, indexed by kind.
    [[nodiscard]] std::array<vtbackend::RGBColor, vtbackend::ScrollbarMarkKindList.size()>
    scrollbarMarkColorsOf(vtbackend::ColorPalette const& palette) noexcept
    {
        auto colors = std::array<vtbackend::RGBColor, vtbackend::ScrollbarMarkKindList.size()> {};
        for (auto const kind: vtbackend::ScrollbarMarkKindList)
            colors[static_cast<size_t>(kind)] = vtbackend::scrollbarMarkColor(palette, kind);
        return colors;
    }

    /// @p colors as the QColor list the scrollbarMarkColors property reports.
    [[nodiscard]] QVariantList toScrollbarMarkColorList(std::span<vtbackend::RGBColor const> colors)
    {
        auto list = QVariantList {};
        list.reserve(static_cast<qsizetype>(colors.size()));
        for (auto const color: colors)
            list.append(QVariant::fromValue(platform::toQColor(color)));
        return list;
    }

    /// @return The bit @p origin takes in a set of output origins gathered between two GUI-thread turns.
    [[nodiscard]] constexpr uint8_t originBit(window::OutputOrigin origin) noexcept
    {
        return static_cast<uint8_t>(1U << std::to_underlying(origin));
    }

    /// @p settings with @p home as the terminal's home directory: the one the composition root read for the
    /// pane header, so the status line's `{Cwd}` and the header above it abbreviate alike.
    /// @param settings The terminal's settings.
    /// @param home The user's home directory.
    /// @return The settings, with it.
    [[nodiscard]] vtbackend::Settings withHomeDirectory(vtbackend::Settings settings, std::string home)
    {
        settings.homeDirectory = std::move(home);
        return settings;
    }

} // namespace

#ifdef CONTOUR_EXTENSIONS
namespace
{
    /// Hands a finished extension build to the session's thread, through its post target: dropped once the
    /// session is gone.
    class SessionOwnerThread final: public vtextension::IOwnerThread
    {
      public:
        explicit SessionOwnerThread(std::shared_ptr<platform::PostTarget> target):
            _target { std::move(target) }
        {
        }

        void post(std::function<void()> job) override { _target->post(std::move(job)); }

      private:
        std::shared_ptr<platform::PostTarget> _target;
    };
} // namespace

struct TerminalSession::ExtensionState
{
    /// @param callbacks What the session hears from its extensions.
    /// @param owner Where their builds are handed back to.
    ExtensionState(vtextension::SessionExtensionCallbacks callbacks,
                   std::shared_ptr<vtextension::IOwnerThread> owner):
        extensions { std::move(callbacks), std::move(owner) }
    {
    }

    /// What the profile declared when the extensions were last rebuilt; a reload that leaves it as it is
    /// restarts nothing. Empty at first: nothing is built for a profile without extensions.
    vtextension::DeclaredExtensions declared;

    /// The running extensions, and what the terminal reaches them through; empty without any.
    vtextension::SessionExtensions extensions;

    /// The questions this session asks, one prompt at a time; made on the first question.
    std::unique_ptr<vtextension::ExtensionConsentQueue> consent;

    /// Whether an extension follows the foreground process, as a worker last saw it: a published frame that
    /// changes it has the poll (dis)armed on the GUI thread, and one that does not posts nothing.
    vtbackend::StatusLineSubscriptionWatch foregroundProcessFollowing {
        vtbackend::StatusLineEventKind::ForegroundProcess
    };

    /// The foreground program the extensions were last told of, so that each is told once; extensions that
    /// load or subscribe later hear it from the terminal, which remembers it.
    std::string toldForegroundProcessName;
};
#endif

TerminalSession::TerminalSession(TerminalSessionManager* manager,
                                 unique_ptr<vtpty::Pty> pty,
                                 ContourGuiApp& app,
                                 std::string profileName,
                                 std::optional<vtbackend::PageSize> initialPageSize,
                                 std::optional<vtpty::Process::ExecInfo> launchedCommand,
                                 std::unique_ptr<platform::Notifier> notifier,
                                 SessionRole role,
                                 SessionHosting hosting):
    _manager { manager },
    _id { createSessionId() },
    _startTime { steady_clock::now() },
    _config { app.config() },
    _profileName { resolveProfileName(_config, profileName, app.profileName()) },
    _profileOverride { profileName.empty() ? std::optional<std::string> {} : std::optional { profileName } },
    _launchedCommand { std::move(launchedCommand) },
    _role { role },
    _hosting { hosting },
    // A mirrored pane is born to a replay of what its host shows: the mirror marks its own replays, and this
    // grace catches what it cannot tell apart.
    _resyncGraceEnd { hosting == SessionHosting::Remote
                          ? std::optional { app.terminalClocks().steady.now() + window::ResyncGracePeriod }
                          : std::nullopt },
    _profile { *_config.profile(_profileName) },
    _app { app },
    _currentColorPreference { app.colorPreference() },
    _localIdentity { app.hostIdentity() },
    _accumulatedPixelScroll {},
    _accumulatedAngleScroll {},
    // Asked of the terminal, for a pane bound to another machine's daemon names no path here.
    _hyperlinkHover { [this] { return _terminal.fileUrlLocalHost(); } },
    _extensionState { createExtensionState() },
    _terminal { *this,
                app.processEnvironment(),
                app.fileSystem(),
                std::move(pty),
                withHomeDirectory(
                    config::motionAdjusted(
                        config::sessionSettings(_config, _profile, _currentColorPreference, initialPageSize),
                        app.accessibilityPreferences().motion),
                    app.sessionsManager().homeDirectory()),
                app.statusLineCommandServices(),
                app.terminalClocks(),
                vtbackend::randomSemanticBlockTokens(),
                app.linkDetectionServices(),
                terminalExtensionServices(),
                app.gitHeadServices() },
    _exitWatcherThread { std::make_unique<ExitWatcherThread>(*this) },
    // _config is declared before _desktopNotifier, so reading it here is well-defined. The delay is
    // fixed for the notifier's life: it decides how a backend that cannot observe a close behaves,
    // which is configuration, not state. Where there is no desktop to raise a notice on, it goes to the
    // tray of the window hosting this session, asked of the manager when it is raised.
    _desktopNotifier {
        notifier ? std::move(notifier)
                 : platform::makeDesktopNotifier(
                       _config.notificationCloseTimeout.value(),
                       [this](QString const& title, QString const& body, std::function<void()> onClicked) {
                           if (_manager != nullptr)
                               _manager->requestWindowNotification(*this, title, body, std::move(onClicked));
                       })
    }
{
    // live_config is watched once, by the manager (TerminalSessionManager::reloadAllSessions), not per
    // session: one save is one parse, and the app's configuration is updated before the sessions are.
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

    // The scrollbar's ticks summarise the whole scrollback, so they are rebuilt at most once per frame
    // however fast output arrives.
    // @see requestScrollbarMarksRefresh
    _scrollbarMarksTimer.setSingleShot(true);
    _scrollbarMarksTimer.setInterval(ScrollbarMarksFrameInterval);
    connect(&_scrollbarMarksTimer, &QTimer::timeout, this, &TerminalSession::refreshScrollbarMarks);

    configureTerminal();
    connectFinishNoticeEvents();

#ifdef CONTOUR_EXTENSIONS
    // Once the terminal exists, which the build reports to. Without extensions this starts nothing (A2).
    reloadExtensions();
#endif
}

void TerminalSession::setHostConnection(vthost::client::ConnectionState state)
{
    if (_hosting != SessionHosting::Remote)
        return;
    auto const wasReconnecting = isReconnecting();
    auto const lastPaneState = paneState();
    // A return to Connected repaints the mirror, which is not news to the tab badge.
    if (auto const grace =
            window::resyncGraceAfter(_hostConnection, state, _app.terminalClocks().steady.now()))
        _resyncGraceEnd = grace;
    _hostConnection = state;
    if (isReconnecting() != wasReconnecting)
        emit reconnectingChanged();
    if (paneState() != lastPaneState)
        emit paneStateChanged();
}

TerminalSession::~TerminalSession()
{
    sessionLog()("Destroying terminal session.");
    // First: a path probe that answers from now on finds nothing to hand its result to.
    _postTarget->withdraw();
    _terminating = true;

    // Stop forwarding renderBufferUpdated() before anything is torn down: the status-line command cache
    // inside _terminal calls it from a pool thread and is only quiesced when _terminal is destroyed,
    // which is after every member declared below it.
    setRenderNotifyTarget(nullptr);

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

    // Bounded all the same: what a device's waitForClosed() waits on is its own business -- a POSIX
    // Process waits for its child to end, which the close only asks of it. terminate() stays as the last
    // resort, and is now followed by the wait() it always needed.
    // wait() on a thread that was never started returns immediately, so the never-started case (a
    // session whose device failed to start) costs nothing.
    if (!_exitWatcherThread->wait(QDeadlineTimer { std::chrono::seconds { 3 } }))
    {
        _exitWatcherThread->terminate();
        _exitWatcherThread->wait();
    }

    if (_screenUpdateThread)
        _screenUpdateThread->join();

#ifdef CONTOUR_EXTENSIONS
    // The runners are destroyed with _extensionState, after this body; their lanes retire their engines a
    // moment later, which only this (the owner's) thread may release.
    if (_extensionState && !_extensionState->declared.extensions.empty())
        scheduleRetiredRelease();
#endif
}

void TerminalSession::detachDisplay(DisplaySurface& display)
{
    sessionLog()("Detaching display from session.");
    Require(_display == &display);
    setRenderNotifyTarget(nullptr);
    // The pointer stood over the display that goes, and nothing else would end its hover: a tab switch
    // detaches the display and attaches it to another session, and switching back brought the tooltip back
    // at the cell it had been entered at, wherever the pointer then was.
    clearHovers();
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

vtbackend::StatusLineFontMetrics TerminalSession::displayFontMetrics()
{
    // The point size the display applied, and the cell size it published: device pixels, unlike what
    // resizeScreen() is given, which a profile may report in logical ones.
    return vtbackend::StatusLineFontMetrics { .pointSize = static_cast<float>(_display->getFontDef().size),
                                              .cellPixelSize = _display->cellSize() };
}

void TerminalSession::updateStatusLineFontMetrics()
{
    // One lock-free question first: without extensions nothing is copied and no lock is taken (A2). Those a
    // reload installs later are told by statusLineExtensionsReplaced().
    if (!_display || !runsStatusLineExtensions())
        return;
    auto const metrics = displayFontMetrics();
    // Telling never waits for an extension: it queues an event for each.
    _terminal.runUnderStateLock([&] { _terminal.setStatusLineFontMetrics(metrics); });
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
    setRenderNotifyTarget(&newDisplay);

    // Start the freshly-attached display with clean cursor-move coalescing state. This heals the rare
    // case where the previous display was torn down after a cursorPositionChanged() post was queued but
    // before it drained (dropping the clear): without this, the stale-set flag would make every future
    // cursorPositionChanged() early-return, freezing IME rectangle tracking and the a11y caret.
    _cursorMovedPostPending.clear(std::memory_order_release);

    // What the pane header shows before the shell reports anything: the directory the session started in.
    refreshDisplayWorkingDirectory();

#ifdef CONTOUR_EXTENSIONS
    // A question asked while no display showed this session (a background tab) is shown now.
    if (_extensionState && _extensionState->consent && _extensionState->consent->current())
        QMetaObject::invokeMethod(
            this, [this]() { emit requestPermissionForExtension(); }, Qt::QueuedConnection);
#endif

    // A closed session has nothing to resize: its PTY is gone. Closing a pane makes QML re-run the
    // Loader binding for the surviving panes, which re-enters setSession() -> attachDisplay() while
    // onClosed() is still unwinding, so this path really is reached after the PTY has been closed.
    // Skipping the resize keeps a dead session from pushing a geometry change down to it; ConPty
    // additionally refuses the resize on its own side, since attachDisplay() is not the only caller.
    if (!isClosed())
    {
        // NB: Inform connected TTY and local Screen instance about initial cell pixel size. Under the lock,
        // as the ViNormalMode action resizes: the resize may write a report, sent once the lock is released.
        {
            auto const _ = std::scoped_lock { _terminal };
            _terminal.resizeScreen(_terminal.totalPageSize(),
                                   _display->reportedPixelSize(_terminal.totalPageSize()));
            // refreshRate() dereferences window()->screen(); pre-window (see below) the posted
            // configureDisplay() sets it once the window exists.
            if (_display->window() != nullptr)
                _terminal.setRefreshRate(_display->refreshRate());
        }
    }

    // configureDisplay() only runs from createRenderer(), i.e. once for the first session, so the
    // ceiling has to be (re)derived here too. See updateImageCanvasCeiling() for the null-checks.
    updateImageCanvasCeiling();

    // The font this display draws in, for the status-line extensions; later changes are told by the
    // display once it has applied them.
    updateStatusLineFontMetrics();

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
        core::locked(_terminal, [this] { return _terminal.viewport().scrollableLineCount(); }));

    // And the scrollbar's ticks, for the same reason: a pane that ran commands in the background has none
    // published for this display's track yet.
    requestScrollbarMarksRefresh();

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
    // Named before a key can be typed: from now on, only the read loop writes to the PTY.
    _terminal.setReaderThread(_screenUpdateThread->get_id());
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

    // Close the device so the session is coherently dead rather than idling on a device that never
    // came up, and record the end so the pane waits the way it does for a shell that died right after
    // starting: the footer names the platform's own reason rather than a bare "it failed" (the
    // parent-side equivalent of the diagnostic the POSIX child writes onto the pty before it exits, see
    // Process_unix.cpp), and Restart, Close or terminate()'s already-closed branch end the wait. Without
    // this the pane would be a zombie -- nothing to prune it, since the exit watcher never started.
    if (!_terminal.device().isClosed())
        _terminal.device().close();
    {
        auto const _ = std::scoped_lock { _onClosedMutex };
        recordEndLocked(EndReason::StartFailed, std::format("{}", failure));
    }
    // start() runs on the GUI thread, so the end is announced there, and outside the lock.
    announceSessionEnd();
}

void TerminalSession::mainLoop()
{
    nameMainLoopThread();

    _mainLoopThreadID = this_thread::get_id();

    sessionLog()("Starting main loop with thread id {}", [&]() {
        stringstream sstr;
        sstr << _mainLoopThreadID;
        return sstr.str();
    }());

    // The loop writes what it queued, the replies, and what the GUI thread queued and woke it for: keys,
    // pastes, the reports a frontend's hold of the lock raised. A write the program leaves unread parks this
    // thread, never the GUI's. @see vtbackend::InputWriter
    while (!_terminating)
    {
        if (!_terminal.processInputOnce())
            break;
        _terminal.flushInput();
    }

    sessionLog()("Event loop terminating (PTY {}).", _terminal.device().isClosed() ? "closed" : "open");
}

void TerminalSession::deviceClosed()
{
    // A requested close ends the session, and its records with it: nothing is left to drop.
    if (!_terminationRequested)
        _terminal.markProcessExited();
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
    else if (core::locked(_onClosedMutex, [this] { return _end; }) != EndReason::None)
    {
        // The device is already closed and the pane waits on its recorded end: this deliberate close
        // (the ClosePane binding, a tab close) is the Close -- prune the pane now, as the footer's
        // Close does; otherwise closing a waiting pane would be a silent no-op.
        dismiss();
        return;
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
    auto const configured = _profile.bell.value().volume;
    auto const volume = [configured](vtbackend::BellVolume level) {
        switch (level)
        {
            case vtbackend::BellVolume::Off: return 0.0f;
            case vtbackend::BellVolume::Low: return configured * 0.5f;
            case vtbackend::BellVolume::High: return configured;
        }

        core::unreachable();
    }(terminal().settings().warningBellVolume);

    emit onBell(volume);

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

    emit isScrollbarVisibleChanged();
    requestScrollbarMarksRefresh(); // the alternate screen shows no ticks
    // Re-check _display at dispatch: a tab switch or split collapse may have detached it (via
    // TerminalDisplay::setSession -> detachDisplay) between this post and the GUI thread running it.
    _display->post([this, type]() {
        if (_display)
            _display->bufferChanged(type);
    });
}

void TerminalSession::screenUpdated()
{
    ZoneScoped;
    // Ahead of the display check: the tab badge's unseen-output dot is for the sessions that have no
    // display -- a background tab's panes are not loaded at all. A tmux pane's capture-pane refill arrives
    // as bytes like any other, so its device says where what the parser just read came from.
    noteOutputForTab(_terminal.device().lastReadOrigin() == vtpty::ReadOrigin::Replay
                         ? window::OutputOrigin::Resync
                         : window::OutputOrigin::Live);
    repaintChangedScreen();
}

void TerminalSession::screenReplayed()
{
    ZoneScoped;
    // A daemon mirror's full replay -- the attach, a resync, a resize -- repaints what the host showed
    // already.
    noteOutputForTab(window::OutputOrigin::Resync);
    repaintChangedScreen();
}

void TerminalSession::repaintChangedScreen()
{
    if (!_display)
        return;

    // Emphatically NOT under Terminal's state mutex: this callback is raised BOTH with that mutex held
    // and without it. Terminal::setMode() ends a synchronized update (DECRST 2026) by calling
    // synchronizedOutput(), which refreshes the render buffer on the already-locked path and then raises
    // this -- mid-parse, on the parser thread, with the mutex held. The mutex is a plain non-recursive
    // std::mutex, so taking it here self-deadlocks on the very sequence neovim, tmux and every other modern
    // TUI ends each frame with. @see Events::progressChanged for the same contract, stated.
    //
    // Which is why the view is not brought down to new output here, but by the terminal, under that mutex,
    // as it applies the output (@see outputFollowing). What is left here is un-parking: a view back at the
    // bottom follows the output again. Its position is read as published, which takes no lock.
    if (!_terminal.viewport().isAwayFromBottom())
        _searchMatchRevealed.store(false, std::memory_order_relaxed);

    // New output can add or remove matches, so the find bar's count goes stale. Re-tallying walks the
    // whole scrollback, so it is neither done here nor once per frame: this only re-arms a timer on
    // the GUI thread, and only while the bar is open. Posted rather than called, for the reason
    // stated above -- this runs on the parser thread, sometimes with the state mutex held.
    if (_isSearchBarOpen.load(std::memory_order_relaxed)
        && !_searchTallyPostPending.test_and_set(std::memory_order_acq_rel))
    {
        platform::postToObject(this,
                               // The flag is cleared where the timer FIRES, not here, so exactly one post is
                               // made per 250 ms window. Clearing it here instead let every following screen
                               // update post again while the timer was still running -- one queued call per
                               // PTY read chunk, which under heavy output is thousands a second.
                               [this]() {
                                   // Re-checked here: the bar can close between this post and its delivery,
                                   // and starting the timer then would fire a tally 250 ms later for a bar
                                   // that is gone.
                                   if (_isSearchBarOpen.load(std::memory_order_relaxed))
                                       _searchTallyTimer.start();
                                   else
                                       _searchTallyPostPending.clear(std::memory_order_release);
                               });
    }

    // New output can grow the history the scrollbar spans, add blocks, or finish one. Only a request, for
    // its travel as well as its ticks: the refresh takes the lock, which may be held here. Nor is the
    // travel counted here while it is not: the count goes through the fold projection, a cache that only
    // the lock's holder may rebuild.
    requestScrollbarMarksRefresh();

    scheduleRedraw();
}

void TerminalSession::noteOutputForTab(window::OutputOrigin origin)
{
    // On the parser thread, possibly with the state mutex held (see screenUpdated): where the session sits,
    // and the connection's state, are the GUI thread's to read, so this only posts. Coalesced as
    // programStatusChanged() is: one queued call at a time, so a stream of output costs one call per turn
    // of the GUI thread's loop, not one per chunk; the origins seen meanwhile are gathered as bits, so live
    // output behind a replay is not lost in it. postToObject targets `this`, so Qt drops the call if the
    // session goes first.
    if (_pendingOutputOrigins.fetch_or(originBit(origin), std::memory_order_acq_rel) != 0)
        return;
    platform::postToObject(this, [this]() {
        auto const origins = _pendingOutputOrigins.exchange(0, std::memory_order_acq_rel);
        if (_manager == nullptr)
            return;
        auto origin = window::OutputOrigin::Resync;
        if ((origins & originBit(window::OutputOrigin::Live)) != 0)
            // The grace is the fallback for a mirror that cannot tell its replay apart: the connection's
            // first moments after it came (back) up.
            origin = _hosting == SessionHosting::Remote
                         ? window::outputOriginOf(
                               _hostConnection, _app.terminalClocks().steady.now(), _resyncGraceEnd)
                         : window::OutputOrigin::Live;
        _manager->noteSessionOutput(modelSessionId(), origin);
    });
}

void TerminalSession::pageResized()
{
    // The first resize after the end is the footer laying out below the cells: what it took, a restart
    // gives back. Later ones (the window, the font) change the page, not what the footer takes.
    // Only the GUI thread's: the footer's layout resizes from there, while a resize on the parser thread --
    // a program's DECSSDT in output still being parsed as the device closes, local or daemon-mirrored -- is
    // not the footer's, and the two members are the GUI thread's alone. Checked first, so that thread
    // reads neither.
    if (QThread::currentThread() == thread() && _linesAtEnd && !_footerLines)
    {
        auto const lines = _terminal.totalPageSize().lines;
        _footerLines = *_linesAtEnd > lines ? *_linesAtEnd - lines : vtbackend::LineCount(0);
    }

    // A window, font or split resize is followed by no output when the program in the pane is quiet, and
    // the reflow renamed every row a tick points at: without this the old ticks stayed, and every click on
    // one was refused, until the program next printed. Only a request: the resizer holds the lock.
    if (_display)
        requestScrollbarMarksRefresh();
}

void TerminalSession::flushInput()
{
    // Wakes the session's read loop, which writes it, and retries what the PTY has no room for yet.
    terminal().flushInput();
}

void TerminalSession::setRenderNotifyTarget(DisplaySurface* target)
{
    auto const lock = std::scoped_lock { _renderNotifyMutex };
    _renderNotifyTarget = target;
}

vtbackend::OutputFollowing TerminalSession::outputFollowing() const noexcept
{
    // Not while the viewport is parked on something the user went looking for. The Vi-mode test used to be
    // the whole guard, and it worked only because starting a search forced Normal mode; the bar deliberately
    // switches no mode, so a search runs with the mode still Insert, and a match revealed in the scrollback
    // would be yanked away by the next byte of output.
    //
    // Two ways to be parked, not one. The bar being open is the obvious one. The other is F3, which the
    // default bindings gate on the Search match mode -- "a pattern is set", which OUTLIVES the bar (@see
    // searchBarClosed) -- so stepping through matches with the bar shut is a supported flow that runs in
    // Insert mode with the bar's flag clear. @see _searchMatchRevealed for how it un-parks.
    if (_isSearchBarOpen.load(std::memory_order_relaxed)
        || _searchMatchRevealed.load(std::memory_order_relaxed))
        return vtbackend::OutputFollowing::Stay;

    return vtbackend::OutputFollowing::Follow;
}

void TerminalSession::renderBufferUpdated()
{
    // May run on a pool thread (see Terminal::Events::renderBufferUpdated), so it forwards to the
    // mutex-guarded copy of _display rather than _display itself. Holding the lock across the call makes a
    // concurrent detach wait for it, so the surface cannot go away mid-forward.
    auto const lock = std::scoped_lock { _renderNotifyMutex };
    if (_renderNotifyTarget != nullptr)
        _renderNotifyTarget->renderBufferUpdated();
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
        case GuardedRole::RerunCommand: applyPendingRerun(allow, remember); break;
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
        // Asked every time unless the user said otherwise for the session: the line is the shell's report,
        // which any program writing to the terminal can forge, so it runs only once it has been read.
        case GuardedRole::RerunCommand: return config::Permission::Ask;
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
                    case GuardedRole::RerunCommand: emit requestPermissionForRerunCommand(); break;
                        // clang-format on
                }
            }
            break;
        }
    }
}

// {{{ status-line extensions
QString TerminalSession::pendingExtensionPermissionText() const
{
#ifdef CONTOUR_EXTENSIONS
    if (_extensionState && _extensionState->consent)
        if (auto const* const request = _extensionState->consent->current())
            return QString::fromStdString(vtextension::consentText(*request));
#endif
    return {};
}

void TerminalSession::allowPendingExtension()
{
#ifdef CONTOUR_EXTENSIONS
    answerExtensionConsent(vtextension::ConsentAnswer::Allowed);
#endif
}

void TerminalSession::denyPendingExtension()
{
#ifdef CONTOUR_EXTENSIONS
    answerExtensionConsent(vtextension::ConsentAnswer::Denied);
#endif
}

void TerminalSession::dismissPendingExtension()
{
#ifdef CONTOUR_EXTENSIONS
    answerExtensionConsent(vtextension::ConsentAnswer::Dismissed);
#endif
}

vtextension::SessionExtensions const* TerminalSession::statusLineExtensions() const noexcept
{
#ifdef CONTOUR_EXTENSIONS
    if (_extensionState)
        return &_extensionState->extensions;
#endif
    return nullptr;
}

bool TerminalSession::pollsForegroundProcess() const noexcept
{
    return _foregroundProcessTimer != nullptr;
}

void TerminalSession::startFollowingForegroundProcess(ProcessNameFollower who)
{
    // A mirrored pane's program runs on its host: this machine's device has no process group to ask. An
    // ended pane's header names no program either: its footer stands where the program ran.
    if (_hosting == SessionHosting::Remote || _processNameFollowers.test(who))
        return;
    if (who == ProcessNameFollower::Header
        && core::locked(_onClosedMutex, [this] { return _end; }) != EndReason::None)
        return;
    _processNameFollowers.enable(who);
    if (!_foregroundProcessTimer)
    {
        _foregroundProcessTimer = std::make_unique<QTimer>();
        _foregroundProcessTimer->setInterval(ForegroundProcessPollInterval);
        connect(
            _foregroundProcessTimer.get(), &QTimer::timeout, this, &TerminalSession::pollForegroundProcess);
        _foregroundProcessTimer->start();
    }
    // At once, so that the new follower does not wait a tick: a name already known is told, not re-read.
    pollForegroundProcess();
}

void TerminalSession::stopFollowingForegroundProcess(ProcessNameFollower who)
{
    _processNameFollowers.disable(who);
    if (_processNameFollowers.none())
        _foregroundProcessTimer.reset();
}

void TerminalSession::pollForegroundProcess()
{
    if (_processNameFollowers.none())
        return;
    // Unknown (the process table could not be read, or nothing leads the group) keeps the last name: a
    // header does not blank for a missed tick.
    if (auto name = _terminal.device().foregroundProcessName(); name && *name != _polledForegroundProcessName)
    {
        _polledForegroundProcessName = std::move(*name);
        emit foregroundProcessNameChanged();
    }
    tellExtensionsForegroundProcess();
}

bool TerminalSession::runsStatusLineExtensions() const noexcept
{
#ifdef CONTOUR_EXTENSIONS
    return _extensionState && !_extensionState->extensions.empty();
#else
    return false;
#endif
}

std::optional<vtbackend::StatusLineExtensionServices> TerminalSession::terminalExtensionServices() noexcept
{
#ifdef CONTOUR_EXTENSIONS
    if (_extensionState)
        return _extensionState->extensions.terminalServices();
#endif
    return std::nullopt;
}

#ifdef CONTOUR_EXTENSIONS
namespace
{
    /// What an extension logs, in Contour's log. Called on the extension's lane.
    void logExtensionMessage(std::string_view name, vtextension::LogLevel level, std::string_view message)
    {
        // Asked first, so that a chatty guest costs nothing while nobody reads the category.
        if (!extensionLog.isEnabled())
            return;
        extensionLog()("{}", vtextension::extensionLogLine(name, level, message));
    }
} // namespace

TerminalSession::ExtensionStatePtr TerminalSession::createExtensionState()
{
    // Always, so that a reload can bring extensions to this pane: empty, it is a forwarder to nothing and a
    // gate subscribed to nothing -- no runtime, no thread, no timer (A2).
    auto state = std::make_unique<ExtensionState>(
        vtextension::SessionExtensionCallbacks {
            .onPermissionNeeded =
                [this](std::string_view hostId, vtextension::Manifest const& manifest) {
                    requestExtensionPermission(hostId, manifest);
                },
            .onLog = &logExtensionMessage,
            .onReplaced = [this]() { statusLineExtensionsReplaced(); } },
        std::make_shared<SessionOwnerThread>(_postTarget));
    return { state.release(), [](ExtensionState* made) { std::default_delete<ExtensionState> {}(made); } };
}

void TerminalSession::reloadExtensions()
{
    auto& state = *_extensionState;
    // A mirrored pane runs none here: its session's extensions run on the remote end, where its terminal
    // is, and its own stays the empty forwarder -- no runtime, no thread, no timer.
    auto declared = _hosting == SessionHosting::Remote ? vtextension::DeclaredExtensions {}
                                                       : config::declaredExtensionsOf(_profile);
    if (declared == state.declared)
        return;
    state.declared = declared;

    if (declared.extensions.empty())
    {
        // Diagnosed even so, but nothing is started for it, and the poll stops.
        if (auto const diagnostic = vtextension::undeclaredTakeOver(declared))
            errorLog()("{}", *diagnostic);
        state.extensions.rebuild(std::move(declared), {}, std::nullopt);
        armForegroundProcessPoll();
        return;
    }

    sessionLog()("Loading the status-line extensions.");
    auto const cellPixelSize = _terminal.runUnderStateLock([this] { return _terminal.cellPixelSize(); });
    // Found, read and loaded on the runtime's build thread; they replace the running ones once loaded.
    state.extensions.rebuild(std::move(declared), cellPixelSize, _app.extensionRuntime().sessionServices());
    // The poll is armed once they say they follow the foreground process: after they have loaded.
}

void TerminalSession::armForegroundProcessPoll()
{
    if (_extensionState->extensions.subscriptions().test(vtbackend::StatusLineEventKind::ForegroundProcess))
        startFollowingForegroundProcess(ProcessNameFollower::Extension);
    else
        stopFollowingForegroundProcess(ProcessNameFollower::Extension);
}

void TerminalSession::extensionFramePublished()
{
    // On an extension's worker, holding no lock: what the extensions follow is read lock-free, and only a
    // change of it is handed to the GUI thread, which (dis)arms the poll.
    auto& state = *_extensionState;
    if (state.foregroundProcessFollowing.changed(state.extensions.subscriptions()))
        _postTarget->post([this] { armForegroundProcessPoll(); });
}

void TerminalSession::statusLineExtensionsReplaced()
{
    {
        auto const lock = scoped_lock { _terminal };
        // Which tells the new ones the foreground process too: the poll tells each name once.
        _terminal.statusLineExtensionsReplaced();
        // And the font the display draws in, which the terminal was not told while there were no extensions;
        // unchanged, it is not told twice.
        if (_display && runsStatusLineExtensions())
            _terminal.setStatusLineFontMetrics(displayFontMetrics());
    }
    // What the new ones follow is known once they have loaded, which a frame they publish tells: forgotten
    // what the ones before them followed first, so that the frame counts as a change, and the poll
    // re-checked.
    _extensionState->foregroundProcessFollowing.reset();
    armForegroundProcessPoll();
    scheduleRetiredRelease();
}

void TerminalSession::requestExtensionPermission(std::string_view hostId,
                                                 vtextension::Manifest const& manifest)
{
    // On the extension's lane: the question is handed to the GUI thread, and nothing waits for the answer.
    _postTarget->post([this,
                       request = vtextension::ExtensionConsentRequest { .hostId = std::string { hostId },
                                                                        .manifest = manifest }]() mutable {
        offerExtensionConsent(std::move(request), _app.extensionRuntime().grants());
    });
}

void TerminalSession::askForHostedExtension(vtextension::ExtensionConsentRequest request,
                                            vtextension::IGrantStore& answers)
{
    offerExtensionConsent(std::move(request), answers);
}

void TerminalSession::offerExtensionConsent(vtextension::ExtensionConsentRequest request,
                                            vtextension::IGrantStore& answers)
{
    if (!_extensionState)
        return;
    auto& consent = _extensionState->consent;
    if (!consent)
        consent = std::make_unique<vtextension::ExtensionConsentQueue>();
    sessionLog()("Status-line extension {} asks to run its declared commands.", request.hostId);
    if (consent->offer(std::move(request), answers) == vtextension::ConsentPrompt::Due)
        emit requestPermissionForExtension();
}

void TerminalSession::answerExtensionConsent(vtextension::ConsentAnswer answer)
{
    if (!_extensionState || !_extensionState->consent)
        return;
    // The next question opens once the dialog that answered this one has closed.
    if (_extensionState->consent->answer(answer) == vtextension::ConsentPrompt::Due)
        QMetaObject::invokeMethod(
            this, [this]() { emit requestPermissionForExtension(); }, Qt::QueuedConnection);
}

std::string_view TerminalSession::foregroundProcessToldToExtensions() const noexcept
{
    return _extensionState ? std::string_view { _extensionState->toldForegroundProcessName }
                           : std::string_view {};
}

void TerminalSession::tellExtensionsForegroundProcess()
{
    // Only while an extension follows: a header's poll tells the extensions nothing they did not ask for.
    auto& state = *_extensionState;
    if (!_processNameFollowers.test(ProcessNameFollower::Extension) || _polledForegroundProcessName.empty()
        || _polledForegroundProcessName == state.toldForegroundProcessName)
        return;
    state.toldForegroundProcessName = _polledForegroundProcessName;
    auto const lock = scoped_lock { _terminal };
    _terminal.foregroundProcessChanged(state.toldForegroundProcessName);
}

void TerminalSession::scheduleRetiredRelease()
{
    _app.releaseRetiredExtensionsSoon();
}
#else
TerminalSession::ExtensionStatePtr TerminalSession::createExtensionState()
{
    return { nullptr, nullptr };
}

void TerminalSession::extensionFramePublished()
{
}

void TerminalSession::tellExtensionsForegroundProcess()
{
}

std::string_view TerminalSession::foregroundProcessToldToExtensions() const noexcept
{
    return {};
}
#endif
// }}}

std::string TerminalSession::foregroundProcessName() const
{
    // Only a pty this process spawned has a process group to ask; a mirrored pane's runs elsewhere, and a
    // remote-hosted pane's program is the host's to name.
    if (_hosting == SessionHosting::Remote)
        return {};
    return _terminal.device().foregroundProcessName().value_or(std::string {});
}

void TerminalSession::updateColorPreference(vtbackend::ColorPreference preference)
{
    if (preference == _currentColorPreference)
        return;

    _currentColorPreference = preference;
    if (auto const* colorPalette = config::preferredColorPalette(_profile.colors.value(), preference))
    {
        // Under the lock, as configureTerminal() re-seeds it: the parser thread reads the palette for every
        // colour query and frame. With mode 2031 set the reset writes a report, sent once the lock is
        // released, as with SRM reset it echoes, which parses. The signal is emitted outside it, because the
        // QML binding it wakes reads back into this session.
        {
            auto const _ = std::scoped_lock { _terminal };
            _terminal.resetColorPalette(*colorPalette);
        }
        requestScrollbarMarksRefresh(); // the ticks are drawn in the scheme's colours

        emit backgroundColorChanged();
    }
}

void TerminalSession::applyMotionPreference(config::MotionPreference motion)
{
    // Recomputed from the configuration rather than remembered, so turning Reduce Motion off restores
    // exactly what the profile configures. Built before the lock, from GUI-thread state only.
    auto const adjusted = config::motionAdjusted(
        config::sessionSettings(_config, _profile, _currentColorPreference, std::nullopt), motion);

    // Locked: the render thread reads both durations as it animates. The factory settings follow too, so a
    // RIS returns to the durations the preference allows, as configureTerminal() hands them over.
    auto const _ = std::scoped_lock { _terminal };
    for (auto const setting: config::MotionSettings)
    {
        _terminal.settings().*setting = adjusted.*setting;
        _terminal.factorySettings().*setting = adjusted.*setting;
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
    // Events hook and released it with that chunk; by now it may well be inside the next one. The replies
    // are flushed once it is released: with SRM reset the flush echoes them, which parses and takes it again.
    {
        auto const _ = std::scoped_lock { _terminal };
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
    }

    sessionLog()(
        "requestCaptureBuffer: Answered {} request(s) with {}.", requests.size(), allow ? "allow" : "deny");
    // The lock's release woke the read loop, which writes the replies, a whole buffer of them perhaps, for as
    // long as the PTY pushes back.
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

    // Under the lock: the parser thread reads what the answer leaves behind at DECSASD 0, and draws the
    // line it puts up. Denied, the line stays hidden, and status_line.sync_to_window_title may show what
    // the application writes there in the window title instead. Putting the line up resizes the page, which
    // may write a report, sent once the lock is released. See ViNormalMode.
    {
        auto const _ = std::scoped_lock { _terminal };
        _terminal.answerHostWritableStatusLineRequest(allow
                                                          ? vtbackend::HostWritableStatusLineAnswer::Shown
                                                          : vtbackend::HostWritableStatusLineAnswer::Denied);
    }
    if (!allow)
        return;

    sessionLog()("Host-writable status line shown.");
    // The lock's release woke the read loop, which writes the resize report.
    flushInput();
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
    // such a value (and a genuinely scheme-less one) as a filesystem path -- asked of the app's filesystem,
    // the one the terminal found the path under the pointer on, not of the disk.
    if (url.scheme().isEmpty() || url.scheme().size() == 1)
    {
        auto const& fileSystem = _app.fileSystem();
        if (auto const path = core::pathFromUtf8(fileOrUrl); path && fileSystem.exists(*path))
            url = QUrl::fromLocalFile(
                platform::toQString(path->is_absolute() ? *path : fileSystem.currentPath() / *path));
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
    // OSC 777 and OSC 9: a notification with neither an identifier to report on nor an occasion of its own,
    // decided and raised as an OSC 99 one is.
    auto notification = vtbackend::DesktopNotification {};
    notification.title = std::string(title);
    notification.body = std::string(content);
    notification.occasion = platform::UnstatedNotificationOccasion;
    notification.focusOnActivation = false;
    showDesktopNotification(notification);
}

void TerminalSession::showDesktopNotification(vtbackend::DesktopNotification const& notification)
{
    // Raised on the parser thread with the terminal's state mutex held, while where the pane is -- which
    // decides whether it is shown -- is the GUI thread's to say: posted, as a later close of it is.
    platform::postToObject(this, [this, notification]() { raiseDesktopNotification(notification); });
}

void TerminalSession::raiseDesktopNotification(vtbackend::DesktopNotification const& notification)
{
    // A standalone session is placed nowhere, so nothing puts it in sight.
    auto const visibility =
        _manager != nullptr ? _manager->sessionVisibility(*this) : platform::SessionVisibility::Hidden;
    if (!platform::isNoticeWanted(platform::notifyWhenOf(notification.occasion), visibility))
    {
        // Never shown, so not live either: the terminal listed it as it arrived.
        core::locked(_terminal, [&]() {
            _terminal.desktopNotificationManager().removeActiveNotification(notification.identifier);
        });
        return;
    }

    // Said as well as shown: a desktop notification is a visual popup, and an assistive client that
    // does not read the desktop's own notifications would otherwise miss it entirely.
    announce(notification.title.empty()
                 ? QString::fromStdString(notification.body)
                 : QStringLiteral("%1: %2").arg(QString::fromStdString(notification.title),
                                                QString::fromStdString(notification.body)));

    // Through the session's notifier on every platform: a pane out of sight has no chrome on screen to pass
    // it on, and where there is no desktop to raise it on the notifier hands it to the hosting window. A
    // notifier that cannot report a close or a click simply never fires what is connected below.
    _desktopNotifier->notify(notification);

    auto const identifier = notification.identifier;

    if (notification.closeEventRequested)
        connectOnceMatching(
            _desktopNotifier.get(),
            this,
            &platform::Notifier::notificationClosed,
            identifier,
            [this, identifier](uint /*reason*/, vtbackend::CloseReport report) {
                // Under the terminal's lock: the parser thread lists notifications as they arrive, and
                // answers in the same reply stream. The answer goes out once the lock is released: raised
                // here, on the GUI thread, it is in no batch of the parser's, whose end would send it.
                {
                    auto const _ = std::scoped_lock { _terminal };
                    _terminal.reply("\033]{}\033\\", vtbackend::buildOSC99CloseResponse(identifier, report));
                    _terminal.desktopNotificationManager().removeActiveNotification(identifier);
                }
            });

    if (notification.reportOnActivation)
        connectOnceMatching(
            _desktopNotifier.get(), this, &platform::Notifier::actionInvoked, identifier, [this, identifier] {
                // Under the terminal's lock, and sent once it is released, as the close's answer is: the
                // reply is phrased by parser state.
                {
                    auto const _ = std::scoped_lock { _terminal };
                    _terminal.reply("\033]99;i={}:p=activated;\033\\", identifier);
                }
            });

    if (notification.focusOnActivation)
        connectOnceMatching(
            _desktopNotifier.get(), this, &platform::Notifier::actionInvoked, identifier, [this] {
                focusTerminalWindow();
            });
}

void TerminalSession::discardDesktopNotification(std::string_view identifier)
{
    // Posted behind the raise it may be about, which waits on the GUI thread for the same reason; issued
    // at once, the close would reach the notifier first and find nothing to withdraw. No #ifdef: where
    // there is nothing to withdraw a notification from, the notifier's close is a no-op.
    platform::postToObject(
        this, [this, identifier = std::string(identifier)]() { _desktopNotifier->close(identifier); });
}

void TerminalSession::focusTerminalWindow()
{
    // The session brought forward -- its pane and tab selected, its window raised and restored -- and not
    // merely the window of a display it may not have: a notification is the case for a pane out of sight.
    // Posted, for placement is the GUI thread's; a standalone session has no manager to place it.
    platform::postToObject(this, [this]() {
        if (_manager != nullptr)
            _manager->revealSession(*this);
    });
}

void TerminalSession::announceSessionEnd()
{
    // Before the footer lays out and takes its rows (totalPageSize() reads lock-free).
    _linesAtEnd = _terminal.totalPageSize().lines;
    // The footer stands where the program ran: its header has no program in front to name.
    stopFollowingForegroundProcess(ProcessNameFollower::Header);
    emit sessionEndChanged();
    emit paneStateChanged();
}

void TerminalSession::recordEndLocked(EndReason reason, std::string detail)
{
    sessionLog()("Session ended ({}): {}", static_cast<int>(reason), detail);
    _end = reason;
    _endDetail = std::move(detail);
}

QString TerminalSession::getEndMessage() const
{
    return QString::fromStdString(paneFooterMessage(_end, _endDetail));
}

FooterRestart TerminalSession::restartOffer() const noexcept
{
    return restartOf(
        _end, _hosting == SessionHosting::Local ? RestartSupport::Supported : RestartSupport::Unsupported);
}

vtbackend::PageSize TerminalSession::restartPageSize() const
{
    auto size = _terminal.totalPageSize();
    size.lines += _footerLines.value_or(vtbackend::LineCount(0));
    return size;
}

void TerminalSession::restart()
{
    if (restartOffer() != FooterRestart::Offered || _manager == nullptr)
        return;
    // The manager puts a new session into this pane and lets this one go, freed later (deleteLater): the
    // key press or click that asked for it is still running on it.
    if (auto const restarted = _manager->restartSession(*this); !restarted)
        sessionLog()("Not restarted: {}.", describe(restarted.error()));
}

void TerminalSession::dismiss()
{
    if (core::locked(_onClosedMutex, [this] { return _end; }) == EndReason::None)
        return;
    sessionLog()("Ended session dismissed. Closing pane.");
    // Prune-then-terminate, mirroring onClosed()'s normal path: remove this pane from the model first so
    // canCloseWindow() can approve closing the window, then let the QML side act on terminated(). Once
    // only: _onClosedHandled is what onClosed()'s normal path claims too. _display re-checked: the prune
    // may tear the display down with the pane.
    if (!_onClosedHandled.exchange(true))
        emit sessionClosed(*this);
    if (_display)
        _display->closeDisplay();
}

void TerminalSession::onClosed()
{
    // The end is announced outside the lock, so a QML binding re-reading it cannot meet a held mutex.
    // onClosed() always runs on the GUI thread (posted by the exit watcher and by TerminalDisplay), so
    // the signal needs no queued hop.
    auto const endBefore = _end;
    {
        auto const _ = std::scoped_lock { _onClosedMutex };
        handleDeviceClosedLocked();
    }
    if (_end != endBefore)
        announceSessionEnd();
}

void TerminalSession::handleDeviceClosedLocked()
{
    sessionLog()("Terminal device closed (thread {})", core::threadName());

    // One exit reaches here twice: from the exit watcher, and from TerminalDisplay's "the device was
    // already closed when the first frame was about to be drawn" check -- which is the norm for a
    // command that exits immediately (`contour terminal echo hello`). The _onClosedHandled guard
    // further down sits BELOW the waiting branches' `return` and so cannot speak for them; a recorded
    // end is what says this exit has already been reported (#2102). It also covers a start failure,
    // whose end reportDeviceStartFailure() has already recorded.
    if (_end != EndReason::None)
        return;

    if (!_terminal.device().isClosed())
        _terminal.device().close();

    auto const now = steady_clock::now();
    auto const diff = std::chrono::duration_cast<std::chrono::seconds>(now - _startTime);

    auto const exitStatus = sessionExitStatusOf(_terminal.device());
    if (exitStatus)
        std::visit(
            [&](auto const& status) {
                sessionLog()(
                    "Process terminated after {} seconds with exit status {}.", diff.count(), status);
            },
            *exitStatus);
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
        // Like the early exit below, this deliberately does not emit sessionClosed: pruning the pane
        // would destroy the screen the footer is shown under. The pane stays alive until its Close.
        recordEndLocked(EndReason::Disconnected, {});
        return;
    }

    // A pager is expected to end at once -- a `q`, or a `more` whose output fits one screen -- so only a
    // shell's quick end is taken for a failure to start, and only when the shell did not say it succeeded:
    // `exit` or Ctrl+D moments after opening a window closes it like at any other time.
    if (_role == SessionRole::Shell && diff < _app.earlyExitThreshold() && !_terminationRequested
        && quickExitNoticeFor(exitStatus) == QuickExitNotice::Show)
    {
        // Deliberately do NOT emit sessionClosed here: removeSession() would prune this pane from the
        // model and tear down its QML item, destroying the very screen -- the shell's last words -- the
        // footer is shown under (and leaving an empty window behind that nothing can close). The pane
        // stays fully alive until its Restart (Return) or Close (see dismiss, terminate).
        auto report = endOf(exitStatus);
        recordEndLocked(report.reason, std::move(report.detail));
        return;
    }

    auto isClosedAlready = _onClosedHandled.load();
    if (isClosedAlready || !_onClosedHandled.compare_exchange_weak(isClosedAlready, true))
    {
        sessionLog()("onClosed called: thread {}, display {}", core::threadName(), _display ? "yes" : "no");
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

namespace
{
    /// Why a command line cannot be run again now.
    enum class RerunRefusal : uint8_t
    {
        NoCommandLine, ///< The block is gone, or reported no command line.
        NoPrompt,   ///< A command is running, or the page on display is no shell's: typed, it would be input.
        Untypeable, ///< Typed, it would lose a character a shell reads as a key -- and be another command.
    };

    /// Whether @p commandLine can be typed at @p terminal's prompt as it is, now.
    ///
    /// Refused rather than sanitised where a character would be taken out (Terminal::pastesUnchanged()), as a
    /// hint's text is: a command missing a character is another command.
    /// @param terminal The terminal, locked.
    /// @param commandLine The command line as reported or recovered.
    /// @return Nothing, or why not.
    [[nodiscard]] std::expected<void, RerunRefusal> checkRerun(vtbackend::Terminal const& terminal,
                                                               std::string_view commandLine)
    {
        if (!terminal.shellIsPrompting())
            return std::unexpected(RerunRefusal::NoPrompt);
        if (!terminal.pastesUnchanged(commandLine))
            return std::unexpected(RerunRefusal::Untypeable);
        return {};
    }

    [[nodiscard]] std::string_view rerunRefusalText(RerunRefusal refusal) noexcept
    {
        switch (refusal)
        {
            case RerunRefusal::NoCommandLine: return "there is no command line to run";
            case RerunRefusal::NoPrompt: return "the shell is not at its prompt";
            case RerunRefusal::Untypeable: return "it cannot be typed as it is";
        }
        return "";
    }
} // namespace

void TerminalSession::applyPendingRerun(bool allow, bool remember)
{
    sessionLog()("applyPendingRerun: allow={}, remember={}", allow, remember);
    if (remember)
        _rememberedPermissions[GuardedRole::RerunCommand] = allow;

    if (!_pendingRerun)
        return;

    // Taken out either way: a refused rerun is finished, not still pending.
    auto const commandLine = std::exchange(_pendingRerun, std::nullopt).value();
    if (!allow)
        return;

    // Locked: the prompt is asked about again -- it may have gone while the dialog was up -- and
    // sendPaste() appends to the input generator the parser thread appends to as well. Flushed once the
    // lock is released. See ViNormalMode.
    auto const _ = std::scoped_lock { _terminal };
    if (auto const checked = checkRerun(terminal(), commandLine.raw()); !checked)
    {
        sessionLog()("Not running the command again: {}.", rerunRefusalText(checked.error()));
        return;
    }
    terminal().sendPaste(commandLine.raw());
    terminal().sendRawInput("\r");
}

QString TerminalSession::pendingRerunCommandLine() const
{
    return _pendingRerun ? QString::fromStdString(_pendingRerun->display()) : QString {};
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

template <typename Change>
void TerminalSession::postToManager(Change change)
{
    if (_manager == nullptr)
        return;
    platform::postToObject(_manager,
                           [manager = _manager, change = std::move(change)]() mutable { change(*manager); });
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
    // display. Queued on the manager and keyed by the session's id, so it outlives neither: a session
    // closed meanwhile is a tab the manager no longer finds.
    postToManager([id = modelSessionId()](TerminalSessionManager& manager) {
        manager.update();                 // indicator status line
        manager.refreshTabForSession(id); // GUI tab strip label
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
    // Post the mutation to the GUI thread, through postToManager for the reason it gives.
    postToManager([color, id = modelSessionId()](TerminalSessionManager& manager) {
        manager.setTabColorForSession(id, color);
    });
}

void TerminalSession::resetWindowFrameColor()
{
    // DECAC item 2 with no colors, or a hard reset (RIS): clear the tab color. Same threading
    // constraint as setWindowFrameColor() above.
    postToManager(
        [id = modelSessionId()](TerminalSessionManager& manager) { manager.resetTabColorForSession(id); });
}

void TerminalSession::defaultBackgroundColorChanged(vtbackend::RGBColor color)
{
    // OSC 11 and its kin. Same threading constraint as setWindowFrameColor() above: the caller holds the
    // terminal lock, so applying the chrome theme happens on the GUI thread, through the manager.
    postToManager([color, id = modelSessionId()](TerminalSessionManager& manager) {
        manager.sessionBackgroundChanged(id, color);
    });
}

vtbackend::RGBColor TerminalSession::liveBackground() const
{
    auto const _ = std::scoped_lock { _terminal };
    return _terminal.colorPalette().defaultBackground;
}

void TerminalSession::progressChanged(vtbackend::Progress /*progress*/)
{
    // OSC 9;4. Same threading constraint as setWindowFrameColor() above: this runs on the parser
    // thread with Terminal::_stateMutex held, and the tab-strip refresh reads the terminal back
    // through resolvedProgress(), which takes that same non-recursive mutex. Post it to the GUI
    // thread, through postToManager for the reason it gives.
    // The value itself is not carried across: the refresh reads it back from the terminal, so the
    // strip cannot paint a state older than the one in effect when the posted call finally runs.
    postToManager([id = modelSessionId()](TerminalSessionManager& manager) {
        manager.refreshTabProgressForSession(id);
    });
}

void TerminalSession::programStatusChanged(vtbackend::ProgramStatusChange const& /*change*/)
{
    // OSC 7501, a mirror's replacement, a prompt or the process's exit. The threading constraint of
    // progressChanged() above, and its answer: the badge reads the records back, under the lock, on the GUI
    // thread, so it never paints a set older than the one in effect when the posted call runs. That is also
    // what lets a program reporting in a loop cost one queued call, not one per report: while one is queued,
    // another would read the same records. postToObject targets `this`, so Qt drops it if the session goes.
    if (_isProgramStatusRefreshPending.exchange(true))
        return;
    platform::postToObject(this, [this]() {
        _isProgramStatusRefreshPending = false;
        if (_manager != nullptr)
            _manager->refreshTabBadgeForSession(modelSessionId());
    });
}

void TerminalSession::workingDirectoryChanged()
{
    // The threading constraint of programStatusChanged() above, and its answer: the GUI thread reads the
    // directory back, so the header never shows one older than the one in effect when the call runs, and a
    // burst of reports queues one call.
    if (_isWorkingDirectoryRefreshPending.exchange(true))
        return;
    _postTarget->post([this]() {
        _isWorkingDirectoryRefreshPending = false;
        refreshDisplayWorkingDirectory();
    });
}

void TerminalSession::contextChanged(vtbackend::ContextId /*activeContext*/)
{
    // An OSC 3008 context carries a cwd too, which the resolver prefers over OSC 7.
    workingDirectoryChanged();
}

void TerminalSession::refreshDisplayWorkingDirectory()
{
    auto shown = QString::fromStdString(shownWorkingDirectory(
        _manager != nullptr ? std::string_view { _manager->homeDirectory() } : std::string_view {}));
    if (shown == _shownWorkingDirectory)
        return;
    _shownWorkingDirectory = std::move(shown);
    emit displayWorkingDirectoryChanged();
}

std::string TerminalSession::shownWorkingDirectory(std::string_view home) const
{
    // Purely presentational, which is why it happens here and not at the source: a caller that wants a
    // path to act on wants the real one.
    auto const resolved = resolveWorkingDirectory(vtbackend::CwdPurpose::Display);
    if (!resolved) // Nothing reported: the directory the session started in, which is on this machine.
        return vtbackend::abbreviateHomePath(displayWorkingDirectory(), home);

    return vtbackend::sanitizeCommandLine(vtbackend::homeAbbreviatedPath(*resolved, home),
                                          vtbackend::SanitizePurpose::Display);
}

void TerminalSession::setPaneProgramStatus(window::TabBadgeKind kind, QString text)
{
    if (kind == _paneProgramStatus && text == _paneProgramStatusText)
        return;
    _paneProgramStatus = kind;
    _paneProgramStatusText = std::move(text);
    emit paneProgramStatusChanged();
}

void TerminalSession::commandBlockFinished(vtbackend::CommandBlockSummary const& summary)
{
    // Same threading constraint as progressChanged() above: the parser thread, with the state mutex
    // held. The summary is a value copy made for exactly this hop; nothing here reads the terminal.
    // Gathered: a call queued per finish would hold the GUI thread's queue to the length of the input.
    // postToObject targets `this`, so Qt cancels the queued call if the session dies first.
    auto const lock = std::scoped_lock { _pendingBlockEventsMutex };
    if (_pendingBlockEvents.noteFinished(summary, _pendingBlockEventsLimit) == BlockEventsCall::Needed)
        platform::postToObject(this, [this]() { handlePendingBlockEvents(); });
}

TerminalSession::BlockEventsCall TerminalSession::PendingBlockEvents::noteFinished(
    vtbackend::CommandBlockSummary const& summary, size_t limit)
{
    auto const call = empty() ? BlockEventsCall::Needed : BlockEventsCall::Queued;
    if (finished.size() == limit)
        finished.pop_front(); // as the store forgets its oldest records
    finished.push_back(summary);
    return call;
}

TerminalSession::BlockEventsCall TerminalSession::PendingBlockEvents::noteChanged(
    vtbackend::CommandBlockChange const& change, size_t limit)
{
    auto const call = empty() ? BlockEventsCall::Needed : BlockEventsCall::Queued;
    lastCommand = change.lastCommand;
    if (changedRecords == ChangedRecords::Unlisted)
        return call;

    // Ascending, and nearly always appended: a change is mostly the newest record's.
    auto const at = std::ranges::lower_bound(changed, change.id);
    if (at != changed.end() && *at == change.id)
        return call;
    if (changed.size() == limit)
    {
        changedRecords = ChangedRecords::Unlisted;
        changed = {};
        return call;
    }
    changed.insert(at, change.id);
    changedRecords = ChangedRecords::Listed;
    return call;
}

void TerminalSession::handlePendingBlockEvents()
{
    auto const events = [this]() {
        auto const lock = std::scoped_lock { _pendingBlockEventsMutex };
        return std::exchange(_pendingBlockEvents, {});
    }();

    // By the time a block finished, its own start had told a `next` notice of an older block to go: so it is
    // here, before the finish raises one in its place.
    for (auto const& summary: events.finished)
    {
        withdrawFinishNoticeIfNextCommandStarted(summary.id);
        handleCommandBlockFinished(summary);
    }

    if (events.changedRecords == ChangedRecords::None)
        return;
    // The signal has QML read the tooltip's text afresh.
    if (_hoveredBlock
        && (events.changedRecords == ChangedRecords::Unlisted
            || std::ranges::binary_search(events.changed, *_hoveredBlock)))
        emit commandBlockHoverChanged();
    withdrawFinishNoticeIfNextCommandStarted(events.lastCommand);
}

void TerminalSession::handleCommandBlockFinished(vtbackend::CommandBlockSummary const& summary)
{
    // A standalone session has no manager to place it on screen, and no window to tell.
    if (_manager == nullptr)
        return;

    // The tab hears of every ending first, whatever the notice policy below makes of it.
    _manager->noteCommandFinished(*this, vtbackend::commandEndingOf(summary.exitCode, summary.outcome));

    auto const& policy = _profile.notifyOnCommandFinish.value();

    // Decided before anything is gathered for the words: the floor rejects nearly every command, and the
    // tab's title is read under its active pane's terminal lock, which that pane's parser may be holding.
    auto const ending = platform::finishNoticeKind(summary, policy, _manager->sessionVisibility(*this));
    if (!ending)
        return;

    // The home is the one the tab tooltip shortens against, so the two never disagree about what `~` is.
    deliverFinishNotice(
        summary.id,
        platform::finishNotificationText(
            summary, *ending, _manager->tabTitleForSession(*this), _manager->homeDirectory(), policy.action),
        policy.clearOn);
}

void TerminalSession::deliverFinishNotice(vtbackend::CommandBlockId block,
                                          platform::FinishNotificationRequest const& request,
                                          config::NotifyClearOn clearOn)
{
    auto const effects = platform::effectsOf(request.action);

    if (effects.test(platform::FinishEffect::Notification))
    {
        // The same identifier every time, so the desktop replaces this session's previous finish
        // notification rather than stacking a second one under it. Where there is no desktop, the
        // notifier hands it to the hosting window's tray.
        auto notification = vtbackend::DesktopNotification {};
        notification.identifier = platform::finishNotificationIdentifier(_id);
        notification.title = request.title;
        notification.body = request.body;
        _desktopNotifier->notify(notification);
        _finishNotice = FinishNotice { .block = block, .clearOn = clearOn };
    }

    if (effects.test(platform::FinishEffect::Bell))
    {
        auto const& bell = _profile.bell.value();
        _manager->requestWindowBell(*this, getBellSource(), static_cast<double>(bell.volume));
        if (bell.alert)
            _manager->requestWindowAlert(*this);
    }
}

void TerminalSession::withdrawFinishNotice()
{
    if (!_finishNotice)
        return;

    forgetFinishNotice();
    // Through the notifier on every platform: a tray message cannot be withdrawn, so where the notice
    // went to a tray this does nothing.
    _desktopNotifier->close(platform::finishNotificationIdentifier(_id));
}

void TerminalSession::forgetFinishNotice() noexcept
{
    _finishNotice.reset();
}

bool TerminalSession::finishNoticeClearsOn(config::NotifyClearOn trigger) const noexcept
{
    return _finishNotice.has_value() && _finishNotice->clearOn == trigger;
}

void TerminalSession::withdrawFinishNoticeIfNextCommandStarted(vtbackend::CommandBlockId lastCommand)
{
    if (!finishNoticeClearsOn(config::NotifyClearOn::Next))
        return;

    // The newest record that ran a command -- still running, or finished since, a newer prompt having
    // closed it without a reported end -- is the notice's own block until a later command starts. A new
    // prompt alone starts nothing: the user may yet walk away from it. With none at all, a reset forgot the
    // block since: `reset` itself, say, whose own record went with it.
    if (lastCommand != _finishNotice->block)
        withdrawFinishNotice();
}

void TerminalSession::connectFinishNoticeEvents()
{
    // Persistent rather than once per notification, as the OSC 99 handlers connect: every finish
    // notification of this session carries the same identifier, so one pair of connections serves all of
    // them, and the OSC 99 handlers sharing these signals filter by their own identifiers.
    auto const identifier = platform::finishNotificationIdentifier(_id);

    connectMatching(_desktopNotifier.get(), this, &platform::Notifier::actionInvoked, identifier, [this]() {
        if (!_finishNotice)
            return;
        forgetFinishNotice();
        _manager->revealSession(*this);
    });

    // Only a close the desktop reported retires it. One a timer merely assumed is no evidence the notice
    // left the desktop, where it may still be withdrawn and clicked.
    connectMatching(_desktopNotifier.get(),
                    this,
                    &platform::Notifier::notificationClosed,
                    identifier,
                    [this](uint /*reason*/, vtbackend::CloseReport report) {
                        if (report == vtbackend::CloseReport::Observed)
                            forgetFinishNotice();
                    });

    // Whatever ends the session -- its shell exiting, its pane or tab closed -- leaves the notice nothing
    // to wait for: neither clear_on event can happen any more, and a click has no pane to bring forward.
    // One that would have been withdrawn is withdrawn now; one left to the desktop is forgotten.
    connect(this, &TerminalSession::sessionClosed, this, [this]() {
        if (finishNoticeClearsOn(config::NotifyClearOn::Never))
            forgetFinishNotice();
        else
            withdrawFinishNotice();
    });
}

void TerminalSession::commandBlockChanged(vtbackend::CommandBlockChange const& change)
{
    // Raised on the parser thread with the terminal's state mutex held, like progressChanged() above, and
    // the hovered block and the finish notification are the GUI thread's: gathered for it, as a finish is.
    // The newest record that ran a command comes with the change, so nothing here reads the store: a read
    // re-heads its blocks first, which after a prompt that took the row of the block before can walk the
    // whole history -- once per change of a flood of them.
    auto const lock = std::scoped_lock { _pendingBlockEventsMutex };
    if (_pendingBlockEvents.noteChanged(change, _pendingBlockEventsLimit) == BlockEventsCall::Needed)
        platform::postToObject(this, [this]() { handlePendingBlockEvents(); });
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

void TerminalSession::userMarkRequested(vtbackend::UserMarkRequest const& request)
{
    platform::postToObject(this, [this, request]() {
        auto const hosted = HostedOperationRequest {
            .operation = request.change == UserMarkChange::Added ? HostedOperation::SetUserMark
                                                                 : HostedOperation::ClearUserMark,
            .row = HostedRow { .stableId = request.stableId, .generation = request.generation },
        };
        if (routeHostedOperation(hosted) != HostedOperationRoute::RunHere)
            return;
        core::locked(_terminal, [&]() {
            _terminal.placeUserMark(request);
            _terminal.screenUpdated();
        });
    });
}

void TerminalSession::searchPromptRequested()
{
    // Reached with the terminal's state mutex HELD (SearchReverse takes it, and `/` arrives inside
    // sendCharEvent). A queued connection is therefore mandatory: the find bar reads terminal state as
    // it opens, and a direct call would re-enter that non-recursive mutex on this very thread.
    platform::postToObject(this, [this]() { emit searchBarRequested(); });
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
        // Where the terminal cursor is on its page, not on the status line DECSASD may have selected.
        if (!restingOnAVisibleMatch && _terminal.inputHandler().mode() == vtbackend::ViMode::Insert)
            _terminal.moveNormalModeCursorTo(_terminal.pageAt(_terminal.cursorPageIndex()).cursor().position);
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
    // And the count being walked: its posted steps find the ticket moved on. A jump it was to make goes
    // with it, or reopening the bar would move the cursor nobody asked to move.
    _searchWalk.reset();
    ++_searchWalkTicket;
    _pendingSearchJump = PendingSearchJump::None;

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

    platform::postToObject(this, [this] { refreshSearchStatus(); });
}

void TerminalSession::searchQueryEnded()
{
    // A pattern too complex to match, met wherever: by a step of the walk, a count, or a frame being drawn --
    // the last on the render thread, under the state mutex. Posted for that reason; the refresh reads
    // Search::problem and says so in the count slot.
    platform::postToObject(this, [this] { refreshSearchStatus(); });
}

void TerminalSession::setSearchPattern(QString const& pattern)
{
    // Locked: in a Visual mode the vi cursor following the match extends the selection, whose report is sent
    // once the lock is released, as SelectAll's is.
    {
        auto const _ = std::scoped_lock { _terminal };
        auto text = pattern.toStdU32String();
        if (text.empty())
            _terminal.clearSearch();
        else
            // Its error, a pattern that does not compile, is held by Search::problem, which the probe reads.
            std::ignore = _terminal.setNewSearchTerm(std::move(text), vtbackend::SearchOrigin::Typed);
        probeSearchLocked();
        _searchPatternText = pattern;
        _searchSummary = renderSearchSummary(_searchStatus);
        refreshSearchFocusedRowsLocked();
    }

    publishSearchState();
    scheduleSearchTally();
}

void TerminalSession::probeSearchLocked()
{
    // A new pattern or new options: whatever was being walked counted something else.
    _searchWalk.reset();
    ++_searchWalkTicket;
    _pendingSearchJump = PendingSearchJump::None;

    auto const& search = _terminal.search();
    if (search.pattern.empty())
    {
        _searchStatus = {};
        return;
    }
    if (search.problem)
    {
        _searchStatus = describeSearchError(*search.problem);
        return;
    }

    // NOT tallied here. Counting walks the whole grid, and this runs on every keystroke, on the GUI thread,
    // with the terminal locked. The probe reads backwards from the cursor for one step's rows at most (R-e)
    // -- which is what makes an incremental search land on the nearest match behind you -- and that is enough
    // to tint the field and enable the buttons. The count follows on the debounce.
    auto const probe = _terminal.probeSearchReverse(_terminal.normalModeCursorPosition());
    if (search.problem) // the probe itself found the pattern too complex
    {
        _searchStatus = describeSearchError(*search.problem);
        return;
    }
    switch (probe.outcome)
    {
        case vtbackend::SearchProbeOutcome::Found:
            // The cursor MUST follow the match: the tally's ordinal, the focused highlight and Enter's
            // starting point all read it.
            _terminal.moveNormalModeCursorTo(*probe.match);
            _searchStatus = describeSearchCounting(MatchPresence::Some);
            break;
        case vtbackend::SearchProbeOutcome::Absent:
            _searchStatus = describeSearchCounting(MatchPresence::None);
            break;
        case vtbackend::SearchProbeOutcome::Undecided:
            // Beyond the probe's reach: the walk, which reads the whole grid anyway, moves the cursor there
            // once it ends -- unless the user moved it meanwhile.
            _pendingSearchJump = PendingSearchJump::ToNearest;
            _searchStatus = describeSearchCounting(MatchPresence::Unknown);
            break;
    }
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

void TerminalSession::toggleSearchOption(int toggle)
{
    if (toggle < 0 || std::cmp_greater_equal(toggle, SearchToggleRows.size()))
        return;

    {
        auto const _ = std::scoped_lock { _terminal };
        auto const options = nextSearchOptions(SearchToggleRows[static_cast<size_t>(toggle)].toggle,
                                               _terminal.search().options);
        // Its error, a pattern that does not compile under them, is held by Search::problem.
        std::ignore = _terminal.setSearchOptions(options);
        // As for a new pattern: the old match may not be one under the new options.
        probeSearchLocked();
        refreshSearchStatusLocked();
    }
    publishSearchState();
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

    std::ignore = focusAdjacentSearchMatch(edge); // whether it moved is read from the cursor below

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
    //
    // Locked, as setSearchPattern() moves the vi cursor.
    {
        auto const _ = std::scoped_lock { _terminal };
        if (_terminal.search().pattern.empty())
            return;

        // The page on display, the one Terminal::search() reads.
        auto const& screen = _terminal.displayedPage();
        auto const from =
            edge == SearchWrapEdge::Top
                ? CellLocation { .line = -boxed_cast<vtbackend::LineOffset>(screen.historyLineCount()),
                                 .column = vtbackend::ColumnOffset(0) }
                : CellLocation { .line = boxed_cast<vtbackend::LineOffset>(_terminal.pageSize().lines) - 1,
                                 .column =
                                     boxed_cast<vtbackend::ColumnOffset>(_terminal.pageSize().columns) - 1 };

        auto const match =
            edge == SearchWrapEdge::Top ? _terminal.search(from) : _terminal.searchReverse(from);
        if (match)
            _terminal.moveNormalModeCursorTo(*match);
    }
}

void TerminalSession::refreshSearchStatus()
{
    {
        auto const l = scoped_lock { _terminal };
        refreshSearchStatusLocked();
    }
    publishSearchState();
}

void TerminalSession::publishSearchState()
{
    emit searchStateChanged();
    announceSearchFocusedRows();
}

void TerminalSession::announceSearchFocusedRows()
{
    if (_searchFocusedRows == _announcedSearchFocusedRows)
        return;
    _announcedSearchFocusedRows = _searchFocusedRows;
    emit searchFocusedRowsChanged();
}

void TerminalSession::runSearchTallyStep()
{
    auto outcome = WalkStepOutcome::Done;
    {
        auto const l = scoped_lock { _terminal };
        outcome = stepSearchTallyLocked();
        _searchSummary = renderSearchSummary(_searchStatus);
        refreshSearchFocusedRowsLocked();
    }
    // The lock is released here, between steps, so the parser thread is never starved by a deep count.
    publishSearchState();
    if (outcome == WalkStepOutcome::More)
        postSearchTallyStep();
}

void TerminalSession::postSearchTallyStep()
{
    // The next step goes through the event loop rather than a loop here: input, output and painting run in
    // between. A walk replaced since, or a bar closed since, drops it.
    platform::postToObject(this, [this, ticket = _searchWalkTicket]() {
        if (ticket == _searchWalkTicket && _searchWalk && _isSearchBarOpen.load(std::memory_order_relaxed))
            runSearchTallyStep();
    });
}

void TerminalSession::startSearchTallyLocked()
{
    _searchWalkOrigin = _terminal.normalModeCursorPosition();
    _searchWalk = _terminal.beginSearchTally(_searchWalkOrigin);
    ++_searchWalkTicket;
    // The first step inline, under the lock already held: a scrollback within one step settles at once, so
    // stepping through a short one never flashes "…" between two counts.
    if (stepSearchTallyLocked() == WalkStepOutcome::More)
        postSearchTallyStep();
}

TerminalSession::WalkStepOutcome TerminalSession::stepSearchTallyLocked()
{
    if (!_searchWalk)
        return WalkStepOutcome::Done;

    auto const progress = _terminal.continueSearchTally(*_searchWalk);
    if (!progress)
    {
        _searchWalk.reset();
        _searchStatus = describeSearchError(progress.error());
        return WalkStepOutcome::Done;
    }

    auto const& walk = _searchWalk->walk;
    if (*progress == vtbackend::WalkProgress::Running)
    {
        _searchStatus = describeSearchProgress(walk.tally());
        return WalkStepOutcome::More;
    }

    auto tally = walk.tally();
    // The keystroke probe found nothing in its reach; the walk has read everything. Jump where it would have
    // landed: the nearest match at or before the cursor, else the last one -- if the cursor is still where
    // the walk began, so a jump never undoes a move the user made meanwhile.
    if (std::exchange(_pendingSearchJump, PendingSearchJump::None) == PendingSearchJump::ToNearest
        && _terminal.normalModeCursorPosition() == _searchWalkOrigin)
    {
        auto const& grid = _terminal.displayedPage().grid();
        auto const nearest = walk.nearestAtOrBefore(grid);
        auto const target = nearest ? nearest : walk.lastMatch(grid);
        if (target)
        {
            _terminal.moveNormalModeCursorTo(*target);
            _terminal.viewport().makeVisibleWithinSafeArea(target->line);
            tally.ordinal = nearest ? walk.nearestAtOrBeforeOrdinal() : tally.total;
        }
    }
    _searchWalk.reset();
    _searchStatus = describeSearch(_terminal.search().pattern, tally);
    return WalkStepOutcome::Done;
}

QString TerminalSession::renderSearchSummary(SearchStatus const& status) const
{
    switch (status.outcome)
    {
        case SearchOutcome::Idle: return {};
        case SearchOutcome::NoMatch: return tr("No results");
        // Nothing found yet, and the walk still going: the same mark as a count still being walked.
        case SearchOutcome::Pending: return tr("…");
        case SearchOutcome::Invalid:
            return status.error == vtbackend::SearchPatternError::TooComplex ? tr("Pattern too complex")
                                                                             : tr("Invalid pattern");
        case SearchOutcome::Matched: break;
    }

    // The count is still being walked. Saying so beats both a stale number and a blank that fills in
    // a moment later looking like a glitch; what has been counted so far is shown, marked as not final.
    if (status.readiness == CountReadiness::Counting)
    {
        if (status.tally.empty())
            return tr("…");
        if (status.tally.ordinal != 0)
            return tr("%1 of %2…").arg(status.tally.ordinal).arg(status.tally.total);
        return tr("%n match(es)…", nullptr, static_cast<int>(status.tally.total));
    }

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
    core::unreachable();
}

void TerminalSession::publishSearchToggles(vtbackend::SearchOptions options)
{
    _searchCaseTooltip = renderSearchCaseTooltip(options.caseSensitivity);

    auto const tooltipOf = [&](SearchToggle toggle, ToggleState state) -> QString {
        auto const on = state == ToggleState::On;
        switch (toggle)
        {
            case SearchToggle::Case: return _searchCaseTooltip;
            case SearchToggle::Regex:
                return on ? tr("Regular expression: on") : tr("Regular expression: off");
            case SearchToggle::WholeWord: return on ? tr("Whole word: on") : tr("Whole word: off");
        }
        core::unreachable();
    };

    _searchToggles.clear();
    for (auto const& row: SearchToggleRows)
    {
        auto const affordance = describeSearchToggle(row.toggle, options);
        auto const fromView = [](std::string_view text) {
            return QString::fromUtf8(text.data(), static_cast<qsizetype>(text.size()));
        };
        _searchToggles.append(QVariantMap {
            { QStringLiteral("toggle"), static_cast<int>(row.toggle) },
            { QStringLiteral("glyph"), fromView(affordance.glyph) },
            { QStringLiteral("checked"), affordance.state == ToggleState::On },
            { QStringLiteral("tooltip"), tooltipOf(row.toggle, affordance.state) },
            { QStringLiteral("accessibleName"), tr(std::string(row.accessibleName).c_str()) },
            { QStringLiteral("objectName"), fromView(row.objectName) },
        });
    }
}

void TerminalSession::refreshSearchStatusLocked()
{
    auto const& search = _terminal.search();
    publishSearchToggles(search.options);
    // Cached rather than read through on demand: searchPattern() is a Q_PROPERTY whose NOTIFY is
    // searchStateChanged, so a binding woken by that signal reads it -- and if reading it took the
    // terminal lock, every emit made while holding that lock would deadlock. Caching it here makes
    // the whole property group lock-free to read, so the emit is safe wherever it happens.
    _searchPatternText = QString::fromStdU32String(search.pattern);
    refreshSearchFocusedRowsLocked();

    // Skipped entirely while the bar is closed: nothing displays the answer, and walking the
    // scrollback to compute one nobody reads is the cost this guard exists to avoid. The LAST status
    // is left standing rather than replaced by an idle one, so nothing can publish a wrong label --
    // except when the pattern itself is gone, where keeping it IS the wrong label.
    if (!_isSearchBarOpen.load(std::memory_order_relaxed))
    {
        if (search.pattern.empty())
        {
            _searchStatus = {};
            _searchSummary = renderSearchSummary(_searchStatus);
        }
        return;
    }

    if (search.pattern.empty())
    {
        _searchWalk.reset();
        _searchStatus = {};
    }
    else if (search.problem)
    {
        // Every refresh path reads it, so a query ended anywhere -- even while a frame was drawn -- says so.
        _searchWalk.reset();
        _searchStatus = describeSearchError(*search.problem);
    }
    else if (_searchWalk && _searchWalk->walk.query() == search.query
             && _searchWalkOrigin == _terminal.normalModeCursorPosition())
    {
        // A walk of this very query from this very place is under way: output arriving meanwhile does not
        // restart it, or a count longer than the debounce would never finish while output streams (Q4).
    }
    else
        startSearchTallyLocked();
    _searchSummary = renderSearchSummary(_searchStatus);
}

void TerminalSession::refreshSearchFocusedRowsLocked()
{
    auto rows = QVariantMap {};
    if (auto const match = _terminal.focusedSearchMatch())
    {
        // Grid lines to viewport rows: a line scrolled back by the offset shows that many rows lower.
        auto const offset = unbox<int>(_terminal.viewport().scrollOffset());
        auto const lastRow = unbox<int>(_terminal.pageSize().lines) - 1;
        auto const first = unbox<int>(match->first.line) + offset;
        auto const last = unbox<int>(match->second.line) + offset;
        if (last >= 0 && first <= lastRow)
        {
            rows.insert(QStringLiteral("first"), std::max(first, 0));
            rows.insert(QStringLiteral("last"), std::min(last, lastRow));
        }
    }
    _searchFocusedRows = std::move(rows);
}

void TerminalSession::refreshSearchFocusedRows()
{
    _searchFocusedRowsPostPending.clear(std::memory_order_release);
    {
        auto const l = scoped_lock { _terminal };
        refreshSearchFocusedRowsLocked();
    }
    announceSearchFocusedRows();
}
// }}}

void TerminalSession::onScrollOffsetChanged(vtbackend::ScrollOffset value)
{
    // The hovers are recomputed on mouse MOVEMENT only, so scrolling slides a different line -- another
    // link, another block -- under a stationary pointer with nothing noticing. Hide rather than recompute:
    // the terminal's own hover state is stale for the same reason, so re-deriving from it here would only
    // show a different wrong answer. Browsers likewise drop a tooltip on scroll. Posted, because output that
    // scrolls a scrolled-back viewport reports the move from the parser thread, and the hover state is the
    // GUI thread's. postToObject targets `this`, so Qt drops the call if the session dies first.
    platform::postToObject(this, [this]() { clearHovers(); });
    // The current match's viewport rows move with the scroll. Posted for the same reason, and because this
    // runs with the terminal's state mutex held: reading the match takes it. At most one post in flight.
    if (!_searchFocusedRowsPostPending.test_and_set(std::memory_order_acq_rel))
        platform::postToObject(this, [this]() { refreshSearchFocusedRows(); });
    emit scrollOffsetChanged(unbox(value));
}
// }}}
// {{{ Input Events

namespace
{
    /// What became of a key or character event once its binding was looked up.
    enum class BindingOutcome : uint8_t
    {
        PassOn = 0, ///< No binding claimed it: it goes on to the next table, then to the application.
        Claimed,    ///< A binding ran, or swallowed it.
    };

    /// Runs @p actions for a press or a repeat, through the repeat filter.
    ///
    /// A press whose actions all decline is claimed only when @p declined says so. A repeat whose actions
    /// were all filtered out is always claimed: the binding owns the chord, so its repeats must not reach
    /// the application as the chord's own encoding (a held Ctrl+Shift+G would send ^G per repeat).
    /// @param actions The bound actions; nullptr when nothing is bound.
    /// @param eventType Press or Repeat.
    /// @param declined What a press whose actions all declined does.
    /// @param execute Runs a list of actions, returning how many ran.
    /// @return Whether the event was claimed.
    template <typename Execute>
    [[nodiscard]] BindingOutcome runBinding(config::ActionList const* actions,
                                            KeyboardEventType eventType,
                                            config::DeclinedInput declined,
                                            Execute const& execute)
    {
        if (actions == nullptr)
            return BindingOutcome::PassOn;

        // Drop actions that must not fire on auto-repeat (e.g. CloseTab/ClosePane/CreateNewTab). The
        // overwhelmingly common held keys (characters, arrows) bind only repeatable actions, so avoid
        // allocating a filtered copy unless there is actually a non-repeatable action to drop.
        auto executed = 0;
        if (eventType != KeyboardEventType::Repeat
            || std::ranges::none_of(*actions, actions::isNonRepeatable))
            executed = execute(*actions);
        else if (auto const repeatable = actions::filterRepeatableActions(*actions); repeatable.empty())
            return BindingOutcome::Claimed;
        else
            executed = execute(repeatable);

        return executed > 0 || declined == config::DeclinedInput::Consume ? BindingOutcome::Claimed
                                                                          : BindingOutcome::PassOn;
    }

    /// Runs the actions of a built-in row, honouring what the row says a declined press does.
    /// @param row The matching row; nullptr when none matched.
    template <typename Input, typename Execute>
    [[nodiscard]] BindingOutcome runFallbackRow(config::FallbackMapping<Input> const* row,
                                                KeyboardEventType eventType,
                                                Execute const& execute)
    {
        if (row == nullptr)
            return BindingOutcome::PassOn;
        return runBinding(&row->mapping.binding, eventType, row->declined, execute);
    }
} // namespace

void TerminalSession::sendKeyEvent(Key key,
                                   KeyboardModifiers modifiers,
                                   KeyboardEventType eventType,
                                   Timestamp now)
{
    input::inputLog()("Key {} event received: {} {}", eventType, modifiers, key);

    // An ended pane waits for its footer's answer: the input mappings still run first (so the ClosePane
    // binding closes it, through terminate()'s already-closed branch, and Alt+Enter still toggles full
    // screen), an unmodified Return they leave alone is Restart, and nothing reaches the PTY, whose
    // program is gone. GUI-thread state, read without the lock (see _end).
    auto const ended = _end != EndReason::None;

    auto const modifierOnly = vtbackend::isModifierKey(key);
    if (modifierOnly)
        onHeldModifiersChanged(vtbackend::heldAfter(key, eventType, modifiers.chord));

    // A bare modifier is no typing: hiding the pointer then hides the very link Ctrl is held to reach.
    // Guarded for display-less sessions (background pane, headless test), which have no cursor to hide.
    if (_profile.mouse.value().hideWhileTyping && _display != nullptr && !modifierOnly)
        _display->setMouseCursorShape(input::MouseCursorShape::Hidden);

    if (eventType != KeyboardEventType::Release)
    {
        auto const flags = _terminal.bindingModes();
        auto const execute = [this](auto const& actions) {
            return executeAllActions(actions);
        };

        // Key bindings match on the chord: a latched lock key must not change which shortcut fires.
        if (runBinding(config::apply(_config.inputMappings.value().keyMappings, key, modifiers.chord, flags),
                       eventType,
                       config::DeclinedInput::PassThrough,
                       execute)
            == BindingOutcome::Claimed)
            return;

        // The user's mappings did not claim this key, so fall back to the built-in ones. They are
        // consulted second on purpose: an explicit binding in the user's config always wins, because it
        // is found first (see builtinFallbackKeyMappings for why a plain default could not reach an
        // existing user at all).
        if (runFallbackRow(
                config::builtinFallbackRow(_config, key, modifiers.chord, flags), eventType, execute)
            == BindingOutcome::Claimed)
            return;
    }
    if (ended)
    {
        // Only a bare Return: a modified one is a chord the user may bind, and a release or repeat is
        // not a request. The Return's release then reaches the NEW session, whose terminal ignores it:
        // a fresh terminal has negotiated no keyboard protocol that reports releases.
        if (eventType == KeyboardEventType::Press && (key == Key::Enter || key == Key::Numpad_Enter)
            && modifiers.chord.none())
            restart();
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
                      core::escape(unicode::convert_to<char>(value)));

    // As in sendKeyEvent: an ended pane runs the input mappings, and passes no text to the PTY.
    auto const ended = _end != EndReason::None;

    if (_profile.mouse.value().hideWhileTyping && _display != nullptr)
        _display->setMouseCursorShape(input::MouseCursorShape::Hidden);

    if (eventType != KeyboardEventType::Release)
    {
        auto const& charMappings = _config.inputMappings.value().charMappings;
        auto const flags = _terminal.bindingModes();
        auto const execute = [this](auto const& actions) {
            return executeAllActions(actions);
        };

        // Bindings are stored folded (see YAMLConfigReader::parseKeyOrChar), because WHICH case a
        // letter arrives in is decided by the route contour::sendKeyEvent() took rather than by the
        // user — and on macOS the option-as-Alt branch even lets CapsLock decide, which would
        // otherwise let a latched lock key pick the shortcut. Match in the same folded form.
        // NB: only the *lookup* is folded; `value` itself is forwarded verbatim to the terminal
        // below, so what the application receives is unchanged.
        //
        // A shortcut written with the base key label (e.g. `Ctrl+Shift+,`) is stored under the base
        // character, but Qt delivers a Shift+punctuation chord as the *shifted* symbol ('<' here). When
        // the direct lookup misses and Shift is held, retry under the un-shifted base so the binding
        // fires as the user intended. The two normalizations are disjoint — unshiftedCodepoint()
        // rewrites only digits and punctuation, foldedBindingCodepoint() only ASCII letters — so they
        // compose in either order. Both tables are looked up through this one rule.
        auto const folded = config::foldedBindingCodepoint(value);
        auto const lookUp = [&](auto const& find) {
            auto const* found = find(folded);
            if (found == nullptr && modifiers.chord.test(vtbackend::Modifier::Shift))
                if (auto const base = input::unshiftedCodepoint(folded); base != folded)
                    found = find(base);
            return found;
        };

        auto const* bound =
            lookUp([&](char32_t ch) { return config::apply(charMappings, ch, modifiers.chord, flags); });
        if (runBinding(bound, eventType, config::DeclinedInput::PassThrough, execute)
            == BindingOutcome::Claimed)
            return;

        // Then the built-in ones, as for keys: an explicit binding of the same chord in the user's config is
        // found first and wins, and one that declines lets the built-in one try.
        auto const* row = lookUp(
            [&](char32_t ch) { return config::builtinFallbackRow(_config, ch, modifiers.chord, flags); });
        if (runFallbackRow(row, eventType, execute) == BindingOutcome::Claimed)
            return;
    }
    if (ended)
        return;
    terminal().sendCharEvent(value, keyIdentity, modifiers, eventType, now);
}

void TerminalSession::sendMousePressEvent(Modifiers modifiers,
                                          MouseButton button,
                                          CellLocation pos,
                                          PixelCoordinate pixelPosition)
{
    auto const uiHandledHint = false;
    input::inputLog()("Mouse press received: {} {}\n", modifiers, button);

    // A left press on a collapsed fold's label expands the fold, as a click on a folded placeholder does in
    // iTerm2: the label is no text of the page, so neither a selection nor the application sees the press,
    // nor its release. Looked up first, which costs one comparison while nothing is collapsed; expanded
    // through the path that republishes the scrollable count.
    if (button == MouseButton::Left && modifiers.none()
        && core::locked(_terminal, [&]() { return _terminal.collapsedFoldLabelAt(pos).has_value(); }))
    {
        withScrollableChange([&](vtbackend::Terminal& terminal) {
            auto const head = terminal.collapsedFoldLabelAt(pos);
            return head && terminal.expandFoldContaining(*head);
        });
        _owedRelease = { .owner = PressOwner::FoldLabel, .button = button };
        return;
    }

    auto const sanitizedModifier = bindingModifiers(modifiers);

    // A binding that names a click count is offered the press BEFORE the terminal's own multi-click
    // selection acts on it -- only the terminal knows the count, and only until it has used it. So is the
    // follow gesture on a link: its chord is the block-selection modifier by default, and following the link
    // hands focus to the browser, so a selection the press started would never see its release. The
    // claimant only looks the binding up (it runs under the lock); the actions run once it is released.
    //
    // The claimant captures one reference, to this struct, so that it fits std::function's small buffer
    // everywhere: a press allocates nothing for it.
    struct PressClaim
    {
        TerminalSession const& session;
        MouseButton button;
        Modifiers modifiers;
        config::ActionList const* claimed = nullptr;
    };
    auto claim = PressClaim { .session = *this, .button = button, .modifiers = sanitizedModifier };
    // Through runUnderStateLock(): a press the application tracks is reported, and with SRM reset the report
    // is echoed, which parses. See ViNormalMode.
    auto const handledByApp = _terminal.runUnderStateLock([&]() {
        return _terminal.sendMousePressEvent(
            modifiers, button, pos, pixelPosition, uiHandledHint, [&claim](uint8_t clickCount) {
                claim.claimed = claim.session.clickCountBinding(claim.button, claim.modifiers, clickCount);
                if (claim.claimed == nullptr)
                    claim.claimed = claim.session.linkFollowBinding(claim.button, claim.modifiers);
                return claim.claimed != nullptr ? vtbackend::MousePressClaim::Claimed
                                                : vtbackend::MousePressClaim::Unclaimed;
            });
    });

    if (claim.claimed != nullptr)
    {
        executeAllActions(*claim.claimed);
        return;
    }

    if (*handledByApp)
        return;

    if (auto const* actions = config::apply(
            _config.inputMappings.value().mouseMappings, button, sanitizedModifier, _terminal.bindingModes()))
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
            config::applyBuiltinFallback(_config, button, sanitizedModifier, _terminal.bindingModes()))
        executeAllActions(*actions);
}

bool TerminalSession::applyFallbackMouseBinding(MouseButton button)
{
    auto const noModifiers = Modifiers { vtbackend::Modifier::None };
    auto const* actions =
        config::applyBuiltinFallback(_config, button, noModifiers, _terminal.bindingModes());
    if (actions == nullptr)
        return false;

    executeAllActions(*actions);
    return true;
}

vtbackend::Modifiers TerminalSession::bindingModifiers(vtbackend::Modifiers modifiers) const
{
    return vtbackend::bindingModifiers(modifiers, _config.bypassMouseProtocolModifiers.value());
}

config::ActionList const* TerminalSession::mouseBinding(MouseButton button, Modifiers modifiers) const
{
    auto const flags = _terminal.bindingModes();
    if (auto const* actions =
            config::apply(_config.inputMappings.value().mouseMappings, button, modifiers, flags))
        return actions;
    return config::applyBuiltinFallback(_config, button, modifiers, flags);
}

config::ActionList const* TerminalSession::linkFollowBinding(MouseButton button, Modifiers modifiers) const
{
    // The follow gesture is a left click (config::followHyperlinkModifiers). The binding first: it is cheap,
    // and most presses are no follow gesture at all.
    if (button != MouseButton::Left)
        return nullptr;
    auto const* binding = mouseBinding(button, modifiers);
    if (binding == nullptr || !config::containsAction<actions::FollowHyperlink>(*binding)
        || !willRun(*binding))
        return nullptr;

    // Then the cell as it is now, as the follow itself will look it up -- not the hover state of the last
    // pointer event, which output or a scroll under a still pointer leaves stale, and which knows nothing of
    // a row detection has not scanned yet. Only a link that opens at once is claimed: an OSC 8 one, or a
    // target known now. A path still to be probed opens later, if it exists at all, so a Ctrl+drag starting
    // on one that does not still block-selects.
    auto const position = _terminal.currentMouseGridPosition();
    if (!position)
        return nullptr;
    auto const lookup = _terminal.lookUpLinkAt(*position);
    if (lookup.hyperlink == nullptr && !lookup.target)
        return nullptr;
    return binding;
}

config::ActionList const* TerminalSession::clickCountBinding(MouseButton button,
                                                             Modifiers modifiers,
                                                             uint8_t clickCount) const
{
    // The terminal offers every press, wheel steps included; only a counted button's can match a count.
    if (!vtbackend::ClickCounter::countsClicks(button))
        return nullptr;

    auto const flags = _terminal.bindingModes();
    auto const* actions =
        config::apply(_config.inputMappings.value().mouseMappings, button, modifiers, flags, clickCount);
    if (actions == nullptr)
        actions = config::applyBuiltinFallback(_config, button, modifiers, flags, clickCount);

    // A binding that will not run must not claim: the press would skip the terminal's own selection and
    // then do nothing at all.
    if (actions != nullptr && !willRun(*actions))
        return nullptr;
    return actions;
}

bool TerminalSession::willRun(config::ActionList const& actions) const
{
    return _allowKeyMappings || config::containsAction<actions::ToggleAllKeyMaps>(actions);
}

void TerminalSession::sendMouseMoveEvent(vtbackend::Modifiers modifiers,
                                         vtbackend::CellLocation pos,
                                         vtbackend::PixelCoordinate pixelPosition)
{
    // NB: This translation depends on the display's margin, so maybe
    //     the display should provide the translation?

    // The main page, read race-free: this runs on the GUI thread, without the terminal lock.
    if (!(pos < terminal().mainPageSize()))
        return;

    auto constexpr UiHandledHint = false;

    // The cursor shape lives on the display; a display-less session (background pane, headless
    // test) has no cursor to change.
    //
    // Normally only on a cell CHANGE, since that is the only thing that can change the answer -- but
    // the gutter changes the shape behind this decision's back, and leaving it lands the pointer on the
    // cell it came from. The flag is that invalidation: without it the gutter's pointing hand survived
    // over the grid until the pointer happened to cross into another cell.
    //
    // A move that carries another chord than the terminal last knew of changes the answer in place too (Ctrl
    // went down while another window had the focus, so no key event came here): the lambda below applies the
    // chord first and reports it.
    auto const decidesPointerShapeForCell =
        (pos != _pointerShapeCell || _isPointerShapeStale) && _display != nullptr;

    // The hovered link is read inside the lock the move already takes, rather than asked for again
    // afterwards. tryGetHoveringHyperlink() re-queries the screen and needs the same non-recursive
    // mutex, so a second acquisition here would be one more chance to deadlock, and a callback fired
    // from vtbackend's own hover-state update would run WITH the lock held and self-deadlock outright.
    // The pointer's shape is decided in there too: whether it rests on a link or a collapsed fold's label
    // asks the link index and the fold cache, which the parser thread and the render fill change. Neither
    // asks the filesystem: a printed path is a link only once detection has found the file, off this thread.
    struct Hover
    {
        /// What the tooltip names: the OSC 8 link's URI, or a detected link's target where it differs from
        /// its text; empty over none.
        std::string uri;
        std::optional<input::MouseCursorShape> targetShape; ///< Over something to open; else the default.
        bool decidesPointerShape = false;                   ///< Whether this move decides the shape.
    };
    // Through runUnderStateLock(): a move the application tracks is reported, as a press is.
    auto const hover = _terminal.runUnderStateLock([&]() -> Hover {
        // The chord first, so that the shape below is read for the chord this move carries; the terminal's
        // own pass over it in sendMouseMoveEvent() then finds nothing new.
        auto const chordChanged =
            _terminal.updateHeldModifiers(modifiers) == vtbackend::LinkRevealChange::Changed;
        auto const decidesPointerShape = decidesPointerShapeForCell || (chordChanged && _display != nullptr);
        _terminal.sendMouseMoveEvent(modifiers, pos, pixelPosition, UiHandledHint);
        // Read only on a new cell, where the tooltip is updated (below).
        auto const tooltipTarget = [&]() -> std::string {
            if (!decidesPointerShapeForCell)
                return {};
            if (auto const link = _terminal.tryGetHoveringHyperlink())
                return link->uri;
            return hoveredDetectedLinkTooltip();
        };
        auto const pointsAtTarget = decidesPointerShape
                                    && (_terminal.linkPointerShape() == vtbackend::LinkPointerShape::Hand
                                        || _terminal.collapsedFoldLabelAt(pos).has_value());
        return Hover { .uri = tooltipTarget(),
                       .targetShape = pointsAtTarget ? std::optional { input::MouseCursorShape::PointingHand }
                                                     : std::nullopt,
                       .decidesPointerShape = decidesPointerShape };
    });

    if (hover.decidesPointerShape)
    {
        _isPointerShapeStale = false;
        _pointerShapeCell = pos;
        if (hover.targetShape)
            _display->setMouseCursorShape(*hover.targetShape);
        else
            setDefaultCursor();
    }

    // The hover state (tooltip) follows the cell, not the chord.
    if (decidesPointerShapeForCell)
        updateHyperlinkHover(hover.uri, pos);
}

namespace
{
    /// Whether a pointer over @p segment can act on a block, for the hover and the press alike.
    /// @param segment The gutter segment under the pointer.
    /// @return Whether it is the block column, the one segment that carries the fold controls, the status
    ///         marks and the blocks' menus.
    [[nodiscard]] constexpr bool isBlockColumn(vtbackend::GutterSegmentKind segment) noexcept
    {
        return segment == vtbackend::GutterSegmentKind::BlockColumn;
    }

    /// The block beside @p gridLine in the gutter's block column.
    ///
    /// Only the primary page has command blocks. Beside any other -- a full-screen application's -- the block
    /// column is empty, and the primary page's block at the same row is not what is beside it.
    /// @param terminal The terminal, locked.
    /// @param gridLine The grid line beside the pointer.
    /// @return The block's id, or nullopt beside none.
    [[nodiscard]] std::optional<vtbackend::CommandBlockId> blockBeside(vtbackend::Terminal const& terminal,
                                                                       vtbackend::LineOffset gridLine)
    {
        if (!terminal.foldingAppliesToDisplayedPage())
            return std::nullopt;
        auto const* const record = terminal.commandBlockAt(gridLine);
        return record != nullptr ? std::optional { record->id } : std::nullopt;
    }
} // namespace

ConsumedByGutter TerminalSession::sendGutterHoverEvent(std::optional<GutterHit> hit)
{
    // A display-less session has no cursor to change and no gutter to be over.
    if (_display == nullptr)
        return ConsumedByGutter::No;

    // The overwhelmingly common case -- the pointer is over the GRID, and with no gutter configured it
    // is the only case. Answered before taking the terminal lock, because this runs on every pointer
    // motion and the ordinary move path is about to take that same lock itself.
    if (!hit)
    {
        clearGutterHover();
        return ConsumedByGutter::No;
    }

    _gutterHovered = true;

    // Beside the timestamps and the line numbers there is nothing to light and nothing to click -- but
    // the pointer is not over the grid either, so the event is consumed all the same.
    auto const onBlockColumn = isBlockColumn(hit->segment);
    auto const [clickable, block] = core::locked(_terminal, [&] {
        _terminal.setGutterHoverLine(onBlockColumn ? std::optional { hit->gridLine } : std::nullopt);
        if (!onBlockColumn)
            return std::pair { false, std::optional<vtbackend::CommandBlockId> {} };
        return std::pair { _terminal.blockColumnPressAt(hit->gridLine)
                               != vtbackend::BlockColumnPress::Swallow,
                           blockBeside(_terminal, hit->gridLine) };
    });

    // The tooltip describes a BLOCK, so it follows the block column only, and moves on only when the
    // pointer reaches another block.
    updateCommandBlockHover(block, hit->screenRow);

    // Consumed either way -- only the shape differs, saying whether there is anything here to click.
    if (clickable)
        _display->setMouseCursorShape(input::MouseCursorShape::PointingHand);
    else
        setDefaultCursor();

    // Leaving the grid ends any hyperlink hover the pointer left behind on its way out.
    leaveGrid();
    return ConsumedByGutter::Yes;
}

ConsumedByGutter TerminalSession::sendGutterPressEvent(std::optional<GutterHit> hit,
                                                       vtbackend::MouseButton button,
                                                       vtbackend::Modifiers modifiers)
{
    if (!hit)
        return ConsumedByGutter::No;

    // What the hover offered, asked again: what a left press does here, and the block beside it.
    struct Target
    {
        vtbackend::BlockColumnPress press = vtbackend::BlockColumnPress::Swallow;
        std::optional<vtbackend::CommandBlockId> block {};
    };
    auto const target = [&] {
        if (!isBlockColumn(hit->segment))
            return Target {};
        return core::locked(_terminal, [&] {
            return Target { .press = _terminal.blockColumnPressAt(hit->gridLine),
                            .block = blockBeside(_terminal, hit->gridLine) };
        });
    }();

    if (button == vtbackend::MouseButton::Left)
    {
        // The fold toggled through the one path every folding action takes, which republishes the scrollable
        // count. Looked up again there, under that lock: the row may have changed since it was asked about.
        if (target.press == vtbackend::BlockColumnPress::ToggleFold)
            withScrollableChange(
                [&](vtbackend::Terminal& terminal) { return terminal.toggleFoldContaining(hit->gridLine); });

        // The press is the gutter's wherever it lands, acted on or not: beside the timestamps, on a blank
        // row, or with folding off, neither the child nor a selection may see it -- nor its release. Let
        // through, it reached them as a press on column 0 of the row.
        //
        // Remembered here rather than by the caller, so the two halves of the handshake cannot drift: a
        // release reported for a press the child never saw leaves it holding a button down that was never
        // pressed. Owed before the menu opens, whose popup takes the release (@see dropOwedRelease).
        _owedRelease = { .owner = PressOwner::Gutter, .button = button };
        if (target.press == vtbackend::BlockColumnPress::OpenBlockMenu && target.block)
            openBlockMenu(*target.block);
        return ConsumedByGutter::Yes;
    }

    // Any other button over a block on the block column does what its binding does over the block's own
    // rows, for that block -- a right press opens its context menu -- as on the sticky header, and the
    // application never hears of it. Anywhere else in the gutter it is the application's, as it always was.
    if (!target.block)
        return ConsumedByGutter::No;

    _owedRelease = { .owner = PressOwner::Gutter, .button = button };
    runBindingForBlock(button, modifiers, *target.block);
    return ConsumedByGutter::Yes;
}

void TerminalSession::runBindingForBlock(vtbackend::MouseButton button,
                                         vtbackend::Modifiers modifiers,
                                         vtbackend::CommandBlockId block)
{
    // Outside the lock, as the grid's bindings run.
    if (auto const* actions = mouseBinding(button, bindingModifiers(modifiers)))
        withContextMenuBlock(block, [&] { executeAllActions(*actions); });
}

void TerminalSession::openBlockMenu(vtbackend::CommandBlockId block)
{
    withContextMenuBlock(block, [this] { (*this)(actions::OpenContextMenu {}); });
}

void TerminalSession::withContextMenuBlock(vtbackend::CommandBlockId block, std::invocable auto&& action)
{
    _contextMenuBlock = block;
    auto const forget = core::Finally { [this] { _contextMenuBlock = {}; } };
    std::invoke(action);
}

PressOwner TerminalSession::sendOwedRelease(vtbackend::MouseButton button)
{
    if (_owedRelease.button != button)
        return PressOwner::None;
    return std::exchange(_owedRelease.owner, PressOwner::None);
}

void TerminalSession::dropOwedRelease()
{
    _owedRelease.owner = PressOwner::None;
}

ConsumedByStickyHeader TerminalSession::sendStickyHeaderPressEvent(
    std::optional<vtbackend::LineOffset> mainPageRow,
    vtbackend::MouseButton button,
    vtbackend::Modifiers modifiers)
{
    // The header covers row 0 and nothing else.
    if (mainPageRow != vtbackend::LineOffset(0))
        return ConsumedByStickyHeader::No;

    // Through the path that takes the lock and announces the scrollable count, which the jump changes when
    // it expands a collapsed fold. The block is read under that lock, through stickyHeaderBlock(): what the
    // last frame drew, which is what the user pressed on.
    auto block = std::optional<vtbackend::CommandBlockId> {};
    auto const consumed = withScrollableChange([&](vtbackend::Terminal& terminal) {
        block = terminal.stickyHeaderBlock();
        if (!block)
            return false;
        // An evicted head has nowhere to go: the jump answers HeadEvicted and the viewport stays. The press
        // is consumed all the same -- the placeholder covers row 0 wherever it shows.
        if (button == vtbackend::MouseButton::Left)
            std::ignore = terminal.scrollToCommandBlockHead(*block);
        return true;
    });

    if (!consumed)
        return ConsumedByStickyHeader::No;

    // Owed before any binding runs: a popup it opens takes the release, and ends the grab that says so
    // (@see dropOwedRelease) while the binding runs.
    _owedRelease = { .owner = PressOwner::StickyHeader, .button = button };

    // Any other button does what its binding does over the block's own rows. The header stands for its
    // block, so a context menu describes that block.
    if (button != vtbackend::MouseButton::Left)
        runBindingForBlock(button, modifiers, *block);
    return ConsumedByStickyHeader::Yes;
}

ConsumedByStickyHeader TerminalSession::sendStickyHeaderHoverEvent(
    std::optional<vtbackend::LineOffset> mainPageRow)
{
    // Row 0 only, so a motion anywhere else pays nothing more; and a display-less session has no pointer.
    if (_display == nullptr || mainPageRow != vtbackend::LineOffset(0))
        return ConsumedByStickyHeader::No;

    // The header the frame on screen drew, as the press reads it.
    if (!core::locked(_terminal, [&] { return _terminal.stickyHeaderBlock().has_value(); }))
        return ConsumedByStickyHeader::No;

    leaveGrid();
    _display->setMouseCursorShape(input::MouseCursorShape::Arrow);
    return ConsumedByStickyHeader::Yes;
}

namespace
{
    /// The most codepoints a link's tooltip shows; a longer target is elided in its middle.
    constexpr auto MaxTooltipLength = size_t { 100 };
} // namespace

std::string const& TerminalSession::hoveredDetectedLinkTooltip()
{
    // A new link is read by pointer, not copied; within the one last read, nothing is read at all. Keyed on
    // the index too, as a new detector's ids start over; and kept elided, so a move along one link copies
    // at most a tooltip's worth.
    auto const id = _terminal.hoveringLinkId();
    auto const* const index = _terminal.detectedLinks();
    if (id != _detectedLinkTooltip.link || index != _detectedLinkTooltip.index)
    {
        auto const* const link = index != nullptr ? index->find(id) : nullptr;
        _detectedLinkTooltip = DetectedLinkTooltip {
            .index = index,
            .link = id,
            .text =
                link != nullptr ? elideMiddle(detectedLinkTooltip(*link), MaxTooltipLength) : std::string {},
        };
    }
    return _detectedLinkTooltip.text;
}

void TerminalSession::updateHyperlinkHover(std::string_view uri, vtbackend::CellLocation cell)
{
    if (!_config.hyperlinkHoverTooltip.value())
        return;

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

    core::locked(_terminal, [&] { _terminal.setGutterHoverLine(std::nullopt); });
    updateCommandBlockHover(std::nullopt, vtbackend::LineOffset(0));
}

void TerminalSession::updateCommandBlockHover(std::optional<vtbackend::CommandBlockId> block,
                                              vtbackend::LineOffset screenRow)
{
    if (_hoveredBlock == block)
        return;

    _hoveredBlock = block;
    // The block column is the gutter's last column, -1, whatever else the gutter shows.
    _commandBlockTooltipAnchor =
        block && _display != nullptr
            ? geometry::cellRectangle(
                  _display->gridMetrics().pageMargin,
                  _display->cellSize(),
                  vtbackend::CellLocation { .line = screenRow, .column = vtbackend::ColumnOffset(-1) },
                  1,
                  _display->devicePixelRatio())
            : QRectF {};
    emit commandBlockHoverChanged();
}

QString TerminalSession::commandBlockTooltip() const
{
    if (!_hoveredBlock)
        return {};

    auto const lock = std::scoped_lock { _terminal };
    auto const* record = _terminal.commandBlocks().find(*_hoveredBlock);
    if (record == nullptr)
        return {};

    // When the command started; a block that never reached its command has only its prompt's time, and one
    // a ;C opened only its command's.
    auto const startedAt =
        _terminal.localTimeOf(record->commandStartedAt.or_else([&] { return record->promptStartedAt; })
                                  .value_or(std::chrono::system_clock::time_point {}));
    auto text = commandBlockTooltipText(*record, startedAt);
    if (text.empty())
        return {};

    // What a click beside its head does: the fold hangs there, and so does the status mark.
    auto const hint = [&] {
        auto const head = vtbackend::headOffsetOf(_terminal.primaryScreen().grid(), *record);
        if (!head)
            return BlockTooltipHint::RightClickOnly;
        switch (_terminal.blockColumnPressAt(*head))
        {
            case vtbackend::BlockColumnPress::ToggleFold:
                return _terminal.foldState().isCollapsed(record->headStableId) ? BlockTooltipHint::Expand
                                                                               : BlockTooltipHint::Collapse;
            case vtbackend::BlockColumnPress::OpenBlockMenu: return BlockTooltipHint::Menu;
            case vtbackend::BlockColumnPress::Swallow: return BlockTooltipHint::RightClickOnly;
        }
        return BlockTooltipHint::RightClickOnly;
    }();
    text += '\n';
    text += blockTooltipHintText(hint);
    return QString::fromStdString(text);
}

void TerminalSession::announceScrollableLineCount(vtbackend::LineCount scrollable)
{
    if (std::exchange(_lastHistoryLineCount, unbox(scrollable)) == unbox(scrollable))
        return;

    emit historyLineCountChanged(unbox(scrollable));
}

void TerminalSession::setScrollbarMarkTrackPixels(int pixels)
{
    auto const track = std::max(pixels, 0);
    if (track == _scrollbarMarkTrackPixels)
        return;

    _scrollbarMarkTrackPixels = track;
    requestScrollbarMarksRefresh();
}

void TerminalSession::requestScrollbarMarksRefresh()
{
    if (_scrollbarMarksPostPending.test_and_set(std::memory_order_acq_rel))
        return;

    // Posted to the session rather than through the display, so a request made while no display is attached
    // still runs once one is. The timer belongs to the GUI thread, so it is started there; a request made
    // while it already runs rides on that one, which fires within the frame either way.
    platform::postToObject(this, [this]() {
        if (!_scrollbarMarksTimer.isActive())
            _scrollbarMarksTimer.start();
    });
}

void TerminalSession::refreshScrollbarMarks()
{
    // Cleared before anything is read, so output arriving during this refresh schedules the next one.
    _scrollbarMarksPostPending.clear(std::memory_order_release);

    // The ticks belong to the scrollbar: nothing to mark where it is configured away. Read here, on the GUI
    // thread, which is the only one that swaps the profile.
    auto const& scrollbar = _profile.scrollbar.value();
    auto const sources = scrollbar.position == config::ScrollBarPosition::Hidden
                             ? vtbackend::ScrollbarMarkSources {}
                             : scrollbar.marks;

    auto scrollable = vtbackend::LineCount(0);
    auto previousKey = std::optional<ScrollbarMarksKey> {};
    auto inputs = core::locked(_terminal, [&]() -> std::optional<std::vector<vtbackend::ScrollbarMarkInput>> {
        // The terminal says when the marks may have moved, whatever moved them; the rest of the key is the
        // scrollbar's own.
        auto const marksRevision = _terminal.scrollbarMarksRevision();

        // The scrollbar's travel, read whether or not the ticks moved: output asks for this refresh to
        // publish it, as screenUpdated() may not count it.
        scrollable = _terminal.viewport().scrollableLineCount();

        auto const key = ScrollbarMarksKey {
            .marksRevision = marksRevision,
            .visibleRowCount = unbox<int64_t>(scrollable) + unbox<int64_t>(_terminal.pageSize().lines),
            .trackPixels = _scrollbarMarkTrackPixels,
            .sources = sources,
            .colors = scrollbarMarkColorsOf(_terminal.colorPalette()),
        };
        if (_scrollbarMarksKey == key)
            return std::nullopt;
        previousKey = std::exchange(_scrollbarMarksKey, key);
        _scrollbarTicksGeneration = _terminal.primaryScreen().grid().stableIdGeneration();

        // Nothing would survive the bucketing, so the marks are not copied out only to be dropped.
        if (key.sources.none() || key.trackPixels <= 0)
            return std::vector<vtbackend::ScrollbarMarkInput> {};
        return _terminal.scrollbarMarkInputs();
    });

    // Outside the lock, as announceScrollableLineCount() requires.
    announceScrollableLineCount(scrollable);
    if (!inputs)
        return;

    // Outside the lock: bucketing reads nothing the parser thread writes, and the signal below wakes QML
    // bindings that reach back into this session.
    //
    // Most moves of the key move no tick: in a deep history a line of output moves every row, while a tick
    // crosses a pixel only every few dozen lines. So the lists QML reads, and every binding over them, are
    // rebuilt only for what changed. A tick is a pixel of the track, so a new track height is a new list
    // even where the pixels came out the same.
    auto const& key = *_scrollbarMarksKey;
    auto ticks = vtbackend::scrollbarMarks(*inputs, key.visibleRowCount, key.trackPixels, key.sources);
    auto const ticksMoved =
        !previousKey || previousKey->trackPixels != key.trackPixels || ticks != _scrollbarTicks;
    auto const colorsMoved = !previousKey || previousKey->colors != key.colors;
    if (!ticksMoved && !colorsMoved)
        return;

    if (ticksMoved)
    {
        _scrollbarMarks = toScrollbarMarkList(ticks, key.trackPixels);
        _scrollbarTicks = std::move(ticks);
    }
    if (colorsMoved)
        _scrollbarMarkColors = toScrollbarMarkColorList(key.colors);
    emit scrollbarMarksChanged();
}

void TerminalSession::onPointerLeft()
{
    clearHovers();
    leaveGrid();
    setDefaultCursor();
}

void TerminalSession::clearHovers()
{
    clearHyperlinkHover();
    clearGutterHover();
}

void TerminalSession::leaveGrid()
{
    clearHyperlinkHover();
    core::locked(_terminal, [&] { _terminal.sendMouseLeaveEvent(); });

    // The shape is decided per grid CELL, and the way back may land on the very cell the pointer left from.
    _isPointerShapeStale = true;
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
    // Locked: a release the application tracks is reported, as a press is.
    {
        auto const _ = std::scoped_lock { _terminal };
        auto const uiHandledHint = false;
        _terminal.sendMouseReleaseEvent(modifiers, button, pixelPosition, uiHandledHint);
    }
    scheduleRedraw();
}

void TerminalSession::performAutoScroll(int direction, vtbackend::LineCount lineCount)
{
    // Locked: the scroll extends the selection, whose report is sent once the lock is released, as
    // SelectAll's is.
    {
        auto const _ = std::scoped_lock { _terminal };
        _terminal.performAutoScroll(direction, lineCount);
    }
}

void TerminalSession::sendFocusInEvent()
{
    // as per Qt-documentation, some platform implementations reset the cursor when leaving the
    // window, so we have to re-apply our desired cursor in focusInEvent().
    setDefaultCursor();

    terminal().sendFocusInEvent();

    if (_display)
        _display->setBlurBehind(_profile.background.value().blur);

    paneSeen();

    scheduleRedraw();
}

void TerminalSession::paneSeen()
{
    if (finishNoticeClearsOn(config::NotifyClearOn::Focus))
        withdrawFinishNotice();
}

void TerminalSession::onHeldModifiersChanged(vtbackend::Modifiers held)
{
    if (_terminal.runUnderStateLock([&]() { return _terminal.updateHeldModifiers(held); })
        == vtbackend::LinkRevealChange::Changed)
        refreshPointerShape();
}

void TerminalSession::refreshPointerShape()
{
    // Stale: the pointer is off the grid, and the next move onto it decides.
    if (_display == nullptr || _isPointerShapeStale)
        return;
    if (core::locked(_terminal, [&] { return _terminal.linkPointerShape(); })
        == vtbackend::LinkPointerShape::Hand)
        _display->setMouseCursorShape(input::MouseCursorShape::PointingHand);
    else
        setDefaultCursor();
}

void TerminalSession::sendFocusOutEvent()
{
    // TODO maybe paint with "faint" colors

    // Focus left with a modifier down (Ctrl+Alt+Tab) reports no release: nothing is held now.
    onHeldModifiersChanged(vtbackend::Modifiers {});
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
    // it). Through runUnderStateLock(): cancelling a dragged selection returns to Normal mode, which puts the
    // indicator status line up and so resizes. See ViNormalMode for the rationale.
    return _terminal.runUnderStateLock([&]() {
        if (!_terminal.selectionAvailable())
            return false;
        _terminal.clearSelection();
        return true;
    });
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

HostedOperationRoute TerminalSession::routeHostedOperation(HostedOperationRequest const& request)
{
    if (_manager == nullptr)
        return HostedOperationRoute::RunHere;
    auto const route = _manager->requestHostedOperation(&terminal().device(), request);
    auto const what = describe(request.operation);
    auto const refuse = [&](std::string_view reason, QString const& notice) {
        errorLog()("Did not {}: {}.", what, reason);
        emit showNotification(
            tr("Attached Session"),
            notice.arg(QString::fromUtf8(what.data(), static_cast<qsizetype>(what.size()))));
    };
    switch (route)
    {
        case HostedOperationRoute::RunHere:
        case HostedOperationRoute::RunOnHost: break;
        case HostedOperationRoute::Refused:
            refuse("this tab's host has no way to", tr("This tab's host cannot %1, so nothing was done."));
            break;
        case HostedOperationRoute::Unreachable:
            refuse("the connection to this tab's host is gone",
                   tr("The connection to this tab's host is gone, so it could not %1."));
            break;
    }
    return route;
}

template <typename Action>
RemoteDispatch TerminalSession::dispatchRemotely(Action const& action)
{
    switch (routeHostedOperation(hostedOperationRequest(action)))
    {
        case HostedOperationRoute::RunHere: return RemoteDispatch::RunHere;
        case HostedOperationRoute::RunOnHost: return RemoteDispatch::Performed;
        case HostedOperationRoute::Refused:
        case HostedOperationRoute::Unreachable: return RemoteDispatch::Refused;
    }
    return RemoteDispatch::Refused;
}

bool TerminalSession::operator()(actions::ClearHistoryAndReset action)
{
    if (auto const dispatch = dispatchRemotely(action); dispatch != RemoteDispatch::RunHere)
        return dispatch == RemoteDispatch::Performed;

    sessionLog()("Clearing history and perform terminal hard reset");

    // Locked, for the reason spelled out at operator()(SoftReset) below; and through the path that
    // republishes the scrollbar, whose history and blocks the reset takes, with no output to follow it.
    withScrollableChange([](vtbackend::Terminal& terminal) {
        terminal.hardReset();
        return true;
    });
    return true;
}

bool TerminalSession::operator()(actions::ClearToPrompt action)
{
    if (auto const dispatch = dispatchRemotely(action); dispatch != RemoteDispatch::RunHere)
        return dispatch == RemoteDispatch::Performed;

    // It takes rows out of the scrollable range, as folding does, so it takes the path that announces that.
    return withScrollableChange([](auto& terminal) { return terminal.clearToPrompt().has_value(); });
}

bool TerminalSession::operator()(actions::CopyPreviousMarkRange)
{
    core::locked(_terminal, [&]() { copyToClipboard(terminal().extractLastMarkRange()); });
    return true;
}

bool TerminalSession::operator()(actions::SelectAll)
{
    // Locked: with DECSET 2030 a new selection is reported, and a report raised on the GUI thread is in no
    // batch of the parser's, whose end would send it. It is sent once the lock is released.
    {
        auto const _ = std::scoped_lock { _terminal };
        terminal().selectAll();
    }
    return true;
}

bool TerminalSession::operator()(actions::OpenContextMenu)
{
    // Not while a left-drag selection is in flight. The popup takes the mouse grab, so the button-release
    // that would end that drag never reaches the display: the selection would go on extending with every
    // later hover, and the auto-scroll timer would go on firing, with no button held down at all.
    if (core::locked(_terminal, [&]() { return terminal().leftMouseButtonPressed(); }))
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
    return core::locked(_terminal, [&]() {
        // Whether the last-command rows can act: they resolve the store's last finished block, and copy its
        // rows. A shell that never reports a command's start (OSC 133;C) has finished none -- tcsh's ;D at
        // its very first prompt included -- and ClearToPrompt keeps a record whose rows it took. Asked
        // without reading the rows, under the lock the parser waits on.
        auto const hasLastCommand = terminal().hasCommandBlockRows(vtbackend::CommandBlockTarget::Last);
        // ...and whether its command line is known, apart from the prompt chrome its rows carry.
        auto const* const lastBlock = terminal().targetedCommandBlock(vtbackend::CommandBlockTarget::Last);
        auto const lastCommandLine = lastBlock != nullptr && !lastBlock->commandLine.empty()
                                         ? command::CommandLinePresence::Present
                                         : command::CommandLinePresence::Absent;

        // The block under the pointer, as an unpinned `target: pointer` action resolves it, and none over an
        // alternate-screen application. Opened from the sticky header or the gutter, the block the press
        // stood for: the menu is the one its rows would open.
        auto const pinned = _contextMenuBlock != vtbackend::CommandBlockId {};
        auto const* const block =
            terminal().targetedCommandBlock(vtbackend::CommandBlockTarget::Pointer, _contextMenuBlock);

        // Neither the header nor the gutter is a cell, so no link is under either.
        auto const hyperlink = pinned ? nullptr : terminal().tryGetHoveringHyperlink();
        // Else a detected link, as far as the follow gesture resolves it without looking: a path only once
        // the probe has confirmed it, for a row carries what it opens and no probe.
        auto const detected = [&]() -> std::optional<vtbackend::LinkTarget> {
            if (pinned || hyperlink)
                return std::nullopt;
            auto const position = terminal().currentMouseGridPosition();
            return position ? terminal().lookUpLinkAt(*position).target : std::nullopt;
        }();

        // The grid line under the pointer, where the right press just put the terminal's: the very position
        // `block` above was resolved from, so the fold row and the block rows name the same block. Not this
        // session's own copy, which only a move over the grid writes -- a right-click in the gutter came
        // with none, and named whichever row the pointer last crossed on the grid. Over the header, a row
        // of its block.
        auto const pointerLine = [&]() -> std::optional<vtbackend::LineOffset> {
            if (!pinned)
                return terminal().currentMouseGridPosition().transform(
                    [](vtbackend::CellLocation position) { return position.line; });
            if (block == nullptr)
                return std::nullopt;
            // Its head, where its fold hangs; the lowest row the block reached -- the row its end mark came
            // on, below the output -- is no row of the fold. The anchor stands in for a head the grid lost.
            return vtbackend::headOffsetOf(terminal().primaryScreen().grid(), *block).or_else([&] {
                return terminal().commandBlockAnchor(*block);
            });
        }();

        // ...when a fold reaches it.
        auto const foldLine = [&]() -> std::optional<vtbackend::LineOffset> {
            if (!_config.folding.value().enabled || !pointerLine)
                return std::nullopt;
            return terminal().foldContaining(*pointerLine) ? pointerLine : std::nullopt;
        }();

        // ...with what its rows may act on.
        auto const blockUnderCursor = [&]() -> std::optional<command::PointedBlock> {
            if (block == nullptr)
                return std::nullopt;
            return command::PointedBlock {
                .id = block->id,
                .output = block->state == vtbackend::CommandBlockState::Prompting
                              ? command::OutputPresence::Absent
                              : command::OutputPresence::Present,
                .commandLine = block->commandLine.empty() ? command::CommandLinePresence::Absent
                                                          : command::CommandLinePresence::Present,
            };
        }();

        return command::ContextMenuState {
            .hasSelection = terminal().selectionAvailable(),
            .clipboardHasText = clipboardHasText,
            .hasLastCommand = hasLastCommand,
            // Only a working directory on this host can be opened by the local file manager, and the row
            // and the action behind it go through ONE resolver so they cannot disagree. A remote OSC 7
            // cwd grays it out, and so does an OSC 3008 cwd behind a container boundary or from another
            // machine -- which would otherwise open the HOST's directory of the same name, a false
            // positive OSC 7 never had, because a context cwd carries no host authority to test.
            // The LOCKED resolver: this whole lambda runs inside core::locked() above, and the
            // terminal's mutex is not recursive.
            .hasLocalWorkingDirectory =
                resolveWorkingDirectoryLocked(vtbackend::CwdPurpose::OpenLocally).has_value(),
            // Left for the window to fill in: whether this tab holds more than one pane is not something
            // a session knows about itself.
            .hasSplits = false,
            .inputProtected = terminal().userInputLock() == vtbackend::UserInputLock::Locked,
            // A property of the build and the machine, asked once here so the menu itself stays a pure
            // function of this snapshot.
            .canSpeak = _app.speechSynthesizer().available(),
            // Taken now, while the pointer is still on the cell the user clicked. The rows built from this
            // carry the URI with them, because by the time one is picked the pointer has moved to the menu.
            .hyperlinkUnderCursor = hyperlink ? hyperlink->uri : std::string {},
            .linkUnderCursor = detected,
            // Taken now for the same reason the hyperlink is: the row acts on the line that was
            // clicked, and the pointer will have left it by the time the row is picked.
            .foldLine = foldLine,
            .blockUnderCursor = blockUnderCursor,
            .livePrompt = terminal().shellIsPrompting() ? command::PromptPresence::Present
                                                        : command::PromptPresence::Absent,
            .lastCommandLine = lastCommandLine,
            .activeProfile = profileName(),
            .profileNames = std::move(profileNames),
            // `hosts` is ordered by name already.
            .hostNames = std::ranges::to<std::vector>(_config.hosts.value() | std::views::keys),
        };
    });
}

bool TerminalSession::operator()(actions::SoftReset action)
{
    if (auto const dispatch = dispatchRemotely(action); dispatch != RemoteDispatch::RunHere)
        return dispatch == RemoteDispatch::Performed;

    sessionLog()("Performing terminal soft reset");

    // A soft reset is far from a handful of flags: it re-establishes the margins and drops the status
    // display, which RESIZES the main page. Run bare on the GUI thread, that grid resize races the parser
    // thread's line writes — a corrupted screen at best, a heap-buffer-overflow at worst.
    //
    // The callbacks the reset raises are safe under the lock: DECSTR (CSI ! p) reaches Terminal::softReset()
    // from the parser thread too, from inside writeToScreen()'s own _stateMutex hold. A flush is not.
    // Unlocking the keyboard (KAM) takes down the status line it put up, and the resize may write a report,
    // which with SRM still reset at that point is echoed, which parses. The parser defers that echo until it
    // has unwound; here the flush waits for the lock's release.
    {
        auto const _ = std::scoped_lock { _terminal };
        terminal().softReset();
    }
    return true;
}

bool TerminalSession::operator()(actions::CopyLastCommandPrompt)
{
    return copyCommandBlock(vtbackend::CommandBlockTarget::Last, vtbackend::CommandBlockPart::Prompt, {})
        .has_value();
}

bool TerminalSession::operator()(actions::CopyLastCommandOutput)
{
    return copyCommandBlock(vtbackend::CommandBlockTarget::Last, vtbackend::CommandBlockPart::Output, {})
        .has_value();
}

bool TerminalSession::operator()(actions::CopyLastCommandBlock)
{
    return copyCommandBlock(
               vtbackend::CommandBlockTarget::Last, vtbackend::CommandBlockPart::PromptAndOutput, {})
        .has_value();
}

namespace
{
    /// @p text, unless it is empty: then the part it was taken from is an empty part. A command that printed
    /// nothing leaves blank rows at most, whose text is "" -- nothing to copy, and nothing to show.
    /// @param text The text taken from a command block, or why there is none.
    /// @return The text when it has any; otherwise why there is none.
    [[nodiscard]] std::expected<std::string, vtbackend::CommandBlockActionError> nonEmpty(
        std::expected<std::string, vtbackend::CommandBlockActionError> text)
    {
        if (text && text->empty())
            return std::unexpected(vtbackend::CommandBlockActionError::EmptyPart);
        return text;
    }
} // namespace

std::expected<void, vtbackend::CommandBlockActionError> TerminalSession::copyCommandBlock(
    vtbackend::CommandBlockTarget target, vtbackend::CommandBlockPart part, vtbackend::CommandBlockId block)
{
    return copyBlockText(core::locked(_terminal, [&]() {
        return terminal().commandBlockText(target, part, vtbackend::CaptureRendition::PlainText, block);
    }));
}

std::expected<void, vtbackend::CommandBlockActionError> TerminalSession::copyBlockText(
    std::expected<std::string, vtbackend::CommandBlockActionError> text)
{
    // Copying nothing is not a copy -- it is the destruction of whatever the user had on the clipboard.
    // `cd /tmp` prints not one character, so its block's output is blank rows at most, and its text is
    // empty. QClipboard::setText("") would replace the URL the user copied a minute ago with nothing at
    // all, and the menu row would look like it did nothing. Refusing leaves the clipboard alone, and tells
    // the caller the row had nothing to give.
    return nonEmpty(std::move(text)).transform([this](std::string const& value) { copyToClipboard(value); });
}

bool TerminalSession::operator()(actions::SelectCommandBlock const& action)
{
    // As with copyCommandBlock(), a block or part that is not there is reported to the caller only; the
    // terminal leaves the selection as it was.
    // Through runUnderStateLock(), as SelectAll's selection is reported.
    return _terminal.runUnderStateLock([&]() {
        return terminal().selectCommandBlock(action.target, action.part, action.block).has_value();
    });
}

bool TerminalSession::operator()(actions::CopyCommandBlock const& action)
{
    return copyCommandBlock(action.target, action.part, action.block).has_value();
}

bool TerminalSession::operator()(actions::CopyCommandLine const& action)
{
    // The record belongs to the store, so it is read under the lock; its copy is sanitised outside it.
    auto const commandLine = core::locked(
        _terminal, [&]() -> std::expected<vtbackend::CommandLineText, vtbackend::CommandBlockActionError> {
            auto const* const record = terminal().targetedCommandBlock(action.target, action.block);
            if (record == nullptr)
                return std::unexpected(vtbackend::CommandBlockActionError::NoBlock);
            return record->commandLine;
        });

    // Keep control filtering in both modes. Multiline copies preserve shell syntax, but their internal
    // newlines may submit input in a destination without bracketed paste, so the profile must opt in.
    return copyBlockText(commandLine.transform([this](vtbackend::CommandLineText const& text) {
               return profile().copyCommandPreserveMultiline.value() ? text.multilineCopyable()
                                                                     : text.copyable();
           }))
        .has_value();
}

bool TerminalSession::operator()(actions::RerunCommand const& action)
{
    // The record belongs to the store, so it is read under the lock, and the prompt is asked about there.
    auto const commandLine =
        core::locked(_terminal, [&]() -> std::expected<vtbackend::CommandLineText, RerunRefusal> {
            auto const* const record = terminal().targetedCommandBlock(action.target, action.block);
            if (record == nullptr || record->commandLine.empty())
                return std::unexpected(RerunRefusal::NoCommandLine);
            return checkRerun(terminal(), record->commandLine.raw()).transform([&] {
                return record->commandLine;
            });
        });
    if (!commandLine)
    {
        sessionLog()("Not running the command again: {}.", rerunRefusalText(commandLine.error()));
        return false;
    }

    // Shown before it runs: the line is what the shell reported, or what the screen showed, and either can
    // be written by any program that writes to the terminal.
    _pendingRerun = *commandLine;
    requestPermission(configuredPermissionFor(GuardedRole::RerunCommand), GuardedRole::RerunCommand);
    return true;
}

namespace
{
    /// The pane OpenCommandOutput opens for @p placement; std::nullopt for none, the program running
    /// detached.
    [[nodiscard]] std::optional<CommandPanePlacement> commandPaneFor(actions::OutputPlacement placement)
    {
        switch (placement)
        {
            case actions::OutputPlacement::Split: return CommandPanePlacement::Split;
            case actions::OutputPlacement::Tab: return CommandPanePlacement::Tab;
            case actions::OutputPlacement::Detached: return std::nullopt;
        }
        return std::nullopt;
    }
} // namespace

namespace
{
    /// What a failure to open is called: its notice's title and text, and what the log says could not be
    /// opened.
    struct OpenFailureText
    {
        char const* title;        ///< The notice's title, translatable.
        char const* message;      ///< The notice's text, translatable as one sentence; %1 is the reason.
        std::string_view subject; ///< For the log, which is not translated: "Could not open <subject>: …".
    };

    /// One row per OpenFailure, in its order.
    constexpr auto OpenFailureTexts = std::array {
        OpenFailureText { .title = QT_TRANSLATE_NOOP("contour::session::TerminalSession", "Open Output"),
                          .message = QT_TRANSLATE_NOOP("contour::session::TerminalSession",
                                                       "Could not open the command output: %1."),
                          .subject = "the command output" },
        OpenFailureText {
            .title = QT_TRANSLATE_NOOP("contour::session::TerminalSession", "Open File"),
            .message = QT_TRANSLATE_NOOP("contour::session::TerminalSession", "Could not open the file: %1."),
            .subject = "the file" },
    };
    static_assert(OpenFailureTexts.size() == static_cast<std::size_t>(OpenFailure::File) + 1);
} // namespace

void TerminalSession::reportOpenFailure(OpenFailure what, std::string_view reason)
{
    auto const& text = OpenFailureTexts.at(static_cast<std::size_t>(what));
    errorLog()("Could not open {}: {}.", text.subject, reason);
    emit showNotification(
        tr(text.title),
        tr(text.message).arg(QString::fromUtf8(reason.data(), static_cast<qsizetype>(reason.size()))));
}

bool TerminalSession::operator()(actions::OpenCommandOutput const& action)
{
    // A program reading its standard input cannot be assumed to interpret SGR, so detached is plain.
    auto const rendition = action.placement != actions::OutputPlacement::Detached
                                   && config::outputFormatFor(action, _config.commandBlocks.value())
                                          == actions::OutputFormat::Sgr
                               ? vtbackend::CaptureRendition::WithSgr
                               : vtbackend::CaptureRendition::PlainText;
    auto const output = nonEmpty(core::locked(_terminal, [&]() {
        return terminal().commandBlockText(
            action.target, vtbackend::CommandBlockPart::Output, rendition, action.block);
    }));

    // No block there, no output yet, or nothing but blank rows: nothing to show, which -- as for the copy
    // actions -- is no error worth a notice.
    if (!output)
    {
        sessionLog()("OpenCommandOutput: {}.", vtbackend::describe(output.error()));
        return false;
    }

    // The program as configured, read as the spawn reads a shell setting (vtpty::invocationToRun): a
    // program and its arguments where it names no program but holds whitespace, and on Windows by
    // CreateProcess's rules, so that `C:\Tools\less.exe -R` keeps its backslashes.
    auto const& program = action.program.empty() ? _config.commandBlocks.value().pager : action.program;
    if (program.find_first_not_of(" \t\r\n") == std::string::npos)
    {
        reportOpenFailure(OpenFailure::CommandOutput,
                          "no program to open it with (program:, else command_blocks.pager)");
        return false;
    }

    // Asked before the output is written anywhere: a window that can open no pane need not write a file
    // only to remove it again.
    auto const pane = commandPaneFor(action.placement);
    if (pane)
        if (auto const refusal = _manager->commandPaneRefusal(this, *pane))
        {
            reportOpenFailure(OpenFailure::CommandOutput, describe(*refusal));
            return false;
        }

    auto& files = _app.commandOutputFiles();
    auto const file = files.create(*output);
    if (!file)
    {
        reportOpenFailure(OpenFailure::CommandOutput, describe(file.error()));
        return false;
    }

    if (!pane)
    {
        // Detached: the program is not Contour's, and outlives it. Once it has started, it holds the file
        // open, so the file is released at once; where the platform will not remove a file a process
        // holds open, it goes with the private directory at exit instead.
        auto const started = _app.externalLauncher().runDetachedWithInput(program, *file);
        files.release(*file);
        if (!started)
            reportOpenFailure(OpenFailure::CommandOutput, platform::describe(started.error()));
        return started.has_value();
    }

    // The pager reads the file it is given after whatever the configuration wrote.
    auto command = vtpty::Process::ExecInfo {};
    command.program = program;
    command.arguments = { core::pathToUtf8(*file) };

    auto const opened = _manager->openCommandPane(this, *pane, command);
    if (!opened)
    {
        files.release(*file);
        reportOpenFailure(OpenFailure::CommandOutput, describe(opened.error()));
        return false;
    }

    // The file lives exactly as long as the pane showing it.
    QObject::connect(
        *opened, &TerminalSession::sessionClosed, *opened, [&files, path = *file]() { files.release(path); });
    return true;
}

bool TerminalSession::operator()(actions::CopyHyperlink const& action)
{
    // A URI the caller pinned wins: the context menu captured the link the user right-clicked, and asking
    // the terminal again now would answer about wherever the pointer has since wandered. A key binding
    // pins nothing, and for it "the link under the cursor" is exactly the right question.
    if (action.link)
    {
        copyToClipboard(action.link->target);
        return true;
    }
    if (!action.uri.empty())
    {
        copyToClipboard(action.uri);
        return true;
    }

    // What the follow gesture would open there: an OSC 8 link, a detected one, a URL found on demand. A path
    // the probe has not confirmed copies nothing, as where it resolves is not known yet.
    auto const address = core::locked(_terminal, [&]() -> std::optional<std::string> {
        auto const position = _terminal.currentMouseGridPosition();
        auto const lookup = position ? _terminal.lookUpLinkAt(*position) : vtbackend::LinkLookup {};
        if (lookup.hyperlink)
            return lookup.hyperlink->uri;
        return lookup.target.transform([](vtbackend::LinkTarget const& target) { return target.target; });
    });
    if (!address)
        return false;

    copyToClipboard(*address);
    return true;
}

bool TerminalSession::operator()(actions::CopySelection copySelection)
{

    switch (copySelection.format)
    {
        case actions::CopyFormat::Text: {
            // Copy the selection in pure text, plus whitespaces and newline.
            auto const text = core::locked(_terminal, [&]() { return terminal().extractSelectionText(); });

            // Nothing selected: with the fallback, copy what the last command printed instead -- so the copy
            // chord right after a command does something useful rather than emptying the clipboard.
            if (text.empty() && copySelection.fallback == actions::CopyFallback::LastCommandOutput)
                return copyCommandBlock(vtbackend::CommandBlockTarget::Last,
                                        vtbackend::CommandBlockPart::Output,
                                        vtbackend::CommandBlockId {})
                    .has_value();

            copyToClipboard(text);
            break;
        }
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
    // Locked: word-wise selection scans grid cells and installs a selector, which is reported as SelectAll's
    // selection is. See ViNormalMode.
    {
        auto const _ = std::scoped_lock { _terminal };
        _terminal.triggerWordWiseSelectionWithCustomDelimiters(customSelector.delimiters);
    }
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
    return focusOrOpenSearch(SearchWrapEdge::Top);
}

bool TerminalSession::operator()(actions::FocusPreviousSearchMatch)
{
    return focusOrOpenSearch(SearchWrapEdge::Bottom);
}

bool TerminalSession::focusOrOpenSearch(SearchWrapEdge edge)
{
    auto const hasPattern = core::locked(_terminal, [&]() { return !_terminal.search().pattern.empty(); });
    if (!hasPattern)
    {
        // Find Next with nothing to find opens the find bar (Review Focus 5): nothing goes to the
        // application, and the cursor stays. Posted, as searchPromptRequested() posts it.
        platform::postToObject(this, [this]() { emit searchBarRequested(); });
        return true;
    }
    // Through the bar's own stepping, so F3 and ⌘G wrap like Enter does (R-h).
    stepSearch(edge);
    return true;
}

bool TerminalSession::focusAdjacentSearchMatch(SearchWrapEdge edge)
{
    // Locked: the step walks the grid and the Vi cursor moves with it. See ViNormalMode. Through
    // runUnderStateLock(): in a Visual mode the cursor extends the selection, whose report is sent once the
    // lock is released, as SelectAll's is.
    return _terminal.runUnderStateLock([&]() {
        // Set BEFORE the step, not after it: the step reveals the match and raises screenUpdated() from
        // inside, so the guard has to already be up by then. @see _searchMatchRevealed.
        auto const wasRevealed = _searchMatchRevealed.exchange(true, std::memory_order_relaxed);

        auto const cursor = _terminal.normalModeCursorPosition();
        auto const nextPosition = edge == SearchWrapEdge::Top ? _terminal.searchNextMatch(cursor)
                                                              : _terminal.searchPrevMatch(cursor);
        if (!nextPosition)
        {
            // RESTORED, not cleared: this step revealed nothing, but a previous one may well have -- a step
            // with nowhere to go is exactly that case, and storing false there would un-park the very match
            // the user is standing on, so the next byte of output scrolls it away.
            _searchMatchRevealed.store(wasRevealed, std::memory_order_relaxed);
            return false;
        }
        _terminal.moveNormalModeCursorTo(nextPosition.value());
        _terminal.viewport().makeVisibleWithinSafeArea(nextPosition->line);
        return true;
    });
}

bool TerminalSession::operator()(actions::SearchSelection)
{
    // The selection's first line (R-k): a pattern spans no line break. Under a regex its metacharacters are
    // escaped, so it finds the selected text itself.
    auto const pattern = core::locked(_terminal, [&]() -> std::optional<std::u32string> {
        auto const text = _terminal.extractSelectionText();
        if (text.empty())
            return std::nullopt;
        auto const firstLine = std::string_view(text).substr(0, std::string_view(text).find('\n'));
        auto line = unicode::convert_to<char32_t>(firstLine);
        if (_terminal.search().options.syntax == vtbackend::SearchSyntax::Regex)
            return vtbackend::escapeSearchRegex(line);
        return line;
    });
    if (!pattern || pattern->empty())
        return false;

    setSearchPattern(QString::fromStdU32String(*pattern));
    platform::postToObject(this, [this]() { emit searchBarRequested(); });
    return true;
}

namespace
{
    /// @param refusal Why a path names no file on this machine.
    /// @return The reason as the log gives it, after "Not opening ...: ".
    [[nodiscard]] constexpr std::string_view refusalReason(vtbackend::PathRefusal refusal) noexcept
    {
        switch (refusal)
        {
            case vtbackend::PathRefusal::RanElsewhere:
                return "the command that printed it ran on another machine.";
            case vtbackend::PathRefusal::ReplicaOfAnotherMachine:
                return "it names a file on the machine the session runs on, not this one.";
        }
        return {};
    }
} // namespace

bool TerminalSession::operator()(actions::FollowHyperlink const& action)
{
    // Pinned by the caller (the context menu, which captured the link the user right-clicked) beats asking
    // the terminal where the pointer happens to rest now. See operator()(CopyHyperlink) above.
    if (action.link)
    {
        openLink(*action.link);
        return true;
    }
    if (!action.uri.empty())
    {
        followHyperlink(vtbackend::HyperlinkInfo { .userId = {}, .uri = action.uri });
        return true;
    }

    // An action reports whether it acted, and the binding falls through where it did not: a path that is
    // being looked for has been acted on, as it opens once found.
    return followLinkUnderPointer() != LinkFollow::NothingThere;
}

/// What a follow gesture found at a cell, read under the terminal lock and acted on once it is released: the
/// desktop's handler is another process, and an executable link runs one, neither of which the parser thread
/// may wait for.
struct TerminalSession::FoundLink
{
    vtbackend::LinkLookup lookup;                    ///< What lies at the cell.
    std::shared_ptr<vtbackend::PathProbe> probe;     ///< Null without link detection.
    core::async::IExecutor* probeExecutor = nullptr; ///< The app's probe thread; null without detection.
};

TerminalSession::FoundLink TerminalSession::findLinkAt(vtbackend::CellLocation position)
{
    auto const* const detector = _terminal.linkDetector();
    return FoundLink { .lookup = _terminal.lookUpLinkAt(position),
                       .probe = detector ? detector->pathProbe() : nullptr,
                       .probeExecutor = detector ? &detector->services().probeExecutor : nullptr };
}

void TerminalSession::followLinkAt(vtbackend::CellLocation position)
{
    // Nothing to fall through to: vi's `gx` consumed its keys whatever lies there.
    std::ignore = follow(core::locked(_terminal, [&] { return findLinkAt(position); }));
}

LinkFollow TerminalSession::followLinkUnderPointer()
{
    // The pointer's cell and what lies there, in one hold of the lock.
    auto found = core::locked(_terminal, [&]() -> std::optional<FoundLink> {
        auto const position = _terminal.currentMouseGridPosition();
        return position ? std::optional { findLinkAt(*position) } : std::nullopt;
    });
    return found ? follow(std::move(*found)) : LinkFollow::NothingThere;
}

LinkFollow TerminalSession::follow(FoundLink found)
{
    // The application's own link, as it wrote it: its file:// to a program is run, as an OSC 8 link's is.
    if (found.lookup.hyperlink)
    {
        followHyperlink(*found.lookup.hyperlink);
        return LinkFollow::Opened;
    }
    if (found.lookup.target)
    {
        openLink(*found.lookup.target);
        return LinkFollow::Opened;
    }
    if (found.lookup.refusal)
    {
        // The path names a file on another machine: its command ran there, or the pane replicates that
        // machine's daemon, for the reason followHyperlink() refuses a file:// link there. Said, as that
        // refusal is.
        errorLog()("Not opening \"{}\": {}",
                   found.lookup.printedPath ? found.lookup.printedPath->target : std::string {},
                   refusalReason(*found.lookup.refusal));
        return LinkFollow::NothingThere;
    }
    if (!found.lookup.pathToProbe || !found.lookup.printedPath || !found.probe
        || found.probeExecutor == nullptr)
        return LinkFollow::NothingThere;

    // Printed too recently to be known: looked for on the probe thread, which a hung mount stalls alone, and
    // opened back on this thread once found. The session may be gone by then; its post target says so.
    vtbackend::probeAsync(
        std::move(found.probe),
        *found.probeExecutor,
        std::move(*found.lookup.pathToProbe),
        [this,
         postTarget = _postTarget,
         printed = std::move(*found.lookup.printedPath),
         directories = std::move(found.lookup.pathDirectories)](vtbackend::ResolvedPath const& answer) {
            if (answer.kind == vtbackend::PathKind::Missing)
                return;
            // Posted to this session, so Qt drops the call should the session die before it runs.
            postTarget->post([this, link = vtbackend::resolvedPathTarget(printed, answer, directories)] {
                openLink(link);
            });
        });
    return LinkFollow::Probing;
}

void TerminalSession::openLink(vtbackend::LinkTarget const& link)
{
    switch (link.kind)
    {
        case vtbackend::LinkKind::Url:
        case vtbackend::LinkKind::Custom:
            // A file:// URL found in output arrives as the path it names (vtbackend::classifyPrintedLink()):
            // never here, where followHyperlink() would run a program it names (§7.6).
            if (vtbackend::isFileUrl(link.target))
            {
                errorLog()("Not opening \"{}\": a file:// link found in output opens as the file it names.",
                           link.target);
                return;
            }
            followHyperlink(vtbackend::HyperlinkInfo { .userId = {}, .uri = link.target });
            return;
        case vtbackend::LinkKind::Path: openLocalPath(link); return;
    }
}

void TerminalSession::openLocalPath(vtbackend::LinkTarget const& link)
{
    auto const& opener = _profile.fileOpener.value();
    // Validated when the configuration was read, and split again for each file it opens. An empty one parses
    // to no program; one with a problem was reported then, and is kept as written only for the settings
    // page: it opens as an empty one does.
    auto const parsed = config::FileOpenerTemplate::parse(opener.command);

    // A directory goes to the file manager, and without a usable file_opener a file to the application it is
    // associated with, which takes no location (§7.3).
    if (link.pathKind == vtbackend::PathKind::Directory || !parsed)
    {
        auto const url = QUrl::fromLocalFile(QString::fromStdString(link.target));
        openExternally(url, link.pathKind == vtbackend::PathKind::Directory ? "folder" : "file", link.target);
        return;
    }
    auto argv = parsed->expand(link.target, link.location);

    auto const pane = commandPaneFor(opener.placement);
    if (!pane)
    {
        // Detached: a program of its own, started without waiting for it.
        auto arguments = QStringList {};
        for (auto const& argument: argv | std::views::drop(1))
            arguments.append(QString::fromStdString(argument));
        if (auto const started =
                _app.externalLauncher().runDetached(QString::fromStdString(argv.front()), arguments);
            !started)
            reportOpenFailure(OpenFailure::File, platform::describe(started.error()));
        return;
    }

    if (_manager == nullptr)
    {
        reportOpenFailure(OpenFailure::File, describe(CommandPaneError::NoPane));
        return;
    }
    // In a pane, working where a relative path was found, or else in the file's own directory.
    auto command = vtpty::Process::ExecInfo {};
    command.program = argv.front();
    command.arguments.assign(std::next(argv.begin()), argv.end());
    command.workingDirectory =
        link.directory.empty()
            ? core::pathFromUtf8(link.target).value_or(std::filesystem::path {}).parent_path()
            : core::pathFromUtf8(link.directory).value_or(std::filesystem::path {});
    if (auto const opened = _manager->openCommandPane(this, *pane, command); !opened)
        reportOpenFailure(OpenFailure::File, describe(opened.error()));
}

bool TerminalSession::operator()(actions::HintMode const& action)
{
    sessionLog()("Activating hint mode with patterns: '{}', action: {}, scope: {}",
                 action.patterns,
                 action.hintAction,
                 action.scope);

    // The table was compiled, and its problems reported, when the profile was applied. Read under the lock,
    // as configureTerminal() replaces it under the lock, filtered by name against that same table, and handed
    // to hint mode in the same hold. Through runUnderStateLock(): hint mode puts the indicator status line
    // up, which resizes. See ViNormalMode.
    terminal().runUnderStateLock([&]() {
        auto const table = terminal().settings().linkPatterns;
        auto names = std::vector<std::string> {};
        if (!action.patterns.empty() && action.patterns != "all")
        {
            for (auto const name: core::split(std::string_view(action.patterns), '|'))
                if (table->find(name) != nullptr)
                    names.emplace_back(name);
            if (names.empty())
                sessionLog()("No hint patterns matched '{}', falling back to all patterns", action.patterns);
            else
                sessionLog()("Filtered to {} hint pattern(s) matching '{}'", names.size(), action.patterns);
        }
        terminal().activateHintMode(vtbackend::HintModeRequest {
            .patterns =
                vtbackend::HintPatternSet { .table = table, .names = std::move(names), .resolve = {} },
            .action = action.hintAction,
            .scope = action.scope,
            .scrollbackLimit = profile().hintScrollbackLines.value(),
        });
    });
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
    spawnNewTerminal(action.profileName);
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
    // By default open the app's Settings window, near this session's window (routed through the manager
    // like every window-scoped op). The explicit `in_editor: true` opt-in instead opens the raw
    // configuration file in the OS's external editor — the historical behavior.
    if (event.target == actions::ConfigurationTarget::SettingsWindow)
    {
        _manager->openSettings(/*acting*/ this);
        return true;
    }

    // fromLocalFile(), not QUrl(path): a bare filesystem path is not a URL. On POSIX it parses as a
    // scheme-less relative reference and on Windows `C:\...` parses with scheme "c", so the desktop
    // was handed something no handler could claim. The two sibling openers here already do this.
    openExternally(QUrl::fromLocalFile(platform::toQString(_config.configFile)),
                   "configuration file",
                   core::platform::normalizePath(_config.configFile));

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
    // Read under the lock, and opened once it is released: the desktop's handler is another process.
    auto const selection = core::locked(_terminal, [&]() { return terminal().extractSelectionText(); });
    openExternally(QUrl(QString::fromStdString(selection)), "selection", selection);
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
        // with the terminal lock held. What is sent is flushed once the lock is released. See ViNormalMode
        // for the rationale.
        {
            auto const _ = std::scoped_lock { _terminal };
            if (paste.evaluateInShell)
                terminal().sendRawInput(string_view { text + "\n" });
            else
                terminal().sendPaste(string_view { text });
        }
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
    if (auto const reloaded = reloadAllConfigs(action.profileName.value_or(_profileName)); !reloaded)
        errorLog()("Failed to load configuration. {}", reloaded.error());

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
    // A file, replayed on its own: standaloneScreenshot() puts the replaying terminal in the state to draw
    // it.
    // Of what is shown: the page on display -- not page 15 for every page but the primary one.
    auto const screenshot = standaloneScreenshot(terminal().displayedPage());
    ofstream ofs { "screenshot.vt", ios::trunc | ios::binary };
    ofs << screenshot;
    return true;
}

bool TerminalSession::operator()(actions::SaveScreenshot)
{
    auto savePath =
        app().dumpStateAtExit().value_or(core::cli::App::instance()->localStateDir())
        / fs::path(std::format("contour-screenshot-{:%Y-%m-%d-%H-%M-%S}.png", chrono::system_clock::now()));

    _display->setScreenshotOutput(savePath);
    sessionLog()("Saving screenshot to {}", core::pathToUtf8(savePath));

    // The log line and the toast are deliberately separate strings: one is for a developer reading a
    // log in English, the other is shown to the user and therefore goes through tr().
    auto const notice =
        tr("Saving screenshot to %1").arg(QDir::toNativeSeparators(platform::toQString(savePath)));
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
    // ViNormalMode for the full GUI-vs-parser-thread rationale. A scroll during a drag extends the selection,
    // whose report is sent once the lock is released, as SelectAll's is.
    {
        auto const _ = std::scoped_lock { _terminal };
        if (terminal().settings().smoothScrolling)
        {
            auto const cellHeight = static_cast<float>(terminal().cellPixelSize().height.as<int>());
            auto const pixels = static_cast<float>(*lineCount) * cellHeight;
            if (terminal().applySmoothScrollPixelDelta(pixels) == vtbackend::SmoothScrollResult::Applied)
                return;
        }
        terminal().viewport().scrollUp(lineCount);
    }
}

void TerminalSession::smoothScrollDown(vtbackend::LineCount lineCount)
{
    // Locked, for the same reasons as smoothScrollUp() above.
    {
        auto const _ = std::scoped_lock { _terminal };
        if (terminal().settings().smoothScrolling)
        {
            auto const cellHeight = static_cast<float>(terminal().cellPixelSize().height.as<int>());
            auto const pixels = -static_cast<float>(*lineCount) * cellHeight;
            if (terminal().applySmoothScrollPixelDelta(pixels) == vtbackend::SmoothScrollResult::Applied)
                return;
        }
        terminal().viewport().scrollDown(lineCount);
    }
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

bool TerminalSession::withScrollableChange(std::invocable<vtbackend::Terminal&> auto&& action)
{
    // Locked: the actions walk the grid's marks and mutate fold state the render pass reads. See
    // ScrollMarkDown. A hard reset from 132 columns resizes the page, which with in-band resize reports on
    // writes a report: the lock holds it back until it is released.
    auto const [handled, scrollable] =
        _terminal.runUnderStateLock([&]() -> std::pair<bool, vtbackend::LineCount> {
            auto const ran = action(terminal());

            // Read under the same lock that just moved it: folding a block takes its rows out of the
            // scrollable range without touching the history depth, and this is the only moment that is
            // visible -- the property reporting that range may neither compute it nor take this lock.
            return { ran, _terminal.viewport().scrollableLineCount() };
        });

    // Outside the lock: the QML binding this wakes runs synchronously on this thread and reaches back
    // into the session. Announcing a count that did not move is a no-op, so the disabled case needs no
    // second channel to say so.
    announceScrollableLineCount(scrollable);

    // Folding moves every tick below the fold, and ClearToPrompt takes the ticks of the rows it removed.
    requestScrollbarMarksRefresh();
    return handled;
}

void TerminalSession::scrollToScrollbarMark(qint64 targetLine)
{
    // Only within the stable-id generation the ticks were published in: a reflow since then renamed every
    // row, and the refresh it scheduled replaces this tick anyway.
    if (!_scrollbarMarksKey)
        return;
    auto const generation = _scrollbarTicksGeneration;

    // Through the path that takes the lock and republishes the scrollable count, which opening a collapsed
    // block changes.
    withScrollableChange([&](vtbackend::Terminal& terminal) {
        return terminal.revealStableLineAtTop(static_cast<int64_t>(targetLine), generation).has_value();
    });
}

bool TerminalSession::operator()(actions::CollapseAllFolds)
{
    return withScrollableChange([](auto& terminal) { return terminal.collapseAllFolds(); });
}

bool TerminalSession::operator()(actions::CollapseLastFold)
{
    return withScrollableChange([](auto& terminal) { return terminal.collapseLastFold(); });
}

bool TerminalSession::operator()(actions::ExpandAllFolds)
{
    // The terminal never refuses an expand, so turning the feature off still lets a user undo what they
    // folded while it was on -- and the rows this hands BACK to the scrollable range have to be announced
    // just as the ones a collapse takes away are, or the scrollbar keeps the travel it had while they were
    // hidden.
    return withScrollableChange([](auto& terminal) { return terminal.expandAllFolds(); });
}

bool TerminalSession::operator()(actions::ToggleFold)
{
    return withScrollableChange(
        [](auto& terminal) { return terminal.toggleFoldContaining(foldAnchorLine(terminal)); });
}

bool TerminalSession::operator()(actions::ToggleFoldAt action)
{
    // The line comes from the context menu, which captured where the user right-clicked -- so this
    // acts on the block they were pointing at, not on whatever the viewport happens to start with.
    return withScrollableChange([line = action.line](auto& terminal) {
        return terminal.toggleFoldContaining(vtbackend::LineOffset(line));
    });
}

bool TerminalSession::operator()(actions::ToggleLastFold)
{
    return withScrollableChange([](auto& terminal) { return terminal.toggleLastFold(); });
}

bool TerminalSession::operator()(actions::ScrollMarkDown)
{
    // Locked: scans the grid for marks and moves the viewport, as smoothScrollUp() does. See ViNormalMode.
    {
        auto const _ = std::scoped_lock { _terminal };
        terminal().viewport().scrollMarkDown();
    }
    return true;
}

bool TerminalSession::operator()(actions::ScrollMarkUp)
{
    // Locked, for the same reasons as ScrollMarkDown above.
    {
        auto const _ = std::scoped_lock { _terminal };
        terminal().viewport().scrollMarkUp();
    }
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
    auto const stepSize = terminal().mainPageSize().lines / LineCount(2);
    smoothScrollDown(stepSize);
    return true;
}

bool TerminalSession::operator()(actions::ScrollPageUp)
{
    auto const stepSize = terminal().mainPageSize().lines / LineCount(2);
    smoothScrollUp(stepSize);
    return true;
}

bool TerminalSession::operator()(actions::ScrollToBottom)
{
    // Locked: mutates the smooth-scroll state and the viewport, as smoothScrollUp() does. See ViNormalMode.
    {
        auto const _ = std::scoped_lock { _terminal };
        // Snap immediately for ScrollToTop/Bottom (animating large distances is impractical).
        terminal().resetSmoothScroll();
        terminal().viewport().scrollToBottom();
    }
    return true;
}

bool TerminalSession::operator()(actions::ScrollToTop)
{
    // Locked, for the same reasons as ScrollToBottom above.
    {
        auto const _ = std::scoped_lock { _terminal };
        // Snap immediately for ScrollToTop/Bottom (animating large distances is impractical).
        terminal().resetSmoothScroll();
        terminal().viewport().scrollToTop();
    }
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
    // Locked: sendRawInput() appends to the input generator, and the parser thread appends to that same
    // generator whenever a sequence replies. Its flush waits for the lock's release. See ViNormalMode.

    // auto const now = steady_clock::now();
    // for (auto const ch: event.chars)
    //     terminal().sendCharPressEvent(static_cast<char32_t>(ch), vtbackend::Modifiers::None, now);
    {
        auto const _ = std::scoped_lock { _terminal };
        terminal().sendRawInput(event.chars);
    }
    return true;
}

bool TerminalSession::operator()(actions::ToggleAllKeyMaps)
{
    _allowKeyMappings = !_allowKeyMappings;
    input::inputLog()("{} key mappings.", _allowKeyMappings ? "Enabling" : "Disabling");

    // Locked: setStatusDisplay() resizes the page and reflows the grid, exactly as ViNormalMode does, and the
    // resize may write a report, sent once the lock is released. The indicator is a notice the terminal puts
    // up, as vi mode's is, not a type the user chose: it selects nothing, and DECSASD keeps what was
    // selected. DEC STD 070 has such a notice of its own, the Error Line (ch. 14 §2.1.1): it "replaces the
    // currently displayed Status Line (Indicator or Host Writable)" for a while, and "The host may not
    // directly program or affect the Error Line".
    {
        auto const _ = std::scoped_lock { _terminal };
        // Put up and taken down as vi mode's is (push/pop), so that re-enabling shows again what it replaced.
        if (!_allowKeyMappings)
        {
            terminal().setStatusLineDefinition(parseStatusLineDefinition(
                "{}", "{Text:text=key bindings disabled,Left= « ,Right= » }", "{}"));
            terminal().pushStatusDisplay(vtbackend::StatusNotice::KeyBindingsDisabled,
                                         StatusDisplayType::Indicator);
        }
        else
        {
            terminal().resetStatusLineDefinition();
            terminal().popStatusDisplay(vtbackend::StatusNotice::KeyBindingsDisabled);
        }
    }
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
    // The user's own lock, not KAM: KAM is the host's, which DECSTR, CSI 2 l and RIS reset, and no host
    // sequence may undo what the user locked. Locked around the terminal calls only; announce() below is Qt
    // accessibility and must not run with the terminal lock held. Through runUnderStateLock(): the lock puts
    // the indicator status line up, which resizes. See ViNormalMode for the rationale.
    auto const userLock = _terminal.runUnderStateLock([&]() {
        auto const toggled = terminal().userInputLock() == vtbackend::UserInputLock::Locked
                                 ? vtbackend::UserInputLock::Unlocked
                                 : vtbackend::UserInputLock::Locked;
        terminal().setUserInputLock(toggled);
        return toggled;
    });
    // What typing does now, and which lock holds it: the user who unlocked while the application's KAM still
    // holds the keyboard hears why typing still does nothing, rather than the "Read-only" they just lifted.
    auto const announcement = [&] {
        if (userLock == vtbackend::UserInputLock::Locked)
            return QObject::tr("Read-only");
        if (_terminal.isModeEnabled(vtbackend::AnsiMode::KeyboardAction))
            return QObject::tr("Read-only: the application locked the keyboard");
        return QObject::tr("Editable");
    }();

    // Assertive: this changes what typing does, so a client mid-sentence should be interrupted rather
    // than tell the user about it after they have already typed into a terminal that ignored them.
    announce(announcement, QAccessible::AnnouncementPoliteness::Assertive);
    return true;
}

bool TerminalSession::operator()(actions::ToggleStatusLine)
{
    // Locked: the status line resizes the page. See ViNormalMode. The user's choice is a selection, which a
    // host-writable status line DECSASD had selected gives way to.
    {
        auto const _ = std::scoped_lock { _terminal };
        if (terminal().statusDisplayType() != StatusDisplayType::Indicator)
            terminal().selectStatusDisplayByUser(StatusDisplayType::Indicator);
        else
            terminal().selectStatusDisplayByUser(StatusDisplayType::None);

        // `savedStatusDisplayType` holds only a value if the application has been overriding
        // the status display type. But the user now actively requests a given type,
        // so make sure restoring will not destroy the user's desire.
        if (terminal().savedStatusDisplayType())
            terminal().setSavedStatusDisplayType(terminal().statusDisplayType());
    }

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
    //
    // With in-band resize reports on, that resize writes a report, and with SRM reset the report is echoed,
    // which parses and takes this lock again: the report goes out once the lock is released.
    {
        auto const _ = std::scoped_lock { _terminal };
        if (terminal().inputHandler().mode() == ViMode::Insert)
            terminal().inputHandler().setMode(ViMode::Normal);
        else if (terminal().inputHandler().mode() == ViMode::Normal)
            terminal().inputHandler().setMode(ViMode::Insert);
    }
    return true;
}

bool TerminalSession::operator()(actions::WriteScreen const& event)
{
    if (auto const dispatch = dispatchRemotely(event); dispatch != RemoteDispatch::RunHere)
        return dispatch == RemoteDispatch::Performed;

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

bool TerminalSession::operator()(actions::CloseOtherTabs const& action)
{
    _manager->runTabAction(actions::Action { action }, this);
    return true;
}

bool TerminalSession::operator()(actions::CloseTabsToRight const& action)
{
    _manager->runTabAction(actions::Action { action }, this);
    return true;
}

bool TerminalSession::operator()(actions::MoveTabToNewWindow const& action)
{
    _manager->runTabAction(actions::Action { action }, this);
    return true;
}

bool TerminalSession::operator()(actions::ShowAbout)
{
    _manager->showAbout(this);
    return true;
}

bool TerminalSession::operator()(actions::MinimizeWindow)
{
    _manager->minimizeWindow(this);
    return true;
}

bool TerminalSession::operator()(actions::ZoomWindow)
{
    _manager->zoomWindow(this);
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

bool TerminalSession::operator()(actions::AttachToHost const& event)
{
    // A process of its own: a GUI process is attached to exactly one daemon for its life.
    // An unknown name is still handled: the new process would only report the same.
    if (!_config.hosts.value().contains(event.name))
    {
        errorLog()("Could not attach to host \"{}\": {}.", event.name, "no such host under hosts:");
        return true;
    }
    auto const command = buildAttachHostCommand(
        _app.programPath(), core::platform::normalizePath(_config.configFile), event.name);
    if (auto const started = _app.externalLauncher().runDetached(command.program, command.arguments);
        !started)
        errorLog()("Could not attach to host \"{}\": {}.", event.name, platform::describe(started.error()));
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

    // The page on display, which the pointer is over -- not the cursor's, with DECPCCM reset.
    using Type = vtbackend::ScreenType;
    switch (_terminal.displayedScreenType())
    {
        case Type::Primary: _display->setMouseCursorShape(input::MouseCursorShape::IBeam); break;
        case Type::Alternate: _display->setMouseCursorShape(input::MouseCursorShape::Arrow); break;
    }
}

bool TerminalSession::reloadConfig(config::Config newConfig, string const& profileName)
{
    // clang-format off
    sessionLog()("Reloading configuration from {} with profile {}",
                 core::pathToUtf8(newConfig.configFile), profileName);
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

    if (config::containsAction<actions::ToggleAllKeyMaps>(actions))
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
    // The terminal's own resolution, so a pane that replicates another machine's daemon holds every
    // directory foreign here (vtbackend::Terminal::bindReplica).
    return _terminal.resolveWorkingDirectory(purpose);
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
        if (auto const directory = core::pathFromUtf8(resolved->path);
            directory && std::filesystem::is_directory(*directory, ec))
            return resolved->path;
    }
#endif
    return "."s;
}

std::string TerminalSession::displayWorkingDirectory() const
{
    auto const directory = [this]() -> std::string {
        // The OSC 3008 ancestry first, then OSC 7: both are the shell speaking, so both track a `cd` made
        // inside a full-screen application, and both can be right for a session that is not on this
        // machine. NOT filtered for locality, unlike workingDirectory() above -- a container's /app is
        // exactly what the user is looking at, and a path worth SHOWING need not be one a child could be
        // spawned in.
        if (auto const resolved = resolveWorkingDirectory(vtbackend::CwdPurpose::Display))
            return resolved->path;

        // Nothing reported (no shell integration, or not yet): fall back to where the session was started.
        // Unlike workingDirectory() this is NOT filtered for local existence — a path worth SHOWING need
        // not be one a child could be spawned in.
#ifndef _WIN32
        if (auto const* ptyProcess = dynamic_cast<vtpty::Process const*>(&_terminal.device()))
            return ptyProcess->workingDirectory();
#endif
        return {};
    }();

    // Shown, so made safe to show: a directory name may hold a line feed or a bidi override, and OSC 7
    // decodes either from an escape -- raw, the first adds a line of the application's choosing under the
    // tab title, the second reverses what follows it.
    return vtbackend::sanitizeCommandLine(directory, vtbackend::SanitizePurpose::Display);
}

void TerminalSession::spawnNewTerminal(std::optional<std::string> const& requestedProfile)
{
    auto const wd = workingDirectory();
    auto const profileName = requestedProfile.value_or(_profileName);

    if (_config.spawnNewProcess.value())
    {
        sessionLog()("spawning new process");
        auto const command = buildSpawnTerminalCommand(
            _app.programPath(), core::platform::normalizePath(_config.configFile), profileName, wd);
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
        // The profile the action named, else none: the daemon then runs the one this client named in its
        // hello, where a local window would run this session's own -- a name the daemon may not have.
        if (_app.requestRemoteWindow(RemoteSpawn { .profileName = requestedProfile.value_or("") }))
            return;
        sessionLog()("spawning new in-process window");
        _app.config().profile(_profileName)->shell.value().workingDirectory =
            core::pathFromUtf8(wd).value_or(fs::path { "." });
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

    // scrollbar.marks and the scrollbar's position are profile state too, read by the ticks' refresh.
    requestScrollbarMarksRefresh();
}

namespace
{
    /// What a page is fitted around besides its cells: the gutter left of the grid and the margins
    /// around it, both read where the page is fitted (session::applyResize).
    struct PageFrame
    {
        int gutterColumns = 0;            ///< The columns the gutter reserves.
        config::WindowMargins margins {}; ///< The profile's margins.

        /// Member-wise equality: a frame that differs in either moves the grid.
        [[nodiscard]] constexpr bool operator==(PageFrame const&) const noexcept = default;
    };

    /// The frame @p terminal's page is drawn in under @p profile.
    /// @param terminal The terminal, whose gutter is the one it draws.
    /// @param profile The profile whose margins surround the page.
    /// @return The gutter's width in columns and the profile's margins.
    [[nodiscard]] PageFrame pageFrameOf(vtbackend::Terminal& terminal, config::TerminalProfile const& profile)
    {
        auto const gutterColumns = core::locked(
            terminal, [&] { return vtbackend::gutterLayoutFor(terminal.settings().gutter).totalColumns; });
        return { .gutterColumns = gutterColumns, .margins = profile.margins.value() };
    }
} // namespace

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
    // Taken before the profile and, inside configureTerminal(), the terminal's gutter are replaced: on a
    // reload _config already is the new one, so the old gutter survives only as what the terminal draws.
    auto const fittedFrame = pageFrameOf(_terminal, _profile);
    _profileName = newProfileName;
    _profile = *newProfile;
    configureTerminal();
#ifdef CONTOUR_EXTENSIONS
    reloadExtensions();
#endif

    // A wider or narrower gutter, or other margins, put the grid somewhere else within the same pixels,
    // and no resize event is coming to say so: the page is refitted here, once for both, and with it the
    // window's size hints, which reserve the same frame. A reload that changed neither keeps its page.
    // _display is re-checked at dispatch: pane rebinding may detach it before the GUI thread runs this.
    if (pageFrameOf(_terminal, _profile) != fittedFrame && _display != nullptr)
        _display->post([this]() {
            if (_display)
                _display->refitPage();
        });

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

namespace
{
    /// The settings the engine reads where it uses them, so assigning one is all a reload has to do for
    /// it. Taken from config::sessionSettings, which the terminal was born from, so a reloaded terminal
    /// reads each exactly as a new one would. A setting the engine copies out at construction or turns
    /// into a mode does not belong here: assigning it would change nothing.
    constexpr auto ReloadedSettings = std::tuple {
        &vtbackend::Settings::goodImageProtocol,
        &vtbackend::Settings::gutter,
        &vtbackend::Settings::autoCollapseFoldOnNewCommand,
        &vtbackend::Settings::foldJumpBehavior,
        &vtbackend::Settings::foldingAvailability,
        &vtbackend::Settings::contextSignalling,
        &vtbackend::Settings::contextTintScope,
        &vtbackend::Settings::statusDisplayPosition,
        &vtbackend::Settings::tabNamingMode,
        &vtbackend::Settings::indicatorStatusLine,
        &vtbackend::Settings::syncWindowTitleWithHostWritableStatusDisplay,
        &vtbackend::Settings::visualizeSelectedWord,
        &vtbackend::Settings::autoScrollOnUpdate,
        &vtbackend::Settings::isInsertAfterYank,
        &vtbackend::Settings::blinkStyle,
        &vtbackend::Settings::screenTransitionStyle,
        &vtbackend::Settings::screenTransitionDuration,
        &vtbackend::Settings::cursorMotionAnimationDuration,
        &vtbackend::Settings::smoothLineScrolling,
        &vtbackend::Settings::smoothScrolling,
        &vtbackend::Settings::momentumScrolling,
        &vtbackend::Settings::stickyHeader,
    };
} // namespace

void TerminalSession::configureTerminal()
{
    // Re-read on every application, as everything below is: the terminal is configured as a new one built
    // now would be. Built before the lock, because it reads only this session's own GUI-thread state, so the
    // parser need not wait for it. The light/dark preference is re-read too, not through
    // updateColorPreference(): that re-seeds only when the preference changed, so a reload that changed
    // the profile's colors kept the old ones.
    _currentColorPreference = _app.colorPreference();
    // Under Reduce Motion as a new terminal would be, so a reload does not bring the glide back.
    auto configured = config::motionAdjusted(
        config::sessionSettings(_config, _profile, _currentColorPreference, std::nullopt),
        _app.accessibilityPreferences().motion);

    // Locked: the profile's status line may resize the page, which may write a report, and so may re-seeding
    // the palette with mode 2031 set. See ViNormalMode.
    {
        auto const _ = std::scoped_lock { _terminal };
        sessionLog()("Configuring terminal.");

        _terminal.setWordDelimiters(_config.wordDelimiters.value());
        _terminal.setExtendedWordDelimiters(_config.extendedWordDelimiters.value());
        _terminal.setMouseProtocolBypassModifiers(configured.mouseProtocolBypassModifiers);
        _terminal.setLinkPatterns(configured.linkPatterns);
        _terminal.setLinkDetectionPolicy(configured.linkDetection);
        // A new table means a new detector, whose link ids start over.
        _detectedLinkTooltip = {};
        _terminal.setLinkDecorationMode(configured.linkDecorationMode);
        _terminal.setLinkFollowModifiers(configured.linkFollowModifiers);
        _terminal.setMouseBlockSelectionModifiers(_config.mouseBlockSelectionModifiers.value());
        _terminal.setLastMarkRangeOffset(configured.copyLastMarkRangeOffset);

        sessionLog()("Setting terminal ID to {}.", _profile.terminalId.value());
        _terminal.setTerminalId(_profile.terminalId.value());
        _terminal.setMaxSixelColorRegisters(_config.images.value().maxImageColorRegisters);
        // NB: the image canvas ceiling is deliberately NOT touched here. It is monitor-derived, and
        // configureTerminal() runs on every config reload and profile switch -- paths that
        // configureDisplay() is not on. Re-deriving it needs a window; assigning the config value here
        // is what used to reset the canvas to 0x0 and make sixel images vanish until the next resize.
        // Re-imposed on every application, and RIS returns to it through the factory settings handed over
        // below.
        _terminal.setMode(vtbackend::DECMode::NoSixelScrolling,
                          configured.sixelScrolling == vtbackend::SixelScrolling::Disabled);
        // The configuration is the Set-Up field DEC STD 070 ch. 14 §3 DECSSDT notes ("The type of Status
        // Line can also be changed through a Set-Up field"), so a type it changes is the user's selection, as
        // the toggle's is -- one that also exits a host-writable line the host put up (ch. 14 §2.3), where
        // mintty's term_reconfig() leaves that line alone. A reload or profile switch that leaves the
        // configured type as it was changes nothing, and leaves what the host selected since alone, as
        // term_reconfig() acts only where the configured value changed (mintty term.c:506-509).
        if (_configuredStatusDisplayType != configured.statusDisplayType)
            _terminal.selectConfiguredStatusDisplay(configured.statusDisplayType);
        _configuredStatusDisplayType = configured.statusDisplayType;
        sessionLog()("imageCanvasCeiling={}, sixelScrolling={}, goodImageProtocol={}",
                     _terminal.imageCanvasCeiling(),
                     _config.images.value().sixelScrolling,
                     _config.images.value().goodImageProtocol);

        // XXX
        // if (!terminalView.renderer().renderTargetAvailable())
        //     return;

        configureCursor(_profile.modeInsert.value().cursor);
        // Re-seeded on every application. backgroundColorChanged is left to
        // emitProfileDerivedPropertiesChanged(), which runs outside this lock.
        if (auto const* colorPalette =
                config::preferredColorPalette(_profile.colors.value(), _currentColorPreference))
            _terminal.resetColorPalette(*colorPalette);
        _terminal.setHistoryLimits(_profile.history.value().limits());
        _terminal.setMouseWheelScrollMultiplier(configured.mouseWheelScrollMultiplier);
        _terminal.setHighlightTimeout(configured.highlightTimeout);
        _terminal.viewport().setScrollOff(_profile.modalCursorScrollOff.value());
        // The profile's policy, re-imposed on every profile application -- so a config reload or a
        // profile switch resets a policy the Aa button had pinned, the same way it resets every other
        // profile-derived setting here. The notify is what keeps the button honest about it: without one,
        // an open bar kept showing the lit glyph of a mode the terminal had already stopped using.
        // Already under this function's own lock (see above) -- taking a second one here
        // self-deadlocks, because Terminal's state mutex is a plain non-recursive std::mutex.
        auto searchOptions = _terminal.search().options;
        searchOptions.caseSensitivity = _profile.searchCaseSensitivity.value();
        std::ignore = _terminal.setSearchOptions(searchOptions); // an error is held by Search::problem
        refreshSearchStatusLocked();

        // Posted rather than emitted: this runs under the lock, and a QML binding woken by this signal
        // reads back into the session.
        platform::postToObject(this, [this]() { publishSearchState(); });

        // Assigned as they are, the engine reading each where it uses it -- folding's switch among them,
        // which every fold verb consults.
        std::apply(
            [&](auto const... setting) { ((_terminal.settings().*setting = configured.*setting), ...); },
            ReloadedSettings);

        // The indicator line is parsed out of its three templates, so the reloaded ones need parsing too --
        // unless disabled key bindings have put their own line up, which re-enabling them takes down again
        // through this very call.
        if (_allowKeyMappings)
            _terminal.resetStatusLineDefinition();

        // Copied out of the settings when the terminal was built, so assigning the settings would change
        // nothing: handed over instead. Neither touches the ancestry or who owns the marks.
        _terminal.contexts().setLimits(configured.contextLimits);
        _terminal.markArbiter().setPolicy(configured.contextMarkPolicy);

        // What the terminal was just configured as is what its resets return to, so RIS after a reload
        // restores the reloaded terminal ID rather than the one the session started with.
        _terminal.reconfigureFactorySettings(std::move(configured));
    }
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

void TerminalSession::applyReloadedConfig(config::Config const& config, string const& profileName)
{
    // findProfile() (not profile()): a reloaded file may no longer have the profile this session runs.
    if (config.findProfile(profileName) == nullptr)
    {
        errorLog()("Currently active profile with name '{}' gone; keeping the configuration it ran under.",
                   profileName);
        return;
    }
    std::ignore = reloadConfig(config, profileName);
}

void TerminalSession::followReload(string const& profileName)
{
    // The manager reaches the sessions it hosts under their own profiles. This one needs its own call
    // when the manager does not host it, or when it is to switch profile.
    if (profileName != _profileName || _manager->sessionForId(modelSessionId()) != this)
        applyReloadedConfig(_app.config(), profileName);
}

std::expected<void, std::string> TerminalSession::reloadAllConfigs(string const& profileName)
{
    // Through the manager, so the file is read once, the app's configuration -- what the next new tab
    // is built from -- is updated, and every other open session follows too.
    return _manager->reloadAllSessions().transform([&] { followReload(profileName); });
}

bool TerminalSession::resetConfig()
{
    auto const ec = config::createDefaultConfig(_config.configFile);
    if (ec)
    {
        errorLog()("Failed to load default config at {}; ({}) {}",
                   core::pathToUtf8(_config.configFile),
                   ec.category().name(),
                   ec.message());
        return false;
    }

    // Read back from the user's own path, by the reload every session shares, rather than taken from a
    // default-constructed Config, whose configFile is empty: every later reload reads the path kept here.
    // This session moves to the default file's default profile; the others keep theirs where it has them.
    auto const reloaded = _manager->reloadAllSessions().transform(
        [this] { followReload(_app.config().defaultProfileName.value()); });
    if (!reloaded)
    {
        errorLog()("Failed to load the default config written to {}: {}",
                   core::pathToUtf8(_config.configFile),
                   reloaded.error());
        return false;
    }
    return true;
}

void TerminalSession::openExternally(QUrl const& url, std::string_view what, std::string_view subject)
{
    if (auto const opened = _app.externalLauncher().openUrl(url); !opened)
        errorLog()("Could not open {} \"{}\": {}.", what, subject, platform::describe(opened.error()));
}

void TerminalSession::followHyperlink(vtbackend::HyperlinkInfo const& hyperlink)
{
    // A pane replicating another machine's daemon names paths on THAT machine, whatever authority its
    // application writes: handed to the desktop, file:///home/u/x would open this machine's file of that
    // name. Not opened at all, as such a pane's working directory is not (vtbackend::Terminal::bindReplica).
    auto const localHost = _terminal.fileUrlLocalHost();
    if (hyperlink.isLocal() && !localHost)
    {
        errorLog()("Not opening \"{}\": it names a file on the machine the session runs on, not this one.",
                   hyperlink.uri);
        return;
    }

    // A file:// URL naming THIS machine is a local path, whichever way the application spelled the
    // authority, and read by the resolver the hover tooltip uses (vtbackend::localPathOfFileUrl): the click
    // opens the file the tooltip names, the drive-letter form file://C:/dir that Qt cannot parse included.
    auto const localPath =
        hyperlink.isLocal() ? vtbackend::localPathOfFileUrl(hyperlink.uri, *localHost) : std::nullopt;
    if (!localPath)
    {
        auto const url = QUrl(QString::fromStdString(hyperlink.uri));
        openExternally(url, "hyperlink", url.toString().toStdString());
        return;
    }

    auto const path = QString::fromStdString(*localPath);
    // Through the app's environment, as every variable is read, not getenv(): this runs on the GUI thread
    // while the PTY and render threads are live.
    auto const editor = _app.processEnvironment().get("EDITOR").value_or(std::string {});
    // Asked of the app's filesystem, where the terminal finds the paths it shows, not of the disk.
    auto const action = core::pathFromUtf8(*localPath)
                            .transform([&](std::filesystem::path const& file) {
                                return localFileAction(file, _app.fileSystem(), NativeExecutableRule, editor);
                            })
                            .value_or(LocalFileAction::OpenOnDesktop);

    // A program is run through `contour config <path>`; any other file is handed to $EDITOR the same way.
    // Anything else -- a directory, a missing path -- goes to the desktop, as a plain local file URL: an
    // authority that survived would be read as a network location, file://host/tmp as //host/tmp.
    // Detached: the second Contour runs until its window is closed, and this is the GUI thread.
    if (action == LocalFileAction::OpenOnDesktop)
    {
        auto const url = QUrl::fromLocalFile(path);
        openExternally(url, "hyperlink", url.toString().toStdString());
        return;
    }

    auto args = QStringList { "config", platform::toQString(_config.configFile) };
    if (action == LocalFileAction::Edit)
        args << QString::fromStdString(editor);
    args << path;
    if (auto const started =
            _app.externalLauncher().runDetached(QString::fromStdString(_app.programPath()), args);
        !started)
        errorLog()("Could not {} \"{}\": {}.",
                   describeAction(action),
                   *localPath,
                   platform::describe(started.error()));
}

// }}}
// {{{ QAbstractItemModel impl
QModelIndex TerminalSession::index(int row, int column, QModelIndex const& parent) const
{
    Require(row == 0);
    Require(column == 0);
    // NOTE: if at all, we could expose session attribs like session id, session type
    // (local process), ...?
    core::ignoreUnused(parent);
    return createIndex(row, column, nullptr);
}

QModelIndex TerminalSession::parent(QModelIndex const& child) const
{
    core::ignoreUnused(child);
    return {};
}

int TerminalSession::rowCount(QModelIndex const& parent) const
{
    core::ignoreUnused(parent);
    return 1;
}

int TerminalSession::columnCount(QModelIndex const& parent) const
{
    core::ignoreUnused(parent);
    return 1;
}

QVariant TerminalSession::data(QModelIndex const& index, int role) const
{
    core::ignoreUnused(index, role);
    Require(index.row() == 0);
    Require(index.column() == 0);

    return { _id };
}

bool TerminalSession::setData(QModelIndex const& index, QVariant const& value, int role)
{
    // NB: Session-Id is read-only.
    core::ignoreUnused(index, value, role);
    return false;
}
// }}}

} // namespace contour::session
