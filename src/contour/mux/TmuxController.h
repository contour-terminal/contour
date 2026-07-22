// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// `TmuxController` — the GUI's tmux -CC mirroring engine.
///
/// One controller = one spawned `tmux -C attach-session` client. The Qt-free
/// gateway and client model run on a MuxLoopThread reactor; every remote
/// PANE becomes a local tab or split whose TerminalSession sits on a
/// `vtpty::ChannelPty` fed the pane's raw %output bytes (the session's own
/// parser emulates — tmux forwards bytes, the client emulates). Input flows
/// back as `send-keys -H` hex; a local pane resize proposes
/// `resize-pane -x -y` upstream. v1 mapping: tmux window = tab, additional
/// panes split the tab; ratio/anchor fidelity is a named follow-up.

#include <contour/SessionFactory.h>
#include <contour/mux/MuxController.h>
#include <contour/mux/MuxLoopThread.h>

#include <vtpty/ChannelPty.h>

#include <QtCore/QObject>

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <expected>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>

#include <muxserver/tmux/TmuxClientModel.h>
#include <muxserver/tmux/TmuxGateway.h>
#include <vtmux/Primitives.h>

namespace contour
{

class TerminalSessionManager;
class TerminalSession;

/// The tmux control-mode command that resumes a paused pane. tmux sends `%pause` for a pane whose
/// output it is buffering; the mirror consumes output as fast as it arrives, so it always wants the
/// pane resumed. The server parses this as `refresh-client -A %N:continue` (@see ControlSession).
/// Pure so the exact wire string the server parser consumes is unit-testable.
/// @param pane The tmux pane id to resume.
/// @return The control-mode command line.
[[nodiscard]] std::string tmuxResumePaneCommand(uint64_t pane);

/// The tmux-mirror session factory and pane registry.
class TmuxController final: public QObject, public SessionFactory, public muxserver::tmux::TmuxModelEvents
{
    Q_OBJECT

  public:
    /// @param tmuxSocket Optional tmux server socket path (-S); empty uses
    ///        tmux's default discovery.
    explicit TmuxController(std::string tmuxSocket);

    /// Stops the reactor (detaching if still connected) and joins.
    ~TmuxController() override;

    TmuxController(TmuxController const&) = delete;
    TmuxController& operator=(TmuxController const&) = delete;
    TmuxController(TmuxController&&) = delete;
    TmuxController& operator=(TmuxController&&) = delete;

    /// Starts the reactor, spawns the tmux client, and blocks until the
    /// first remote pane was discovered (or failure/timeout).
    [[nodiscard]] std::expected<void, std::string> connectAndWait(std::chrono::milliseconds timeout);

    /// Detaches and joins the reactor thread. Idempotent.
    void stop();

    /// Realizes every discovered-but-unrealized pane as a tab (first pane of
    /// its tmux window) or a split (subsequent panes) in @p window.
    void adoptPendingPanes(TerminalSessionManager& manager, vtmux::WindowId window);

    /// Applies any pending `%window-renamed` titles to the tabs of realized tmux
    /// windows (a tmux window maps to a tab). A rename for a not-yet-realized window
    /// stays pending until its first pane is adopted. Runs on the GUI thread.
    void applyPendingRenames(TerminalSessionManager& manager);

    // SessionFactory: hands out a ChannelPty bound to the next pending pane.
    [[nodiscard]] std::unique_ptr<vtpty::Pty> createPty(
        std::optional<std::string> cwd,
        std::optional<vtbackend::PageSize> pageSize = std::nullopt,
        std::optional<vtpty::Process::ExecInfo> commandOverride = std::nullopt,
        std::optional<std::string> profileName = std::nullopt) override;

    [[nodiscard]] bool canCreateSession() const noexcept override;

    // TmuxModelEvents (reactor thread) — structure changes queue realizations.
    void paneAdded(uint64_t window, uint64_t pane, int columns, int lines) override;
    void paneRemoved(uint64_t window, uint64_t pane) override;
    void windowRenamed(uint64_t window, std::string const& name) override;
    void panePaused(uint64_t pane, bool paused) override;
    void exited(std::string const& reason) override;

  signals:
    /// A remote pane appeared that has no local realization yet.
    void remotePaneDiscovered();

    /// A tmux window was renamed (%window-renamed); the GUI reflects it onto the tab.
    void tabTitleChanged();

    /// The tmux client ended (%exit, error, or disconnect).
    void connectionClosed();

  private:
    class PaneFeed;

    struct PendingPane
    {
        uint64_t window = 0;
        uint64_t pane = 0;
        int columns = 80;
        int lines = 25;
        bool vertical = false; ///< The split direction joining it to its window.
    };

    [[nodiscard]] coro::Task<void> runClient(net::EventLoop* loop);

    /// GUI-side (pty destructor): forgets a pane's pty binding.
    void unbindPane(uint64_t pane);

    /// Reactor-side (feed destructor): forgets a pane's feed.
    void dropFeed(uint64_t pane);

    /// Closes every bound pty (EOF to its session) — the disconnect path.
    void closeAllPanes();

    std::string _tmuxSocket;
    MuxLoopThread _reactor;

    mutable std::mutex _mutex;
    std::condition_variable _connected;
    using State = MuxConnectPhase;
    State _state = State::Connecting;
    std::string _failure;
    std::deque<PendingPane> _pending;
    std::unordered_map<uint64_t, PaneFeed*> _feeds;                 ///< Model-owned sinks, by pane.
    std::unordered_map<uint64_t, vtpty::ChannelPty*> _ptys;         ///< Bound ptys, by pane.
    std::unordered_map<uint64_t, TerminalSession*> _actingByWindow; ///< Split anchor per tmux window.
    std::unordered_map<uint64_t, std::string> _pendingRenames; ///< %window-renamed titles awaiting apply.
    muxserver::tmux::TmuxGateway* _gateway = nullptr;          ///< Reactor-owned; valid while serving.
    int _tmuxPid = -1;
    bool _stopped = false;

    /// LAST member, destroyed FIRST: its pane sinks (PaneFeed) unregister
    /// from _feeds/_mutex in their destructors, which must still be alive.
    /// (The destructor body's stop() has already joined the reactor by then.)
    muxserver::tmux::TmuxClientModel _model;
};

} // namespace contour
