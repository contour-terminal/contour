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

#include <vtbackend/Settings.h>
#include <vtbackend/Terminal.h>

#include <vtpty/PageSize.h>
#include <vtpty/Pty.h>

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include <net/EventLoop.h>
// Part of this header's contract, not an implementation detail: the settings a host is constructed
// with — and any a SessionSpawnRequest carries — are normalized through hostedSessionSettings.
#include <vthost/ClientSizePolicy.h>
#include <vthost/SessionSettings.h>
#include <vtworkspace/ModelEvents.h>
#include <vtworkspace/SessionModel.h>

namespace vthost
{

/// Creates the backing PTY for one new session. Production spawns the user's
/// shell over a real PTY (vtpty::Process); tests inject a MockPty factory.
using PtyFactory = std::function<std::unique_ptr<vtpty::Pty>(vtbackend::PageSize)>;

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
    virtual void sessionNotify(vtworkspace::SessionId /*session*/,
                               std::string const& /*title*/,
                               std::string const& /*body*/)
    {
    }

    /// @p session wrote the clipboard (OSC 52); @p data is the raw, decoded text.
    /// The daemon forwards it unconditionally — the CLIENT applies its own
    /// clipboard-write permission.
    virtual void sessionCopyToClipboard(vtworkspace::SessionId /*session*/, std::string const& /*data*/) {}
};

/// One hosted session: the terminal (owning its PTY) plus the pump thread
/// feeding it, mirroring the GUI's TerminalSession::mainLoop.
class HostedSession
{
  public:
    /// @param id The model-side session identity.
    /// @param pty The backing PTY (moved into the terminal).
    /// @param settings The terminal's factory settings.
    /// @param onScreenUpdated Invoked on the PUMP thread after each processed
    ///        input batch (the host marshals it onto the loop).
    /// @param onClosed Invoked on the PUMP thread once the PTY closed and the
    ///        pump loop ended (the host marshals it onto the loop).
    HostedSession(vtworkspace::SessionId id,
                  std::unique_ptr<vtpty::Pty> pty,
                  vtbackend::Settings settings,
                  std::function<void()> onScreenUpdated,
                  std::function<void()> onBell,
                  std::function<void(std::string, std::string)> onNotify,
                  std::function<void(std::string)> onCopyToClipboard,
                  std::function<void()> onClosed);

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

    [[nodiscard]] vtworkspace::SessionId id() const noexcept { return _id; }
    [[nodiscard]] vtbackend::Terminal& terminal() noexcept { return _terminal; }

  private:
    /// The Terminal::Events glue: forwards the terminal events the daemon
    /// mirrors — the per-batch screen update, the bell, desktop notifications
    /// (OSC 9/777/99) and OSC 52 clipboard writes — to the host's callbacks;
    /// everything else keeps the Null default. Adding a mirrored event is a
    /// callback here plus a row in SessionStreamEvents.
    struct Events final: vtbackend::Terminal::NullEvents
    {
        Events(std::function<void()> screenUpdated,
               std::function<void()> bell,
               std::function<void(std::string, std::string)> notify,
               std::function<void(std::string)> copyToClipboard):
            onScreenUpdated(std::move(screenUpdated)),
            onBell(std::move(bell)),
            onNotify(std::move(notify)),
            onCopyToClipboard(std::move(copyToClipboard))
        {
        }

        std::function<void()> onScreenUpdated;
        std::function<void()> onBell;
        std::function<void(std::string, std::string)> onNotify;
        std::function<void(std::string)> onCopyToClipboard;
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
                onNotify(std::string { title }, std::string { body });
        }
        void showDesktopNotification(vtbackend::DesktopNotification const& notification) override
        {
            if (onNotify)
                onNotify(notification.title, notification.body);
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
    };

    void pumpLoop();

    vtworkspace::SessionId _id;
    Events _events; ///< Must outlive _terminal (referenced by it).
    vtbackend::Terminal _terminal;
    std::function<void()> _onClosed;
    std::unique_ptr<std::thread> _pumpThread;
};

/// What ONE session-creating request overrides about the session it spawns.
///
/// A struct rather than a parameter so a future per-spawn knob (a working directory, a shell
/// override) is a field here instead of a fourth argument at three call sites.
struct SessionSpawnRequest
{
    /// The emulation settings for this session; nullopt uses the host's factory settings.
    ///
    /// This is how a client's own profile reaches the sessions IT creates without touching sessions
    /// that already exist — the property that keeps two clients on different profiles from fighting
    /// over one session. Whatever is supplied still goes through
    /// @ref hostedSessionSettings.
    std::optional<vtbackend::Settings> settings {};
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
    /// @param startPumps Whether new sessions start their PTY pump thread
    ///        (disabled by tests that drive terminals directly).
    /// @param sizePolicy How the authoritative client area is resolved when several attached
    ///        clients report different ones. Fixed at construction: two differently-configured
    ///        daemons are two different daemons, not one in two states.
    SessionHost(net::EventLoop& loop,
                PtyFactory ptyFactory,
                vtbackend::Settings settings,
                bool startPumps = true,
                ClientSizePolicy sizePolicy = ClientSizePolicy::Latest);
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
    /// @return The created window, or nullptr on failure (nothing is leaked).
    vtworkspace::Window* createWindow(SessionSpawnRequest const& request = {});

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

    /// @return The number of live hosted sessions.
    [[nodiscard]] std::size_t sessionCount() const noexcept { return _sessions.size(); }

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

    net::EventLoop& _loop;
    PtyFactory _ptyFactory;
    vtbackend::Settings _settings;
    vtpty::PageSize _pageSize; ///< The RESOLVED authoritative client area (see pageSize()).
    bool _startPumps;
    ClientSizePolicy _sizePolicy;
    /// What each attached client reported it can display, keyed by its stream subscription so the
    /// entry lives exactly as long as the client does (@see unsubscribeStream). `_pageSize` is
    /// resolved from these, never assigned from one of them.
    std::unordered_map<SessionStreamEvents*, ClientArea> _clientAreas;
    /// Stamps each report so `ClientSizePolicy::Latest` can order them. A counter rather than a
    /// clock, so a test can reproduce an ordering exactly.
    uint64_t _nextAreaSequence = 1;

    uint64_t _nextSessionId = 1;
    std::optional<vtworkspace::SessionId> _pendingSessionId; ///< Consumed by the model's allocator.
    std::unordered_map<uint64_t, std::unique_ptr<HostedSession>> _sessions;
    std::vector<vtworkspace::ModelEvents*> _subscribers;
    std::vector<SessionStreamEvents*> _streamSubscribers;

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
