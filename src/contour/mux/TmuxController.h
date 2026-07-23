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
/// `resize-pane -x -y` upstream. A tmux window maps to a tab: a window first seen
/// with a multi-pane layout is realized as its WHOLE tree (faithful split ratios and
/// shape); panes added later split the tab incrementally.

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
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>

#include <muxserver/tmux/LayoutString.h>
#include <muxserver/tmux/TmuxClientModel.h>
#include <muxserver/tmux/TmuxGateway.h>
#include <vtmux/LayoutTree.h>
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

/// The tmux control-mode command that splits pane @p pane. tmux: `-h` splits left|right (our
/// Vertical), the default stacks top|bottom (our Horizontal) — @see ControlSession::commandSplitWindow.
/// Pure so the wire format the server parser consumes is unit-testable.
/// @param pane The tmux pane id to split.
/// @param vertical Whether to split left|right (`-h`) rather than top|bottom.
/// @return The control-mode command line.
[[nodiscard]] std::string tmuxSplitWindowCommand(uint64_t pane, bool vertical);

/// The tmux control-mode command that closes pane @p pane (`kill-pane -t %N`).
/// @param pane The tmux pane id to kill.
/// @return The control-mode command line.
[[nodiscard]] std::string tmuxKillPaneCommand(uint64_t pane);

/// A tmux window's binary layout tree converted for realization through
/// `TerminalSessionManager::applyLayoutToWindow`: a single-tab `vtmux::Layout` carrying the split
/// orientations and ratios, plus the map from each converted leaf pane (by its stable address inside
/// `layout`) to the tmux pane id that backs it. Mirrors `muxserver::client::WireLayout`.
struct TmuxWindowLayout
{
    vtmux::Layout layout;                                            ///< A single tab: the window's tree.
    std::unordered_map<vtmux::LayoutPane const*, uint64_t> leafPane; ///< Converted leaf → tmux pane id.
};

/// Converts a tmux `BinaryLayout` tree into a realizable single-tab `vtmux::Layout` (splits keep their
/// orientation and first-child ratio) plus the leaf→pane map — the tmux analogue of
/// `muxserver::client::wireToLayout`. Pure, so the tree conversion is unit-testable. The leaf map is
/// keyed by addresses inside the returned `layout`; a move preserves them (build the map only once the
/// tree is in place), so pass the SAME object to `applyLayoutToWindow`.
/// @param tree The window's binary layout tree.
/// @return The realizable layout and its leaf→tmux-pane map.
[[nodiscard]] TmuxWindowLayout tmuxLayoutToWindowLayout(muxserver::tmux::BinaryLayout const& tree);

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

    /// Realizes discovered-but-unrealized panes into @p window. A tmux window first seen with a
    /// multi-pane layout (all its panes pending) is realized as its WHOLE tree via
    /// applyLayoutToWindow — faithful split ratios and shape. A window's first (or only) pane, or a
    /// pane arriving after the window is already shown, is realized as a tab / an incremental split.
    void adoptPendingPanes(TerminalSessionManager& manager, vtmux::WindowId window);

    /// Binds the NEXT createPty() to tmux pane @p pane rather than popping the FIFO pending queue. The
    /// whole-tree realizer calls this — via applyLayoutToWindow's beforeLeafSeed — right before each
    /// leaf's backing session is created, so that pane's mirror pty binds to exactly this tmux pane.
    void setNextBindPane(uint64_t pane);

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

    /// SessionFactory: a GUI split of the pane backed by @p actingPty is authored on tmux
    /// (`split-window`) rather than split locally — tmux's %layout-change re-realizes the new pane
    /// through the mirror. A split issued BY the reconciler itself (while realizing an existing tmux
    /// pane) is not re-authored — it builds the mirror pane locally. Returns false (a local split) if
    /// the pty is not bound to a tmux pane.
    [[nodiscard]] bool requestRemoteSplit(vtpty::Pty const* actingPty, bool vertical) override;

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
        double ratio = 0.5;    ///< The split's first-child share (for an incremental split).
    };

    /// Realizes tmux window @p tmuxWindow's whole layout @p tree as a tab in @p guiWindow, binding each
    /// leaf to its tmux pane and seeding the window's split anchor. Runs on the GUI thread.
    void realizeWindowLayout(TerminalSessionManager& manager,
                             vtmux::WindowId guiWindow,
                             uint64_t tmuxWindow,
                             muxserver::tmux::BinaryLayout const& tree);

    /// Realizes ONE pending pane of @p window incrementally: its first pane as a background tab, a
    /// later pane as a split of the window's anchor (at the pane's remote ratio). Runs on the GUI
    /// thread. @return true if a pane was consumed (progress made); false if creation stalled.
    [[nodiscard]] bool realizeOnePane(TerminalSessionManager& manager, vtmux::WindowId guiWindow);

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
    std::unordered_set<uint64_t>
        _remotelyClosed; ///< Panes tmux removed; their unbind must not echo kill-pane.
    /// A deep copy of each not-yet-realized window's tmux layout tree, captured on the reactor thread
    /// (paneAdded) so the GUI thread can realize the whole tree without racing the model. Keyed by tmux
    /// window; dropped once the window is realized.
    std::unordered_map<uint64_t, muxserver::tmux::BinaryLayout> _pendingTrees;
    std::optional<uint64_t> _nextBindPane; ///< The tmux pane the next createPty() binds to (whole-tree).
    bool _realizing =
        false; ///< True while adoptPendingPanes realizes tmux panes, so its splits build locally.
    muxserver::tmux::TmuxGateway* _gateway = nullptr; ///< Reactor-owned; valid while serving.
    int _tmuxPid = -1;
    bool _stopped = false;

    /// LAST member, destroyed FIRST: its pane sinks (PaneFeed) unregister
    /// from _feeds/_mutex in their destructors, which must still be alive.
    /// (The destructor body's stop() has already joined the reactor by then.)
    muxserver::tmux::TmuxClientModel _model;
};

} // namespace contour
