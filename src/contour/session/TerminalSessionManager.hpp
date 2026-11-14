#pragma once

#include <contour/command/CommandHistory.hpp>
#include <contour/command/CommandHistoryStore.hpp>
#include <contour/config/ChromeTokenGroups.hpp>
#include <contour/config/LayoutStore.hpp>
#include <contour/config/ScrollBarMode.hpp>
#include <contour/config/ShellKind.hpp>
#include <contour/platform/SecureInput.hpp>
#include <contour/platform/SecureInputGate.hpp>
#include <contour/platform/SessionVisibility.hpp>
#include <contour/platform/ShellRcEditor.hpp>
#include <contour/platform/TaskbarProgressRouter.hpp>
#include <contour/platform/TaskbarProgressSink.hpp>
#include <contour/session/InProcessDirectory.hpp>
#include <contour/session/NodeDragSession.hpp>
#include <contour/session/SessionFactory.hpp>
#include <contour/session/SessionRole.hpp>
#include <contour/session/SwitchDirectory.hpp>
#include <contour/session/WindowBinding.hpp>
#include <contour/session/WindowSwitching.hpp>
#include <contour/window/ChromeFocus.hpp>
#include <contour/window/NodeDrag.hpp>
#include <contour/window/NoticeModel.hpp>
#include <contour/window/NoticeQueue.hpp>
#include <contour/window/PaneHeaderPolicy.hpp>
#include <contour/window/PaneKey.hpp>
#include <contour/window/SaveLayoutCheck.hpp>
#include <contour/window/ShellIntegrationOffer.hpp>
#include <contour/window/SidebarDropPolicy.hpp>
#include <contour/window/SwitcherRows.hpp>
#include <contour/window/TabAttention.hpp>
#include <contour/window/TaskbarAttentionTally.hpp>
#include <contour/window/WindowId.hpp>
#include <contour/window/WindowPositionStore.hpp>
#include <contour/window/WindowRole.hpp>

#include <core/platform/Clock.hpp>

#include <QtCore/QFileSystemWatcher>
#include <QtCore/QObject>
#include <QtCore/QPoint>
#include <QtCore/QPointer>
#include <QtCore/QRect>
#include <QtCore/QRectF>
#include <QtCore/QTimer>
#include <QtGui/QColor>
#include <QtQml/QQmlEngine>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <variant>
#include <vector>

#include <vthost/RestoredCounts.hpp>
#include <vthost/WorkspaceGit.hpp>
#include <vtworkspace/ModelEvents.hpp>
#include <vtworkspace/Move.hpp>
#include <vtworkspace/Pane.hpp>
#include <vtworkspace/Primitives.hpp>
#include <vtworkspace/SessionModel.hpp>
#include <vtworkspace/Tab.hpp>

class QScreen;

namespace contour::display
{
class TerminalDisplay;
class WindowHost;
} // namespace contour::display

namespace contour::window
{
class WindowController;
} // namespace contour::window

namespace contour::session
{

class TerminalSession;

class PaneProxy;

/// Why a set of sessions is being torn down — the caller's intent, which nothing downstream can
/// recover: a PTY is destroyed on a pane close, a tab close, a window close and an app quit alike.
/// It only matters for a session the local process does not own (a daemon-hosted one in attach
/// mode), where the two answers are opposites, so every teardown entry point must state which it is.
enum class SessionEnd : uint8_t
{
    /// The user ended these sessions for good (a pane or tab close). A daemon-hosted session is
    /// closed upstream too, via SessionFactory::requestRemoteClose, so it does not linger headless.
    Destroy,
    /// Only the local view goes away (a window close, an app quit, a lost connection, a window's switch
    /// to another workspace, a pane its window's layout no longer names). A daemon-hosted session keeps
    /// running and returns on the next attach — that is the whole point of daemon mode, so nothing is
    /// authored upstream here. It is let go of, though (SessionFactory::releaseRemoteSession): it may
    /// return over the very connection it was shown through.
    Detach,
};

/// Where openCommandPane() puts the pane running the command.
enum class CommandPanePlacement : uint8_t
{
    Split = 0, ///< A new pane stacked below the acting one, in its tab.
    Tab,       ///< A new tab in the acting pane's window.
};

/// Why openCommandPane() opened no pane.
enum class CommandPaneError : uint8_t
{
    RemoteSessions = 0, ///< Sessions run on a daemon or a tmux server (attach mode): none runs it.
    NoPane,             ///< No session can be made right now, or the acting session is in no tab.
};

/// A short reason for @p error, for logs and user-facing notices.
/// @param error The failure.
/// @return A lower-case phrase without a trailing period.
[[nodiscard]] constexpr std::string_view describe(CommandPaneError error) noexcept
{
    switch (error)
    {
        case CommandPaneError::RemoteSessions:
            return "a window attached to a daemon or a tmux server cannot open a pane for it; "
                   "use placement: detached instead";
        case CommandPaneError::NoPane: return "no pane could be opened for it";
    }
    return "unknown error";
}

/// Whether a footer's Reconnect is running.
enum class ReconnectRun : uint8_t
{
    Idle = 0, ///< None is: a connection that fails now was not asked for from a footer.
    Running,  ///< reconnectHost() started one, and the daemon has not come back or refused yet.
};

/// Whether showPane() found the pane it was asked to show.
enum class PaneShowing : uint8_t
{
    NotHeld = 0, ///< The window's displayed workspace holds no such pane; nothing changed.
    Shown,       ///< Its tab is active and the pane focused and seen.
};

/// Why restartSession() started no new session.
enum class RestartError : uint8_t
{
    NotOffered = 0, ///< The session has not ended, its end offers no Restart, or the remote end hosts it.
    NotHosted,      ///< No pane of this manager carries the session.
    NoSession,      ///< The session factory made no new session.
};

/// A short reason for @p error, for logs.
/// @param error The failure.
/// @return A lower-case phrase without a trailing period.
[[nodiscard]] constexpr std::string_view describe(RestartError error) noexcept
{
    switch (error)
    {
        case RestartError::NotOffered: return "the ended session offers no restart";
        case RestartError::NotHosted: return "no pane carries the session";
        case RestartError::NoSession: return "no new session could be made";
    }
    return "unknown error";
}

/// Why TerminalSessionManager::moveNode() moved nothing, the reason kept as each stage stated it: the place
/// refused the drag (window::moveRefusalOf()), the host would not take what it was asked
/// (SessionFactory::requestRemoteMove(): Refused, or Local where only the host may move), or the model
/// refused (or named nothing to move or to land on).
using MoveFailure = std::variant<window::MoveRefusal, RemoteRouting, vtworkspace::MoveError>;

/**
 * Session-lifetime service, SessionModel host, and per-window ModelEvents router.
 *
 * The manager owns the single Qt-free vtworkspace::SessionModel (the authoritative window/tab/pane tree,
 * including per-tab title and color, so a future daemon and its network clients share the same
 * state) and the SessionId <-> TerminalSession* registry, and it creates/tears down the backing
 * TerminalSessions. It is NOT the GUI adapter: each OS window has its own WindowController (a
 * QAbstractListModel + PaneProxy tree + window services) that the QML binds to. The manager mints
 * those controllers (createWindowController) and, as the vtworkspace::ModelEvents implementer, ROUTES each
 * model change to the owning window's controller (tab events by WorkspaceId; pane events resolve
 * TabId -> WorkspaceId). Session -> display ownership lives solely on the pane tree (the QML `session:`
 * binding -> TerminalDisplay::setSession); the manager holds no per-display session map.
 *
 * It is the one thing in session/ that still names the concrete display and window types, and that is
 * deliberate: it IS the window/display registry, so routing "which window owns this display" needs
 * both. Everything else in the layer speaks the interfaces instead -- TerminalSession holds a
 * DisplaySurface, and windowHostForDisplay() below is where the concrete controller becomes the
 * display::WindowHost the display layer declared. Moving the registry above session/ would finish the
 * job; nothing else depends on it happening.
 *
 * It is NOT a list model: the GUI tab strip renders from the per-window WindowController's
 * QAbstractListModel (WindowController::Roles). Every tab/pane operation here is routed by an
 * explicit window identity — either the calling controller's vtworkspace::WorkspaceId or the acting
 * session's hosting tab — never by an implicit "the" window.
 */
class TerminalSessionManager: public QObject, public vtworkspace::ModelEvents
{
    Q_OBJECT
    Q_MOC_INCLUDE("contour/session/TerminalSession.hpp")
    Q_MOC_INCLUDE("contour/window/WindowController.hpp")
    Q_PROPERTY(bool multimediaReady READ isMultimediaReady NOTIFY multimediaReadyChanged)
    QML_ELEMENT

  public:
    /// @param app     The owning application (spawn context, app-wide services).
    /// @param factory The PTY factory backing the sessions of every window no binding claims (@see
    ///                bindWindow) (injected: production wires AppSessionFactory, tests an in-memory MockPty
    ///                factory). Must outlive this manager (both live on ContourGuiApp, factory declared
    ///                first).
    /// @param layouts Persistence for named layouts, for SaveLayout (injected: production wires
    ///                FileLayoutStore, tests an in-memory store). Must outlive this manager.
    /// @param commands Persistence for the command palette's most-recently-used list (injected:
    ///                production wires FileCommandHistoryStore, tests an in-memory store). Must
    ///                outlive this manager.
    /// @param homeDirectory The user's home directory, as the tab tooltip and the finish notification
    ///                write it as `~`. Read once, by the composition root, so nothing below it reads
    ///                the environment.
    /// @param taskbarProgressSink Application-owned native progress adapter; must outlive the manager.
    /// @param secureInput The OS's secure keyboard entry, which only this manager's gate calls. Owned by the
    ///                app and declared before it, so it outlives the gate, whose destruction turns it off.
    /// @param windowSwitching What switches a window between daemons and workspaces, and what the switching
    ///                actions dispatch to (remote::WindowSwitcher, which sits above session/). Borrowed and
    ///                not called during construction: it may be built on this manager, and so after it, but
    ///                must outlive every request made through it.
    /// @param switchDirectory What the switching actions and their palette rows list: local daemons, shown
    ///                hosts and a window's workspaces. Borrowed like @p windowSwitching.
    TerminalSessionManager(ContourGuiApp& app,
                           SessionFactory& factory,
                           config::LayoutStore& layouts,
                           command::CommandHistoryStore& commands,
                           std::string homeDirectory,
                           platform::TaskbarProgressSink& taskbarProgressSink,
                           platform::ISecureInput& secureInput,
                           WindowSwitching& windowSwitching,
                           SwitchDirectory& switchDirectory);

    /// Out of line: the members' destructors need the complete TerminalSession and WindowController,
    /// which this header only forward-declares.
    ~TerminalSessionManager() override;

    /// The owning application (spawn context, app-wide services). For the window controllers.
    [[nodiscard]] ContourGuiApp& app() noexcept { return _app; }

    /// @return What the switching actions dispatch to; fixed at construction.
    [[nodiscard]] WindowSwitching& windowSwitching() const noexcept { return _windowSwitching; }

    /// @return What the switching actions and their palette rows list; fixed at construction.
    [[nodiscard]] SwitchDirectory& switchDirectory() const noexcept { return _switchDirectory; }

    /// The user's home directory, for shortening a path on this machine to `~/...`. Fixed at
    /// construction; empty shortens nothing.
    [[nodiscard]] std::string const& homeDirectory() const noexcept { return _homeDirectory; }

    /// Creates a backing session + its model tab in @p window (the calling controller's window).
    /// @param window      The target window.
    /// @param profileName Profile to launch the session with, or std::nullopt for the app default.
    /// @param command     A command to run instead of the profile's shell, or std::nullopt.
    /// @param role        What the session's program is to the user.
    /// @param startDirectory Where the shell starts when @p window has no active pane to inherit that from
    ///        (a workspace made just now), as UTF-8; std::nullopt for the profile's.
    TerminalSession* createSessionInBackground(
        vtworkspace::WorkspaceId window,
        std::optional<std::string> const& profileName = std::nullopt,
        std::optional<vtpty::Process::ExecInfo> const& command = std::nullopt,
        SessionRole role = SessionRole::Shell,
        std::optional<std::string> startDirectory = std::nullopt);

    /// Creates the terminal Settings previews a profile in: a SessionRole::Preview session over a mock
    /// device, showing PreviewTranscript at PreviewPageSize, under @p config's @p profileName. It is the
    /// caller's, in no window and not among the manager's sessions, so a configuration reload does not touch
    /// it and no layout saves it; its configuration is applied to it by whoever owns it.
    /// @param config      The configuration to preview: the app's with the drafts put in place.
    /// @param profileName The profile of @p config to run under.
    /// @return The session.
    /// @pre GUI thread.
    [[nodiscard]] std::unique_ptr<TerminalSession> createPreviewSession(config::Config const& config,
                                                                        std::string const& profileName);

    /// Creates a new tab in @p window (the GUI "+" button entry point, via WindowController).
    /// @param window      The target window.
    /// @param profileName Profile to launch the tab with, or std::nullopt for the app default.
    TerminalSession* createSession(vtworkspace::WorkspaceId window,
                                   std::optional<std::string> const& profileName = std::nullopt);

    /// Creates and activates a new tab in @p window.
    /// @param window      The target window.
    /// @param profileName Profile to launch the tab with, or std::nullopt for the app default.
    void createNewTab(vtworkspace::WorkspaceId window,
                      std::optional<std::string> const& profileName = std::nullopt)
    {
        // Attach mode: author the tab on the daemon (B3-Qt); its layout re-push
        // reconciles it into a local tab. A local factory returns false and the
        // tab is created here as usual.
        //
        // @p window travels along as one of its ptys: a remote model mints its own window ids, so
        // a session it already hosts is the only name both ends share. Dropping it entirely — as
        // this did — sent every window's "+" to the daemon's FIRST workspace, so a tab opened in a
        // second attach-mode window appeared in the wrong one and the window clicked in gained
        // nothing.
        //
        // A panel window (the quick terminal) is bound to no host: its local factory builds the tab here.
        if (factoryFor(window).requestRemoteTab(actingPtyOfWindow(window),
                                                RemoteSpawn { .profileName = profileName.value_or("") }))
            return;
        createSession(window, profileName);
    }

    /// Creates and activates a new tab in the window hosting @p acting (the CreateNewTab keybinding).
    /// Creates a tab in the window the acting session belongs to.
    ///
    /// @param acting      The session the request came from.
    /// @param profileName The profile to launch under, or nullopt for the default.
    void createNewTab(TerminalSession* acting, std::optional<std::string> const& profileName = std::nullopt);

    /// Closes every tab in the window the acting session belongs to.
    ///
    /// With a single window open this closes the application -- through the window's own teardown,
    /// which is the tested path, rather than through actions::Quit's bare exit().
    void closeAllTabs(TerminalSession* acting);

    /// Looks up @p name in the app's configured layouts and appends its tabs to the window hosting
    /// @p acting (the LaunchLayout action). Logs and no-ops if the layout is unknown or @p acting has
    /// no hosting window.
    /// @param name   The layout's key in Config::layouts.
    /// @param acting The session that triggered the action; its hosting window is the target.
    void launchLayout(std::string const& name, TerminalSession* acting);

    /// Saves the window hosting @p acting as a named layout, persisted to layouts.yml (the SaveLayout
    /// action). Thin wrapper around saveWindowLayout(); no-ops if @p acting has no hosting window.
    /// @param name   The key to save the layout under in Config::layouts.
    /// @param acting The session that triggered the action; its hosting window is serialized.
    void saveLayout(std::string const& name, TerminalSession* acting);

    /// Opens the save-as name prompt over the window hosting @p acting (a nameless `SaveLayout` action),
    /// routed exactly like beginTabColorPick(). The window then calls back into saveLayout() with the
    /// name the user typed. No-ops if @p acting has no hosting window.
    /// @param acting The session that triggered the action; its hosting window shows the prompt.
    void beginSaveLayoutPrompt(TerminalSession* acting);

    /// Serializes @p window's live tab/pane tree into a Layout, persists it through the
    /// injected LayoutStore, and — only once the store has accepted it, so runtime state can never
    /// claim more than what is actually saved — stores it under @p name in the app's in-memory config.
    /// @param window The window whose tabs/panes to serialize.
    /// @param name   The key to save the layout under.
    /// @param replace Whether a layout already saved under @p name -- in the layouts file as it is read now,
    ///                or in the configuration -- may be replaced; Keep refuses it with NameTaken, checked
    ///                where the store is read, so a name taken since the dialog checked it is not
    ///                overwritten.
    /// @return Nothing on success, or the reason the layout could not be saved.
    [[nodiscard]] std::expected<void, config::LayoutSaveError> saveWindowLayout(
        vtworkspace::WorkspaceId window, std::string const& name, window::LayoutReplace replace);

    /// Where layouts are persisted: the `layouts.yml` sibling of the loaded config file — which is
    /// exactly where loadConfigFromFile() merges it back from, so saving and loading can never
    /// disagree (a custom `--config` path moves both together).
    /// @return The layout store's path for this run's configuration.
    [[nodiscard]] std::filesystem::path layoutsFilePath() const;

    /// The names of the layouts saved: the configuration's and the layouts file's, as it reads now. A file
    /// that cannot be read adds none (saving refuses then anyway).
    /// @return The names, each once.
    [[nodiscard]] std::vector<std::string> savedLayoutNames() const;

    /// Opens the command palette over the window hosting @p acting (the OpenCommandPalette action).
    /// No-ops if @p acting has no hosting window.
    /// @param acting The session that triggered the action; its hosting window shows the palette.
    void openCommandPalette(TerminalSession* acting);

    /// Shows the app's one Settings window (the OpenConfiguration action's default), in whatever state that
    /// window is in: built on first use, else as it was left. Even with no hosting window, Settings opens.
    /// @param acting The session that triggered the action; Settings opens on its hosting window's screen,
    ///               or on the primary screen when it has none.
    void openSettings(TerminalSession* acting);

    /// Shows the Connect picker as a sheet over the window hosting @p acting (the NewWindowTo action).
    /// @param acting The session that triggered the action; the sheet opens on its hosting window's screen.
    void openConnectPicker(TerminalSession* acting);

    /// The daemon and workspace switching actions. Each acts on the window hosting @p acting, asks
    /// windowSwitching() and returns at once: the answer is logged when it comes. A window with no binding
    /// (sessions in-process) has nothing to switch between, so each is a logged no-op there.
    /// @{

    /// Switches (or, for SwitchMode::Open, opens) @p target for the window hosting @p acting.
    /// @param acting The session that triggered the action.
    /// @param target Where to go.
    /// @param mode Replace what the window shows, or show the target in a new window.
    void requestSwitch(TerminalSession const& acting, SwitchTarget target, SwitchMode mode);

    /// Shows the next or previous daemon of the ring (@see nextDaemon) in the window hosting @p acting.
    /// @param acting The session that triggered the action.
    /// @param direction Which way to step.
    void cycleDaemon(TerminalSession const& acting, actions::CycleDirection direction);

    /// Shows the next or previous workspace of the window's daemon, as the directory lists them.
    /// @param acting The session that triggered the action.
    /// @param direction Which way to step.
    void cycleWorkspace(TerminalSession const& acting, actions::CycleDirection direction);

    /// Opens the daemon and workspace switcher popup over the window hosting @p acting.
    /// @param acting The session that triggered the action.
    void showSwitcher(TerminalSession const& acting);

    /// Opens the switcher as the workspace chip's dropdown: hanging below the chip, listing the workspaces of
    /// the daemon the window hosting @p acting is on, and nothing else.
    /// @param acting The session of the window whose chip was clicked.
    /// @param chip The chip, in its window's scene coordinates.
    void showWorkspaceDropdown(TerminalSession const& acting, QRectF chip);

    /// (In-process) Gives the window hosting @p acting a workspace of its own with one new tab and shows it;
    /// the workspace shown until now keeps running, hidden. Does nothing for a window a daemon serves, which
    /// has its workspaces from the daemon.
    /// @param acting The session that triggered the action.
    void newWorkspace(TerminalSession const& acting);

    /// (In-process) What newWorkspace() does once it knows where the new workspace starts: a workspace of its
    /// own for the window hosting @p acting, shown, whose first shell starts in @p cwd and which is named
    /// after that folder (@see vtbackend::defaultWorkspaceName) -- once, so a later cd renames nothing. A
    /// failed first session drops the workspace again.
    /// @param acting The session that triggered the action.
    /// @param cwd The folder the first shell starts in, as UTF-8; empty or "." for the profile's.
    void newWorkspaceStartingIn(TerminalSession const& acting, std::string const& cwd);

    /// (In-process) Labels @p workspace, which the in-process directory of its window lists
    /// (InProcessDirectory); an empty label removes it. Not for a daemon's workspace, whose label is its
    /// daemon's.
    /// @param workspace The workspace.
    /// @param label Its label.
    void setWorkspaceLabel(vtworkspace::WorkspaceId workspace, std::string label);

    /// Renames the workspace @p workspace that @p window lists: in its daemon for a window a daemon serves
    /// (the daemon's WorkspaceMeta then reaches every window), in this process otherwise. A workspace the
    /// window does not own is left alone.
    /// @param window The window the user acts in.
    /// @param workspace The workspace's id, as the window's directory lists it (DirectoryWorkspace::id).
    /// @param label Its new label; empty clears it.
    void renameWorkspace(window::WindowController& window, uint64_t workspace, std::string label);

    /// Sets the title of the tab @p tab, whichever workspace of @p window's directory holds it, shown or not:
    /// in its daemon for a window a daemon serves (named by a session it holds), in this process's model
    /// otherwise. A tab the window does not list is left alone.
    /// @param window The window the user acts in.
    /// @param tab The tab's id, as the window's directory lists it (DirectoryTab::id).
    /// @param title Its new title; empty returns it to its automatic one.
    void renameTab(window::WindowController& window, uint64_t tab, std::string title);

    /// Moves the workspace @p workspace that @p window lists to place @p toPosition of its directory, where
    /// a daemon (or this process's model) orders it. The place is clamped.
    /// @param window The window the user acts in.
    /// @param workspace The workspace's id, as the window's directory lists it.
    /// @param toPosition The 0-based place in the directory.
    void moveWorkspace(window::WindowController& window, uint64_t workspace, int toPosition);

    /// Moves the tab @p tab, whichever workspace of @p window's directory holds it, to place @p toPosition
    /// of the workspace @p toWorkspace; none of its sessions closes. A tab of another host's, or into another
    /// host's workspace, stays where it is.
    /// @param window The window the user acts in.
    /// @param tab The tab's id, as the window's directory lists it (DirectoryTab::id).
    /// @param toWorkspace The destination workspace's id, as the directory lists it.
    /// @param toPosition The 0-based place among the destination's tabs.
    void moveTabToWorkspace(window::WindowController& window,
                            uint64_t tab,
                            uint64_t toWorkspace,
                            int toPosition);

    /// Moves a pane or a whole tab of @p window's directory: into a workspace as a tab, into a tab as its
    /// splits (beside that tab's active pane), or into a workspace of its own, which the window then shows.
    /// None of its sessions closes; a tab or a workspace it empties closes. A tab into a workspace is
    /// moveTabToWorkspace(). On a daemon the daemon moves it (MoveNode), and its push brings the result.
    /// @param window The window the user acts in.
    /// @param node The pane, by its key, or the tab, as the directory names them.
    /// @param destination Where it goes; a MoveTab's position is the 0-based place it ends at.
    void moveSidebarNode(window::WindowController& window,
                         window::SidebarNode const& node,
                         window::NodeDestination const& destination);

    /// @param window A window.
    /// @return What the host of the workspace @p window shows can move (SessionFactory::hostMoves()).
    [[nodiscard]] window::HostMoves hostMovesOf(window::WindowController const& window) const noexcept;

    /// @param window The window the user acts in.
    /// @param subject What moves: a tab or a pane.
    /// @param landing What the move does.
    /// @return Why @p window's host would not make the move (window::sidebarHostTakes()); None where it
    /// would.
    [[nodiscard]] window::MoveRefusal sidebarMoveRefusal(window::WindowController const& window,
                                                         window::DragSubject subject,
                                                         window::SidebarLanding landing) const noexcept;

    /// Closes the workspace @p workspace that @p window lists, with all its tabs and panes. A workspace that
    /// may be running a program is asked about first, as a tab is: the window raises
    /// closeWorkspaceConfirmationRequested(), and confirmCloseWorkspace() closes it. An in-process window's
    /// last workspace is kept: closing it would leave the window with nothing to show.
    /// @param window The window the user acts in.
    /// @param workspace The workspace's id, as the window's directory lists it.
    void closeWorkspace(window::WindowController& window, uint64_t workspace);

    /// Closes the workspace @p workspace that @p window lists without asking: the answer to
    /// closeWorkspaceConfirmationRequested(). One that is gone already closes nothing.
    /// @param window The window the user acts in.
    /// @param workspace The workspace's id, as the window's directory lists it.
    void confirmCloseWorkspace(window::WindowController& window, uint64_t workspace);

    /// @return The labels of the in-process workspaces.
    [[nodiscard]] WorkspaceLabels const& workspaceLabels() const noexcept { return _workspaceLabels; }

    /// Tells the windows bound to @p binding that what its directory lists changed
    /// (window::WindowController::workspaceDirectoryChanged), as a daemon's connection announces it. GUI
    /// thread.
    /// @param binding The binding whose directory changed.
    void notifyWorkspaceDirectoryChanged(WindowBinding const& binding) const;

    /// Asks for a label (the switcher popup's label prompt), starts a new local daemon under it, and opens a
    /// new window on it, for the window hosting @p acting.
    /// @param acting The session that triggered the action.
    void newDaemon(TerminalSession const& acting);

    /// Upgrades the daemon of the window hosting @p acting in place (WindowBinding::upgradeDaemonOnCommand);
    /// a refusal is shown to that window as a notice and logged, as is a window with no daemon.
    /// @param acting The session that triggered the action.
    void upgradeDaemon(TerminalSession const& acting);
    /// @}

    /// Re-reads the master configuration from disk once (picking up any GUI side-file changes) into
    /// the app's configuration -- what the next new tab is built from -- and pushes it to every live
    /// session, each under its own profile. Every reload goes through here: the settings page's
    /// Save/Delete, the live_config file watcher, and the ReloadConfig and ResetConfig actions.
    /// @return Why the file could not be read; the app's configuration and the sessions are then
    ///         left as they were.
    [[nodiscard]] std::expected<void, std::string> reloadAllSessions();

    /// Tells the user of @p notice in the window hosting @p acting; logs it when no window hosts it.
    /// GUI thread, and never under the terminal lock: a toast that shows asks where to go, which reads the
    /// active pane's cursor under that lock (WindowController::toastHost()).
    /// @param notice What to tell.
    /// @param acting The session the notice is about.
    void postNotice(window::Notice notice, TerminalSession const* acting);

    /// Offers to install shell integration in @p session's pane, when the facts allow it (window::offerFor():
    /// a known shell on this machine, not dismissed, its startup file not integrated yet, and no card shown
    /// in this run). Called by a session whose user entered ShellIntegrationProbeCommands commands with no
    /// OSC 133 mark seen. GUI thread.
    /// @param session The session whose pane shows the card.
    /// @param shell The shell running in it.
    void offerShellIntegration(TerminalSession& session, config::ShellKind shell);

    /// Adds @p line to @p shell's startup file, once, and tells the user in @p session's window: that it
    /// was added (also when the file already had it), or why it was not. GUI thread.
    /// @param session The session the user answered the card in.
    /// @param shell The shell to integrate.
    /// @param line The line the card showed, which is exactly what is written.
    /// @return What the editor did, or why it did nothing.
    [[nodiscard]] std::expected<platform::RcInstall, platform::ShellRcError> installShellIntegration(
        TerminalSession& session, config::ShellKind shell, std::string_view line);

    /// Remembers that the user said "Not Now" for @p shell, so no card offers it again. A state that cannot
    /// be written is logged: the card still hides. GUI thread.
    /// @param shell The shell declined.
    void dismissShellIntegration(config::ShellKind shell);

    /// Tells the user of @p notice in every window: what is about the application, not one session.
    void postNoticeToEveryWindow(window::Notice const& notice);

    /// Tells the user of @p notice in every window bound to @p binding: what is about its connection.
    /// @param binding The binding whose windows are told.
    /// @param notice  The notice.
    void postNoticeToWindowsOf(WindowBinding const& binding, window::Notice const& notice);

    /// Holds what @p binding's daemon restored until a window of the layout that follows is up to be told it
    /// in (@see deliverRestoreSummaryTo). A summary @p binding still holds is replaced. GUI thread.
    /// @param binding The binding whose daemon restored it.
    /// @param counts  The windows and tabs that came back.
    void noteWorkspaceRestored(WindowBinding const& binding, vthost::RestoredCounts counts);

    /// Tells the window @p window of the summary its binding holds (noteWorkspaceRestored()), once: posts the
    /// "Restored 2 windows, 7 tabs" notice there and forgets it, so a second window shows nothing. Without a
    /// held summary, a binding, or a controller for @p window, does nothing. GUI thread.
    /// @param window The window the daemon's restored layout was just realized or reconciled in.
    void deliverRestoreSummaryTo(vtworkspace::WorkspaceId window);

    /// Forgets the summary @p binding holds (noteWorkspaceRestored()), undelivered: the layout it belonged to
    /// showed no window it could go to, and a toast about a restore must not surface in a window opened
    /// later. GUI thread.
    /// @param binding The binding whose summary goes.
    void expireRestoreSummary(WindowBinding const& binding) noexcept;

    /// Takes every notice of @p kind away, in every window.
    void clearNoticeInEveryWindow(window::NoticeKind kind);

    /// Opens the configuration file in the user's editor (a config-reload banner's action), through the
    /// app's external launcher; a launch that fails is logged.
    void openConfigFile();

    /// Opens the terminal context menu over the pane of @p acting (the OpenContextMenu action).
    ///
    /// Makes that pane active first, so the menu is built from the state of the pane the user actually
    /// right-clicked and every row it offers runs against that same pane — not against whichever pane
    /// happened to be active beforehand. No-ops if @p acting has no hosting window.
    ///
    /// @param acting The session that was right-clicked.
    void openContextMenu(TerminalSession* acting);

    /// Opens the menu of one command block over the pane of @p acting: the block's header and rows alone,
    /// where the context menu adds the pane's own. Makes that pane active first, as openContextMenu() does.
    /// No-ops if @p acting has no hosting window.
    /// @param acting The session the block is in.
    /// @param block The block, pinned by id.
    /// @param anchor Where the menu opens, in the window's coordinates; at the pointer when absent.
    void openBlockMenu(TerminalSession* acting,
                       vtbackend::CommandBlockId block,
                       std::optional<QPointF> anchor);

    /// Records that the command @p id was just run, and persists the updated list.
    ///
    /// App-wide, not per-window: "recently used" is a fact about the USER, so a command run in one
    /// window is recent in all of them.
    ///
    /// Persisted on every run rather than at exit, so the list survives a crash or a kill -9 — the
    /// file is a few hundred bytes and the write is atomic (see FileCommandHistoryStore).
    ///
    /// @param id The command id that was run (see commandId()).
    void recordCommand(std::string const& id);

    /// The app-wide most-recently-used command list, for the palette models to read.
    [[nodiscard]] command::CommandHistory const& commandHistory() const noexcept { return _commandHistory; }

    /// Brings the history into a usable state: re-applies the configured
    /// `command_palette_recent_count` (so editing it and reloading the config takes effect without a
    /// restart), and seeds the list from the store on first use.
    ///
    /// Idempotent, and called from BOTH entry points (openCommandPalette and recordCommand) rather than
    /// only the first — see recordCommand() for why relying on the ordering would fail silently.
    void ensureCommandHistoryReady();

    /// Where the command palette's MRU list is persisted: the `command-history.yml` sibling of the
    /// loaded config file, sited exactly like layoutsFilePath() so a custom `--config` moves it too.
    /// @return The command-history store's path for this run's configuration.
    [[nodiscard]] std::filesystem::path commandHistoryFilePath() const;

    /// The path of @p fileName as a sibling of the loaded config file (the config home when no file was
    /// loaded). The one place the machine-written stores' siting rule lives.
    /// @param fileName The file's name, e.g. "layouts.yml".
    [[nodiscard]] std::filesystem::path configSiblingPath(std::string_view fileName) const;

    /// Appends every tab of @p layout to @p window, building a real PTY-backed session for each leaf
    /// pane (via createBackingSession) before handing the pane tree to realizeLayoutTab. Used by both
    /// launchLayout and startup (--layout / default_layout).
    /// @param window   The target window.
    /// @param layout   The layout to realize (must have at least one tab).
    /// @param pageSize The total page size each pane's grid and child PTY are born at. Pass the live
    ///                 window's running size (LaunchLayout into an existing window) so commands that
    ///                 read the terminal size at startup see the real one; @c std::nullopt (a
    ///                 brand-new window at startup) uses the profile's configured terminalSize.
    /// @param beforeLeafSeed Invoked (when set) with each leaf pane immediately before its backing
    ///                 session is created — the hook attach mode uses to bind the pane about to be born
    ///                 to a specific remote session (via vthost::client::WireLayout::leafSession).
    /// @return false if @p layout has no tabs (nothing to apply); true otherwise.
    bool applyLayoutToWindow(vtworkspace::WorkspaceId window,
                             config::Layout const& layout,
                             std::optional<vtbackend::PageSize> pageSize = std::nullopt,
                             std::function<void(config::LayoutPane const&)> const& beforeLeafSeed = {});

    // Keyboard tab navigation/reordering. Every entry point takes the ACTING session (the one that
    // received the keybinding) and targets that session's hosting window, so a keybinding in any
    // OS window operates on that window's tabs.
    void switchToPreviousTab(TerminalSession* acting);
    void switchToTabLeft(TerminalSession* acting);
    void switchToTabRight(TerminalSession* acting);
    void switchToTab(int position, TerminalSession* acting);
    void closeTab(TerminalSession* acting);
    void moveTabTo(int position, TerminalSession* acting);
    void moveTabToLeft(TerminalSession* session);
    void moveTabToRight(TerminalSession* session);
    /// Opens the inline title editor for the active tab of the window hosting @p acting (the
    /// SetTabTitle keybinding), routed like the other tab ops via the acting session's window.
    /// @param acting The session that received the keybinding; its hosting window is the target.
    void beginTabTitleEdit(TerminalSession* acting);
    /// Runs a tab action against the tab hosting @p acting, through the hosting window's controller
    /// (which alone maps a tab to its row). Does nothing when @p acting has no tab or window.
    /// @param action The tab action to run.
    /// @param acting The session that received the action; its tab is the target.
    void runTabAction(actions::Action const& action, TerminalSession* acting);
    /// Shows the About dialog over the window hosting @p acting.
    /// @param acting The session that received the action; its hosting window is the target.
    void showAbout(TerminalSession* acting);
    /// Minimizes the window hosting @p acting.
    /// @param acting The session that received the action; its hosting window is the target.
    void minimizeWindow(TerminalSession* acting);
    /// Maximizes the window hosting @p acting, or restores it when it is maximized.
    /// @param acting The session that received the action; its hosting window is the target.
    void zoomWindow(TerminalSession* acting);
    /// Moves keyboard focus through the chrome of the window hosting @p session (the focus actions).
    /// @param session The session that received the action; its hosting window is the target.
    /// @param action Where focus goes.
    /// @return Moved when focus changed area; Stayed otherwise, and the key then reaches the terminal.
    window::FocusMove moveChromeFocus(TerminalSession const& session, window::ChromeFocusAction action);
    /// The window controller a sidebar action of @p session acts on.
    /// @param session The session that received the action.
    /// @return The controller of the window hosting @p session, or nullptr when it has none.
    [[nodiscard]] window::WindowController* windowOf(TerminalSession const& session) const noexcept
    {
        return controllerHostingSession(&session);
    }
    /// Opens the color picker for the active tab of the window hosting @p acting (a `SetTabColor`
    /// keybinding that names no color), routed exactly like beginTabTitleEdit().
    /// @param acting The session that received the keybinding; its hosting window is the target.
    void beginTabColorPick(TerminalSession* acting);
    /// Colors the active tab of the window hosting @p acting (a `SetTabColor` keybinding that names a
    /// color) as the user's own choice, so it outranks an application's DECAC color.
    /// @param color The color to apply.
    /// @param acting The session that received the keybinding; its hosting window is the target.
    void setActiveTabColor(vtbackend::RGBColor color, TerminalSession* acting);
    /// Drops the user's color from the active tab of the window hosting @p acting (the `ResetTabColor`
    /// keybinding), letting an application-assigned DECAC color resurface.
    /// @param acting The session that received the keybinding; its hosting window is the target.
    void resetActiveTabColor(TerminalSession* acting);

    /// Lets go of a session that has ended: closes its pane, forgets it, and frees it once control
    /// returns to the event loop.
    /// @param session The ended session. One this manager did not make is left alone.
    void removeSession(TerminalSession& session);
    void currentSessionIsTerminated();

    // {{{ Tab-strip operations (window-routed; called by WindowController with its own WorkspaceId)
    void activateTab(vtworkspace::WorkspaceId window, int index);
    /// Shows the pane a window's ledger knows as @p key in @p window's displayed workspace: activates its
    /// tab, focuses it, and counts it as seen.
    /// @param window The window's displayed workspace.
    /// @param key    The pane's key (paneKeyOf).
    /// @return Shown, or NotHeld when the displayed workspace holds no such pane, and nothing changed.
    [[nodiscard]] PaneShowing showPane(vtworkspace::WorkspaceId window, window::PaneKey key);
    /// Counts @p window's active pane as seen: its output, bell and finished marks clear and its
    /// program-status records up to the newest count as seen.
    /// @param window The window's displayed workspace.
    void markActivePaneSeen(vtworkspace::WorkspaceId window);
    void moveTab(vtworkspace::WorkspaceId window, int fromIndex, int toIndex);
    /// Moves the tab at row @p fromIndex of window @p from into window @p to at row @p toIndex,
    /// transplanting it intact (its sessions survive). Drives the drag of a tab between windows.
    /// @param from      The source window's id.
    /// @param fromIndex The tab's row in @p from.
    /// @param to        The destination window's id.
    /// @param toIndex   The destination row.
    void moveTabToWorkspace(vtworkspace::WorkspaceId from,
                            int fromIndex,
                            vtworkspace::WorkspaceId to,
                            int toIndex);
    /// Grafts the whole tab at row @p fromIndex of window @p from beside the pane @p leaf of tab @p target
    /// in window @p to, on @p side, keeping every session (none is closed or re-created). The sessions
    /// are re-pointed at @p target, the grafted active pane gets focus, and a source window this empties
    /// is closed. Drives dropping a dragged tab on the edge of a pane.
    ///
    /// On a daemon-hosted window the merge is authored on the daemon (SessionFactory::requestRemoteTabMerge)
    /// and its layout re-push reconciles it in; nothing changes here until then. Callers ask
    /// tabMergeSupport() first.
    /// @param from      The source window's id.
    /// @param fromIndex The tab's row in @p from.
    /// @param to        The destination window's id.
    /// @param target    The tab holding @p leaf.
    /// @param leaf      The pane to split.
    /// @param side      The side of @p leaf the tab goes on.
    void mergeTabIntoSplit(vtworkspace::WorkspaceId from,
                           int fromIndex,
                           vtworkspace::WorkspaceId to,
                           vtworkspace::TabId target,
                           vtworkspace::PaneId leaf,
                           vtworkspace::SplitSide side);
    /// Applies a move -- a pane or a whole tab, beside a pane, onto one, into a tab well or into a new window
    /// -- to this manager's model, keeping its own bookkeeping in step. Every moved TerminalSession is the
    /// same object afterwards, shown wherever the model now puts it: none is spawned or closed. A window the
    /// move empties is closed.
    ///
    /// The local half of a move only, never routed: what the attach-mode reconciler replays of a move the
    /// daemon already made (contour::remote::applyRemoteLayout), and what an in-process drop builds on.
    /// @param source What moves.
    /// @param target Where it lands.
    /// @return Where focus lands, or why the model refused (and changed nothing).
    [[nodiscard]] std::expected<vtworkspace::MoveResult, vtworkspace::MoveError> applyNodeMove(
        vtworkspace::MoveSource const& source, vtworkspace::MoveTarget const& target);

    /// Who hosts the sessions of @p window, as a drag payload names it: the daemon's run where they are
    /// hosted by a daemon that states one, this process otherwise.
    /// @param window A window of this manager; one it does not know is answered as an unbound one.
    [[nodiscard]] window::HostIdentity hostIdentity(vtworkspace::WorkspaceId window) const;

    /// The id the host knows @p session by, as a drag payload names it: its SessionId in-process, its daemon
    /// session id when daemon-hosted.
    /// @param session A session of this manager.
    /// @return The id; std::nullopt for a daemon-hosted session not bound to a remote one yet.
    [[nodiscard]] std::optional<uint64_t> hostSessionIdOf(TerminalSession const& session) const;

    /// The session host @p host knows by @p hostSession (@see hostSessionIdOf), if this process shows it.
    /// @param host Who hosts it, as a drag payload names it (@see hostIdentity).
    /// @param hostSession Its id there.
    [[nodiscard]] TerminalSession* sessionForHostId(window::HostIdentity const& host,
                                                    uint64_t hostSession) const;

    /// The drag payload of the pane showing @p session.
    /// @return The payload; std::nullopt for a session no pane shows, or one the host has no id for yet.
    [[nodiscard]] std::optional<window::NodeDrag> paneDragOf(TerminalSession const& session) const;

    /// The drag payload of the tab at row @p tabIndex of @p window: a Tab for one of several panes, a Pane
    /// for one of one.
    /// @return The payload; std::nullopt for no such tab, or one with a pane the host has no id for yet.
    [[nodiscard]] std::optional<window::NodeDrag> tabDragOf(vtworkspace::WorkspaceId window,
                                                            int tabIndex) const;

    /// Resolves @p drag against the model: the local tab it is or came from, what it moves, and why it cannot
    /// be dropped at each place (window::moveRouteOf(), window::moveRefusalOf()), as the host it names serves
    /// moves. Into a window bound to another host it is refused whatever the place (@see moveRefusalIn).
    /// @param drag The drag, from whichever process started it.
    /// @return The drag, resolved.
    [[nodiscard]] ResolvedDrag resolveNodeDrag(window::NodeDrag drag) const;

    /// Why @p drag cannot be dropped at @p place in @p window: the place's own refusal, or, for a window
    /// bound to another host than the window the drag came from (@see bindWindow), that it belongs to
    /// another host. Two windows each on a link of its own to one daemon share their host
    /// (@see servedByOneHost).
    /// @param drag   The drag, resolved (resolveNodeDrag()).
    /// @param window The window it hovers.
    /// @param place  Where in @p window.
    /// @return The refusal; window::MoveRefusal::None when it can be dropped there.
    [[nodiscard]] window::MoveRefusal moveRefusalIn(ResolvedDrag const& drag,
                                                    vtworkspace::WorkspaceId window,
                                                    window::MovePlace place) const;

    /// The drag @p nodePayload names, resolved: parsed and resolved once per drag, and from then on read back
    /// for every hover until the layout changes. The drag this process runs is the one its session holds; a
    /// drag of another process is resolved as it enters a window (@see forgetHoveredDrag).
    /// @param nodePayload The drag's `application/x-contour-node` JSON.
    /// @return The drag; null for JSON that is no node drag.
    [[nodiscard]] ResolvedDrag const* resolveNodeDrag(QString const& nodePayload) const;

    /// A drag entered a window: the drag of another process it may be is resolved afresh.
    void forgetHoveredDrag() noexcept { _hoveredDrag.reset(); }

    /// Moves what @p drag names to @p target, the same sessions alive throughout: none is spawned or closed.
    ///
    /// In-process the move is made here (applyNodeMove): an emptied window closes, the window it landed in is
    /// raised and focused, and its screen reader is told. A new window is spawned on @p screen at @p at and
    /// takes the node as it opens (consumePendingTransplant). Daemon-hosted, the daemon is asked
    /// (SessionFactory::requestRemoteMove) and nothing changes here until its layout push; the push that
    /// ends with the move does the rest (@see stageRemoteMoveWindow, completeRemoteMoves). A daemon older
    /// than MoveNode is asked with the tab verbs it served, where the move table has a route for it.
    /// @param drag   What moves, resolved (resolveNodeDrag()).
    /// @param target Where it lands, in this process's ids.
    /// @param screen The screen a new window opens on; may be null.
    /// @param at     Where a new window opens, in global coordinates.
    /// @return Nothing, or why nothing moved: the place's refusal for a drag the place does not take (another
    ///         host's), the host's answer where it would not take the move, UnknownSource for a drag naming
    ///         nothing this host has (closed since), UnknownTarget for a target that is gone, and the model's
    ///         own refusals.
    [[nodiscard]] std::expected<void, MoveFailure> moveNode(ResolvedDrag const& drag,
                                                            vtworkspace::MoveTarget const& target,
                                                            QScreen* screen,
                                                            QPoint at);
    /// moveNode() of @p drag as the model has it now.
    [[nodiscard]] std::expected<void, MoveFailure> moveNode(window::NodeDrag const& drag,
                                                            vtworkspace::MoveTarget const& target,
                                                            QScreen* screen,
                                                            QPoint at)
    {
        return moveNode(resolveNodeDrag(drag), target, screen, at);
    }

    /// Starts the one node drag of this process: from here until endNodeDrag(), its owner's delegate runs
    /// the OS drag, and the layout must not change under it, so afterNodeDrag() holds moves back. A drag
    /// still running is ended first, in its own window (endNodeDrag()).
    /// @param owner   The window controller that owns the drag.
    /// @param drag    What it drags.
    /// @param carried The pane whose header carries it, which shows its placeholder; null for a tab.
    /// @return The JSON the drag carries.
    [[nodiscard]] QString beginNodeDrag(QObject const& owner,
                                        window::NodeDrag drag,
                                        TerminalSession* carried);
    /// Ends the drag @p owner owns, if it owns one: the moves held back are made, from the event loop, once
    /// the delegate that carried the drag has returned from it. Calling it for any other owner, or twice,
    /// does nothing.
    /// @param owner The window controller that owns the drag.
    /// @return The session that ended, for its owner to read how it ended (NodeDragSession::probe());
    ///         destroying it forgets the drag. Null when @p owner owned none.
    [[nodiscard]] std::unique_ptr<NodeDragSession> endNodeDrag(QObject const* owner);
    /// Makes @p move now, or, while an OS drag of a node is in flight, once it has ended. A drop is handled
    /// inside the drag's event loop, and a move that rebuilt the layout there would destroy the delegate the
    /// loop is running on.
    /// @param move What to do; it must not name anything it does not own, as the window it came from may be
    ///             gone by then.
    void afterNodeDrag(std::function<void()> move);
    /// @return The pane whose header carries the drag @p owner owns; null for none, or a tab's drag.
    [[nodiscard]] TerminalSession* carriedPane(QObject const* owner) const noexcept;

    /// How long a move asked of the daemon waits for the push that makes it before it is given up: its
    /// window opens wherever windows open, and nothing is announced or focused for it.
    static constexpr auto RemoteMoveTimeout = std::chrono::seconds { 5 };

    /// A move asked of the daemon, waited for until the push that makes it ends (@see moveNode).
    struct PendingRemoteMove
    {
        uint64_t request = 0;                   ///< Its number (RemoteMove::request).
        window::HostIdentity host;              ///< The daemon asked, which names @ref sessions.
        WindowBinding const* binding = nullptr; ///< The link it was asked through; null for none.
        std::vector<uint64_t> sessions;         ///< What moves, by the daemon's ids.
        window::DropPayload payload;            ///< What the receiving window's screen reader is told moved.
        window::MoveLanding landing;            ///< Where it lands, as the screen reader is told it.
        std::optional<window::WindowPosition> spawnAt; ///< NewWindow only: where its window opens.
        core::platform::SteadyTimePoint deadline;      ///< When it is given up (RemoteMoveTimeout).
        /// Whether its push has ended, so the reconcile now running places it.
        enum class Phase : uint8_t
        {
            Asked = 0, ///< Sent; the push that makes it has not ended.
            Landing,   ///< Its push ended: the running reconcile places it.
        } phase = Phase::Asked;
    };

    /// @return The move asked of the daemon that is waited for: asked, neither refused nor past
    ///         RemoteMoveTimeout, and not yet placed; null for none.
    [[nodiscard]] PendingRemoteMove const* livePendingRemoteMove() const noexcept;

    /// The daemon refused move @p request, and changed nothing: nothing waits for it any more.
    /// @param request The request's number (SessionFactory::requestRemoteMove), or 0 for one not matched.
    void remoteMoveRefused(uint64_t request);

    /// The pushes of the moves numbered @p landed have ended (NativeController::takeLandedMoves): the move
    /// this manager waits for, if among them, lands with the reconcile now running.
    void noteRemoteMovesLanded(std::span<uint64_t const> landed);

    /// The reconcile is about to open an OS window for a daemon workspace holding @p daemonSessions: when
    /// that is the window a landing move asked for, stages where it opens.
    /// @return The screen it opens on; null for wherever windows open.
    [[nodiscard]] QScreen* stageRemoteMoveWindow(std::span<uint64_t const> daemonSessions);

    /// How much of the daemon's workspaces a reconcile placed (@see completeRemoteMoves).
    enum class ReconcileReach : uint8_t
    {
        Partial =
            0, ///< A window it was to open is still to come: it skipped one, whose OS window is spawning.
        Whole, ///< Every window it was shown is in place.
    };

    /// The reconcile placed what a landing move moved: its window is raised and focused, the moved pane takes
    /// focus, and the window's screen reader is told. Nothing waits for the move after. A reconcile that
    /// reached only part of the windows leaves the move waiting, until its deadline, for the one that places
    /// the window it landed in: dropped, that window would open without its position or announcement.
    /// @param reach How much of the daemon's workspaces the reconcile placed.
    void completeRemoteMoves(ReconcileReach reach);

    /// Holds back the closing of a window a move empties (applyNodeMove()) while it lives. A reconcile of one
    /// push takes one for all of its windows' passes: a pass may take every tab out of a window that a later
    /// pass fills again, and closing it in between would drop the daemon workspace it shows. Holds nest; the
    /// last one to go closes each window a move emptied that is still empty and not kept.
    class EmptiedWindowHold
    {
      public:
        /// @param manager The manager whose emptied windows stay open meanwhile.
        explicit EmptiedWindowHold(TerminalSessionManager& manager) noexcept;
        ~EmptiedWindowHold();
        EmptiedWindowHold(EmptiedWindowHold const&) = delete;
        EmptiedWindowHold& operator=(EmptiedWindowHold const&) = delete;
        EmptiedWindowHold(EmptiedWindowHold&&) = delete;
        EmptiedWindowHold& operator=(EmptiedWindowHold&&) = delete;

        /// Keeps @p window open once the last hold goes, though a move emptied it: it shows a daemon
        /// workspace that still holds tabs, which the next reconcile realizes into it.
        /// @param window The window.
        void keep(vtworkspace::WorkspaceId window) const;

      private:
        TerminalSessionManager& _manager;
    };

    /// @return A hold on the closing of the windows moves empty (@see EmptiedWindowHold).
    [[nodiscard]] EmptiedWindowHold holdEmptiedWindows() noexcept { return EmptiedWindowHold { *this }; }

    /// Drops what was staged for the next window, whether or not it took it: ContourGuiApp::newWindow() calls
    /// it once that window's load returned, so a load that made no window leaves nothing for the next one.
    void endWindowSpawn() noexcept;
    /// Whether a tab of window @p from can be merged into a split of window @p to: where the sessions are
    /// hosted locally, or on a daemon that serves the merge (a remote end that does not would undo a local
    /// one). A daemon's layout is pushed per workspace and a client reconciles one at a time, so a merge
    /// there stays within one window. Never between two windows bound to different hosts (@see bindWindow).
    /// @param from The window holding the dragged tab.
    /// @param to   The window holding the pane to split.
    [[nodiscard]] TabMergeSupport tabMergeSupport(vtworkspace::WorkspaceId from,
                                                  vtworkspace::WorkspaceId to) const noexcept
    {
        if (bindingOf(from) != bindingOf(to))
            return TabMergeSupport::Unsupported;
        auto const& factory = factoryFor(from);
        if (factory.hosting() == SessionHosting::Remote && from != to)
            return TabMergeSupport::Unsupported;
        return factory.tabMergeSupport();
    }
    /// Tears the tab at row @p fromIndex of window @p from into a brand-new OS window. The new window
    /// is spawned (on @p targetScreen if given) and adopts the tab as its sole tab; if that empties the
    /// source window, it is closed. Drives dropping a dragged tab onto empty desktop.
    /// @param from         The source window's id.
    /// @param fromIndex    The tab's row in @p from.
    /// @param targetScreen The screen the new window should open on (may be nullptr).
    void tearOffTabToNewWindow(vtworkspace::WorkspaceId from, int fromIndex, QScreen* targetScreen);
    void closeTabAtIndex(vtworkspace::WorkspaceId window, int index);
    void closeOtherTabs(vtworkspace::WorkspaceId window, int index);
    void closeTabsToRight(vtworkspace::WorkspaceId window, int index);

    /// The predefined tab-color palette (a grid of swatches) the user picks from, as QColors.
    [[nodiscard]] QVariantList tabColorPalette() const;
    // }}}

    // {{{ Split-pane operations (drive the vtworkspace model; Phase 2)
    /// Splits the acting session's tab's active pane along @p vertical (true: side-by-side;
    /// false: stacked), creating a backing TerminalSession for the new leaf.
    /// @param vertical Split orientation.
    /// @param acting The session that received the keybinding; its hosting tab is the target.
    ///        Null or unknown to the model is a no-op.
    /// @param ratio The new split's first-child space share, in (0, 1). Defaults to an even split; the
    ///        tmux mirror passes the remote split's ratio so a mirrored split reproduces its proportions.
    void splitActivePane(bool vertical, TerminalSession* acting, double ratio = 0.5);
    /// Opens a pane running @p command next to @p acting: a split in its tab, or a tab in its window
    /// (OpenCommandOutput's pager). The pane is a session like any other; it ends when the command does.
    /// @param acting The session the action came from.
    /// @param placement Split or tab.
    /// @param command The program and arguments to run instead of a shell.
    /// @return The new pane's session, never null; or why there is none.
    [[nodiscard]] std::expected<TerminalSession*, CommandPaneError> openCommandPane(
        TerminalSession* acting, CommandPanePlacement placement, vtpty::Process::ExecInfo const& command);
    /// Why openCommandPane() would open no pane right now, asked before anything is prepared for one.
    /// It is the rule openCommandPane() itself applies first.
    /// @param acting The session the action came from.
    /// @param placement Split or tab.
    /// @return The reason; std::nullopt when a pane can be opened.
    [[nodiscard]] std::optional<CommandPaneError> commandPaneRefusal(TerminalSession* acting,
                                                                     CommandPanePlacement placement) const;
    /// Closes the active pane of the acting session's tab (closing the tab if it was the last pane).
    /// @param acting The session that received the keybinding; its hosting tab is the target.
    /// @param end Whether the user is ending this pane's session (the ClosePane action) or the pane
    ///        is only being dropped locally because it is already gone upstream (the attach
    ///        reconciler's subtractive pass). Mandatory: the two are indistinguishable afterwards.
    void closeActivePane(TerminalSession* acting, SessionEnd end);
    /// Moves pane focus within the acting session's tab in the given direction.
    /// @param direction The direction to move pane focus.
    /// @param acting The session that received the keybinding; its hosting tab is the target.
    void focusPane(vtworkspace::FocusDirection direction, TerminalSession* acting);
    /// Swaps the acting session's active pane with its neighbor in @p direction (see
    /// vtworkspace::SessionModel::swapActivePane).
    /// @param direction The direction of the neighbor to swap with.
    /// @param acting The session that received the keybinding; its hosting tab is the target.
    void swapPane(vtworkspace::FocusDirection direction, TerminalSession* acting);
    /// Moves (re-parents) the acting session's active pane across its neighbor in @p direction (see
    /// vtworkspace::SessionModel::moveActivePane).
    /// @param direction The direction to move the active pane.
    /// @param acting The session that received the keybinding; its hosting tab is the target.
    void movePane(vtworkspace::FocusDirection direction, TerminalSession* acting);
    /// Flips the orientation of the acting session's active pane's split.
    /// @param acting The session that received the keybinding; its hosting tab is the target.
    void toggleActivePaneOrientation(TerminalSession* acting);
    /// Grows or shrinks the acting session's active pane in @p direction by @p fraction.
    /// @param direction The side the active pane grows toward.
    /// @param fraction The ratio delta magnitude in (0, 1).
    /// @param acting The session that received the keybinding; its hosting tab is the target.
    void resizeActivePane(vtworkspace::FocusDirection direction, double fraction, TerminalSession* acting);
    /// Toggles zoom on the acting session's active pane (see
    /// vtworkspace::SessionModel::toggleActivePaneZoom).
    /// @param acting The session that received the keybinding; its hosting tab is the target.
    void toggleActivePaneZoom(TerminalSession* acting);
    /// Starts an ended session again in the pane it leaves: a new session under the same profile,
    /// launched command and page size, in the ended one's spawn directory, takes the leaf's place (its
    /// PaneId, the active pane and a zoom unchanged), takes the focus if the ended one had it, and the
    /// ended session is retired (freed later, since the call that asked is still running on it).
    /// @param ended The session whose pane is restarted; its endAction() must be Restart.
    /// @return The new session; or why there is none, in which case nothing changed.
    [[nodiscard]] std::expected<TerminalSession*, RestartError> restartSession(TerminalSession& ended);

    /// @param session The session whose ended pane's footer asks.
    /// @return Whether that footer can offer Reconnect: the factory of @p session's window is attached to a
    ///         daemon.
    [[nodiscard]] ReconnectSupport reconnectSupport(TerminalSession const& session) const noexcept
    {
        return factoryOf(session).reconnectSupport();
    }

    /// The footer's Reconnect: seeks the daemon again, and every pane lost with the connection shows it is
    /// reconnecting meanwhile. A refusal to even start puts "Could not reconnect: {reason}." back in their
    /// footers. Once the daemon's layout is back, rebindDisconnected() takes the panes' sessions back.
    /// Only the panes of windows bound to the same link as @p acting's are touched; a window no link claims
    /// has no connection to seek.
    /// @param acting The session whose footer asked: the connection sought is its window's.
    void reconnectHost(TerminalSession const& acting);

    /// @p binding's daemon is back and has pushed its layout: every Disconnected pane of a window bound to it
    /// whose remote session that layout still names mirrors it again, in place (its PaneId, the active pane
    /// and a zoom unchanged); one it does not name keeps its footer. Called for every layout the daemon
    /// pushes (one per workspace): a Reconnect that is running ends once no Disconnected pane of the
    /// binding's is left, or in reconnectFailed().
    /// @param binding The binding whose daemon pushed the layout; its factory takes the panes back.
    void rebindDisconnected(WindowBinding& binding);

    /// The reconnect @p binding was running ended for good without bringing the daemon back: its
    /// Disconnected panes say so in their footers, as "Could not reconnect: {reason}.". Nothing when none was
    /// running.
    /// @param binding The binding whose reconnect ended.
    /// @param reason  Why it ended, as the connection says it.
    void reconnectFailed(WindowBinding const& binding, std::string_view reason);

    /// @p binding's connection to its daemon ended for good: a Reconnect that was running is over (@see
    /// reconnectFailed), and then every live pane of its windows is told it lost the connection. In that
    /// order, so the "Could not reconnect" footer reaches only the panes that were already Disconnected,
    /// never those that this loss has just made so. Call before the controller's stop() closes the mirror
    /// ptys.
    /// @param binding The binding whose connection ended.
    /// @param reason  Why the connection ended, as it says it.
    void connectionClosed(WindowBinding const& binding, std::string_view reason);
    // }}}

    // {{{ Model service used by PaneProxy + WindowController
    /// The TerminalSession backing @p id, or nullptr. (Public for PaneProxy.)
    [[nodiscard]] TerminalSession* sessionForId(vtworkspace::SessionId id) const noexcept;

    /// @param pty A pty, as a session factory handed it out.
    /// @return The live session @p pty backs; null if none does.
    [[nodiscard]] TerminalSession* sessionForPty(vtpty::Pty const* pty) const noexcept;

    /// The session of @p window's active pane -- the one the user looks at -- else any live session of the
    /// window.
    /// @param window The window.
    /// @return The session; nullptr when the window has no live pane.
    [[nodiscard]] TerminalSession* actingSessionOfWindow(vtworkspace::WorkspaceId window) const;

    /// The pty backing @p window's active pane, or nullptr when the window has no live pane.
    ///
    /// How a WINDOW is named to a remote session factory: window ids are minted per model, so the
    /// only handle both ends share is a session the window hosts, which the factory keys by its
    /// pty (@see SessionFactory::requestRemoteTab, requestRemoteSplit).
    /// @param window The window to name.
    /// @return A pty of that window, or nullptr.
    [[nodiscard]] vtpty::Pty const* actingPtyOfWindow(vtworkspace::WorkspaceId window) const;

    /// Asks the session factory where an operation on the engine of the session backed by @p pty runs,
    /// sending it to the remote end when that is where (@see SessionFactory::requestHostedOperation).
    /// @param pty The pty backing the acting session.
    /// @param request The operation, with its text.
    /// @return Where it runs.
    [[nodiscard]] HostedOperationRoute requestHostedOperation(vtpty::Pty const* pty,
                                                              HostedOperationRequest const& request)
    {
        auto const* const session = sessionForPty(pty);
        return (session != nullptr ? factoryOf(*session) : _localFactory)
            .requestHostedOperation(pty, request);
    }

    /// Whether @p id is the active leaf of tab @p tab.
    [[nodiscard]] bool isActivePane(vtworkspace::TabId tab, vtworkspace::PaneId id) const noexcept;
    /// Sets the split ratio of node @p id in tab @p tab.
    void setPaneRatio(vtworkspace::TabId tab, vtworkspace::PaneId id, double ratio);
    /// Makes leaf @p id the active pane of tab @p tab.
    void activatePane(vtworkspace::TabId tab, vtworkspace::PaneId id);

    /// Records how tab @p tab's active pane was last reached and, when that changes, tells the tab's proxies,
    /// whose focus reads it. PaneProxy::activate() records Pointer here, since a click on the pane already
    /// active moves nothing; every active-pane change records itself (activePaneChanged()): Keyboard for a
    /// FocusPane* move, Pointer for anything else.
    /// @param tab The tab.
    /// @param activation How.
    void notePaneActivation(vtworkspace::TabId tab, window::PaneActivation activation);

    /// @param tab The tab.
    /// @return How its active pane was last reached; Pointer for a tab never told.
    [[nodiscard]] window::PaneActivation lastPaneActivation(vtworkspace::TabId tab) const noexcept;

    /// Runs a pane header button's action on leaf @p leaf of tab @p tab: makes the leaf the active pane, as
    /// a click on it would (PaneActivation::Pointer), then runs the button's catalog action on its session.
    /// Nothing happens for a tab or leaf that no longer exists, or a leaf without a session.
    /// @param tab    The tab hosting the leaf.
    /// @param leaf   The leaf whose header the button is on.
    /// @param action The button.
    void runPaneHeaderAction(vtworkspace::TabId tab,
                             vtworkspace::PaneId leaf,
                             window::PaneHeaderAction action);
    // }}}

    void updateColorPreference(vtbackend::ColorPreference const& preference);

    /// Applies the OS motion preference to every session's terminal.
    /// @param motion The OS motion preference.
    void updateMotionPreference(config::MotionPreference motion);

    /// Hands the system's scroller style to every session, so each overlay scrollbar follows it live.
    /// GUI thread.
    /// @param system The system's scroller style, or nullopt where the platform publishes none.
    void updateSystemScrollBars(std::optional<config::ScrollBarMode> system);

    /// What every window derives from the chrome's configuration, refreshed after it changed (a reload, an
    /// OS appearance or accessibility change): which pane headers show, and so which sessions follow the
    /// program in front of them; and the title band's material and geometry. The one fan-out
    /// ContourGuiApp::refreshChromeStyle() calls. GUI thread.
    void refreshChromeDependents();

    void focusOnDisplay(display::TerminalDisplay* display);

    /// Single authority for terminal (VT) focus: sends a focus-OUT to the previously focused session
    /// and a focus-IN to @p next (nullptr = nothing focused), so exactly one session is ever focused.
    /// Every focus source (Qt focus events, model tab/pane changes, cross-window moves) routes here.
    /// With `theme: terminal`, focusing a session whose live background differs from the previous one's
    /// (or focusing one after none) re-evaluates the chrome's light/dark; focusing nothing changes nothing.
    /// @param next The session that should now hold terminal focus, or nullptr for none.
    void setFocusedSession(TerminalSession* next);

    /// A session's default background changed at runtime (OSC 11, a reset, a palette re-seed). When that
    /// session holds terminal focus, the chrome theme is re-applied over @p color; otherwise nothing
    /// happens, and the chrome catches up when the session is focused. MUST be called on the GUI thread.
    /// @param id The session (by model id) whose background changed.
    /// @param color Its new default background.
    void sessionBackgroundChanged(vtworkspace::SessionId id, vtbackend::RGBColor color);

    /// Clears terminal focus iff @p session currently holds it (a display losing Qt focus). A no-op
    /// when @p session is not the focused one, so an outgoing pane's focus-out cannot cancel the
    /// focus-in the model already delivered to the incoming pane.
    /// @param session The session whose display just lost Qt focus.
    void clearFocusIfCurrent(TerminalSession* session);

    /// The OS window that currently owns keyboard focus, as a model identity rather than a Qt pointer.
    /// VT focus only ever moves within this window, so a model event raised in a background window (a
    /// layout applying its tabs, a tab dropped in by a drag) cannot steal the focused terminal.
    /// @return The focus-owning window, or std::nullopt when no window of this process owns focus.
    [[nodiscard]] std::optional<vtworkspace::WorkspaceId> focusedWindow() const noexcept
    {
        return _focusedWindow;
    }

    /// Records @p window as the focus-owning OS window AND re-points terminal focus at its active leaf.
    /// Idempotent. Ownership is also recorded — without re-pointing, since there is no session to point
    /// at yet or the caller supplies its own — when a window is spawned (createWindowController) and
    /// when one of its displays takes Qt focus (focusOnDisplay). @see clearFocusedWindow.
    /// @param window The window that just became the active OS window.
    void setFocusedWindow(vtworkspace::WorkspaceId window);

    /// Revokes focus ownership iff @p window currently holds it, leaving no window focused and no
    /// terminal focused. A no-op for any other window, so a background window deactivating cannot
    /// clear the focused window's terminal focus.
    /// @param window The window that just stopped being the active OS window (or is being destroyed).
    void clearFocusedWindow(vtworkspace::WorkspaceId window);

    /// The window an application-wide command runs in: the focused one; else, with none focused (every
    /// window minimised, or another application in front), the one that held focus last while it lives;
    /// else the oldest window left. A panel (WindowLevel::Panel, the quick terminal) is none of these: it
    /// may be hidden, and a command run there would be run out of sight.
    /// @return That window's controller, or nullptr when no ordinary window is left.
    [[nodiscard]] window::WindowController* applicationController() const noexcept;

    /// @param window A window of this manager.
    /// @return Whether it is a panel (WindowLevel::Panel, the quick terminal), which is no part of an
    ///         attached host's workspace; false for an ordinary window and for one that is not known.
    [[nodiscard]] bool isPanelWindow(vtworkspace::WorkspaceId window) const noexcept;

    /// @param window A window of this manager.
    /// @return Whether it is an ordinary window (WindowLevel::Normal), as opposed to a panel; false for a
    ///         window that is not known.
    [[nodiscard]] bool isNormalWindow(vtworkspace::WorkspaceId window) const noexcept;

    /// @p window changed how it is presented -- minimised, restored, maximised. Restoring the
    /// focus-owning window brings its focused pane back in front of the user with no focus change at
    /// all, which is news to that pane all the same. @see TerminalSession::paneSeen
    /// @param window The window whose state changed.
    void windowStateChanged(vtworkspace::WorkspaceId window);

    /// @p window's surface was exposed. A restore, an un-occlusion or a desktop switch is announced by the
    /// platform BEFORE the surface is exposed, so windowStateChanged() still finds it hidden; this is the
    /// re-decision of what the displays' frames are for, once it is not.
    /// @param window The window whose surface was exposed.
    void windowExposed(vtworkspace::WorkspaceId window) const;

    /// Where @p session sits relative to what the user is looking at, for finish notifications.
    ///
    /// Gathers the four facts -- terminal focus, the hosting tab's selection, a zoomed sibling, the
    /// window's minimised state -- and leaves the rule to platform::sessionVisibilityFor(), where it is
    /// table-tested. GUI thread.
    /// @param session The session to place; one no tab hosts is Hidden unless it holds focus.
    /// @return Its visibility.
    [[nodiscard]] platform::SessionVisibility sessionVisibility(TerminalSession const& session) const;

    /// @param session A session of this manager.
    /// @return Whether @p session's bell sounds and alerts, and its desktop and finish notifications are
    ///         shown, rather than only marking its tab: always in-process; for a daemon's session only while
    ///         its window holds the daemon's raise role (WindowBinding::raiseRole), so that a bell rings
    ///         once across the windows on that daemon. GUI thread.
    [[nodiscard]] bool raises(TerminalSession const& session) const;

    /// The label the tab strip shows for the tab hosting @p session -- read back through the strip's
    /// own TitleRole, so a notification names the tab exactly as the user sees it.
    /// @param session The session whose tab to name.
    /// @return The label, or empty when no tab hosts @p session.
    [[nodiscard]] std::string tabTitleForSession(TerminalSession const& session) const;

    /// Brings @p session in front of the user: makes its pane and then its tab active, leaving the
    /// settings page if it covers the window, then restores, raises and activates the window. What
    /// clicking a finish notification does. A no-op for a session no tab hosts.
    /// @param session The session to reveal.
    void revealSession(TerminalSession const& session);

    /// Shows a tray message through the window hosting @p session (platforms without a desktop
    /// Notifier). Routed through the window because a session in a background tab has no SessionChrome
    /// of its own. A no-op for a session no window hosts.
    /// @param session   The session the message is about.
    /// @param title     The message title.
    /// @param body      The message body.
    /// @param onClicked What a click on the message does, besides bringing its window forward.
    void requestWindowNotification(TerminalSession const& session,
                                   QString const& title,
                                   QString const& body,
                                   std::function<void()> onClicked);

    /// Plays a bell sound in the window hosting @p session, routed as requestWindowNotification is.
    /// @param session The session ringing.
    /// @param source  The sound's URL (TerminalSession::getBellSource()); empty plays nothing.
    /// @param volume  0.0 (silent) .. 1.0 (loudest).
    void requestWindowBell(TerminalSession const& session, QString const& source, double volume);

    /// Flashes the taskbar entry of the window hosting @p session, routed as requestWindowNotification is.
    /// @param session The session asking for attention.
    void requestWindowAlert(TerminalSession const& session);

    void update() { updateStatusLine(); }

    /// Invalidates TitleRole on every tab row of EVERY window's tab strip (each WindowController
    /// re-emits for its own rows). Needed when a change can shift more than one label at once: a
    /// structural reorder/close/insert renumbers the {TabPosition} of later tabs, and a config
    /// reload may change the tab-label template.
    void refreshAllTabTitles();

    /// Invalidates TitleRole for the tab hosting @p session ON ITS OWNING WINDOW's tab strip, so
    /// the strip recomputes that tab's label after a runtime window-title / tab-name change. The
    /// label is derived from the tab's active leaf, so this is correct even when @p session is a
    /// background pane (the active-leaf label is recomputed; if @p session is not the active leaf
    /// the label simply stays the same). No-op if no tab hosts @p session. MUST be called on the
    /// GUI thread (it emits dataChanged on the owning controller).
    /// @param session The session whose hosting tab should refresh its label.
    void refreshTabForSession(vtworkspace::SessionId session);

    /// Refreshes only the progress roles of the tab hosting @p session, so the strip repaints its
    /// progress indicator without re-expanding the tab label. No-op if no tab hosts @p session.
    /// MUST be called on the GUI thread (it emits dataChanged on the owning controller).
    /// @param session The session whose hosting tab should refresh its progress indicator.
    void refreshTabProgressForSession(vtworkspace::SessionId session);

    /// The program-status path (OSC 7501): recomputes @p session's own pane status
    /// (TerminalSession::setPaneProgramStatus) and repaints the badge of the tab hosting it, which pools the
    /// records of all its panes. Marks no output. No-op if no tab hosts @p session. GUI thread.
    /// @param session The session whose records changed.
    void refreshTabBadgeForSession(vtworkspace::SessionId session);

    /// What @p session's terminal does with typed text changed (TerminalSession::refreshInputEcho): repaints
    /// the lock on the tab hosting it, and re-decides the OS's secure input. GUI thread.
    /// @param session The session whose echo changed.
    void noteInputEchoChanged(TerminalSession const& session);

    /// Moves the OS's secure input to what window::desiredSecureInput says for the configured mode, whether
    /// one of our windows has focus, and the focused session's echo. Called at every change of the three:
    /// focus (session or window), an echo change, a session's removal, a reload. GUI thread.
    void refreshSecureInput();

    /// The configured `secure_input` changed (a reload): every pane's lock and every tab's are re-announced,
    /// and the OS's secure input is re-decided, so turning the setting off releases it at once. GUI thread.
    void secureInputModeChanged();

    /// @param session A session.
    /// @return Whether @p session is the one that has the keys. GUI thread.
    [[nodiscard]] bool isFocusedSession(TerminalSession const& session) const noexcept
    {
        return _focusedSession == &session;
    }

    /// @return The OS's secure input as the gate last set it.
    [[nodiscard]] platform::SecureInputState secureInputState() const noexcept
    {
        return _secureInputGate.state();
    }

    /// The output path: @p session wrote something. While it is off screen, and the output is live, its
    /// tab's badge shows the unseen-output dot; the badge is repainted only when that changed. GUI thread.
    /// @param session The session written to.
    /// @param origin  Where the output came from; a mirror's resync is not news.
    void noteSessionOutput(vtworkspace::SessionId session, window::OutputOrigin origin);

    /// The bell rang in @p session: while it is off screen, and the bell is live, its tab's badge shows the
    /// bell until the tab is seen. The alert (Dock bounce, taskbar flash) is the session's own business and
    /// is not decided here. GUI thread.
    /// @param session The session that rang.
    /// @param origin  Where the BEL came from; a replay of output the user already had is not news.
    void noteBell(vtworkspace::SessionId session, window::OutputOrigin origin);

    /// A command in @p session finished: while it is off screen, its tab's badge shows how it ended until
    /// the tab is seen. GUI thread.
    /// @param session The session it ran in.
    /// @param ending  How it ended.
    void noteCommandFinished(TerminalSession const& session, vtbackend::CommandEnding ending);

    /// Every session of @p window that is on screen now has been seen: its marks clear. Called whenever what
    /// is on screen may have grown -- the active tab changed, the window was restored, a zoom ended.
    /// @param window The window whose sessions to look at.
    void markVisibleSessionsSeen(vtworkspace::WorkspaceId window);

    /// Counts @p session, a pane of @p window, as seen: the window's ledger clears its marks and takes the
    /// session's newest program-status generation as seen.
    /// @param window  The window displaying the session's workspace.
    /// @param session The pane's session.
    void markPaneSeen(vtworkspace::WorkspaceId window, vtworkspace::SessionId session);

    /// Tells every session of @p window that its display's exposure may have changed (a window-state change).
    /// @param window The window.
    void notifyDisplaysExposureChanged(vtworkspace::WorkspaceId window) const;

    /// @param session The session asked about.
    /// @return What it has to tell, in the ledger of the window that owns it; nothing when no window does.
    [[nodiscard]] window::SessionAttention attentionOf(vtworkspace::SessionId session) const;

    /// What the OS taskbar should show right now, aggregated across every reporting session.
    ///
    /// The staleness policy is read from the config here, at the point of use, rather than pushed
    /// into the router: the config file has not been read when this manager is constructed, and this
    /// way a reload needs no second wiring step. @see platform::TaskbarProgressRouter::resolve.
    /// @param now The current time; defaulted so ordinary callers need not supply one, and stated
    ///        outright by a test rather than slept for.
    /// @return The aggregate; ProgressState::Inactive means "show nothing".
    [[nodiscard]] platform::TaskbarProgress taskbarProgress(
        std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now()) const;

    /// Resolves progress only from panes and tabs belonging to the requested OS window.
    /// @param window The OS window; every workspace it owns counts.
    /// @param now Current monotonic time.
    /// @return Window-scoped aggregate, including background tabs and panes.
    [[nodiscard]] platform::TaskbarProgress taskbarProgress(window::WindowId window,
                                                            std::chrono::steady_clock::time_point now) const;

    /// Attaches the native taskbar before the OS window is first shown.
    /// @param window The OS window.
    /// @param nativeWindow The Qt window backing it.
    void bindTaskbarWindow(window::WindowId window, QWindow& nativeWindow);

    /// Records @p progress for @p session, for the taskbar aggregate above.
    ///
    /// @param session The reporting session.
    /// @param progress What it reported.
    /// @param now When it reported it; defaulted, and stated outright by a test.
    void recordProgress(vtworkspace::SessionId session,
                        vtbackend::Progress progress,
                        std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now())
    {
        _taskbarProgress.onProgress(session.value, progress, now);
        scheduleTaskbarProgress();
    }

    /// Sets an explicit @p title on the tab hosting @p session, through the authoritative SessionModel
    /// (which repaints the owning window's tab strip). Used by the tmux mirror to reflect a
    /// `%window-renamed` onto the tab (a tmux window maps to a tab). No-op if no tab hosts @p session.
    /// MUST be called on the GUI thread (it mutates the GUI-facing model).
    /// @param session The session (by model id) whose hosting tab to title.
    /// @param title The new tab title.
    void setTabTitleForSession(vtworkspace::SessionId session, std::string title);

    /// Assigns @p color to the tab hosting @p session (DECAC item 2 "window frame"), routing through
    /// the authoritative SessionModel so the existing tab-color pipeline repaints the tab strip. The
    /// color is recorded under vtworkspace::TabColorSource::Application, so it stays hidden behind a color
    /// the user picked themselves and surfaces once the user clears theirs. No-op if no tab hosts
    /// @p session. MUST be called on the GUI thread (it mutates the GUI-facing model).
    /// @param session The session (by model id) whose hosting tab should be colored.
    /// @param color The color to assign.
    void setTabColorForSession(vtworkspace::SessionId session, vtbackend::RGBColor color);

    /// Resets the tab hosting @p session back to no application-assigned color (DECAC item 2 with no
    /// colors, or a hard reset). A color the user picked is left alone. No-op if no tab hosts
    /// @p session. MUST be called on the GUI thread.
    /// @param session The session (by model id) whose hosting tab color should be cleared.
    void resetTabColorForSession(vtworkspace::SessionId session);

    /// Clears the destroyed @p display from any controller that held it as its focused display. Called
    /// when the display's QML item / pane is torn down. Session->display ownership lives on the pane
    /// tree, so there is no per-display session map to scrub. Focus ownership is unaffected: it is held
    /// per WINDOW (@see _focusedWindow), so a tab switch still notifies its sessions across this gap.
    /// @param display The display being destroyed.
    void detachDisplay(display::TerminalDisplay* display) noexcept;

    // {{{ Per-OS-window controllers (Stage 2 of the per-window refactor)
    /// Mints a fresh vtworkspace::Workspace, creates its per-OS-window WindowController and registers it. The
    /// controller is the QML-facing adapter for one ApplicationWindow: it owns that window's tab-strip
    /// list-model, PaneProxy tree and window services, and delegates structural / session-lifetime
    /// operations back to this manager tagged with its WorkspaceId. Owned via QQmlEngine CppOwnership
    /// (like sessions and proxies). Main.qml calls this from Component.onCompleted before creating its
    /// first tab.
    Q_INVOKABLE contour::window::WindowController* createWindowController();

    /// createWindowController(), its menus presented as @p menuPresentation says rather than as the app's
    /// chrome style does: the seam a test opens native menus through, on a style that has none.
    /// @param menuPresentation Whether the window's menus open natively or in the scene.
    /// @return The new controller.
    window::WindowController* createWindowController(config::MenuPresentation menuPresentation);

    /// createWindowController(), its notices said through what @p noticeAnnouncer builds: the seam a test
    /// hears a window's announcements through.
    /// @param menuPresentation Whether the window's menus open natively or in the scene.
    /// @param noticeAnnouncer Builds what the window's notices are said through.
    /// @return The new controller.
    window::WindowController* createWindowController(config::MenuPresentation menuPresentation,
                                                     window::NoticeAnnouncerFactory const& noticeAnnouncer);

    /// Stages what the NEXT createWindowController() window is for, which that call consumes: the quick
    /// terminal's window is made by the app, not by a user action, and its Main.qml mints its controller
    /// itself. Mirrors the staging of a pending spawn screen.
    /// @param role The role of the next window.
    void stageWindowRole(window::WindowRole role) noexcept { _stagedRole = role; }

    /// If a tab tear-off is pending (staged by tearOffTabToNewWindow before this window was spawned),
    /// transplants that tab into @p newController's window and returns true; the window must NOT then
    /// create its own first tab. Returns false when there is no pending transplant, in which case
    /// Main.qml creates the usual fresh first tab. Consumes the staged request (fires at most once).
    /// @param newController The freshly-created controller of the just-spawned window.
    /// @return true if a torn-off tab was adopted; false if the window should create its own first tab.
    Q_INVOKABLE bool consumePendingTransplant(contour::window::WindowController* newController);

    /// Looks up the app's startup layout (ContourGuiApp::layoutName(), from `--layout` or the config's
    /// `default_layout`) and, if found, applies it to @p controller's window. Called by Main.qml right
    /// after consumePendingTransplant() when no tab was transplanted, so a freshly-spawned window can
    /// open pre-populated instead of falling back to a single blank tab. One-shot: only the FIRST
    /// window of the process consumes the startup layout — every later window (NewTerminalWindow)
    /// gets its usual single default tab instead of re-running all of the layout's commands.
    /// @param controller The freshly-created controller of the just-spawned window.
    /// @return true if a startup layout was found and applied; false if there is none configured, it is
    ///         unknown, it has no tabs, or it was already consumed by an earlier window, in which case
    ///         the window should create its usual first tab.
    Q_INVOKABLE bool consumeDefaultLayout(contour::window::WindowController* controller);

    /// Binds @p window to @p binding: every session the window makes from then on is made through
    /// binding.sessions(), and every request about one of its sessions goes there, instead of to the factory
    /// this manager was constructed with. A window bound already is bound anew: it leaves the windows of what
    /// it was bound to (@see windowsBoundTo), which is told when that was its last one
    /// (WindowBinding::lastWindowReleased). The window's host label and chip are refreshed whenever its
    /// binding changes. GUI thread.
    ///
    /// Post-construction by necessity (cyclic wiring): the binding is built on this manager, and the window
    /// exists only once this manager made it, so each window is told to the manager once it is adopted.
    /// @param window  A window of this manager.
    /// @param binding What serves it. Not owned; it outlives the binding, which releaseBinding() or the
    ///                window's close ends.
    void bindWindow(vtworkspace::WorkspaceId window, WindowBinding& binding);

    /// @param window A window of this manager.
    /// @return What @p window is bound to; null for a window none claims, whose sessions the factory this
    ///         manager was constructed with makes.
    [[nodiscard]] WindowBinding const* bindingOf(vtworkspace::WorkspaceId window) const noexcept;

    /// The one record of which windows a binding serves: what bindWindow() bound to it, and neither a close
    /// nor a later bindWindow() took away since.
    /// @param binding A binding.
    /// @return Its windows, in the order they were bound; none for a binding that serves none. Valid until
    ///         the next bindWindow(), releaseBinding() or window close.
    [[nodiscard]] std::span<vtworkspace::WorkspaceId const> windowsBoundTo(
        WindowBinding const& binding) const noexcept;

    /// Unbinds every window bound to @p binding, which serves none of them any more: what they make from then
    /// on is made by the factory this manager was constructed with, and they show no host. An attach-window
    /// binder @p binding installed goes with them (@see setAttachWindowBinder). GUI thread.
    /// @param binding The binding that stops.
    void releaseBinding(WindowBinding const& binding);

    /// @param window A window of this manager.
    /// @return The remote host @p window's binding is attached to (WindowBinding::hostLabel); empty for a
    ///         window none claims, and so for every local one.
    [[nodiscard]] std::string_view hostLabelOf(vtworkspace::WorkspaceId window) const noexcept;

    /// @param window A window of this manager.
    /// @return What @p window says about the daemon it is on (WindowBinding::chromeLabel): the remote host or
    ///         a non-default local daemon's label; empty for the default daemon and a local window.
    [[nodiscard]] std::string_view chromeLabelOf(vtworkspace::WorkspaceId window) const noexcept;

    /// @param window A window of this manager.
    /// @return The daemon @p window is on, as a switch names it (WindowBinding::daemonTarget); none for a
    ///         window no binding serves, whose sessions run in-process.
    [[nodiscard]] std::optional<SwitchTarget> daemonTargetOf(vtworkspace::WorkspaceId window) const;

    /// @param window A window.
    /// @return Whether the daemon the window is bound to runs on this machine; Local for a window with none.
    [[nodiscard]] window::DaemonLocality daemonLocalityOf(vtworkspace::WorkspaceId window) const;

    /// @param window A window of this manager.
    /// @return What @p window's drift badge shows: its own daemon's drift (WindowBinding::daemonDrift); null
    ///         for a local window.
    [[nodiscard]] window::DaemonDriftModel* daemonDriftOf(vtworkspace::WorkspaceId window) const noexcept;

    /// @param session A session of this manager.
    /// @return The remote host the window holding @p session is attached to; empty for a local window, and
    ///         for a session no window holds.
    [[nodiscard]] std::string_view hostLabelOf(TerminalSession const& session) const noexcept;

    /// New Window from @p acting's window: on a daemon, a window of a connection of its own opens, showing
    /// the daemon's first workspace no window shows, else a new one (SiblingShows::DaemonsChoice), instead
    /// of a local window opening.
    /// @param acting The session the new window is asked from.
    /// @param screen Where the window opens; null for the default placement.
    /// @return Routed when the daemon's binding took the request; Local for a local window, which then opens
    ///         a local one, and for a binding with no daemon.
    [[nodiscard]] RemoteRouting requestRemoteWindow(TerminalSession const& acting, QScreen* screen);

    /// Installs the attach-mode window binder, in place of any installed before. It decides whether a
    /// freshly-spawned OS window should adopt a pending daemon workspace's layout instead of creating the
    /// usual fresh first tab. @see consumeAttachWindow.
    /// @param owner What installs it, and so removes it: by clearAttachWindowBinder(), or as it is
    ///        released (releaseBinding()).
    /// @param binder Invoked with each spawned window's controller; returns true if it adopted it.
    void setAttachWindowBinder(WindowBinding const& owner,
                               std::function<bool(contour::window::WindowController*)> binder)
    {
        _attachWindowBinderOwner = &owner;
        _attachWindowBinder = std::move(binder);
    }

    /// Removes the attach-window binder @p owner installed; leaves one another installed since alone.
    /// @param owner What installed it.
    void clearAttachWindowBinder(WindowBinding const& owner) noexcept
    {
        if (_attachWindowBinderOwner != &owner)
            return;
        _attachWindowBinderOwner = nullptr;
        _attachWindowBinder = nullptr;
    }

    /// @return What installed the attach-window binder in place; null when none is.
    [[nodiscard]] WindowBinding const* attachWindowBinderOwner() const noexcept
    {
        return _attachWindowBinderOwner;
    }

    /// Main.qml calls this in Component.onCompleted (like consumeDefaultLayout): in attach mode a
    /// window may have been spawned to host a specific daemon workspace (B4). If the installed binder
    /// adopts @p controller's window for that daemon workspace, this returns true and the window must NOT
    /// create its own first tab — its tabs come from reconciling the daemon workspace's layout instead.
    /// @param controller The freshly-created controller of the just-spawned window.
    /// @return true if the window was bound to a pending daemon workspace; false otherwise.
    Q_INVOKABLE bool consumeAttachWindow(contour::window::WindowController* controller);

    /// The controller whose window displays @p workspace, or nullptr: none when no window shows it, a hidden
    /// workspace of a window included. Used by the ModelEvents router to forward each Qt row/signal emission
    /// to the controller whose tab strip lists the event's tabs. In daemon mode each window realizes its own
    /// local copy of a daemon workspace, so a local workspace is displayed by at most one window.
    /// @param workspace The workspace.
    /// @return The controller displaying it, or nullptr.
    [[nodiscard]] window::WindowController* controllerDisplaying(
        vtworkspace::WorkspaceId workspace) const noexcept;

    /// The controller whose window owns @p workspace, shown or not, or nullptr.
    /// @param workspace The workspace.
    /// @return The controller owning it, or nullptr.
    [[nodiscard]] window::WindowController* controllerOwning(
        vtworkspace::WorkspaceId workspace) const noexcept;

    /// @param window A window id.
    /// @return Its controller, or nullptr when the window is gone.
    [[nodiscard]] window::WindowController* controllerOf(window::WindowId window) const noexcept;

    /// (In-process) Creates a workspace owned by @p window and leaves it hidden; the window keeps showing
    /// what it showed.
    /// @param window The owning window; must exist.
    /// @return The new workspace's id.
    [[nodiscard]] vtworkspace::WorkspaceId createWorkspaceIn(window::WindowId window);

    /// (In-process) Makes @p window show the @p index-th (0-based) workspace it owns; out of range, or an
    /// unknown window, changes nothing.
    /// @param window The window.
    /// @param index  The position among its owned workspaces.
    void selectWorkspace(window::WindowId window, std::size_t index) const;

    /// Tells the manager that @p controller now shows another workspace than @p previous: the focus follows
    /// the window, and what just came into view counts as seen. Called by WindowController::displayWorkspace.
    /// @param controller The window that switched.
    /// @param previous   The workspace it showed until now.
    void windowDisplayChanged(window::WindowController& controller, vtworkspace::WorkspaceId previous);

    /// Forgets what the manager keeps per workspace for @p workspace, which a window dropped from the model:
    /// its binding to a daemon, and any close it waited for. The window that held it stays. Called by
    /// WindowController::dropWorkspaceIfEmpty.
    /// @param workspace The workspace gone from the model.
    void forgetWorkspace(vtworkspace::WorkspaceId workspace);

    /// The taskbar's count of waiting tabs may have changed outside the windows' own tabs -- a daemon's
    /// directory of the workspaces a window does not display: it is published anew. GUI thread.
    void refreshTaskbarAttention();

    /// The key @p session's seen-state is kept under in a window's ledger: the daemon's id for a session
    /// bound to a daemon session (@see hostSessionIdOf), the local id otherwise, and for a session this
    /// manager does not know.
    /// @param session The session.
    /// @return Its key.
    [[nodiscard]] window::PaneKey paneKeyOf(vtworkspace::SessionId session) const;

    /// The controller owning @p display's OS window, for routing focus. Matches the controller that has
    /// adopted this display's QQuickWindow (ownsOSWindow); falls back to the sole controller before any
    /// window has recorded its QQuickWindow (first focus-in). Null if there are no controllers.
    /// @param display The display that just focused in (may be null).
    /// @return The owning controller, or nullptr.
    [[nodiscard]] window::WindowController* controllerForDisplay(
        display::TerminalDisplay* display) const noexcept;

    /// The same routing as @ref controllerForDisplay, answered as the interface the DISPLAY layer
    /// declares for its window (@c display::WindowHost) rather than as the concrete controller.
    ///
    /// This is what lets a display forward a full-screen or resize request without naming a window type:
    /// it sees only the role, while the manager -- which is the window/display registry, and so knows
    /// both -- performs the lookup. Null when there is no window yet (offscreen tests, pre-show).
    /// @param display The display asking for its window (may be null).
    /// @return The owning window host, or nullptr.
    [[nodiscard]] display::WindowHost* windowHostForDisplay(display::TerminalDisplay* display) const noexcept;

    /// Drops the controller for a closed OS window (WindowController::closeWindow calls this last). "Last
    /// window" is _controllersByWindow.size() == 1; when it is, the manager clears residual session
    /// registries. deleteLater()s the controller.
    /// @param windowId The window whose controller is being removed.
    void removeWindowController(window::WindowId windowId);

    // Narrow SERVICE interface the WindowController reads through (the manager stays the SessionModel host
    // + SessionId<->TerminalSession registry). These expose the shared model/registry without moving
    // session lifetime out of the manager.
    [[nodiscard]] vtworkspace::SessionModel& model() const noexcept { return *_model; }
    /// The backing sessions of every leaf pane in @p tab (public for WindowController::closeTab paths).
    [[nodiscard]] std::vector<TerminalSession*> sessionsOfTab(vtworkspace::Tab* tab) const
    {
        return sessionsInTab(tab);
    }
    /// @param window A window of this manager.
    /// @return The backing sessions of every leaf pane of every tab of @p window, tab by tab; none for a
    ///         window the model does not know.
    [[nodiscard]] std::vector<TerminalSession*> sessionsOfWindow(vtworkspace::WorkspaceId window) const;
    /// Terminates each of @p sessions (public whole-tab close primitive for WindowController).
    /// @param sessions The backing sessions to tear down.
    /// @param end Whether the user ended them or only their local view is going away.
    void terminate(std::span<TerminalSession* const> sessions, SessionEnd end)
    {
        terminateSessions(sessions, end);
    }
    // }}}

    /// Tells every live session of a window bound to @p binding that its daemon connection died, before
    /// their ptys are closed: that one connection feeds them all, so losing it ends them at once. The
    /// sessions of every other window are not touched. @see TerminalSession::noteTransportLost
    /// @param binding The binding whose connection died.
    void noteTransportLost(WindowBinding const& binding);

    /// Tells every session of a window bound to @p binding where the connection to the daemon (or tmux
    /// server) its pane mirrors stands: each whose factory hosts it remotely shows that it is reconnecting
    /// while the connection is sought again; a local one ignores it. @see TerminalSession::setHostConnection
    ///
    /// Also what those windows' host chips show: each of their controllers is told when the chip's state
    /// changes. Every other window keeps its own.
    /// @param binding The binding whose connection it is.
    /// @param state   Where the connection stands now.
    void setHostConnection(WindowBinding const& binding, vthost::client::ConnectionState state);

    /// @param window A window of this manager.
    /// @return Where the connection of @p window's binding stands, as setHostConnection() was last told;
    ///         Connected until it is told otherwise, and for a window no binding claims (a local session has
    ///         no connection to lose).
    [[nodiscard]] vthost::client::ConnectionState hostConnection(
        vtworkspace::WorkspaceId window) const noexcept;

    /// Returns whether the Qt multimedia backend has been initialized.
    [[nodiscard]] bool isMultimediaReady() const noexcept { return _multimediaReady; }

    /// Sets the multimedia readiness flag and emits multimediaReadyChanged().
    void setMultimediaReady(bool ready)
    {
        if (_multimediaReady != ready)
        {
            _multimediaReady = ready;
            emit multimediaReadyChanged();
        }
    }

    // {{{ vtworkspace::ModelEvents — turn model changes into Qt model/signal notifications
    void tabAboutToBeAdded(vtworkspace::WorkspaceId window, int index) override;
    void tabAdded(vtworkspace::WorkspaceId window, vtworkspace::TabId id, int index) override;
    void tabAboutToBeRemoved(vtworkspace::WorkspaceId window, int index) override;
    void tabClosed(vtworkspace::WorkspaceId window, vtworkspace::TabId tab, int index) override;
    void tabAboutToBeMoved(vtworkspace::WorkspaceId window, int fromIndex, int toIndex) override;
    void tabMoved(vtworkspace::WorkspaceId window,
                  vtworkspace::TabId tab,
                  int fromIndex,
                  int toIndex) override;
    void workspaceMoved(vtworkspace::WorkspaceId workspace, int fromIndex, int toIndex) override;
    void tabAboutToBeMovedToWorkspace(vtworkspace::WorkspaceId from,
                                      int fromIndex,
                                      vtworkspace::WorkspaceId to,
                                      int toIndex) override;
    void tabMovedToWorkspace(vtworkspace::WorkspaceId from,
                             vtworkspace::TabId tab,
                             int fromIndex,
                             vtworkspace::WorkspaceId to,
                             int toIndex) override;
    void activeTabChanged(vtworkspace::WorkspaceId window, vtworkspace::TabId tab, int index) override;
    void paneSplit(vtworkspace::TabId tab,
                   vtworkspace::PaneId splitNode,
                   vtworkspace::PaneId newLeaf) override;
    void paneClosed(vtworkspace::TabId tab,
                    vtworkspace::PaneId closed,
                    vtworkspace::PaneId survivor) override;
    void activePaneChanged(vtworkspace::TabId tab, vtworkspace::PaneId leaf) override;
    void paneRatioChanged(vtworkspace::TabId tab, vtworkspace::PaneId splitNode, double ratio) override;
    void paneOrientationChanged(vtworkspace::TabId tab,
                                vtworkspace::PaneId splitNode,
                                vtworkspace::SplitState newState) override;
    void paneSwapped(vtworkspace::TabId tab, vtworkspace::PaneId a, vtworkspace::PaneId b) override;
    void paneSessionReplaced(vtworkspace::TabId tab, vtworkspace::PaneId leaf) override;
    void paneZoomChanged(vtworkspace::TabId tab, std::optional<vtworkspace::PaneId> zoomedLeaf) override;
    void paneTreeRestructured(vtworkspace::TabId tab) override;
    void tabTitleChanged(vtworkspace::TabId tab) override;
    void tabColorChanged(vtworkspace::TabId tab) override;
    // }}}

  signals:
    void multimediaReadyChanged();

    /// The node drag began or ended (@see beginNodeDrag, endNodeDrag): which pane is carried changed.
    void nodeDragChanged();

    /// A window's controller was removed, because its OS window closed. The app asks itself whether
    /// anything is left that keeps it running.
    void windowRemoved();

    /// A pane asked for the menu of one command block, before the menu is routed to the window that hosts it.
    /// @param acting The session the block is in.
    /// @param block The block, by id.
    void blockMenuRequested(TerminalSession* acting, vtbackend::CommandBlockId block);

    /// A pane asked for its context menu, before the menu is routed to the window that hosts it.
    ///
    /// The routing itself no-ops without a window, so this is the one point at which "the right-click
    /// reached the OpenContextMenu action" is observable independently of there being a GUI to show it in.
    /// @param acting The session that was right-clicked.
    void contextMenuRequested(TerminalSession* acting);

  private:
    /// Points @p controller at the source of the workspaces it lists: its binding's, else its own process's.
    /// @param controller The window.
    void updateDirectorySource(window::WindowController& controller) const;

    /// @param directory The folder a new workspace's first pane starts in.
    /// @return The name the workspace gets once, at creation (@see vtbackend::defaultWorkspaceName).
    [[nodiscard]] std::string defaultWorkspaceName(std::filesystem::path const& directory);

    /// Names a new workspace after the folder @p directory, once (@see defaultWorkspaceName()); an unknown
    /// (empty) folder names nothing.
    /// @param workspace The new workspace.
    /// @param directory The folder of the pane it starts with.
    void nameNewWorkspace(vtworkspace::WorkspaceId workspace, std::filesystem::path const& directory);

    /// moveSidebarNode() in a window that lists a daemon's workspaces: one MoveNode, named by the daemon's
    /// sessions the directory lists. A pane or a tab sent into a new workspace is followed there once the
    /// daemon's push lists it (WindowController::followIntoNewWorkspace()).
    void moveSidebarNodeRemotely(window::WindowController& window,
                                 window::SidebarNode const& node,
                                 window::NodeDestination const& destination);

    /// moveSidebarNode() in a window that lists this process's workspaces: a model move, which a node of
    /// another host's, or one into a workspace of another host's, does not make.
    void moveSidebarNodeInProcess(window::WindowController& window,
                                  window::SidebarNode const& node,
                                  window::NodeDestination const& destination);

    /// Moves @p moving into a workspace of its own, made in @p window as a new workspace is, named after its
    /// folder and shown there. Nothing for what already is all its workspace holds.
    /// @param window The window the user acts in.
    /// @param moving The pane or tab, in the model.
    void moveIntoNewWorkspace(window::WindowController& window, vtworkspace::MoveSource const& moving);

    /// Drops what an in-process directory holds of @p workspace, which is gone: its label and its git state.
    /// @param workspace The workspace.
    void forgetDirectoryState(vtworkspace::WorkspaceId workspace);

    /// Tells every in-process window that what its directory lists changed.
    void notifyDirectoriesChanged() const;

    /// Looks the git state of @p workspace up again, for the directory of its active pane. There is no timer
    /// behind it, unlike a daemon's: an in-process workspace is refreshed when its active pane's working
    /// directory changes, when its active tab or pane does, and when a command finishes (OSC 133;D).
    /// @param workspace The workspace.
    void refreshWorkspaceGit(vtworkspace::WorkspaceId workspace);

    /// refreshWorkspaceGit() for the workspace of @p session, if the session is its active pane's.
    /// @param session A session.
    void refreshGitOfSession(vtworkspace::SessionId session);

    /// Opens the switcher popup over the window hosting @p acting, showing @p prompt; a logged no-op for a
    /// window with no daemon behind it.
    /// @param acting The session that triggered the action.
    /// @param prompt The list, or the "New daemon…" label prompt.
    /// @param what The action's name, for the log.
    /// @param scope Every daemon, or the window's own alone.
    /// @param control The control it drops down from, in its window's scene coordinates; none centres it.
    void openSwitcherOver(TerminalSession const& acting,
                          window::SwitcherPrompt prompt,
                          std::string_view what,
                          window::SwitcherScope scope,
                          std::optional<QRectF> control);

    /// Repaints the tab hosting @p session for its lock (WindowController::Roles::SecureRole), if it has one.
    void notifySecureTab(TerminalSession const& session);

    /// Puts a new session into the pane @p ended leaves, the work restartSession() and rebindDisconnected()
    /// share: the leaf keeps its PaneId, the active pane, a zoom and the focus follow, and @p ended is
    /// retired. The session factory decides what the new session is (a shell here, a daemon session it was
    /// told).
    /// @param ended The session whose pane it is.
    /// @return The new session; or why there is none, in which case nothing changed.
    [[nodiscard]] std::expected<TerminalSession*, RestartError> replaceEndedSession(TerminalSession& ended);

    /// Splits the acting session's tab's active pane, running @p command in the new leaf (a shell when
    /// none). The local half of splitActivePane(), shared with openCommandPane().
    /// @param acting The session whose tab to split (its pane, when it is a leaf of that tab).
    /// @param direction The split's orientation.
    /// @param ratio The new split's first-child space share, in (0, 1).
    /// @param command The program to run in the new leaf, or std::nullopt for the profile's shell.
    /// @param role What the new leaf's program is to the user.
    /// @return The new leaf's session, or nullptr when no split happened.
    TerminalSession* splitPane(TerminalSession* acting,
                               vtworkspace::SplitState direction,
                               double ratio,
                               std::optional<vtpty::Process::ExecInfo> const& command,
                               SessionRole role);

    /// Starts watching the configuration file when live_config asks for it; called as sessions are
    /// created, because the file is not loaded yet when the manager is constructed. One watcher for
    /// the whole app, so a save is read once rather than once per session.
    void watchConfigFile();

    /// The watcher's change notification: watches the path again (an editor that saves by replacing
    /// the file makes the watcher drop it) and schedules one reload.
    /// @param path The file that changed.
    void onConfigFileChanged(QString const& path);

    /// Closes @p window if a tab transplant just emptied it — an empty window (no tabs) is not a valid
    /// state. Called after a cross-window tab move; MUST run after the transplant so removeWindow (via
    /// closeWindow) never tears down the moved tab's now-relocated sessions. While an EmptiedWindowHold
    /// lives, the window is noted instead, and closed once the last hold goes.
    /// @param window The source window a tab was just moved out of.
    void closeWindowIfEmpty(vtworkspace::WorkspaceId window);

    /// Tells the user a tab merge was refused (window::TabMergeRefusedText), in the window of @p acting, or
    /// in every window when no session is known.
    void postTabMergeRefused(TerminalSession const* acting);

    /// Tells the session factory that @p splitNode's ratio is now @p ratio, naming the split by the
    /// pty of one leaf from each of its children (@see SessionFactory::reportSplitRatio).
    ///
    /// Both ratio mutators — a divider drag and the ResizePane keybinding — land on paneRatioChanged,
    /// so routing from there is what keeps this a single touchpoint rather than one per action.
    /// @param tab       The tab owning the split.
    /// @param splitNode The split node whose ratio changed.
    /// @param ratio     Its new first-child share.
    void reportSplitRatio(vtworkspace::TabId tab, vtworkspace::PaneId splitNode, double ratio);

    /// Repaints @p tab's badge roles on its window's strip.
    /// @param tab The tab whose badge inputs changed.
    void refreshTabBadge(vtworkspace::Tab const& tab);

    /// Repaints the badge of the tab hosting @p session, as the output path does: its pane's own status is
    /// left alone, since output changes no record. @param session The session.
    void refreshTabBadgeOf(vtworkspace::SessionId session);
    /// @return The newest generation among @p session's program-status records, read under its terminal's
    ///         lock; 0 when it has none.
    [[nodiscard]] static uint64_t statusGenerationOf(TerminalSession const& session);

    /// Recomputes @p session's pane status from its own records (window::paneStatusBadgeOf), under its
    /// terminal's lock, and hands it to the session. @param session The session.
    void refreshPaneProgramStatus(TerminalSession& session);

    /// Tells the session factory how @p tab is presented now (@see SessionFactory::reportTabPresentation):
    /// what activePaneChanged, paneZoomChanged, tabTitleChanged, tabColorChanged and activeTabChanged call,
    /// each with the tab it is about. Nothing for a factory whose sessions this process hosts.
    /// @param tab The tab whose presentation may have changed.
    void reportTabPresentation(vtworkspace::TabId tab);

    /// Re-syncs terminal focus after an active-tab/-pane change in @p controller: if it owns the
    /// focused display, moves focus to its new active-leaf session (symmetric out/in). A background
    /// window's change does not steal focus.
    /// @param controller The window whose active tab/pane just changed.
    void syncFocusForWindow(window::WindowController* controller);

    /// Tells the focused session that the user is looking at it, when it is on screen and focus did
    /// not move to say so: its window was activated again, or restored. @see TerminalSession::paneSeen
    void focusedSessionSeen();

    /// @param window A window of this manager.
    /// @return The factory that makes its sessions and is asked about them: its binding's (@see bindWindow),
    ///         else the one this manager was constructed with. A window no binding claims -- the quick
    ///         terminal among them -- is local, and that is the only rule.
    [[nodiscard]] SessionFactory& factoryFor(vtworkspace::WorkspaceId window) const noexcept;

    /// @param window A window of this manager.
    /// @return What @p window is bound to, as it was bound (@see bindingOf); null for a local window.
    [[nodiscard]] WindowBinding* mutableBindingOf(vtworkspace::WorkspaceId window) const noexcept;

    /// @param binding A binding.
    /// @return The controllers of the windows bound to @p binding that have one yet.
    [[nodiscard]] std::vector<window::WindowController*> controllersBoundTo(
        WindowBinding const& binding) const;

    /// @param binding A binding.
    /// @return The sessions of its windows that ended because the connection to their host went
    ///         (EndReason::Disconnected).
    [[nodiscard]] std::vector<TerminalSession*> disconnectedOf(WindowBinding const& binding) const;

    /// Whether a binding still serves a window once one left it.
    enum class BindingMembers : uint8_t
    {
        Remaining = 0, ///< It serves others still.
        None,          ///< That was its last.
    };

    /// Takes @p window from the windows of @p binding (@see windowsBoundTo).
    /// @param binding What @p window was bound to.
    /// @param window The window that leaves it.
    /// @return Whether @p binding serves any window still.
    [[nodiscard]] BindingMembers forgetMember(WindowBinding const& binding, vtworkspace::WorkspaceId window);

    /// Has @p window's controller, if it has one yet, refresh what it reads of its binding: its host label
    /// and chip.
    /// @param window The window whose binding changed.
    void notifyHostBindingChanged(vtworkspace::WorkspaceId window) const;

    /// @param source What a move moves.
    /// @return The window it is in; WorkspaceId {} for a pane no window shows.
    [[nodiscard]] vtworkspace::WorkspaceId windowOfSource(vtworkspace::MoveSource const& source) const;
    /// @return Whether the model still holds what @p source names: a move made once a drag ended may find it
    ///         closed.
    [[nodiscard]] bool holdsMoveSource(vtworkspace::MoveSource const& source) const;

    /// @param session A session of this manager.
    /// @return The factory of the window @p session is in (@see factoryFor); the one this manager was
    ///         constructed with for a session in no window.
    [[nodiscard]] SessionFactory& factoryOf(TerminalSession const& session) const noexcept;

    /// @param host Who hosts a drag's sessions, as its payload names it.
    /// @return Every binding whose factory is that host -- a daemon serves each of its windows through a link
    ///         of its own --, each once; none for the factory this manager was constructed with, and for a
    ///         host this process is attached to through none.
    [[nodiscard]] std::vector<WindowBinding*> bindingsServing(window::HostIdentity const& host) const;

    /// @param host Who hosts a drag's sessions, as its payload names it.
    /// @return One binding whose factory is that host (@see bindingsServing); null for none.
    [[nodiscard]] WindowBinding* bindingServing(window::HostIdentity const& host) const;

    /// @return Whether windows @p first and @p second are served by one host that moves between them: the
    ///         same binding (or none for both), or two links of one daemon.
    [[nodiscard]] bool servedByOneHost(vtworkspace::WorkspaceId first,
                                       vtworkspace::WorkspaceId second) const noexcept;

    /// @param host Who hosts a drag's sessions, as its payload names it.
    /// @return Whether this process shows that host's sessions: it is this process, or one it is bound to.
    [[nodiscard]] bool isOwnHost(window::HostIdentity const& host) const;

    /// @param host Who hosts a drag's sessions, as its payload names it.
    /// @return The factory of that host's windows: the binding serving it, else the one this manager was
    ///         constructed with.
    [[nodiscard]] SessionFactory& factoryServing(window::HostIdentity const& host) const;

    /// @param drag A drag, resolved.
    /// @return What would make its move: the factory of the window it came from, where this process shows
    ///         it; else the one serving the host it names (@see factoryServing).
    [[nodiscard]] SessionFactory& factoryOfDrag(ResolvedDrag const& drag) const;

    /// Creates a TerminalSession backing the given vtworkspace session id, registers it in the
    /// SessionId<->TerminalSession maps, and claims C++ ownership. Does NOT touch the model (the
    /// caller has already created the corresponding tab or split leaf).
    /// @param factory   What makes the session's pty: its window's (@see factoryFor).
    /// @param sessionId The pre-minted vtworkspace session id to back.
    /// @param cwd       Working directory the new shell inherits, if any.
    /// @param pageSize  Initial grid size for the child PTY, if the caller inherits the live window's
    ///                  page size (a new tab/split); @c std::nullopt lets the factory use the profile
    ///                  default (a brand-new window).
    /// @param commandOverride Command to launch instead of the profile's configured shell, if any.
    /// @param profileName Profile to run this session under, if any; @c std::nullopt (the default)
    ///                    selects the application's default profile.
    /// @param role What the session's program is to the user.
    TerminalSession* createBackingSession(
        SessionFactory& factory,
        vtworkspace::SessionId sessionId,
        std::optional<std::string> cwd,
        std::optional<vtbackend::PageSize> pageSize = std::nullopt,
        std::optional<vtpty::Process::ExecInfo> const& commandOverride = std::nullopt,
        std::optional<std::string> const& profileName = std::nullopt,
        SessionRole role = SessionRole::Shell);

    /// Looks up a layout by name in the app's (inline + file) layout map, logging a miss.
    /// @param name    The layout name to find.
    /// @param context What is looking it up (e.g. "LaunchLayout"), for the log line.
    /// @return The layout, or nullptr when no layout carries that name.
    [[nodiscard]] config::Layout const* findLayout(std::string const& name, std::string_view context) const;

    /// The 0-based row of @p tab within its OWNING window, or -1. Window-agnostic: the owning
    /// window is resolved through the model (workspaceOfTab), so this is correct for any tab of any
    /// window.
    [[nodiscard]] int rowOfTab(vtworkspace::TabId tab) const noexcept;

    /// The model tab whose tree contains a leaf hosting @p session, or nullptr.
    [[nodiscard]] vtworkspace::Tab* findTabHostingSession(vtworkspace::SessionId session) const noexcept;

    /// The model tab backing tab-strip row @p index of @p window, or nullptr.
    [[nodiscard]] vtworkspace::Tab* tabAtRow(vtworkspace::WorkspaceId window, int index) const noexcept;

    /// Collects the backing TerminalSessions of every leaf pane in @p tab (empty if @p tab is null).
    /// @param tab The tab whose pane sessions to gather.
    /// @return The backing sessions, in pane-tree order.
    [[nodiscard]] std::vector<TerminalSession*> sessionsInTab(vtworkspace::Tab* tab) const;

    /// @return The sessions of every pane of every tab of @p workspace, in tab order.
    [[nodiscard]] std::vector<TerminalSession*> sessionsInWorkspace(
        vtworkspace::Workspace const& workspace) const;

    /// @return Whether @p window lists the workspaces of a daemon (or another host), whose labels, order and
    ///         tabs that host owns, rather than its own process's.
    [[nodiscard]] bool listsRemotely(window::WindowController const& window) const noexcept;

    /// @return Whether @p window owns one workspace or none: an in-process window keeps its last one, so
    ///         closeWorkspace() and confirmCloseWorkspace() refuse to close it.
    [[nodiscard]] static bool ownsOneWorkspaceOnly(window::WindowController const& window);

    /// @return Whether @p window holds @p workspace among the workspaces it owns.
    [[nodiscard]] static bool ownsWorkspace(window::WindowController const& window,
                                            vtworkspace::WorkspaceId workspace) noexcept;

    /// Gathers the backing sessions of every tab in @p window matching @p predicate, in row order.
    /// Shared by the bulk-close operations (closeOtherTabs / closeTabsToRight), which snapshot the doomed
    /// sessions before the model mutates.
    /// @param window The window whose tabs to scan.
    /// @param predicate Selects which tabs' sessions to gather. Receives the tab's row (already known to
    ///                  the row-ordered scan) so a positional predicate need not re-derive it via a linear
    ///                  rowOfTab() lookup, and the tab itself.
    /// @return The matching tabs' backing sessions.
    [[nodiscard]] std::vector<TerminalSession*> gatherSessionsOfTabsWhere(
        vtworkspace::Workspace& window,
        std::function<bool(int row, vtworkspace::Tab*)> const& predicate) const;

    /// Terminates each of @p sessions, the single whole-tab close primitive shared by all close
    /// paths (CloseTab, the tab ✕ button, "Close Other Tabs", "Close Tabs to the Right").
    /// terminate() drives sessionClosed -> removeSession, which collapses the model to the survivor on
    /// each pane close and tears the tab down with its last pane. The caller passes a stable snapshot
    /// (e.g. from sessionsInTab) so the model mutations terminate() triggers do not invalidate the
    /// iteration. Keeping this in one place ensures the close entry points stay consistent.
    ///
    /// @p end is mandatory rather than defaulted so a new close path cannot silently inherit the
    /// wrong answer: getting it wrong either kills a daemon session the user only detached from, or
    /// leaves one running that they closed.
    /// @param sessions The backing sessions to terminate.
    /// @param end Whether the user ended these sessions or only their local view is going away.
    void terminateSessions(std::span<TerminalSession* const> sessions, SessionEnd end);

    /// Authors the end of @p session upstream when it is not locally owned (attach mode), so a
    /// daemon-hosted session the user closed does not linger there. A no-op for a local session.
    ///
    /// Must run while @p session still holds its pty: the remote factory identifies the session BY
    /// its pty, and the teardown that follows destroys it.
    /// @param session The session being ended for good.
    void authorRemoteEnd(TerminalSession* session);

    /// Tells the factory of @p session that the session ends as @p end says: authorRemoteEnd() for a
    /// Destroy, SessionFactory::releaseRemoteSession() for a Detach. Under the same rule as
    /// authorRemoteEnd(): while @p session holds its pty and sits in its tab.
    /// @param session The session whose pane goes; none is a no-op.
    /// @param end Why it goes.
    void endRemotely(TerminalSession* session, SessionEnd end);

    /// authorRemoteEnd() for each of @p sessions. Must run while each still sits in its tab: the window
    /// holding the tab names the host to tell (@see factoryOf), so a caller about to close those tabs in the
    /// model authors their ends first.
    /// @param sessions The sessions being ended for good.
    void authorRemoteEnds(std::span<TerminalSession* const> sessions);

    /// Resolves the model tab that hosts @p session (any of its split panes), or nullptr if @p
    /// session is unknown to the registries or has no model tab.
    /// @param session The backing session whose hosting tab to locate.
    /// @return The hosting tab, or nullptr.
    [[nodiscard]] vtworkspace::Tab* tabHostingSession(TerminalSession const* session) const noexcept;

    /// Where a session sits: its tab, that tab's window, and the window's controller. Each link is
    /// nullptr when the one before it is, or when it does not resolve.
    struct SessionHost
    {
        vtworkspace::Tab* tab = nullptr;                ///< The tab hosting the session.
        vtworkspace::Workspace* window = nullptr;       ///< The window holding @ref tab.
        window::WindowController* controller = nullptr; ///< The controller of @ref window.
    };

    /// Resolves tab, window and controller for @p session in one walk: the single rule every
    /// "which window is this session in" question shares.
    /// @param session The backing session to locate (nullptr resolves nothing).
    /// @return Its host; every member is nullptr when no tab hosts @p session.
    [[nodiscard]] SessionHost hostOf(TerminalSession const* session) const noexcept
    {
        auto* tab = tabHostingSession(session);
        auto* win = tab != nullptr ? _model->workspace(_model->workspaceOfTab(tab->id())) : nullptr;
        return SessionHost { .tab = tab,
                             .window = win,
                             .controller = win != nullptr ? controllerOwning(win->id()) : nullptr };
    }

    /// @param session A session of the model.
    /// @return The seen-state ledger of the window owning the workspace that holds it; nullptr when none
    /// does.
    [[nodiscard]] window::TabAttention* ledgerOf(vtworkspace::SessionId session) const noexcept;

    /// The seen-state of panes about to change windows, as a move notes it before it happens.
    struct AttentionCarry
    {
        vtworkspace::SessionId session; ///< The pane's session.
        window::PaneKey key;            ///< What it is kept under.
        window::TabAttention* from;     ///< The ledger that holds it now.
    };

    /// @param sessions The sessions a move is about to relocate.
    /// @return Where each one's seen-state is now, to hand to carryAttention() once the move is done.
    [[nodiscard]] std::vector<AttentionCarry> beginCarryingAttention(
        std::span<vtworkspace::SessionId const> sessions) const;

    /// Moves each carried pane's marks from the ledger it was in to the one of the window that now owns it.
    /// @param carried What beginCarryingAttention() noted.
    void carryAttention(std::span<AttentionCarry const> carried);

    /// The leaf of @p tab that @p session backs.
    /// @param tab     The tab to search.
    /// @param session The backing session to find.
    /// @return The leaf, or nullptr when @p session is not one of @p tab's panes.
    [[nodiscard]] static vtworkspace::Pane* leafOfSession(vtworkspace::Tab const& tab,
                                                          TerminalSession const& session);

    /// Makes @p session's pane the active pane of @p tab; a no-op when it is not one of @p tab's panes.
    /// @param tab     The tab hosting @p session.
    /// @param session The session whose pane to activate.
    void activateSessionPane(vtworkspace::Tab const& tab, TerminalSession const& session)
    {
        if (auto const* leaf = leafOfSession(tab, session); leaf != nullptr)
            activatePane(tab.id(), leaf->id());
    }

    /// Resolves the model window hosting @p session (through its tab), or nullptr.
    /// @param session The backing session whose hosting window to locate.
    /// @return The hosting window, or nullptr.
    [[nodiscard]] vtworkspace::Workspace* windowHostingSession(TerminalSession const* session) const noexcept
    {
        return hostOf(session).window;
    }

    /// Resolves the QML-facing controller of the window hosting @p session, or nullptr.
    ///
    /// The routing every keybinding that targets "my window's UI" needs: a session receives the chord,
    /// but the tab strip / palette / context menu it must act on belongs to the window that session
    /// happens to sit in.
    /// @param session The backing session that received the keybinding.
    /// @return The hosting window's controller, or nullptr when the session has no window (or no
    ///         controller has been registered for it).
    [[nodiscard]] window::WindowController* controllerHostingSession(
        TerminalSession const* session) const noexcept
    {
        return hostOf(session).controller;
    }

    /// Shared move primitive for the public move-tab entry points: reorders the tab hosting @p session to
    /// model row @p targetRow through the authoritative model and refreshes the status line. The callers
    /// own their distinct target-row computation and bounds checks; this resolves the session to its tab
    /// then defers to moveTabByTab() for the common move / updateStatusLine mechanism.
    /// @param session   The backing session whose tab to move (nullptr or unknown is a no-op).
    /// @param targetRow The destination row in 0-based model tab-space.
    void moveTabToRow(TerminalSession* session, int targetRow);

    /// A move into a window of its own, staged until that window exists.
    struct PendingTransplant
    {
        vtworkspace::MoveSource source; ///< What moves.
        window::DropPayload payload;    ///< What the new window's screen reader is told moved.
    };

    /// What a move target is, for the drag that moves there: read once per move (describeTarget()).
    struct DescribedTarget
    {
        window::MovePlace place = window::MovePlace::Edge;         ///< Where it is, as the tables read it.
        window::MoveLanding landing = window::MoveLanding::Beside; ///< Where it lands, as announced.
        std::optional<vtworkspace::WorkspaceId> workspace; ///< The workspace it lands in; none for a new one.
        vtworkspace::MoveTargetKind kind = vtworkspace::MoveTargetKind::Edge; ///< What kind of target.
        /// The host's id of the pane the daemon is asked to move beside or onto (a tab well's: its window's
        /// active pane; a new window's: 0); none for one the host has no id for yet.
        std::optional<uint64_t> hostSession;
        vtworkspace::SplitSide side = vtworkspace::SplitSide::Right; ///< Edge only: the side.
        int index = 0;                                               ///< Tab well only: the position.
    };

    /// Describes @p target for @p drag: one pass over the target, which the move, its route, its refusal,
    /// its announcement and the daemon's request all read.
    /// @return The description; std::nullopt for a target that names nothing that exists.
    [[nodiscard]] std::optional<DescribedTarget> describeTarget(ResolvedDrag const& drag,
                                                                vtworkspace::MoveTarget const& target) const;
    /// The local move source @p drag names.
    [[nodiscard]] std::optional<vtworkspace::MoveSource> localSourceOf(ResolvedDrag const& drag) const;
    /// Asks the daemon to make the move (@see moveNode), and waits for it.
    [[nodiscard]] std::expected<void, MoveFailure> requestHostMove(ResolvedDrag const& drag,
                                                                   DescribedTarget target,
                                                                   QScreen* screen,
                                                                   QPoint at);
    /// Carries out a whole-tab move by the tab verbs a daemon older than MoveNode served
    /// (MoveRoute::TabMove).
    [[nodiscard]] std::expected<void, MoveFailure> moveTabLegacy(ResolvedDrag const& drag,
                                                                 vtworkspace::MoveTarget const& target,
                                                                 QScreen* screen,
                                                                 QPoint at);
    /// Ends the node drag, whoever owns it (@see endNodeDrag).
    [[nodiscard]] std::unique_ptr<NodeDragSession> takeNodeDrag();
    /// The layout changed: a drag resolved before names what it did then (@see ResolvedDrag::layout).
    void layoutChanged() noexcept { ++_layoutGeneration; }
    /// Where a window opened for a drop at @p at on @p screen is placed.
    [[nodiscard]] static std::optional<window::WindowPosition> spawnPositionFor(QScreen* screen, QPoint at);
    /// Stages @p pending for the next window, spawned on @p screen at @p at (@see consumePendingTransplant).
    void spawnWindowFor(PendingTransplant const& pending, QScreen* screen, QPoint at);
    /// Opens a window of a connection of its own on @p binding's daemon, beside @p asking; one that cannot
    /// open is logged under @p what and told in @p asking.
    /// @param binding The daemon binding @p asking is bound to.
    /// @param asking The window asked from.
    /// @param shows What the new window shows (a New Window, or a tab torn off).
    /// @param activeTab The tab the new window activates; none for the daemon's own choice.
    /// @param screen Where the window opens; null for the default placement.
    /// @param what What asked, for the log.
    /// @return Routed once the window's connection is under way; Refused otherwise.
    RemoteRouting openSiblingWindow(WindowBinding& binding,
                                    vtworkspace::WorkspaceId asking,
                                    SiblingShows shows,
                                    std::optional<vtworkspace::TabId> activeTab,
                                    QScreen* screen,
                                    std::string_view what) const;
    /// Tells the screen reader of the window @p moved landed in what happened.
    void announceMove(vtworkspace::MoveResult const& moved,
                      window::DropPayload payload,
                      window::MoveLanding landing) const;

    /// The single move mechanism: reorders @p tab to row @p targetRow of its OWNING window (resolved
    /// through the model) and refreshes the status line. Every move entry point funnels through here
    /// so the post-move refresh lives in exactly one place.
    /// @param tab       The tab to move (nullptr is a no-op).
    /// @param targetRow The destination row in 0-based model tab-space.
    void moveTabByTab(vtworkspace::Tab* tab, int targetRow);

    /// The tab a pane action should target: the tab hosting @p acting (the session that received
    /// the keybinding). Keyboard pane actions act on the tab the user is typing in — there is no
    /// "the active tab" fallback, since with several OS windows that would be ambiguous.
    /// @param acting The acting session (nullptr or unknown yields nullptr).
    /// @return The target tab, or nullptr.
    [[nodiscard]] vtworkspace::Tab* paneActionTargetTab(TerminalSession* acting) const noexcept
    {
        return tabHostingSession(acting);
    }

    /// Activates the tab at tab-strip row @p row of @p window: makes it that window's active tab
    /// (so the rendered split tree and subsequent pane operations target it). No-op if @p row is
    /// out of range.
    void activateModelTabByRow(vtworkspace::WorkspaceId window, int row);

    /// Refreshes every window's indicator status line. The per-window tab list + per-pane marker fan-out
    /// lives on each WindowController (WindowController::updateStatusLine), so this simply routes to every
    /// controller — the single publish path, replacing the old _displayStates iteration. A rename/recolor
    /// in a background window reaches here (refreshGuiTabInfoForStatusLine -> update()) and refreshes that
    /// window's status line because its controller republishes to its own tabs' sessions.
    void updateStatusLine();

    /// Where the taskbar's progress answer comes from. It holds neither a clock nor a timeout of its
    /// own -- both are supplied per call -- so nothing here can drift from the configured value.
    platform::TaskbarProgressRouter _taskbarProgress {};
    QTimer _taskbarProgressTimer;

    /// Publishes current window aggregates and schedules the next expiry, using the injected clock.
    void refreshTaskbarProgress();
    void scheduleTaskbarProgress();

    /// Tells the taskbar sink how many tabs are Blocked (OSC 7501), by its scope: the total for an
    /// Application sink, each window's own for a Window sink. Only what changed since the last call is
    /// sent. Runs from refreshTaskbarProgress(), which every badge, tab and window change schedules.
    void publishTaskbarAttention();
    /// What the sink was last told, so that an unchanged count costs the OS no call.
    window::AttentionTally _publishedAttention {};

    ContourGuiApp& _app;
    /// What makes the sessions of every window no binding claims. @see factoryFor
    SessionFactory& _localFactory;
    WindowSwitching& _windowSwitching;
    SwitchDirectory& _switchDirectory;
    /// What each bound window is bound to, keyed by its WorkspaceId. Not owned: a binding outlives the
    /// windows it serves (@see bindWindow, releaseBinding). A window absent from it is served by
    /// _localFactory.
    std::unordered_map<uint64_t, WindowBinding*> _bindingOfWindow;
    /// The reverse of _bindingOfWindow: the windows each binding serves, in binding order. A binding serving
    /// none is absent. @see windowsBoundTo
    std::unordered_map<WindowBinding const*, std::vector<vtworkspace::WorkspaceId>> _windowsOfBinding;
    config::LayoutStore& _layoutStore;
    command::CommandHistoryStore& _commandHistoryStore;
    platform::TaskbarProgressSink& _taskbarProgressSink;
    /// The only caller of the OS's secure keyboard entry. Declared early, so it is destroyed late -- after
    /// everything that may still ask it to move -- and its destruction turns secure input off. GUI thread.
    platform::SecureInputGate _secureInputGate;
    /// The session whose pane the OS's secure input protects now: the focused one while the gate is On.
    /// GUI thread. @see refreshSecureInput
    std::optional<vtworkspace::SessionId> _securedSession;
    std::string _homeDirectory; ///< The user's home, read once by the composition root.
    /// Whether this run showed the shell-integration card already: at most one card per run of the app.
    window::CardSlot _shellCardSlot = window::CardSlot::Free;
    /// The app-wide most-recently-used command list. Seeded from the store on the first palette open
    /// (see openCommandPalette()); every window's palette model reads it, and recordCommand() writes it
    /// back through the store.
    command::CommandHistory _commandHistory;
    /// Whether _commandHistory has been seeded from the store yet. The seeding is deferred rather than
    /// done in the constructor because the configured capacity is not known that early — see there.
    bool _commandHistoryLoaded = false;
    std::chrono::seconds _earlyExitThreshold;
    /// The live_config watcher, created by watchConfigFile() once the configuration is loaded.
    std::unique_ptr<QFileSystemWatcher> _configFileWatcher;
    /// Coalesces the change notifications of one save -- an editor writing in steps, and platforms
    /// that report a write as several changes -- into one reload.
    QTimer _configReloadTimer;
    // The process-wide "focused display". Session->display ownership lives on the pane tree; this is only
    // The single session that currently holds terminal (VT) focus across all windows, or nullptr.
    // setFocusedSession() is the only mutator that NOTIFIES (the symmetric focus-out/focus-in pair);
    // the teardown paths (removeSession, and removeWindowController via clearFocusedWindow) drop the
    // back-pointer without notifying a session that is already dying.
    TerminalSession* _focusedSession = nullptr;

    // Which OS window owns focus, as a model identity rather than a Qt pointer: this is what gates
    // whether a model event may move VT focus. A pointer to the focused display cannot serve, because
    // it is null between a display teardown and the next Qt focus-in -- and a tab switch in that gap
    // notified nobody. @see syncFocusForWindow.
    std::optional<vtworkspace::WorkspaceId> _focusedWindow;

    // The window that held focus before it was cleared, kept where _focusedWindow is not: what an
    // application-wide command runs in while no window is focused. @see applicationController.
    std::optional<vtworkspace::WorkspaceId> _lastFocusedWindow;

    /// When each window last gained focus, by a counter: the most recently focused window of a daemon speaks
    /// for it in the application's taskbar count (window::applicationWaiting).
    std::unordered_map<window::WindowId, uint64_t> _focusOrder;
    uint64_t _focusCounter = 0; ///< The last value handed to _focusOrder.

    bool _multimediaReady = false;

    /// What one binding's connection is in, as its link reports it. A binding absent from _linkStates is in
    /// the default of each.
    struct LinkState
    {
        /// Where the connection to the host stands. @see setHostConnection
        vthost::client::ConnectionState connection = vthost::client::ConnectionState::Connected;
        /// Whether a Reconnect from a footer is running: begun by reconnectHost(), over by
        /// rebindDisconnected() or reconnectFailed().
        ReconnectRun reconnect = ReconnectRun::Idle;
        /// What the daemon's restore brought back, until a window is told it. @see noteWorkspaceRestored
        std::optional<vthost::RestoredCounts> restoreSummary;
    };
    /// Each binding's connection, keyed by the binding; dropped by releaseBinding(). GUI thread.
    std::unordered_map<WindowBinding const*, LinkState> _linkStates;

    /// @param binding A binding.
    /// @return The live sessions of every window bound to @p binding.
    [[nodiscard]] std::vector<TerminalSession*> sessionsBoundTo(WindowBinding const& binding) const;

    // {{{ vtworkspace model integration
    // The Qt-free layout model: the authoritative window/tab/pane tree. The model owns the
    // authoritative tab title and color; _sessionsById maps each leaf's SessionId to its backing
    // TerminalSession.
    std::unique_ptr<vtworkspace::SessionModel> _model;
    // Per-OS-window QML adapters, keyed by window::WindowId. The ModelEvents router forwards each Qt
    // row/signal emission to the controller for the event's window. Owned via QQmlEngine CppOwnership.
    std::unordered_map<window::WindowId, window::WindowController*> _controllersByWindow;
    uint64_t _nextWindowId = 1; ///< The next WindowId to mint.
    // What an in-process window lists of its workspaces (InProcessDirectory): their labels, the status of
    // their panes' sessions and their git state, which one tracker looks up for every window. The directories
    // come last, so that they go before what they read.
    WorkspaceLabels _workspaceLabels;
    std::unique_ptr<window::PaneStatusLookup> _paneStatus;
    std::unique_ptr<vthost::WorkspaceGitTracker> _gitTracker; ///< Null where git is not tracked.
    std::unordered_map<window::WindowId, std::unique_ptr<InProcessDirectory>> _inProcessDirectories;
    /// What the next createWindowController() window is for. @see stageWindowRole
    window::WindowRole _stagedRole = window::WindowRole::Normal;
    // Registry mapping a vtworkspace session id to the Qt TerminalSession that backs it. The reverse
    // (TerminalSession -> SessionId) needs no map: each TerminalSession stores its own
    // modelSessionId().
    std::unordered_map<uint64_t, TerminalSession*> _sessionsById;
    // SessionId -> the tab hosting that session's leaf, so updateStatusLine() and
    // findTabHostingSession() resolve a session's tab in O(1) instead of walking every tab's tree.
    std::unordered_map<uint64_t, vtworkspace::TabId> _tabBySession;
    // TabId -> how that tab's active pane was last reached (@see notePaneActivation); a tab absent from it
    // was reached by the pointer. Pruned when the tab closes.
    std::unordered_map<uint64_t, window::PaneActivation> _paneActivations;
    // The tab a FocusPane* action is moving the active pane of, while focusPane() runs it: what tells
    // activePaneChanged() that the move is the keyboard's.
    std::optional<vtworkspace::TabId> _keyboardPaneMove;
    uint64_t _nextSessionId { 1 };
    // The id pre-minted for the next tab the model is about to create, consumed by the model's
    // SessionAllocator so a model tab and its backing TerminalSession share one id.
    std::optional<vtworkspace::SessionId> _pendingSessionId;
    // Whether a window already consumed the startup layout (--layout / default_layout). The app's
    // layout name is stable for the whole process, but the layout must apply to the FIRST window
    // only — not to every window spawned later (see consumeDefaultLayout()).
    bool _startupLayoutConsumed = false;

    // A move into a new window staged by moveNode() or tearOffTabToNewWindow(): what the next spawned window
    // should adopt as its sole tab instead of creating a fresh one. Consumed exactly once by
    // consumePendingTransplant() from that window's Main.qml. Mirrors ContourGuiApp::_pendingSpawnScreen.
    std::optional<PendingTransplant> _pendingTransplant;
    // Where the next window spawned for a move opens, staged with it; consumed by createWindowController().
    std::optional<window::WindowPosition> _pendingSpawnPosition;
    // How many EmptiedWindowHolds live, and the windows a move emptied meanwhile, in the order it did.
    int _emptiedWindowHolds = 0;
    std::vector<vtworkspace::WorkspaceId> _emptiedWindows;
    // The move asked of the daemon that this manager waits for, until the push that makes it ends, the
    // daemon refuses it, the connection is lost, or RemoteMoveTimeout passes.
    std::optional<PendingRemoteMove> _pendingRemoteMove;
    // The one node drag this process runs, while it runs.
    std::unique_ptr<NodeDragSession> _nodeDrag;
    // The drag of another process last hovered, resolved as it entered (resolveNodeDrag()).
    struct HoveredDrag
    {
        QString payload;                  ///< Its JSON.
        std::optional<ResolvedDrag> drag; ///< What it resolved as; none for JSON that is no node drag.
    };
    mutable std::optional<HoveredDrag> _hoveredDrag;
    // Bumped by every change of the layout's shape, so a drag resolved before is resolved again.
    uint64_t _layoutGeneration = 0;
    // The number the next move asked of the daemon goes out under.
    uint64_t _nextRemoteMove = 1;

    // Attach-mode window binder installed by ContourGuiApp (B4): a freshly-spawned window asks it
    // (via consumeAttachWindow, from Main.qml) whether it hosts a pending daemon workspace. Empty when
    // not attached, so consumeAttachWindow is a no-op for ordinary local windows.
    std::function<bool(contour::window::WindowController*)> _attachWindowBinder;
    /// What installed _attachWindowBinder; null while none is installed. @see setAttachWindowBinder
    WindowBinding const* _attachWindowBinderOwner = nullptr;

    // Cached QVariantList of the (immutable) tab-color palette, built lazily on first request.
    mutable QVariantList _tabColorPaletteCache;
    // }}}
};

} // namespace contour::session

Q_DECLARE_INTERFACE(contour::session::TerminalSessionManager, "org.contour.TerminalSessionManager")
