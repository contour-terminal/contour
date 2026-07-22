// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// `SessionHost` — the daemon-side owner of terminal sessions.
///
/// This is the second consumer the vtmux model was designed for (see
/// vtmux/ModelEvents.h): where the Qt GUI maps a SessionId to a TerminalSession,
/// the host maps it to an owned {Pty, Terminal} pair pumped by a dedicated
/// thread, exactly like the GUI's per-session Terminal::mainLoop split.
///
/// Threading: the vtmux::SessionModel and all SessionHost methods are confined
/// to the event-loop thread. Session pump threads never touch the model — they
/// marshal completion (PTY closed) through EventLoop::post().

#include <vtbackend/Settings.h>
#include <vtbackend/Terminal.h>

#include <vtpty/PageSize.h>
#include <vtpty/Pty.h>

#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include <net/EventLoop.h>
#include <vtmux/ModelEvents.h>
#include <vtmux/SessionModel.h>

namespace muxserver
{

/// Creates the backing PTY for one new session. Production spawns the user's
/// shell over a real PTY (vtpty::Process); tests inject a MockPty factory.
using PtyFactory = std::function<std::unique_ptr<vtpty::Pty>(vtbackend::PageSize)>;

/// Per-connection observer of the hosted sessions' output streams. Every
/// callback fires on the loop thread; any number of observers may subscribe
/// concurrently (one per attached client), so no observer ever silences
/// another by connecting or disconnecting.
class SessionStreamEvents
{
  public:
    virtual ~SessionStreamEvents() = default;

    /// @p session processed new output — the delta/notification trigger.
    virtual void sessionScreenUpdated(vtmux::SessionId /*session*/) {}

    /// A raw PTY output chunk of @p session BEFORE the parser consumed it —
    /// the control-mode %output byte tap.
    virtual void sessionOutput(vtmux::SessionId /*session*/, std::string const& /*bytes*/) {}

    /// @p session was destroyed (its shell exited); fires after the host removed
    /// it. Observers holding per-session state must drop it here, or it leaks for
    /// the connection's whole lifetime.
    virtual void sessionClosed(vtmux::SessionId /*session*/) {}

    /// @p session rang the bell (BEL).
    virtual void sessionBell(vtmux::SessionId /*session*/) {}

    /// @p session raised a desktop notification (OSC 9 / OSC 777 / OSC 99).
    virtual void sessionNotify(vtmux::SessionId /*session*/,
                               std::string const& /*title*/,
                               std::string const& /*body*/)
    {
    }

    /// @p session wrote the clipboard (OSC 52); @p data is the raw, decoded text.
    /// The daemon forwards it unconditionally — the CLIENT applies its own
    /// clipboard-write permission.
    virtual void sessionCopyToClipboard(vtmux::SessionId /*session*/, std::string const& /*data*/) {}
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
    HostedSession(vtmux::SessionId id,
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

    [[nodiscard]] vtmux::SessionId id() const noexcept { return _id; }
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
    };

    void pumpLoop();

    vtmux::SessionId _id;
    Events _events; ///< Must outlive _terminal (referenced by it).
    vtbackend::Terminal _terminal;
    std::function<void()> _onClosed;
    std::unique_ptr<std::thread> _pumpThread;
};

/// Owns every hosted session and the authoritative vtmux::SessionModel,
/// fanning each completed model change out to subscribed observers.
class SessionHost final: public vtmux::ModelEvents
{
  public:
    /// @param loop The event loop all model mutation is confined to.
    /// @param ptyFactory Creates the backing PTY for each new session.
    /// @param settings Factory settings applied to every session's terminal.
    /// @param startPumps Whether new sessions start their PTY pump thread
    ///        (disabled by tests that drive terminals directly).
    SessionHost(net::EventLoop& loop,
                PtyFactory ptyFactory,
                vtbackend::Settings settings,
                bool startPumps = true);
    ~SessionHost() override;

    SessionHost(SessionHost const&) = delete;
    SessionHost& operator=(SessionHost const&) = delete;
    SessionHost(SessionHost&&) = delete;
    SessionHost& operator=(SessionHost&&) = delete;

    /// @return The authoritative session/layout model.
    [[nodiscard]] vtmux::SessionModel& model() noexcept { return _model; }

    /// @return The host's window (the daemon starts with exactly one).
    [[nodiscard]] vtmux::WindowId windowId() const noexcept { return _window; }

    /// @return The authoritative client area, in cells: layout projection and
    ///         PTY sizes derive from it. Starts at the settings' page size.
    [[nodiscard]] vtpty::PageSize pageSize() const noexcept { return _pageSize; }

    /// Applies a new client area (an accepted `refresh-client -C` proposal):
    /// every tab's leaves are resized to their cell-space projection under the
    /// new area, and future sessions spawn at the projected sizes.
    void applyClientSize(vtpty::PageSize size);

    /// Creates a tab whose first pane is backed by a freshly spawned session,
    /// using the same pre-mint handshake as the GUI: the backing session is
    /// created first, then the model's allocator hands its id back.
    /// @return The created tab, or nullptr on failure (nothing is leaked).
    vtmux::Tab* createTab();

    /// Splits @p tab's active pane, backing the new leaf with a fresh session.
    /// @param tab The tab whose active pane splits.
    /// @param orientation The split axis.
    /// @param ratio The first child's share.
    void splitActivePane(vtmux::TabId tab, vtmux::SplitState orientation, double ratio = 0.5);

    /// @return The terminal backing @p session, or nullptr if unknown.
    [[nodiscard]] vtbackend::Terminal* terminal(vtmux::SessionId session) noexcept;

    /// @return The number of live hosted sessions.
    [[nodiscard]] std::size_t sessionCount() const noexcept { return _sessions.size(); }

    /// Registers @p observer for every completed model change. Not owned.
    void subscribe(vtmux::ModelEvents* observer);

    /// Removes @p observer. Idempotent.
    void unsubscribe(vtmux::ModelEvents* observer);

    /// Registers @p observer for session output-stream events (screen updates
    /// and the raw byte tap). Not owned; callbacks fire on the loop thread.
    void subscribeStream(SessionStreamEvents* observer);

    /// Removes @p observer from the stream fan-out. Idempotent.
    void unsubscribeStream(SessionStreamEvents* observer);

    /// Handles a session whose PTY closed (shell exited): prunes its pane from
    /// the model (prune-then-terminate) and destroys the session. Invoked on
    /// the loop thread — by the pump's posted completion in production, or
    /// directly by tests.
    void handleSessionExit(vtmux::SessionId session);

    // vtmux::ModelEvents — every completed change fans out to subscribers.
    void tabAdded(vtmux::WindowId window, vtmux::TabId tab, int index) override;
    void tabClosed(vtmux::WindowId window, vtmux::TabId tab, int index) override;
    void tabMoved(vtmux::WindowId window, vtmux::TabId tab, int fromIndex, int toIndex) override;
    void activeTabChanged(vtmux::WindowId window, vtmux::TabId tab, int index) override;
    void paneSplit(vtmux::TabId tab, vtmux::PaneId splitNode, vtmux::PaneId newLeaf) override;
    void paneClosed(vtmux::TabId tab, vtmux::PaneId closed, vtmux::PaneId survivor) override;
    void activePaneChanged(vtmux::TabId tab, vtmux::PaneId leaf) override;
    void paneRatioChanged(vtmux::TabId tab, vtmux::PaneId splitNode, double ratio) override;
    void tabTitleChanged(vtmux::TabId tab) override;
    void tabColorChanged(vtmux::TabId tab) override;
    void paneOrientationChanged(vtmux::TabId tab, vtmux::PaneId splitNode, vtmux::SplitState state) override;
    void paneSwapped(vtmux::TabId tab, vtmux::PaneId a, vtmux::PaneId b) override;
    void paneZoomChanged(vtmux::TabId tab, std::optional<vtmux::PaneId> zoomedLeaf) override;
    void paneTreeRestructured(vtmux::TabId tab) override;

  private:
    /// Spawns and registers the backing session for the next model allocation
    /// (the pre-mint half of the handshake).
    /// @return The minted id, or nullopt if the PTY factory failed.
    [[nodiscard]] std::optional<vtmux::SessionId> seedSession();

    /// Resizes every leaf's terminal to its cell-space projection under the
    /// current client area — run after every layout-shape change so PTY sizes
    /// never drift from the advertised layout.
    void reprojectLayouts();

    /// Fans one completed model change out to every subscriber, invoking @p method
    /// on each with @p args. Single-sources the observer loop the ModelEvents
    /// overrides below all share.
    /// @param method A vtmux::ModelEvents member function pointer.
    /// @param args The event arguments (copied once here, then passed to each observer).
    template <typename Method, typename... Args>
    void fanOut(Method method, Args... args)
    {
        for (auto* observer: _subscribers)
            (observer->*method)(args...);
    }

    /// Reprojects PTY sizes onto the new layout, THEN fans the event out — the
    /// required order for every layout-shape change, so shells never see a size
    /// that lags what observers are about to advertise. Spelling the reproject at
    /// the call site keeps a newly added shape-changing event from silently
    /// forgetting it.
    /// @param method A vtmux::ModelEvents member function pointer.
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
    vtpty::PageSize _pageSize; ///< The authoritative client area (see pageSize()).
    bool _startPumps;

    uint64_t _nextSessionId = 1;
    std::optional<vtmux::SessionId> _pendingSessionId; ///< Consumed by the model's allocator.
    std::unordered_map<uint64_t, std::unique_ptr<HostedSession>> _sessions;
    std::vector<vtmux::ModelEvents*> _subscribers;
    std::vector<SessionStreamEvents*> _streamSubscribers;

    vtmux::SessionModel _model; ///< Last member: its callbacks reach into the host.
    vtmux::WindowId _window;
};

/// Scoped stream subscription: connection coroutines keep one in their frame
/// so the observer is removed even when the serve loop unwinds early.
class ScopedStreamSubscription
{
  public:
    /// Subscribes @p observer to @p host for this object's lifetime.
    ScopedStreamSubscription(SessionHost& host, SessionStreamEvents& observer):
        _host(host), _observer(observer)
    {
        _host.subscribeStream(&_observer);
    }

    ~ScopedStreamSubscription() { _host.unsubscribeStream(&_observer); }

    ScopedStreamSubscription(ScopedStreamSubscription const&) = delete;
    ScopedStreamSubscription& operator=(ScopedStreamSubscription const&) = delete;
    ScopedStreamSubscription(ScopedStreamSubscription&&) = delete;
    ScopedStreamSubscription& operator=(ScopedStreamSubscription&&) = delete;

  private:
    SessionHost& _host;
    SessionStreamEvents& _observer;
};

/// Scoped model-events subscription: a native client keeps one in its frame so
/// its layout observer is removed even when the serve loop unwinds early.
class ScopedModelSubscription
{
  public:
    /// Subscribes @p observer to @p host's model fan-out for this object's lifetime.
    ScopedModelSubscription(SessionHost& host, vtmux::ModelEvents& observer): _host(host), _observer(observer)
    {
        _host.subscribe(&_observer);
    }

    ~ScopedModelSubscription() { _host.unsubscribe(&_observer); }

    ScopedModelSubscription(ScopedModelSubscription const&) = delete;
    ScopedModelSubscription& operator=(ScopedModelSubscription const&) = delete;
    ScopedModelSubscription(ScopedModelSubscription&&) = delete;
    ScopedModelSubscription& operator=(ScopedModelSubscription&&) = delete;

  private:
    SessionHost& _host;
    vtmux::ModelEvents& _observer;
};

} // namespace muxserver
