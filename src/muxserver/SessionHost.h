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
    /// The Terminal::Events glue: forwards the per-batch screen update to the
    /// host's callback; everything else keeps the Null default.
    struct Events final: vtbackend::Terminal::NullEvents
    {
        explicit Events(std::function<void()> handler): onScreenUpdated(std::move(handler)) {}

        std::function<void()> onScreenUpdated;

        void screenUpdated() override
        {
            if (onScreenUpdated)
                onScreenUpdated();
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

    /// Sets the callback invoked (on the loop thread) whenever a session's
    /// screen processed new output — the delta/notification trigger.
    void setScreenUpdatedHandler(std::function<void(vtmux::SessionId)> handler)
    {
        _onScreenUpdated = std::move(handler);
    }

    /// Sets the callback invoked (on the loop thread) with each raw PTY output
    /// chunk BEFORE the parser consumed it — the control-mode %output byte tap.
    void setOutputHandler(std::function<void(vtmux::SessionId, std::string const&)> handler)
    {
        _onOutput = std::move(handler);
    }

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

    net::EventLoop& _loop;
    PtyFactory _ptyFactory;
    vtbackend::Settings _settings;
    bool _startPumps;

    uint64_t _nextSessionId = 1;
    std::optional<vtmux::SessionId> _pendingSessionId; ///< Consumed by the model's allocator.
    std::unordered_map<uint64_t, std::unique_ptr<HostedSession>> _sessions;
    std::vector<vtmux::ModelEvents*> _subscribers;
    std::function<void(vtmux::SessionId)> _onScreenUpdated;
    std::function<void(vtmux::SessionId, std::string const&)> _onOutput;

    vtmux::SessionModel _model; ///< Last member: its callbacks reach into the host.
    vtmux::WindowId _window;
};

} // namespace muxserver
