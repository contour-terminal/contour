// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// `TmuxController` — the GUI's tmux -CC mirroring engine.
///
/// One controller = one spawned `tmux -C attach-session` client. The Qt-free
/// gateway and client model run on a ReactorThread reactor; every remote
/// PANE becomes a local tab or split whose TerminalSession sits on a
/// `vtpty::ChannelPty` fed the pane's raw %output bytes (the session's own
/// parser emulates — tmux forwards bytes, the client emulates). Input flows
/// back as `send-keys -H` hex; a local pane resize proposes
/// `resize-pane -x -y` upstream. A tmux window maps to a tab: a window first seen
/// with a multi-pane layout is realized as its WHOLE tree (faithful split ratios and
/// shape); panes added later split the tab incrementally.

#include <contour/remote/ReactorThread.h>
#include <contour/remote/RemoteController.h>
#include <contour/session/SessionFactory.h>

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

#include <vthost/tmux/LayoutString.h>
#include <vthost/tmux/TmuxClientModel.h>
#include <vthost/tmux/TmuxGateway.h>
#include <vtworkspace/LayoutTree.h>
#include <vtworkspace/Primitives.h>

namespace contour
{

namespace session
{
    class TerminalSessionManager;
    class TerminalSession;
} // namespace session

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

/// The tmux control-mode command that creates a new window (`new-window`) — a new TAB in Contour's
/// model. tmux emits %window-add plus a layout for it, which the mirror realizes as a fresh tab.
/// Pure so the wire string the server parser consumes is unit-testable.
/// @return The control-mode command line.
[[nodiscard]] std::string tmuxNewWindowCommand();

/// A tmux window's binary layout tree converted for realization through
/// `TerminalSessionManager::applyLayoutToWindow`: a single-tab `vtworkspace::Layout` carrying the split
/// orientations and ratios, plus the map from each converted leaf pane (by its stable address inside
/// `layout`) to the tmux pane id that backs it. Mirrors `vthost::client::WireLayout`.
struct TmuxWindowLayout
{
    vtworkspace::Layout layout; ///< A single tab: the window's tree.
    std::unordered_map<vtworkspace::LayoutPane const*, uint64_t> leafPane; ///< Converted leaf → tmux pane id.
};

/// Converts a tmux `BinaryLayout` tree into a realizable single-tab `vtworkspace::Layout` (splits keep their
/// orientation and first-child ratio) plus the leaf→pane map — the tmux analogue of
/// `vthost::client::wireToLayout`. Pure, so the tree conversion is unit-testable. The leaf map is
/// keyed by addresses inside the returned `layout`; a move preserves them (build the map only once the
/// tree is in place), so pass the SAME object to `applyLayoutToWindow`.
/// @param tree The window's binary layout tree.
/// @return The realizable layout and its leaf→tmux-pane map.
[[nodiscard]] TmuxWindowLayout tmuxLayoutToWindowLayout(vthost::tmux::BinaryLayout const& tree);

/// The tmux-mirror session factory and pane registry.
class TmuxController final:
    public QObject,
    public session::SessionFactory,
    public vthost::tmux::TmuxModelEvents,
    public RemoteController
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

    // connectAndWait() and stop() are inherited from RemoteController; this
    // controller supplies the hooks below.

    /// Realizes discovered-but-unrealized panes into @p window. A tmux window first seen with a
    /// multi-pane layout (all its panes pending) is realized as its WHOLE tree via
    /// applyLayoutToWindow — faithful split ratios and shape. A window's first (or only) pane, or a
    /// pane arriving after the window is already shown, is realized as a tab / an incremental split.
    void adoptPendingPanes(session::TerminalSessionManager& manager, vtworkspace::WindowId window);

    /// Binds the NEXT createPty() to tmux pane @p pane rather than popping the FIFO pending queue. The
    /// whole-tree realizer calls this — via applyLayoutToWindow's beforeLeafSeed — right before each
    /// leaf's backing session is created, so that pane's mirror pty binds to exactly this tmux pane.
    void setNextBindPane(uint64_t pane);

    /// Applies any pending `%window-renamed` titles to the tabs of realized tmux
    /// windows (a tmux window maps to a tab). A rename for a not-yet-realized window
    /// stays pending until its first pane is adopted. Runs on the GUI thread.
    void applyPendingRenames(session::TerminalSessionManager& manager);

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

    /// SessionFactory: a GUI "new tab" is authored on the tmux server as a new window
    /// (`new-window`) rather than created locally; tmux's %window-add + layout re-realizes it through
    /// the mirror as a new tab. Without this override the base returns false and a "new tab" gesture is
    /// a silent no-op in mirror mode (canCreateSession() is false in steady state). A tab created BY the
    /// reconciler itself (while realizing) is not re-authored.
    [[nodiscard]] bool requestRemoteTab(vtpty::Pty const* actingPty) override;

    /// SessionFactory: the user ENDED the pane backed by @p pty, so kill it on the tmux server too
    /// rather than leave it running there with nothing showing it.
    ///
    /// The counterpart of `unbindPane`, which merely forgets a binding and cannot tell WHY the pty
    /// went away — a pane close, a window close, an app quit and a lost connection all destroy one.
    /// Inferring "the user closed this pane" there is what made closing a Contour window kill every
    /// mirrored pane, and with them the shells running in the user's tmux session. A pty that is
    /// not bound to a tmux pane is ignored.
    /// @param pty The pty backing the pane to close.
    void requestRemoteClose(vtpty::Pty const* pty) override;

    /// Records the extent tmux now reports for @p pane, so a pane not yet realized is born at the
    /// current size rather than its discovery size. Runs on the REACTOR thread (the model's pane
    /// sink). @see TmuxController::PaneFeed::resize for why a REALIZED pane is left alone.
    /// @param pane The tmux pane id.
    /// @param columns Its width in cells.
    /// @param lines Its height in cells.
    void notePaneExtent(uint64_t pane, int columns, int lines);

    // TmuxModelEvents (reactor thread) — structure changes queue realizations.
    void paneAdded(uint64_t window, uint64_t pane, int columns, int lines) override;
    void paneRemoved(uint64_t window, uint64_t pane) override;

    /// TmuxModelEvents: tmux re-parented a live pane (`break-pane` / `join-pane`). Realized as a
    /// teardown in the source tab plus a fresh realization in the destination one, because a pane
    /// is a leaf of ONE tab's split tree and the workspace model has no cross-tab move. The
    /// model-owned sink survives the round trip (which is why the model reports a move rather than
    /// a remove/add pair), so output arriving in between is buffered and flushed on the new bind.
    void paneMoved(uint64_t fromWindow, uint64_t toWindow, uint64_t pane) override;

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

    /// A pane awaiting realization, plus the snapshot of its window's tree that the GUI thread
    /// realizes it against. @see capturePendingPane.
    struct PaneCapture
    {
        PendingPane record;
        std::optional<vthost::tmux::BinaryLayout> tree; ///< Unset when the window has no tree yet.
    };

    /// Reads everything the GUI thread needs about pane @p pane of window @p window out of the
    /// reactor-owned model, so realization never races it: the split that joins the pane to its
    /// window, and a deep copy of that window's whole layout tree.
    ///
    /// Runs on the REACTOR thread. Shared by discovery (@ref paneAdded) and re-parenting
    /// (@ref paneMoved), which need the identical snapshot for the identical reason.
    /// @param window The tmux window the pane belongs to now.
    /// @param pane The tmux pane.
    /// @param columns Its discovered width; a move carries the previous record's over instead.
    /// @param lines Its discovered height.
    /// @return The pending record and the window's tree.
    [[nodiscard]] PaneCapture capturePendingPane(uint64_t window,
                                                 uint64_t pane,
                                                 int columns = 80,
                                                 int lines = 25) const;

    /// Realizes tmux window @p tmuxWindow's whole layout @p tree as a tab in @p guiWindow, binding each
    /// leaf to its tmux pane and seeding the window's split anchor. Runs on the GUI thread.
    void realizeWindowLayout(session::TerminalSessionManager& manager,
                             vtworkspace::WindowId guiWindow,
                             uint64_t tmuxWindow,
                             vthost::tmux::BinaryLayout const& tree);

    /// Realizes ONE pending pane of @p window incrementally: its first pane as a background tab, a
    /// later pane as a split of the window's anchor (at the pane's remote ratio). Runs on the GUI
    /// thread. @return true if a pane was consumed (progress made); false if creation stalled.
    [[nodiscard]] bool realizeOnePane(session::TerminalSessionManager& manager,
                                      vtworkspace::WindowId guiWindow);

  protected:
    [[nodiscard]] coro::Task<void> runClient(net::EventLoop* loop) override;

    // RemoteController hooks: the tmux-specific half of the shared connect lifecycle.
    void detachOnReactor() override
    {
        if (_gateway != nullptr)
            _gateway->detach();
    }
    void closeReactorBindings() override { closeAllPanes(); }
    [[nodiscard]] std::string connectTimeoutMessage() const override
    {
        return "timed out waiting for the tmux session's first pane";
    }
    [[nodiscard]] std::string connectClosedMessage() const override
    {
        return "tmux client ended during attach";
    }

  private:
    /// @param pty The pty to resolve.
    /// @return The tmux pane @p pty is bound to, or nullopt for a pty this controller did not hand
    ///         out. Callers must hold `_mutex`: the registry is shared with the reactor thread.
    [[nodiscard]] std::optional<uint64_t> paneForPtyLocked(vtpty::Pty const* pty) const;

    /// GUI-side (pty destructor): forgets a pane's pty binding.
    void unbindPane(uint64_t pane);

    /// Reactor-side (feed destructor): forgets a pane's feed.
    void dropFeed(uint64_t pane);

    /// Closes every bound pty (EOF to its session) — the disconnect path.
    void closeAllPanes();

    // The reactor and the connect state machine (_reactor, _mutex, _connected, _state, _failure,
    // _stopped) live in RemoteController; the registry below is tmux-specific.
    std::string _tmuxSocket;
    std::deque<PendingPane> _pending;
    std::unordered_map<uint64_t, PaneFeed*> _feeds;         ///< Model-owned sinks, by pane.
    std::unordered_map<uint64_t, vtpty::ChannelPty*> _ptys; ///< Bound ptys, by pane.
    /// Split anchor per tmux window, as a session ID resolved back to a live
    /// TerminalSession at use time — a raw pointer would dangle the moment the
    /// anchored pane dies (tab close, or tmux removing the pane).
    std::unordered_map<uint64_t, vtworkspace::SessionId> _actingByWindow;
    std::unordered_map<uint64_t, std::string> _pendingRenames; ///< %window-renamed titles awaiting apply.
    /// Panes tmux re-parented, by pane id, holding the record their DESTINATION realization needs.
    /// Parked here rather than in `_pending` until the source pty's unbind arrives: realizing while
    /// that pty is still bound would have realizeOnePane read the dying binding as "already
    /// realized" and drop the pane, and a new binding installed before the old destructor ran would
    /// be erased by it (both keyed only by pane id).
    std::unordered_map<uint64_t, PendingPane> _movedPanes;
    /// A deep copy of each not-yet-realized window's tmux layout tree, captured on the reactor thread
    /// (paneAdded) so the GUI thread can realize the whole tree without racing the model. Keyed by tmux
    /// window; dropped once the window is realized.
    std::unordered_map<uint64_t, vthost::tmux::BinaryLayout> _pendingTrees;
    std::optional<uint64_t> _nextBindPane; ///< The tmux pane the next createPty() binds to (whole-tree).
    bool _realizing =
        false; ///< True while adoptPendingPanes realizes tmux panes, so its splits build locally.
    vthost::tmux::TmuxGateway* _gateway = nullptr; ///< Reactor-owned; valid while serving.
    int _tmuxPid = -1;

    /// LAST member, destroyed FIRST: its pane sinks (PaneFeed) unregister from _feeds and the
    /// (base-owned) _mutex in their destructors, which must still be alive — _feeds is destroyed
    /// after _model, and _mutex lives in RemoteController (destroyed last of all). The destructor
    /// body's stop() has already joined the reactor by then.
    vthost::tmux::TmuxClientModel _model;
};

} // namespace contour
