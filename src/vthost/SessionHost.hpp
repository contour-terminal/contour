// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// `SessionHost` — the daemon-side owner of terminal sessions.
///
/// This is the second consumer the vtworkspace model was designed for (see
/// vtworkspace/ModelEvents.h): where the Qt GUI maps a SessionId to a TerminalSession,
/// the host maps it to an owned {Pty, Terminal} pair pumped by a dedicated
/// thread, exactly like the GUI's per-session Terminal::mainLoop split.
///
/// Threading: the vtworkspace::SessionModel and all SessionHost methods are confined
/// to the event-loop thread. Session pump threads never touch the model — they
/// marshal completion (PTY closed) through EventLoop::post().

#include <vtbackend/core/TerminalContext.hpp>
#include <vtbackend/screen/Settings.hpp>
#include <vtbackend/screen/Terminal.hpp>
#include <vtbackend/screen/TerminalClocks.hpp>

#include <vtpty/PageSize.hpp>
#include <vtpty/Process.hpp>
#include <vtpty/Pty.hpp>

#include <core/Environment.hpp>
#include <core/net/DeadlineTimer.hpp>
#include <core/net/EventLoop.hpp>
#include <core/platform/FileSystem.hpp>
#include <core/platform/NativeFileSystem.hpp>

#include <cstddef>
#include <cstdint>
#include <expected>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <vector>
// Part of this header's contract, not an implementation detail: the settings a host is constructed
// with — and any a SessionSpawnRequest carries — are normalized through hostedSessionSettings.
#include <vthost/ClientSizePolicy.hpp>
#include <vthost/ExtensionHosting.hpp>
#include <vthost/PermissionWire.hpp>
#include <vthost/ProfileCatalog.hpp>
#include <vthost/SessionSettings.hpp>
#include <vthost/proto/Pdu.hpp>
#include <vtworkspace/LayoutTree.hpp>
#include <vtworkspace/ModelEvents.hpp>
#include <vtworkspace/SessionModel.hpp>

namespace vthost
{

/// Creates the backing PTY for one new session. Production spawns the user's shell (or the
/// override, when a caller names one) over a real PTY (vtpty::Process); tests inject a MockPty
/// factory. The override, when engaged, replaces the daemon's configured program/arguments
/// (only when a program is actually named) and/or working directory — see
/// @ref SessionSpawnRequest::commandOverride and Daemon.cpp's makeShellPtyFactory for the exact
/// overlay rule, which mirrors AppSessionFactory::createPty's for a local session.
using PtyFactory = std::function<std::unique_ptr<vtpty::Pty>(
    vtbackend::PageSize, std::optional<vtpty::Process::ExecInfo> const& commandOverride)>;

/// Creates the backing PTY of a session a host profile was resolved for: the profile's whole shell (a layout
/// pane's command and directory already laid over it), under the profile's own sandbox policy.
/// @see SessionHostConfig::profilePtys
using ProfilePtyFactory = std::function<std::unique_ptr<vtpty::Pty>(
    vtbackend::PageSize, vtpty::Process::ExecInfo const& shell, SandboxEscape sandbox)>;

/// Whether a resize request actually moved a grid.
///
/// Reported rather than re-derived by the caller because only the terminal knows: a request
/// is clamped before it is applied (@see vtbackend::Terminal::clampedTotalPageSize), so a
/// size that differs from the current one can still land on it. Two places deciding this
/// independently is exactly how they come to disagree.
enum class SizeChange : bool
{
    Unchanged, ///< The grid already had this size; nothing was resized.
    Applied,   ///< The grid moved; mirrors of it are now stale.
};

/// Why the host performed no engine operation a client asked for.
enum class OperationError : uint8_t
{
    UnknownSession = 0, ///< No hosted session has that id: it exited, or never existed.
    UnknownOperation,   ///< The operation is not one this host knows, as from a newer client.
};

/// @param error The failure.
/// @return A lower-case phrase for a log line.
[[nodiscard]] constexpr std::string_view describe(OperationError error) noexcept
{
    switch (error)
    {
        case OperationError::UnknownSession: return "unknown session";
        case OperationError::UnknownOperation: return "unknown operation";
    }
    return "unknown error";
}

/// Observer of the hosted sessions' output streams. Every callback fires on the loop thread; any
/// number of observers may subscribe concurrently, so no observer ever silences another by
/// connecting or disconnecting.
///
/// Mostly one per attached client — but NOT only: the daemon itself subscribes a single
/// LastSessionWatcher for `sessionClosed`, so the subscriber count is not a client count.
class SessionStreamEvents
{
  public:
    virtual ~SessionStreamEvents() = default;

    /// @return Whether the observer shows sessions' screens, as a client does: only while one does is the
    ///         indicator row that sessions' extensions draw repainted, and one subscribing is handed it
    ///         current.
    ///         A tmux control-mode observer (tmux::ControlSession) keeps the default, so counts as a client
    ///         for repaints although it shows no indicator row.
    [[nodiscard]] virtual bool mirrorsScreens() const noexcept { return true; }

    /// @p session processed new output — the delta/notification trigger.
    virtual void sessionScreenUpdated(vtworkspace::SessionId /*session*/) {}

    /// A raw PTY output chunk of @p session BEFORE the parser consumed it —
    /// the control-mode %output byte tap.
    virtual void sessionOutput(vtworkspace::SessionId /*session*/, std::string const& /*bytes*/) {}

    /// @p session was destroyed (its shell exited); fires after the host removed
    /// it. Observers holding per-session state must drop it here, or it leaks for
    /// the connection's whole lifetime.
    virtual void sessionClosed(vtworkspace::SessionId /*session*/) {}

    /// @p session's grid changed size, and the resize has already been applied.
    ///
    /// This exists because a resize is the one state change that reaches EVERY observer while
    /// being requested by only one: `vtbackend::Terminal::resizeScreen` raises no screen update, so
    /// a client that did not ask for the resize learns nothing until the session next produces
    /// output — and until then renders the old grid. Announcing it from the host (rather than from
    /// the connection that received the request) is what makes every path that moves a grid —
    /// a client-area re-projection, a per-pane refinement, a layout change — announce it once.
    ///
    /// Fires with the terminal lock RELEASED, because an observer will take it to snapshot.
    virtual void sessionResized(vtworkspace::SessionId /*session*/) {}

    /// @p session rang the bell (BEL).
    virtual void sessionBell(vtworkspace::SessionId /*session*/) {}

    /// @p session raised a desktop notification (OSC 9 / OSC 777 / OSC 99).
    /// @p occasion is what an OSC 99 notification's `o=` stated, or nullopt for OSC 9 / OSC 777.
    virtual void sessionNotify(vtworkspace::SessionId /*session*/,
                               std::string const& /*title*/,
                               std::string const& /*body*/,
                               std::optional<vtbackend::DisplayOccasion> /*occasion*/)
    {
    }

    /// @p session wrote the clipboard (OSC 52); @p data is the raw, decoded text.
    /// The daemon forwards it unconditionally — the CLIENT applies its own
    /// clipboard-write permission.
    virtual void sessionCopyToClipboard(vtworkspace::SessionId /*session*/, std::string const& /*data*/) {}

    /// A status-line extension of @p session asks whether it may run its declared commands. Fires once per
    /// question while it stays open: a client attaching later is asked through
    /// SessionHost::pendingPermissionRequests() instead.
    virtual void sessionPermissionRequested(vtworkspace::SessionId /*session*/,
                                            proto::PermissionRequest const& /*request*/)
    {
    }
};

/// Whose a window is, as one attached client asks about it.
///
/// A window an Attach connection's attach or NewWindow created is that connection's own — and stays
/// claimed after it leaves, so a second GUI never inherits a first one's window. A window anything else
/// created (the startup layout, a control connection) is unclaimed: the primary viewer follows it.
enum class WindowClaim : uint8_t
{
    Unclaimed = 0, ///< No client's: created by the host itself, or through a control connection.
    Yours = 1,     ///< Created for the client asking.
    Others = 2,    ///< Created for another client, attached or gone.
};

/// What a hosted session hands a desktop notification on as: its title, its body, and the occasion
/// an OSC 99 notification stated -- nullopt for OSC 9 / OSC 777, which state none.
using SessionNotifyCallback =
    std::function<void(std::string, std::string, std::optional<vtbackend::DisplayOccasion>)>;

/// A hosted session's status-line extensions, as its host hands them to it.
struct HostedSessionExtensions
{
    /// The session's extensions; null for none, which builds its terminal without any.
    std::unique_ptr<ISessionExtensions> extensions {};
    /// An extension published a frame. Invoked on the extension's WORKER, holding no lock: it must not
    /// block, and touches no session state (the host marshals it onto the loop).
    std::function<void()> onPublished {};
    /// The extensions began or stopped following the foreground process, as a frame they published shows --
    /// what they follow is known only once they have loaded. Invoked on the extension's WORKER, holding no
    /// lock, and only on such a change: it must not block (the host marshals it onto the loop).
    std::function<void()> onForegroundFollowingChanged {};
};

/// One hosted session: the terminal (owning its PTY) plus the pump thread
/// feeding it, mirroring the GUI's TerminalSession::mainLoop.
class HostedSession
{
  public:
    /// @param id The model-side session identity.
    /// @param pty The backing PTY (moved into the terminal).
    /// @param settings The terminal's factory settings.
    /// @param identity The machine the daemon runs on, which the terminal decides a command's working
    ///        directory's locality against.
    /// @param clocks The clocks the terminal reads. Borrowed: they must outlive the session.
    /// @param fileSystem Where the terminal looks printed paths up. Borrowed: it must outlive the session.
    /// @param onScreenUpdated Invoked on the PUMP thread after each processed
    ///        input batch (the host marshals it onto the loop).
    /// @param onClosed Invoked on the PUMP thread once the PTY closed and the
    ///        pump loop ended (the host marshals it onto the loop).
    /// @param env The process environment the hosted terminal reads through.
    /// @param extensions The session's status-line extensions, started once the terminal exists; none by
    ///        default.
    /// @param profileName The host profile the session was spawned under; empty for none named.
    HostedSession(vtworkspace::SessionId id,
                  core::Environment const& env,
                  std::unique_ptr<vtpty::Pty> pty,
                  vtbackend::Settings settings,
                  vtbackend::HostIdentity identity,
                  vtbackend::TerminalClocks clocks,
                  core::platform::FileSystem const& fileSystem,
                  std::function<void()> onScreenUpdated,
                  std::function<void()> onBell,
                  SessionNotifyCallback onNotify,
                  std::function<void(std::string)> onCopyToClipboard,
                  std::function<void()> onClosed,
                  HostedSessionExtensions extensions = {},
                  std::string profileName = {});

    /// Joins the pump thread; the PTY must have been closed first (terminate()).
    ~HostedSession();

    HostedSession(HostedSession const&) = delete;
    HostedSession& operator=(HostedSession const&) = delete;
    HostedSession(HostedSession&&) = delete;
    HostedSession& operator=(HostedSession&&) = delete;

    /// Starts the PTY device and the pump thread. Not started in tests that
    /// drive the terminal directly via writeToScreen.
    void start();

    /// Closes the PTY device, which ends the pump loop.
    void terminate();

    /// What the pump does with what a client handed a session: writes it to the PTY, or parses it.
    using Delivery = void (*)(vtbackend::Terminal& terminal, std::string_view bytes);

    /// Hands @p bytes to the pump thread, which delivers them with @p deliver between two reads, after
    /// everything handed over before them. The loop thread handing them over must neither park on a PTY write
    /// while the application leaves its input unread nor sleep in a parse (DECSCLM). Without a pump (not
    /// started), delivered at once, the caller standing in for it.
    /// @param deliver What the pump does with @p bytes.
    /// @param bytes What a client handed over.
    void deliverOnPump(Delivery deliver, std::string_view bytes);

    /// Writes @p bytes, a client's input, to the PTY, through deliverOnPump().
    /// @param bytes The input, as the client encoded it.
    void writeInput(std::string_view bytes);

    /// @return How many bytes handed to deliverOnPump() are not yet delivered: queued, or being delivered.
    [[nodiscard]] std::size_t inputBacklog() const;

    [[nodiscard]] vtworkspace::SessionId id() const noexcept { return _id; }
    [[nodiscard]] vtbackend::Terminal& terminal() noexcept { return _terminal; }

    /// @return The host profile the session was spawned under — the one resolved, so the default's name
    ///         when the name asked for was unknown — or empty when it was spawned under none.
    [[nodiscard]] std::string const& profileName() const noexcept { return _profileName; }

    /// @return Whether the session runs status-line extensions.
    [[nodiscard]] bool hasExtensions() const noexcept { return _extensions != nullptr; }

    /// @return Whether the session's extensions follow its foreground program.
    [[nodiscard]] bool followsForegroundProcess() const noexcept
    {
        return _extensions != nullptr
               && _extensions->subscriptions().test(vtbackend::StatusLineEventKind::ForegroundProcess);
    }

    /// Has the indicator row its extensions draw repainted on the loop, once for however many requests arrive
    /// before it is: through the same pending flag a published frame goes through. Without extensions,
    /// nothing. Any thread.
    void requestStatusLineRepaint() const
    {
        if (_events.onStatusLineExtensionPublished)
            _events.onStatusLineExtensionPublished();
    }

    /// Forgets what the session's extensions followed: they were replaced by ones that have not loaded yet,
    /// whose first frame that follows the foreground process must count as the change it is. Loop thread.
    void forgetForegroundFollowing() noexcept { _foregroundFollowing.reset(); }

    /// Tells the session's extensions of a new foreground program, when they follow it and it changed. Asks
    /// the PTY, so it reads `/proc` (or its platform's equivalent): loop thread, at most once a second.
    void pollForegroundProcess();

  private:
    /// The Terminal::Events glue: forwards the terminal events the daemon
    /// mirrors — the per-batch screen update, the bell, desktop notifications
    /// (OSC 9/777/99) and OSC 52 clipboard writes — to the host's callbacks,
    /// and answers for the host's identity; everything else keeps the Null
    /// default. Adding a mirrored event is a callback here plus a row in
    /// SessionStreamEvents.
    struct Events final: vtbackend::Terminal::NullEvents
    {
        Events(vtbackend::HostIdentity hostIdentity,
               std::function<void()> screenUpdated,
               std::function<void()> bell,
               SessionNotifyCallback notify,
               std::function<void(std::string)> copyToClipboard,
               std::function<void()> extensionPublished):
            identity(std::move(hostIdentity)),
            onScreenUpdated(std::move(screenUpdated)),
            onBell(std::move(bell)),
            onNotify(std::move(notify)),
            onCopyToClipboard(std::move(copyToClipboard)),
            onStatusLineExtensionPublished(std::move(extensionPublished))
        {
        }

        /// Set at construction and never reassigned: localIdentity() hands out views into it on the
        /// parser thread.
        vtbackend::HostIdentity identity;
        std::function<void()> onScreenUpdated;
        std::function<void()> onBell;
        SessionNotifyCallback onNotify;
        std::function<void(std::string)> onCopyToClipboard;
        /// A status-line extension published a frame: invoked on its worker. Fixed at construction, as the
        /// worker reads it without a lock.
        std::function<void()> onStatusLineExtensionPublished;
        /// The app asked to show the host-writable status line (DECSSDT 2); the
        /// daemon honors it by switching the terminal's status display on (the
        /// frontend decision the GUI makes too).
        std::function<void()> onShowHostWritableStatusLine;

        void screenUpdated() override
        {
            if (onScreenUpdated)
                onScreenUpdated();
        }
        void bell() override
        {
            if (onBell)
                onBell();
        }
        void notify(std::string_view title, std::string_view body) override
        {
            if (onNotify)
                onNotify(std::string { title }, std::string { body }, std::nullopt);
        }
        void showDesktopNotification(vtbackend::DesktopNotification const& notification) override
        {
            // The occasion travels, so the client decides it by the rule a local OSC 99 is decided by.
            if (onNotify)
                onNotify(notification.title, notification.body, notification.occasion);
        }
        void copyToClipboard(std::string_view data) override
        {
            if (onCopyToClipboard)
                onCopyToClipboard(std::string { data });
        }
        void requestShowHostWritableStatusLine() override
        {
            if (onShowHostWritableStatusLine)
                onShowHostWritableStatusLine();
        }
        void extensionFramePublished() override
        {
            if (onStatusLineExtensionPublished)
                onStatusLineExtensionPublished();
        }
        /// The host's identity, as TerminalSession reports the GUI's: a record's working directory is
        /// local when it names the machine the daemon runs on.
        [[nodiscard]] vtbackend::LocalIdentity localIdentity() const noexcept override
        {
            return identity.view();
        }
    };

    void pumpLoop();

    /// What a client handed over, waiting for the pump. @see deliverOnPump()
    struct PendingDelivery
    {
        Delivery deliver;  ///< What the pump does with @ref bytes.
        std::string bytes; ///< What it does it with.
    };

    /// Delivers what deliverOnPump() queued, oldest first, on the pump thread.
    void deliverPending();

    vtworkspace::SessionId _id;
    std::string _profileName; ///< @see profileName
    /// The session's status-line extensions; null without any. Declared before _terminal, which it outlives:
    /// ~Terminal revokes the sink their workers publish through before they are destroyed.
    std::unique_ptr<ISessionExtensions> _extensions;
    /// Whether the extensions follow the foreground process, as a worker last saw it. Before _events, whose
    /// published-frame callback reads it.
    vtbackend::StatusLineSubscriptionWatch _foregroundFollowing {
        vtbackend::StatusLineEventKind::ForegroundProcess
    };
    Events _events; ///< Must outlive _terminal (referenced by it).
    vtbackend::Terminal _terminal;
    /// The foreground program the extensions were last told of, so that each is told once; extensions that
    /// load or subscribe later hear it from the terminal, which remembers it. Loop thread only.
    std::string _foregroundProcessName;
    std::function<void()> _onClosed;
    std::unique_ptr<std::thread> _pumpThread;
    /// Guards _pending and _pendingBytes: the loop thread queues, the pump thread delivers.
    mutable std::mutex _pendingMutex;
    /// What clients handed over and the pump has not taken yet, oldest first; consecutive bytes delivered
    /// alike share an entry.
    std::vector<PendingDelivery> _pending;
    std::size_t _pendingBytes = 0; ///< What _pending holds, and what the pump took off it and is delivering.
};

/// What ONE session-creating request overrides about the session it spawns.
///
/// A struct rather than a parameter so a future per-spawn knob is a field here instead of a
/// fourth argument at three call sites.
struct SessionSpawnRequest
{
    /// The emulation settings for this session; nullopt uses the host's factory settings.
    ///
    /// This is how a client's own profile reaches the sessions IT creates without touching sessions
    /// that already exist — the property that keeps two clients on different profiles from fighting
    /// over one session. Whatever is supplied still goes through
    /// @ref hostedSessionSettings.
    std::optional<vtbackend::Settings> settings {};

    /// Overrides the program/arguments and/or working directory this session's shell runs —
    /// currently engaged only by @ref SessionHost::realizeStartupLayout for a startup layout
    /// pane's `command`/`arguments`/`directory`. Passed straight through to the @ref PtyFactory,
    /// which applies it with the same overlay rule as a local session's
    /// AppSessionFactory::createPty: `program`/`arguments` overlay only when `program` is
    /// non-empty (a directory-only override must not wipe the daemon's default arguments), and
    /// `workingDirectory` overlays independently whenever it is non-empty.
    std::optional<vtpty::Process::ExecInfo> commandOverride {};

    /// The host profile the session runs under, resolved through SessionHostConfig::profiles: its shell
    /// and settings replace @ref settings and the daemon's shell, and @ref commandOverride still overlays
    /// that shell. Empty names the catalog's default profile, whose shell it runs — @ref settings, when
    /// set, still shaping its emulation; without a catalog the paths above stay as they are.
    std::string profileName {};
};

/// Who feeds a hosted session's terminal from its PTY.
enum class SessionPumping : uint8_t
{
    Manual = 0,   ///< Nobody: the caller drives the terminals directly, as tests do.
    Threaded = 1, ///< Each session runs its own PTY pump thread, as a daemon does.
};

/// When what clients hand a session -- input for its application, text for its screen -- backs up while the
/// application leaves its input unread: the connection it comes from is read no further once the session
/// holds
/// @ref high bytes of it, and is read again once the session holds no more than @ref low. Backpressure rather
/// than a refusal: nothing is dropped, and no client but the one filling the session waits.
struct InputWatermarks
{
    std::size_t high = std::size_t { 1024 } * 1024; ///< Where a connection's reads pause.
    std::size_t low = std::size_t { 256 } * 1024;   ///< Where they resume.
};

/// How a SessionHost is set up beyond its collaborators. Fixed at construction: two
/// differently-configured hosts are two different hosts, not one in two states.
struct SessionHostConfig
{
    /// Whether new sessions start their PTY pump thread.
    SessionPumping pumping = SessionPumping::Threaded;
    /// How the authoritative client area is resolved when several attached clients report
    /// different ones.
    ClientSizePolicy sizePolicy = ClientSizePolicy::Latest;
    /// The profile's configured startup layout. A pane's `command`/`arguments`/`directory` are
    /// honored (see @ref SessionSpawnRequest::commandOverride), and so is its `profile` (else its tab's)
    /// when @ref profiles is set; without a catalog a pane naming one is logged and otherwise ignored.
    /// Empty `tabs` (the default) keeps today's behavior: the window starts with no tab at all.
    vtworkspace::Layout startupLayout {};
    /// The machine the daemon runs on, as every session it spawns reports it. The default knows
    /// nothing, so only an OSC 7 URL with no host counts as local.
    /// @see vtbackend::readHostIdentity
    vtbackend::HostIdentity identity {};
    /// The clocks every session it spawns reads: command durations, record and row times, and the session
    /// epoch, all of which travel to the clients. Borrowed: they must outlive the host. The default is the
    /// system's; tests pass manual ones to drive time.
    vtbackend::TerminalClocks clocks = vtbackend::TerminalClocks::system();
    /// Where every session it spawns looks up the paths printed on its screen. Borrowed: it must outlive
    /// the host. The default is the machine's own.
    std::reference_wrapper<core::platform::FileSystem const> fileSystem =
        core::platform::NativeFileSystem::instance();
    /// Where a connection's reads pause while its input backs up in a session, and resume.
    InputWatermarks inputWatermarks {};
    /// The status-line extensions every session it spawns runs; null for none, which constructs, polls and
    /// wakes nothing of them (A2). Shared only so that a test can keep a handle on a fake: the host destroys
    /// its share after its sessions, and nobody else is to hold one past the host. @see IExtensionHosting
    std::shared_ptr<IExtensionHosting> extensions {};
    /// The host's profiles, in which every spawn resolves its shell (SessionSpawnRequest::profileName, or the
    /// default profile for none) and passes it to the PTY factory as a whole shell; null for none, which
    /// keeps the factory settings and the PTY factory's own shell for every session.
    std::shared_ptr<ProfileCatalog const> profiles {};
    /// What spawns a session resolved through @ref profiles, so that it runs under its own profile's sandbox
    /// policy; null hands the resolved shell to the host's PtyFactory as its override instead, which knows no
    /// policy (tests that do not ask about one).
    ProfilePtyFactory profilePtys {};
    /// Which run of the daemon this host is, as every ServerHello states it
    /// (proto::ServerHello::daemonInstance): the daemon draws it at random as it starts. 0 states none, as an
    /// embedded host or a test does.
    uint64_t instance = 0;
};

/// Owns every hosted session and the authoritative vtworkspace::SessionModel,
/// fanning each completed model change out to subscribed observers.
class SessionHost final: public vtworkspace::ModelEvents
{
  public:
    /// @param loop The event loop all model mutation is confined to.
    /// @param ptyFactory Creates the backing PTY for each new session.
    /// @param settings Factory settings applied to every session's terminal, normalized through
    ///        @ref hostedSessionSettings — so a caller handing over a bare `vtbackend::Settings`
    ///        still gets a host whose sessions can be served, rather than one whose every scrolling
    ///        batch degenerates to a full snapshot.
    /// @param env The process environment every session this host spawns reads through.
    /// @param config How the host is set up: pumping, the client-area policy, the startup layout
    ///        and the machine identity.
    SessionHost(core::net::EventLoop& loop,
                PtyFactory ptyFactory,
                vtbackend::Settings settings,
                core::Environment const& env,
                SessionHostConfig config = {});

    /// Ends every session. With extensions, in the order nothing they started can outlive what it reaches:
    /// the sessions go first (every runner posts its last job), then IExtensionHosting::stop() cancels the
    /// commands in flight and joins the extension threads, then one turn of the loop drops what they posted
    /// for sessions that are gone, while what those jobs hold can still be released; the extensions go
    /// last, with the members. Not while the loop runs a turn, on any thread: with extensions that is a
    /// precondition, checked.
    ~SessionHost() override;

    SessionHost(SessionHost const&) = delete;
    SessionHost& operator=(SessionHost const&) = delete;
    SessionHost(SessionHost&&) = delete;
    SessionHost& operator=(SessionHost&&) = delete;

    /// @return The authoritative session/layout model.
    [[nodiscard]] vtworkspace::SessionModel& model() noexcept { return _model; }

    /// @return The normalized factory settings every session gets unless a
    ///         @ref SessionSpawnRequest overrides them. Callers layering a client's stated
    ///         preference onto the daemon's own settings need this as their base.
    [[nodiscard]] vtbackend::Settings const& settings() const noexcept { return _settings; }

    /// @return The machine the daemon runs on, as every session it spawns reports it — and as a
    ///         snapshot states it, so a client on another machine can tell "local" there from local
    ///         to itself.
    [[nodiscard]] vtbackend::HostIdentity const& identity() const noexcept { return _identity; }

    /// @return Which run of the daemon this host is; 0 when none was stated. @see SessionHostConfig::instance
    [[nodiscard]] uint64_t instance() const noexcept { return _instance; }

    /// @return The host's window (the daemon starts with exactly one).
    [[nodiscard]] vtworkspace::WindowId windowId() const noexcept { return _window; }

    /// @return The authoritative client area, in cells: layout projection and
    ///         PTY sizes derive from it. Starts at the settings' page size.
    [[nodiscard]] vtpty::PageSize pageSize() const noexcept { return _pageSize; }

    /// Records what @p client can display and re-resolves the authoritative client area from every
    /// attached client's report: each tab's leaves are then resized to their cell-space projection
    /// under the resolved area, and future sessions spawn at the projected sizes.
    ///
    /// A report is not an instruction. Several clients share one grid, so the size is a function of
    /// ALL of their reports under @ref ClientSizePolicy, not of the last one to arrive — which is
    /// what this used to be, and what made two differently-sized clients take turns resizing every
    /// application on the daemon.
    ///
    /// The registry is keyed by the client's stream subscription, so `unsubscribeStream` drops the
    /// entry: a client's area counts for exactly as long as the client is there to be told about
    /// the consequences.
    /// @param client The reporting client, identified by its stream observer.
    /// @param size What that client can display, in cells.
    /// @return Whether anything moved. Callers resync their mirrors on @c Applied only:
    ///         a resync is a full-grid snapshot, so treating a no-op as a change costs a
    ///         client its entire screen state for nothing.
    SizeChange applyClientSize(SessionStreamEvents* client, vtpty::PageSize size);

    /// Resizes ONE session's terminal to the grid a client actually renders that pane at,
    /// leaving the client area and every other pane alone.
    ///
    /// The projection `applyClientSize` performs is the host's own estimate — it must exist, since
    /// clients that never report per-pane sizes (tmux control mode) are served from it — but it
    /// divides by split RATIO, where a client laying panes out in pixels (or whose user dragged a
    /// divider) knows the real answer. This refines one leaf on top of that estimate. The next
    /// structural change re-projects and thereby discards the refinement, which is correct: those
    /// sizes are stale the moment the tree changes, and the client re-reports.
    /// @param session The session whose terminal to resize; unknown ids are ignored.
    /// @param size The pane's grid, in cells.
    /// @return Whether that session's grid actually moved (@c Unchanged for an unknown id).
    SizeChange applyPaneSize(vtworkspace::SessionId session, vtpty::PageSize size);

    /// Creates a tab whose first pane is backed by a freshly spawned session,
    /// using the same pre-mint handshake as the GUI: the backing session is
    /// created first, then the model's allocator hands its id back.
    /// @param request What this creation overrides about the session it spawns.
    /// @param beside  Which WINDOW to create the tab in, named by a session it already hosts —
    ///        the addressing every other layout verb uses (@see proto::SplitPane), because a
    ///        window id is minted per model and a client's are not the host's. Unset, or naming a
    ///        session this host does not hold, falls back to @ref windowId().
    /// @return The created tab, or nullptr on failure (nothing is leaked).
    vtworkspace::Tab* createTab(SessionSpawnRequest const& request = {},
                                std::optional<vtworkspace::SessionId> beside = std::nullopt);

    /// Creates a NEW window with a first tab (an empty window is useless), backed by
    /// a fresh session — the daemon side of a client "new window" (B4).
    /// @param request What this creation overrides about the session it spawns.
    /// @param creator The attached client the window is created for, which then claims it (@see
    ///        windowClaim) before its first tab is announced; null for a window no client claims.
    /// @return The created window, or nullptr on failure (nothing is leaked).
    vtworkspace::Window* createWindow(SessionSpawnRequest const& request = {},
                                      SessionStreamEvents const* creator = nullptr);

    /// @param window The window asked about.
    /// @param client The attached client asking.
    /// @return Whether @p window was created for @p client, for another client, or for none.
    [[nodiscard]] WindowClaim windowClaim(vtworkspace::WindowId window,
                                          SessionStreamEvents const* client) const noexcept;

    /// Counts @p client as an attached native client (`ConnectionRole::Attach`), from its handshake on.
    /// Idempotent.
    /// @param client The connection, by its stream observer.
    void noteClientAttached(SessionStreamEvents const* client);

    /// Stops counting @p client, releases its primary-viewer role, and leaves the windows it claimed
    /// claimed by nobody present (still not unclaimed). Idempotent, and a no-op for a client never counted.
    /// @param client The connection, by its stream observer.
    void noteClientDetached(SessionStreamEvents const* client);

    /// @return How many native Attach connections are handshaken. Neither a Control connection nor a tmux
    ///         gateway counts — unlike the daemon's ClientCensus, which counts every client for idle exit.
    [[nodiscard]] std::size_t connectedClients() const noexcept { return _attachedClients.size(); }

    /// Makes @p client the primary viewer — the client whose launch took every window, which also follows
    /// the windows no client claims, created later — unless another client holds that role.
    /// @param client An attached client.
    void claimPrimaryViewer(SessionStreamEvents const* client);

    /// @param client An attached client.
    /// @return Whether @p client is the primary viewer.
    [[nodiscard]] bool isPrimaryViewer(SessionStreamEvents const* client) const noexcept
    {
        return client != nullptr && _primaryViewer == client;
    }

    /// Splits @p tab's active pane, backing the new leaf with a fresh session.
    /// @param tab The tab whose active pane splits.
    /// @param orientation The split axis.
    /// @param ratio The first child's share.
    /// @param request What this creation overrides about the session it spawns.
    void splitActivePane(vtworkspace::TabId tab,
                         vtworkspace::SplitState orientation,
                         double ratio = 0.5,
                         SessionSpawnRequest const& request = {});

    /// @return The terminal backing @p session, or nullptr if unknown.
    [[nodiscard]] vtbackend::Terminal* terminal(vtworkspace::SessionId session) noexcept;

    /// @return The hosted session @p session, or nullptr if unknown.
    [[nodiscard]] HostedSession const* hostedSession(vtworkspace::SessionId session) const noexcept;

    /// Swaps the host's profile catalog for @p profiles, as the host's configuration was reloaded.
    ///
    /// Live reconfiguration is the feature (AGENT.md's carve-out): a daemon outlives many edits of its
    /// configuration, and restarting it would end every session. New sessions resolve against
    /// @p profiles; every running session takes its profile's scrollback limits from it, under its
    /// terminal's lock, and its clients are told. The shell, environment and directory of a running
    /// session never change — its process already runs with them.
    /// @param profiles The new catalog; null keeps the current one.
    void replaceProfiles(std::shared_ptr<ProfileCatalog const> profiles);

    /// Hands @p bytes, a client's input, to session @p session's PTY without parking the calling loop thread:
    /// the session's pump writes them. @see HostedSession::writeInput()
    /// @param session The session the input is for; an unknown one drops it.
    /// @param bytes The input, as the client encoded it.
    void writeInput(vtworkspace::SessionId session, std::string_view bytes);

    /// When a client last handed @p session input, on the host loop's clock. Loop thread only.
    ///
    /// The time doubles as the input's identity (FlushInputs::answeredInput compares stamps), which holds
    /// while the loop runs on a clock that advances between inputs, as SteadyClock does. A per-turn
    /// CachedClock would give every input of one turn the same stamp and one echo flush between them.
    /// @param session The session asked about.
    /// @return The time of the session's last writeInput(); nullopt for none or an unknown session.
    [[nodiscard]] std::optional<core::platform::SteadyTimePoint> lastInputAt(
        vtworkspace::SessionId session) const;

    /// @param session The session asked about.
    /// @return How many bytes of a client's input @p session holds unwritten; zero for an unknown one.
    [[nodiscard]] std::size_t inputBacklog(vtworkspace::SessionId session) const;

    /// @return Where a connection's reads pause while its input backs up, and resume. @see InputWatermarks
    [[nodiscard]] InputWatermarks const& inputWatermarks() const noexcept { return _inputWatermarks; }

    /// Performs the engine operation a client asked for on the terminal of the session it names, and every
    /// attached mirror follows through its deltas: one the loop thread performs announces a screen update
    /// itself, and a write handed to the session's pump raises one as it is parsed, as the application's
    /// output does.
    ///
    /// What each operation does, and on which thread, is one row of a table in the implementation: adding an
    /// operation is a row there and an enumerator in proto::EngineOperation.
    /// @param request The session, the operation, and what it acts with: WriteScreen's text, a user
    ///        mark's row.
    /// @return Nothing on success; why nothing was performed otherwise.
    [[nodiscard]] std::expected<void, OperationError> performOperation(
        proto::PerformOperation const& request);

    /// Closes an open status-line extension's permission request with a client's answer, which is recorded
    /// for every session: from now on the extension's commands run, or are refused. A dismissed prompt
    /// (Pending) closes it and records nothing; the question is asked again when an extension next asks. An
    /// answer to no open request is ignored: only a question the host asked is a client's to answer.
    /// @param hostId The installed extension that asked.
    /// @param digest The manifest digest the answer is bound to.
    /// @param answer The answer.
    void answerPermission(std::string_view hostId, std::string_view digest, GrantState answer);

    /// @return Whether the hosted programs are asked who runs in the foreground: only while a session's
    ///         extensions follow it.
    [[nodiscard]] bool pollsForegroundProcesses() const noexcept
    {
        return _foregroundPoll.has_value() && !_foregroundPoll->settled();
    }

    /// @return The permission requests no client has answered yet, oldest first: what a client attaching now
    ///         is to be asked. Valid until the host next changes them.
    [[nodiscard]] std::span<proto::PermissionRequest const> pendingPermissionRequests() const noexcept
    {
        return _pendingPermissions;
    }

    /// @return The number of live hosted sessions.
    [[nodiscard]] std::size_t sessionCount() const noexcept { return _sessions.size(); }

    /// @return Whether a subscribed observer shows sessions' screens (@see
    /// SessionStreamEvents::mirrorsScreens):
    ///         whether a client is attached.
    [[nodiscard]] bool hasScreenMirrors() const noexcept;

    /// Registers @p observer for every completed model change. Not owned.
    void subscribe(vtworkspace::ModelEvents* observer);

    /// Removes @p observer. Idempotent.
    void unsubscribe(vtworkspace::ModelEvents* observer);

    /// Registers @p observer for session output-stream events (screen updates
    /// and the raw byte tap). Not owned; callbacks fire on the loop thread.
    void subscribeStream(SessionStreamEvents* observer);

    /// Removes @p observer from the stream fan-out. Idempotent.
    void unsubscribeStream(SessionStreamEvents* observer);

    /// Handles a session whose PTY closed (shell exited): prunes its pane from
    /// the model (prune-then-terminate) and destroys the session. Invoked on
    /// the loop thread — by the pump's posted completion in production, or
    /// directly by tests.
    void handleSessionExit(vtworkspace::SessionId session);

    // vtworkspace::ModelEvents — every completed change fans out to subscribers.
    void tabAdded(vtworkspace::WindowId window, vtworkspace::TabId tab, int index) override;
    void tabClosed(vtworkspace::WindowId window, vtworkspace::TabId tab, int index) override;
    void tabMoved(vtworkspace::WindowId window, vtworkspace::TabId tab, int fromIndex, int toIndex) override;
    void activeTabChanged(vtworkspace::WindowId window, vtworkspace::TabId tab, int index) override;
    void paneSplit(vtworkspace::TabId tab,
                   vtworkspace::PaneId splitNode,
                   vtworkspace::PaneId newLeaf) override;
    void paneClosed(vtworkspace::TabId tab,
                    vtworkspace::PaneId closed,
                    vtworkspace::PaneId survivor) override;
    void activePaneChanged(vtworkspace::TabId tab, vtworkspace::PaneId leaf) override;
    void paneRatioChanged(vtworkspace::TabId tab, vtworkspace::PaneId splitNode, double ratio) override;
    void tabTitleChanged(vtworkspace::TabId tab) override;
    void tabColorChanged(vtworkspace::TabId tab) override;
    void paneOrientationChanged(vtworkspace::TabId tab,
                                vtworkspace::PaneId splitNode,
                                vtworkspace::SplitState state) override;
    void paneSwapped(vtworkspace::TabId tab, vtworkspace::PaneId a, vtworkspace::PaneId b) override;
    void paneZoomChanged(vtworkspace::TabId tab, std::optional<vtworkspace::PaneId> zoomedLeaf) override;
    void paneTreeRestructured(vtworkspace::TabId tab) override;

  private:
    /// Spawns and registers the backing session for the next model allocation
    /// (the pre-mint half of the handshake).
    ///
    /// The single funnel every creation path goes through, which is why the host invariants are
    /// re-applied here to a request's settings rather than trusted from the caller.
    /// @param request What this creation overrides about the session.
    /// @return The minted id, or nullopt if the PTY factory failed.
    [[nodiscard]] std::optional<vtworkspace::SessionId> seedSession(SessionSpawnRequest const& request);

    /// What a spawn runs with once the host's profiles have had their say.
    struct ResolvedSpawn
    {
        std::optional<vtbackend::Settings> settings;             ///< Nullopt for the factory settings.
        std::optional<vtpty::Process::ExecInfo> commandOverride; ///< The shell, or the pane's override.
        std::string profileName {};              ///< The named profile resolved; empty for none.
        std::optional<SandboxEscape> sandbox {}; ///< The profile's policy; nullopt without one.
    };

    /// Resolves @p request in the host's profiles (@see SessionHostConfig::profiles).
    /// @param request What the spawn asked for.
    /// @param id The session about to be spawned, for the log.
    /// @return What it runs with.
    [[nodiscard]] ResolvedSpawn resolveSpawn(SessionSpawnRequest const& request,
                                             vtworkspace::SessionId id) const;

    /// @param spawn What the spawn runs with.
    /// @return Its PTY, through the profile PTY factory when a profile was resolved and one is set; null when
    ///         the factory failed.
    [[nodiscard]] std::unique_ptr<vtpty::Pty> makePty(ResolvedSpawn const& spawn) const;

    /// Destroys session @p session and forgets everything kept beside it, so no erase site can leave a
    /// stale entry behind.
    /// @param session The id of the session to drop; an unknown one is a no-op.
    void eraseSession(uint64_t session);

    /// Realizes @p layout's tabs into @p window in order, via the shared
    /// vtworkspace::realizeLayoutTab. Called once from the constructor body (never the
    /// initializer list — @c _window is the LAST member to construct). Each leaf's
    /// command/arguments/directory become a @ref SessionSpawnRequest::commandOverride, and its
    /// profile (else its tab's) the @ref SessionSpawnRequest::profileName.
    /// @param window The window to realize every tab into (the host's own, freshly created).
    /// @param layout The resolved startup layout; a no-op when its `tabs` is empty.
    void realizeStartupLayout(vtworkspace::WindowId window, vtworkspace::Layout const& layout);

    /// Resizes every leaf's terminal to its cell-space projection under the
    /// current client area — run after every layout-shape change so PTY sizes
    /// never drift from the advertised layout.
    /// @return Whether any leaf's grid moved, so callers can skip a resync of mirrors
    ///         whose grids are already correct.
    SizeChange reprojectLayouts();

    /// Resizes @p backing to @p size while holding its terminal lock, then announces the change to
    /// every stream subscriber (@see SessionStreamEvents::sessionResized).
    ///
    /// resizeScreen mutates shared terminal state and does NOT lock internally; the session's pump
    /// thread writes the same grid under _stateMutex. Every resize path here must therefore hold the
    /// terminal lock across the call, exactly as the GUI's sole caller does
    /// (TerminalSession::attachDisplay) — hence the one helper both of ours go through. Being that
    /// one helper is also why the announcement belongs here: no resize path can forget to make it.
    /// @param session The session whose grid this is, for the announcement.
    /// @param backing The terminal to resize.
    /// @param size The requested grid, in cells; clamped by the terminal before it is applied.
    /// @return Whether the grid moved.
    [[nodiscard]] SizeChange resizeLocked(vtworkspace::SessionId session,
                                          vtbackend::Terminal& backing,
                                          vtpty::PageSize size);

    /// Fans one completed model change out to every subscriber, invoking @p method
    /// on each with @p args. Single-sources the observer loop the ModelEvents
    /// overrides below all share.
    /// @param method A vtworkspace::ModelEvents member function pointer.
    /// @param args The event arguments (copied once here, then passed to each observer).
    template <typename Method, typename... Args>
    void fanOut(Method method, Args... args)
    {
        for (auto* observer: _subscribers)
            (observer->*method)(args...);
    }

    /// The same, for the session STREAM observers — the other subscriber list.
    /// @param method A SessionStreamEvents member function pointer.
    /// @param args The event arguments (copied once here, then passed to each observer).
    template <typename Method, typename... Args>
    void fanOutStream(Method method, Args... args)
    {
        for (auto* observer: _streamSubscribers)
            (observer->*method)(args...);
    }

    /// Recomputes `_pageSize` from every attached client's reported area under the configured
    /// policy. Called whenever the reported set changes — a report arriving, or a client leaving.
    /// Leaves `_pageSize` alone when no client has reported, since the hosted sessions still need
    /// to be some size.
    void resolveAuthoritativeArea();

    /// The session hooks a new session's status-line extensions reach the host through, every one of which
    /// does nothing once the session is gone.
    /// @param session The session about to be spawned.
    /// @return Its extensions; none without IExtensionHosting.
    [[nodiscard]] HostedSessionExtensions makeSessionExtensions(vtworkspace::SessionId session);

    /// Repaints @p session's indicator status line from its extensions and tells them what they newly follow,
    /// as a frame would: the daemon renders none, and its clients mirror the row. Takes the terminal lock.
    void refreshStatusLine(HostedSession& session);

    /// A frame of @p session's extensions was published: repaints the row its clients mirror, and tells them.
    void extensionFramePublished(vtworkspace::SessionId session);

    /// Other extensions run in @p session now: its terminal is told, and its clients the row they draw.
    void extensionsReplaced(vtworkspace::SessionId session);

    /// Opens @p request for every client to answer, unless it is answered or open already.
    void offerPermission(vtworkspace::SessionId session, proto::PermissionRequest request);

    /// Arms the once-a-second poll of the hosted programs, unless it is armed or no session's extensions
    /// follow the foreground process. Asked again whenever a session's extensions are replaced.
    void armForegroundPoll();

    /// Moves the open permission questions @p gone asked to another session whose extensions run, or closes
    /// them when none is left. Loop thread.
    /// @param gone The session that exited, no longer hosted.
    /// @return The questions moved, as they read now, for the observers to be told of.
    [[nodiscard]] std::vector<proto::PermissionRequest> rehomePermissionRequests(vtworkspace::SessionId gone);

    /// @return Whether some session's extensions follow the foreground process.
    [[nodiscard]] bool followsForegroundProcess() const noexcept;

    /// What the poll does: tells every session's extensions of its foreground program, and re-arms while
    /// any session runs extensions. @param host The SessionHost.
    static void pollForegroundProcesses(void* host);

    /// Releases what the runners of a session just destroyed retire, a second from now: a runner retires
    /// its engine only once its lane ran its last job. Only with extensions.
    void scheduleRetiredRelease();

    /// What the release timer does. @param host The SessionHost.
    static void releaseRetired(void* host);

    /// Reprojects PTY sizes onto the new layout, THEN fans the event out — the
    /// required order for every layout-shape change, so shells never see a size
    /// that lags what observers are about to advertise. Spelling the reproject at
    /// the call site keeps a newly added shape-changing event from silently
    /// forgetting it.
    /// @param method A vtworkspace::ModelEvents member function pointer.
    /// @param args The event arguments (forwarded to fanOut).
    template <typename Method, typename... Args>
    void fanOutAfterReproject(Method method, Args... args)
    {
        reprojectLayouts();
        fanOut(method, args...);
    }

    /// What a job another thread posts reaches the host through: revoked, on the loop thread, as the host
    /// goes, so that a job run or destroyed later finds no host rather than a dead one.
    struct HostGate
    {
        SessionHost* host = nullptr; ///< Null once the host is gone. Loop thread only.
    };

    core::net::EventLoop& _loop;
    std::shared_ptr<HostGate> _gate;
    PtyFactory _ptyFactory;
    ProfilePtyFactory _profilePtys; ///< @see SessionHostConfig::profilePtys
    vtbackend::Settings _settings;
    core::Environment const& _environment;
    vtbackend::HostIdentity _identity; ///< Handed to every session this host spawns.
    uint64_t _instance;                ///< @see instance()
    vtbackend::TerminalClocks _clocks; ///< Read by every session this host spawns.
    vtpty::PageSize _pageSize;         ///< The RESOLVED authoritative client area (see pageSize()).
    SessionPumping _pumping;           ///< Whether new sessions start their PTY pump thread.
    InputWatermarks _inputWatermarks;  ///< @see SessionHostConfig::inputWatermarks
    ClientSizePolicy _sizePolicy;
    /// Where every session this host spawns looks up the paths printed on its screen.
    core::platform::FileSystem const& _fileSystem;
    /// The host's profiles; null for none. @see SessionHostConfig::profiles
    std::shared_ptr<ProfileCatalog const> _profiles;
    /// What each attached client reported it can display, keyed by its stream subscription so the
    /// entry lives exactly as long as the client does (@see unsubscribeStream). `_pageSize` is
    /// resolved from these, never assigned from one of them.
    std::unordered_map<SessionStreamEvents*, ClientArea> _clientAreas;
    /// Stamps each report so `ClientSizePolicy::Latest` can order them. A counter rather than a
    /// clock, so a test can reproduce an ordering exactly.
    uint64_t _nextAreaSequence = 1;

    uint64_t _nextSessionId = 1;
    std::optional<vtworkspace::SessionId> _pendingSessionId; ///< Consumed by the model's allocator.
    /// The extensions every session runs; null for none. Declared before _sessions, so that it is destroyed
    /// after every session's extensions: its runtime joins the threads their runners lived on.
    std::shared_ptr<IExtensionHosting> _extensions;
    std::unordered_map<uint64_t, std::unique_ptr<HostedSession>> _sessions;
    /// When each session last got client input, on `_loop`'s clock (@see lastInputAt). Erased with the
    /// session's `_sessions` entry, by eraseSession().
    std::unordered_map<uint64_t, core::platform::SteadyTimePoint> _lastInput;
    /// Every permission request no client answered yet, oldest first; one per installed extension and
    /// manifest digest.
    std::vector<proto::PermissionRequest> _pendingPermissions;
    /// Asks the hosted programs who runs in the foreground, once a second; armed only while a session runs
    /// extensions.
    std::optional<core::net::DeadlineTimer> _foregroundPoll;
    /// Releases what runners retired, a while after a session with extensions went. @see
    /// scheduleRetiredRelease
    std::optional<core::net::DeadlineTimer> _retiredRelease;
    std::vector<vtworkspace::ModelEvents*> _subscribers;
    std::vector<SessionStreamEvents*> _streamSubscribers;
    /// The handshaken native Attach connections. @see connectedClients
    std::vector<SessionStreamEvents const*> _attachedClients;
    /// The client whose launch took every window; null while none is attached. @see claimPrimaryViewer
    SessionStreamEvents const* _primaryViewer = nullptr;
    /// Every window created for a client, by window id: that client while it is attached, null once it
    /// left. A window absent here is unclaimed. @see windowClaim
    std::unordered_map<uint64_t, SessionStreamEvents const*> _windowCreators;

    vtworkspace::SessionModel _model; ///< Last member: its callbacks reach into the host.
    vtworkspace::WindowId _window;
};

/// Scoped subscription: keeps @p Observer subscribed to @p Host for this object's
/// lifetime. The Host's subscribe/unsubscribe methods are passed as callables so the
/// same template serves both stream and model-event subscriptions.
template <typename Host, typename Observer>
class ScopedSubscription
{
  public:
    using SubscribeFn = void (Host::*)(Observer*);
    using UnsubscribeFn = void (Host::*)(Observer*);

    /// Subscribes @p observer to @p host via @p subscribe for this object's lifetime.
    ScopedSubscription(Host& host, Observer& observer, SubscribeFn subscribe, UnsubscribeFn unsubscribe):
        _host(host), _observer(observer), _unsubscribe(unsubscribe)
    {
        (_host.*subscribe)(&_observer);
    }

    ~ScopedSubscription() { (_host.*_unsubscribe)(&_observer); }

    ScopedSubscription(ScopedSubscription const&) = delete;
    ScopedSubscription& operator=(ScopedSubscription const&) = delete;
    ScopedSubscription(ScopedSubscription&&) = delete;
    ScopedSubscription& operator=(ScopedSubscription&&) = delete;

  private:
    Host& _host;
    Observer& _observer;
    UnsubscribeFn _unsubscribe;
};

/// Scoped stream subscription: connection coroutines keep one in their frame
/// so the observer is removed even when the serve loop unwinds early.
using ScopedStreamSubscription = ScopedSubscription<SessionHost, SessionStreamEvents>;

/// Scoped model-events subscription: a native client keeps one in its frame so
/// its layout observer is removed even when the serve loop unwinds early.
using ScopedModelSubscription = ScopedSubscription<SessionHost, vtworkspace::ModelEvents>;

/// Factory for stream subscriptions — avoids repeating the member-function
/// pointers at every call site.
inline ScopedStreamSubscription makeScopedStreamSubscription(SessionHost& host, SessionStreamEvents& observer)
{
    return ScopedStreamSubscription {
        host, observer, &SessionHost::subscribeStream, &SessionHost::unsubscribeStream
    };
}

/// Factory for model-event subscriptions.
inline ScopedModelSubscription makeScopedModelSubscription(SessionHost& host,
                                                           vtworkspace::ModelEvents& observer)
{
    return ScopedModelSubscription { host, observer, &SessionHost::subscribe, &SessionHost::unsubscribe };
}

} // namespace vthost
