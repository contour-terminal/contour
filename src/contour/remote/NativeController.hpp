// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// `NativeController` — the GUI's native-protocol attach engine.
///
/// One controller = one connection to a `contour daemon`. It runs the
/// Qt-free `vthost::client::NativeClient` on its own reactor thread
/// (ReactorThread) and doubles as the app's `SessionFactory` while attached:
/// every locally created tab is backed by a `vtpty::ChannelPty` bound to one
/// remote session. Input and resize flow out through that pty's sinks onto the
/// reactor.
///
/// Remote OUTPUT does not come back through it. A per-session `ScreenMirror`
/// populates the pane's `vtbackend::Terminal` grid directly from the delta
/// stream, so the display stack never learns the session is remote — and
/// nothing is lost to an escape-sequence round trip on the way. That needs the
/// terminal, which does not exist when `createPty` hands out the pty, so
/// `bindTerminal` closes the cycle and primes the mirror.

#include <contour/remote/ReactorThread.hpp>
#include <contour/remote/RemoteController.hpp>
#include <contour/remote/RemoteLayout.hpp>
#include <contour/remote/SelectAnswer.hpp>
#include <contour/session/HostedOperation.hpp>
#include <contour/session/SessionFactory.hpp>
#include <contour/window/WorkspaceDirectory.hpp>

#include <vtbackend/screen/Settings.hpp>

#include <vtpty/ChannelPty.hpp>

#include <core/platform/SystemPower.hpp>

#include <QtCore/QObject>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <expected>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <ranges>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include <vthost/Transport.hpp>
#include <vthost/WorkspaceVisibility.hpp>
#include <vthost/client/ClientSupervisor.hpp>
#include <vthost/client/LayoutReconstruction.hpp>
#include <vthost/client/NativeClient.hpp>
#include <vthost/client/ScreenMirror.hpp>
#include <vthost/latency/StartupTimeline.hpp>
#include <vtworkspace/Primitives.hpp>

namespace contour::session
{
class TerminalSessionManager;
}

namespace contour::remote
{

/// The wire operation a hosted operation travels as: the conversion at the protocol boundary, which keeps
/// the session layer free of the protocol. The two enums name the same operations by the same values, as
/// the assertion below pins, so the conversion is a cast; a value no enumerator names travels as itself,
/// for the host to refuse.
/// @param operation The operation a session asks for.
/// @return Its proto::EngineOperation.
[[nodiscard]] constexpr vthost::proto::EngineOperation toWireOperation(
    contour::session::HostedOperation operation) noexcept
{
    return static_cast<vthost::proto::EngineOperation>(std::to_underlying(operation));
}

static_assert(
    [] {
        using contour::session::HostedOperation;
        using vthost::proto::EngineOperation;
        // One pair per operation: a HostedOperation added without the EngineOperation it travels as fails the
        // count, and one whose value differs fails the comparison.
        constexpr auto Pairs = std::array {
            std::pair { HostedOperation::ClearToPrompt, EngineOperation::ClearToPrompt },
            std::pair { HostedOperation::ClearHistoryAndReset, EngineOperation::ClearHistoryAndReset },
            std::pair { HostedOperation::SoftReset, EngineOperation::SoftReset },
            std::pair { HostedOperation::WriteScreen, EngineOperation::WriteScreen },
            std::pair { HostedOperation::SetUserMark, EngineOperation::SetUserMark },
            std::pair { HostedOperation::ClearUserMark, EngineOperation::ClearUserMark },
            std::pair { HostedOperation::UseDarkColors, EngineOperation::UseDarkColors },
            std::pair { HostedOperation::UseLightColors, EngineOperation::UseLightColors },
        };
        return Pairs.size() == contour::session::HostedOperationTable.size()
               && std::ranges::all_of(Pairs, [](auto const& pair) {
                      return std::to_underlying(pair.first) == std::to_underlying(pair.second);
                  });
    }(),
    "every HostedOperation travels as the proto::EngineOperation of its name and value");

/// The attach-mode session factory and remote-session registry.
class NativeController final: public QObject, public contour::session::SessionFactory, public RemoteController
{
    Q_OBJECT

  public:
    /// Resolves this client's profile in the configuration the GUI holds now: the emulation settings
    /// to ask the daemon for on the next session this client creates, or why they cannot be resolved.
    /// Called on the GUI thread.
    using SessionSettingsSource = std::function<std::expected<vtbackend::Settings, std::string>()>;

    /// The light/dark preference the app shows now, or nullopt while it knows none yet (the OS's scheme is
    /// read only once the QApplication exists, after the first connection may already be under way).
    /// Called on the reactor thread, so it must be safe to call concurrently with the GUI thread.
    using ColorPreferenceSource = std::function<std::optional<vtbackend::ColorPreference>()>;

    /// What a reconnect follows when none of the workspaces it resumes survived.
    enum class ResumeFallback : uint8_t
    {
        Launch = 0, ///< What a launch follows (proto::AttachIntentKind::Resume): the launch's link.
        /// Nothing: its window's workspace is gone, and the window closes
        /// (proto::AttachIntentKind::Select). A link that took its workspace over from another, which
        /// must not take what other windows resume.
        Nothing,
    };

    /// Whether this client answers the daemon's requests to switch a window (proto::SwitchRequest).
    enum class SwitchRequests : uint8_t
    {
        /// Capability::WorkspaceSwitch is not offered, as by a client that predates it: the daemon fails
        /// every `workspace.switch` for this client's windows itself.
        Declined = 0,
        Answered, ///< Offered; each request is handed to the GUI (switchRequested()).
    };

    /// Everything a controller is built with.
    struct Config
    {
        /// How to reach the daemon: the local unix control socket, or a TLS-encrypted, token-authenticated
        /// TCP endpoint. Opened afresh for every reconnect; its describe() names the daemon in every log line
        /// and failure message.
        std::shared_ptr<vthost::ITransportFactory> transport;
        /// Which of the daemon's workspaces the first connection follows; every reconnect resumes the
        /// workspaces the lost connection followed.
        vthost::proto::AttachIntent intent;
        /// The host profile this client's sessions run, stated in every ClientHello; empty for the daemon's
        /// default.
        std::string profileName;
        /// The absolute directory the session the first connection creates starts in, stated in that
        /// connection's ClientHello only -- a reconnect creates nothing of its own; empty for the profile's.
        std::filesystem::path workingDirectory = {};
        /// The emulation settings to state in the ClientHello -- this client's own resolved profile -- or
        /// nullopt to take whatever the daemon hosts with.
        std::optional<vtbackend::Settings> attachSettings;
        /// Asked before every session this client creates, so a session created after a configuration reload
        /// gets the reloaded settings; empty to keep @p attachSettings. When it fails, the settings last
        /// stated stay in force.
        SessionSettingsSource sessionSettings;
        /// Asked as every hello is built, so the daemon builds the sessions this client follows under the
        /// preference the app shows at that moment -- not the one it showed when this config was made, which
        /// a link cloned later from it would otherwise state again. Empty, or nullopt from it, states what
        /// requestHostedOperation() last stated, and none before that. Changes between hellos arrive through
        /// requestHostedOperation().
        ColorPreferenceSource colorPreference;
        /// What the connect budget is measured on (@see RemoteController::awaitConnected). Borrowed: it must
        /// outlive the controller.
        core::platform::IClock const& clock;
        /// Where the connect, the layout and the first session are stamped. Borrowed: it must outlive the
        /// controller.
        vthost::latency::StartupTimeline& timeline;
        /// Where the system's sleep and wake are heard of, so a wake has the daemon probed rather than the
        /// connection given up; null where nothing reports them. Subscribed to for the controller's
        /// lifetime. Borrowed: it must outlive the controller.
        core::platform::ISystemPowerMonitor* powerMonitor = nullptr;
        /// How long each reconnect waits after a loss; a seam for tests, the production schedule by default.
        vthost::client::BackoffSchedule backoff {};
        /// What a reconnect follows when none of the workspaces it resumes survived.
        ResumeFallback resumeFallback = ResumeFallback::Launch;
        /// Whether the daemon may ask this client to switch its windows.
        SwitchRequests switchRequests = SwitchRequests::Answered;
    };

    /// @param config What the controller is built with.
    explicit NativeController(Config config);

    /// Stops the reactor (detaching if still connected) and joins.
    ~NativeController() override;

    NativeController(NativeController const&) = delete;
    NativeController& operator=(NativeController const&) = delete;
    NativeController(NativeController&&) = delete;
    NativeController& operator=(NativeController&&) = delete;

    // connectAndWait() and stop() are inherited from RemoteController; this
    // controller supplies the hooks below.

    /// @return The daemon's release as the ServerHello of the last accepted connection stated it; empty until
    ///         the handshake is through (valid after connectAndWait) and for a daemon that predates the
    ///         field.
    [[nodiscard]] std::string daemonRelease() const;

    /// @return What the handshake of the last accepted connection agreed on, captured as it completed; empty
    ///         until then.
    [[nodiscard]] std::optional<vthost::proto::Negotiated> negotiated() const;

    /// @return Where the connection to the daemon stands: Reconnecting while a lost one is sought again, its
    ///         bindings and ptys kept meanwhile. Safe from any thread.
    [[nodiscard]] vthost::client::ConnectionState connectionState() const;

    /// Whether a prompt of this controller's attach waits for the user to be looking at a window.
    enum class UserAttention : uint8_t
    {
        NotNeeded = 0,
        Needed, ///< The connection says it needs attention (vthost::client::ConnectionState::NeedsAttention).
    };

    /// Says whether a prompt of this attach is held until its window is on screen. While it is, a connection
    /// that is being sought again reports NeedsAttention; the reconnect itself goes on, as it never waits for
    /// anyone. GUI thread.
    /// @param attention Whether a prompt waits.
    void setUserAttention(UserAttention attention);

    /// @return How many remote sessions await a local tab.
    [[nodiscard]] std::size_t pendingCount() const;

    /// @return True if @p session already has a local pane bound to it — used by
    ///         the incremental layout reconciler to skip tabs it already realized.
    [[nodiscard]] bool isBound(uint64_t session) const;

    /// @return True if @p session is claimed locally either way — bound to a pane,
    ///         or tombstoned by a user close the daemon has not acknowledged yet
    ///         (a stale layout push must not resurrect it). One lock acquisition
    ///         for the reconciler walks that check per leaf.
    [[nodiscard]] bool isClaimed(uint64_t session) const;

    /// @return The remote session bound to local pty @p pty, or nullopt. Lets the
    ///         reconciler map a GUI pane back to its daemon session (to split/close
    ///         the right pane).
    [[nodiscard]] std::optional<uint64_t> sessionForPty(vtpty::Pty const* pty) const;

    /// The daemon's row that the row @p row of @p session's mirror holds: what the daemon is asked to act on
    /// when the user acts on a row of the mirror. Call on the reactor, where the mirror is applied.
    /// @see vthost::client::ScreenMirror::hostRowOf
    /// @return The daemon's row, or std::nullopt when the mirror cannot name it.
    [[nodiscard]] std::optional<vthost::proto::RowAddress> hostRowOf(uint64_t session,
                                                                     contour::session::HostedRow row) const;

    /// Asks the daemon to create a new tab (B3-Qt). The daemon honors it and
    /// re-pushes its layout, which the GUI reconciles into a new local tab. A no-op
    /// once detached.
    /// @param actingPty A pty of the WORKSPACE the tab belongs in — the daemon hosts more than one
    ///        and resolves the workspace from the session this pty is bound to. Null or unbound
    ///        falls back to the daemon's first workspace.
    /// @param spawn What the tab's session runs: its profile, by the name the daemon resolves.
    void requestCreateTab(vtpty::Pty const* actingPty, contour::session::RemoteSpawn const& spawn);

    /// Asks the daemon to split the pane hosting the remote session bound to local
    /// pty @p actingPty (@p vertical orientation). The daemon honors it and
    /// re-pushes its layout, which reconciles into a local split. A no-op if the pty
    /// is not bound or the connection is gone.
    /// @param spawn What the new pane's session runs: its profile, by the name the daemon resolves.
    void requestSplitPane(vtpty::Pty const* actingPty,
                          bool vertical,
                          contour::session::RemoteSpawn const& spawn);

    /// Asks the daemon to close the pane hosting @p session (destroying that remote
    /// session). Sent when the user closes a mirror pane, so the session is really
    /// removed rather than left running headless. Asked while the connection is down, it is
    /// sent once the same daemon answers again, and dropped if another run of it does.
    void requestClosePane(uint64_t session);

    /// A status-line extension the daemon runs for a pane asks whether it may run its declared commands.
    struct HostedPermissionRequest
    {
        vtpty::Pty const* pty = nullptr;          ///< The pty of the pane to ask in.
        vthost::proto::PermissionRequest request; ///< What it asks, as the daemon sent it.
    };

    /// Takes the questions extensionPermissionRequested() announced, for the panes they are asked in. One
    /// for a session not bound to a pane yet stays, and is announced again once it is. GUI thread.
    /// @return The questions, oldest first.
    [[nodiscard]] std::vector<HostedPermissionRequest> takePermissionRequests();

    /// Answers a question takePermissionRequests() handed out: the daemon records the answer, for every
    /// session. A no-op once detached.
    /// @param hostId The installed extension that asked.
    /// @param digest The manifest digest the answer is bound to.
    /// @param answer The user's answer.
    void answerPermission(std::string hostId, std::string digest, vthost::GrantState answer);

    /// Takes the switch requests switchRequested() announced, oldest first. GUI thread.
    /// @return The requests, as the daemon sent them.
    [[nodiscard]] std::vector<vthost::proto::SwitchRequest> takeSwitchRequests();

    /// Answers a request takeSwitchRequests() handed out, on the connection up now: one that came after the
    /// request's own was lost is not the one the daemon waits on, and ignores it. A no-op once detached.
    /// @param reply The request's id, what came of it, and why.
    void replySwitch(vthost::proto::SwitchReply reply);

    /// @return The daemon workspace ids currently known, ascending: every workspace the daemon lists, the
    ///         selected one and the others. A thread-safe snapshot.
    [[nodiscard]] std::vector<uint64_t> workspaceIds() const;

    /// @return The daemon's most recent tab/pane layout for @p daemonWorkspace, or
    ///         nullopt if that workspace is unknown. A thread-safe copy — the reactor
    ///         thread updates it. The GUI reconstructs one OS window's tree from it.
    [[nodiscard]] std::optional<vthost::proto::LayoutState> layout(uint64_t daemonWorkspace) const;

    /// @return The primary (lowest-id) daemon workspace's layout, or nullopt if none has
    ///         arrived yet. Convenience for the single-workspace path.
    [[nodiscard]] std::optional<vthost::proto::LayoutState> layout() const;

    /// @return The primary (lowest-id) daemon workspace's layout converted for
    ///         `vtworkspace::realizeLayoutTab` (an empty layout if none has arrived), plus
    ///         its leaf→remote-session map. The layout executor realizes this to
    ///         reproduce the daemon tree.
    [[nodiscard]] vthost::client::WireLayout wireLayout() const;

    /// @return The layouts the GUI reconciles against, as one snapshot of every workspace: those of the last
    ///         push the daemon ended (proto::LayoutBatchEnd) when Capability::LayoutBatch is shared, so the
    ///         frames of a push still arriving never mix with the last one's; the latest otherwise. A copy
    ///         of pointers, cheap to take once per reconcile and share between its OS windows. Any thread.
    [[nodiscard]] LayoutSnapshot settledLayouts() const;

    /// @return The settled layout (@see settledLayouts) of @p daemonWorkspace; null if it has none.
    /// @param daemonWorkspace The daemon's workspace id.
    [[nodiscard]] std::shared_ptr<vthost::proto::LayoutState const> settledLayout(
        uint64_t daemonWorkspace) const;

    /// @return The settled layout (@see settledLayouts) of the primary (lowest-id) workspace; null if none.
    [[nodiscard]] std::shared_ptr<vthost::proto::LayoutState const> settledLayout() const;

    /// Binds the NEXT createPty() to remote session @p session (instead of popping
    /// the FIFO pending queue). The layout executor calls this — via
    /// applyLayoutToWindow's beforeLeafSeed — right before each pane's backing
    /// session is created, so the imminent pane binds to exactly that session.
    void setNextBindSession(uint64_t session);

    /// Brackets a layout realization. While set, canCreateSession() reports true
    /// even with no pending session — during realization panes are bound by
    /// setNextBindSession, not the FIFO queue.
    void setRealizingLayout(bool realizing);

    /// GUI thread. Once the GUI reconciled a layout of a reconnect: re-sends every bound pane's size once
    /// (never the client area), even though no display geometry changed. No-op otherwise.
    void reassertGeometryAfterReconcile();

    /// GUI thread. Has every later reconnect follow what a launch would when none of the workspaces it
    /// resumes survived (ResumeFallback::Launch), as the link that did so for this daemon ended and this one
    /// takes its place (@see passOnLaunchResume). Configured at construction otherwise; changed live because
    /// which of a daemon's links is its launch's outlives the link that was.
    void resumeAsLaunch();

    /// @return What a reconnect follows when none of the workspaces it resumes survived.
    [[nodiscard]] ResumeFallback resumeFallback() const;

    /// GUI thread. Resolves the session settings anew -- after a configuration reload -- and states them:
    /// to the daemon at once, for the sessions this connection creates from now on, and in the hello of
    /// every later connection. Keeps the settings last stated when they cannot be resolved; no-op without
    /// a session settings source.
    void restateSessionSettings();

    /// GUI thread. Has this connection select @p workspace: the daemon pushes its layout and its sessions'
    /// screens in full from then on and the other workspaces' panes as summaries (directory()), and a
    /// reconnect resumes it. Takes effect when the daemon's next layout batch states the selection
    /// (selectedWorkspace()).
    /// @param workspace The daemon workspace; 0 asks the daemon to create one and select it.
    /// @return The request's number, which its answer names (takeSelectAnswers()); why nothing was sent
    ///         otherwise: the connection is not up, or the daemon predates the selection.
    /// @param directoryFrom The daemon session of the pane asking for a new workspace (@p workspace 0): the
    ///        new workspace's first shell starts in that pane's working directory. Ignored otherwise.
    [[nodiscard]] std::expected<uint64_t, std::string> select(
        uint64_t workspace, std::optional<uint64_t> directoryFrom = std::nullopt);

    /// GUI thread. Tells the daemon whether this window has the user's focus; it decides from the reports of
    /// every attached window which one raises for a bell (raiseRole()). Stated again to every connection that
    /// replaces a lost one.
    /// @param focus The window's focus now.
    void reportFocus(vthost::proto::WindowFocus focus);

    /// GUI thread.
    /// @return Whether this window rings and raises for a bell or notification, as the daemon decided it for
    ///         what the GUI thread handles now: a role change takes effect behind the events sent before it,
    ///         so a pane's bell is judged by the role it arrived under. Quiet until the daemon says. A
    ///         session folds the bells of one GUI-loop turn into one post (TerminalSession::bell), so of two
    ///         bells straddling a role change within that turn, the second is judged by the first's role.
    [[nodiscard]] vthost::proto::RaiseRole raiseRole() const noexcept;

    /// @return The daemon workspace this window selects, 0 for none: as the daemon stated it with the last
    ///         layout batch. Takes _mutex.
    [[nodiscard]] uint64_t selectedWorkspace() const noexcept;

    /// @return Every workspace of the daemon this window can list, with the tabs and panes of each and what
    ///         is known of them: the layouts the daemon pushes for every workspace, and the status it
    ///         pushes for every pane, mirrored or not. Takes _mutex.
    [[nodiscard]] window::WorkspaceDirectory directory() const;

    /// @return Whether any pty this controller made is still alive, bound to a remote session or detached
    ///         from one: the controller must outlive it, as the pty calls back into it until its destruction
    ///         unbinds it here. Takes _mutex. @see paneUnbound
    [[nodiscard]] bool bindsPanes() const;

    /// @return Whether a layout realization is in progress (so a split it triggers
    ///         is built locally, not re-authored on the daemon).
    [[nodiscard]] bool isRealizingLayout() const;

    /// Pairs local @p tab with the daemon tab @p wireTab, the one it shows: a later push names the daemon
    /// tab, and the reconcile brings this tab in line with it. A daemon tab is paired with one local tab at
    /// most, so a pairing the daemon tab had before goes. GUI thread.
    /// @param tab The local tab.
    /// @param wireTab The daemon's id of the tab (proto::WireTab::tabId).
    void pairTab(vtworkspace::TabId tab, uint64_t wireTab);

    /// @return The local tab paired with the daemon tab @p wireTab (@see pairTab), if any.
    /// @param wireTab The daemon's id of the tab.
    [[nodiscard]] std::optional<vtworkspace::TabId> tabPairedWith(uint64_t wireTab) const;

    /// Forgets the pairing of every local tab not among @p liveTabs: a closed tab's id names nothing, and
    /// the daemon's tab ids it was paired with may be handed out again.
    /// @param liveTabs The TabId values of the tabs that exist.
    void forgetClosedTabs(std::unordered_set<uint64_t> const& liveTabs);

    /// @return The daemon tab local @p tab was last reconciled against (@see pairTab), if any.
    /// @param tab The local tab.
    [[nodiscard]] std::optional<uint64_t> pairedWireTab(vtworkspace::TabId tab) const;

    // SessionFactory: hands out a ChannelPty bound to the next pending
    // remote session; cwd/command/profile do not apply to remote sessions.
    [[nodiscard]] std::unique_ptr<vtpty::Pty> createPty(
        std::optional<std::string> cwd,
        std::optional<vtbackend::PageSize> pageSize = std::nullopt,
        std::optional<vtpty::Process::ExecInfo> commandOverride = std::nullopt,
        std::optional<std::string> profileName = std::nullopt) override;

    [[nodiscard]] bool canCreateSession() const noexcept override;

    /// SessionFactory: every pane this factory makes mirrors a session the daemon hosts.
    [[nodiscard]] contour::session::SessionHosting hosting() const noexcept override
    {
        return contour::session::SessionHosting::Remote;
    }

    /// SessionFactory: the remote end is a Contour daemon.
    [[nodiscard]] contour::session::SessionHostKind hostKind() const noexcept override
    {
        return contour::session::SessionHostKind::Daemon;
    }

    /// SessionFactory: a GUI "new tab" is authored on the daemon (requestCreateTab)
    /// rather than created locally; the daemon's layout re-push reconciles it in.
    [[nodiscard]] bool requestRemoteTab(vtpty::Pty const* actingPty,
                                        contour::session::RemoteSpawn const& spawn) override
    {
        requestCreateTab(actingPty, spawn);
        return true;
    }

    /// SessionFactory: a GUI split is authored on the daemon; the layout re-push
    /// reconciles the new pane in. A split issued BY the reconciler itself (while
    /// realizing) is not re-authored — it builds the pane locally.
    [[nodiscard]] bool requestRemoteSplit(vtpty::Pty const* actingPty,
                                          bool vertical,
                                          contour::session::RemoteSpawn const& spawn) override
    {
        if (isRealizingLayout())
            return false;
        requestSplitPane(actingPty, vertical, spawn);
        return true;
    }

    /// SessionFactory: whether the daemon serves proto::MergeTabIntoSplit (Capability::TabMerge shared).
    /// Unsupported before the handshake and against an older daemon: a tab dropped on a pane then tears off.
    [[nodiscard]] contour::session::TabMergeSupport tabMergeSupport() const noexcept override;

    /// SessionFactory: a tab dropped on a pane's edge is merged into the split by the daemon, which moves its
    /// sessions and re-pushes the layout. Refused when a pty is not bound to a daemon session, or the daemon
    /// does not serve the merge; Local only while the reconciler applies a push.
    [[nodiscard]] contour::session::RemoteRouting requestRemoteTabMerge(
        std::span<vtpty::Pty const* const> tabPtys,
        vtpty::Pty const* targetPty,
        vtworkspace::SplitSide side) override;

    /// SessionFactory: whether the daemon moves panes and tabs for this client: it serves proto::MoveNode and
    /// ends each push (Capability::MoveNode and Capability::LayoutBatch shared), so that the GUI can realize
    /// a move from the whole push that made it. Unsupported before the handshake and against an older
    /// daemon, which a dragged pane is then refused by.
    [[nodiscard]] contour::session::MoveSupport moveSupport() const noexcept override;

    /// SessionFactory: a pane or tab dropped in a daemon-hosted window is moved by the daemon, which
    /// re-pushes the layouts of every window it touched; applyRemoteLayout() realizes them by moving the
    /// existing sessions. Refused when no session is named, a pane source names several, or the daemon does
    /// not serve the move; Local only while the reconciler applies a push. The daemon's answer is reported
    /// under the move's request number: moveRefused() for a refusal, takeLandedMoves() once the push that
    /// made it ended.
    [[nodiscard]] contour::session::RemoteRouting requestRemoteMove(
        contour::session::RemoteMove const& move) override;

    /// SessionFactory: the daemon's workspace verbs. Refused where the daemon does not share
    /// Capability::WorkspaceMeta, which it must to serve them.
    [[nodiscard]] contour::session::RemoteRouting requestRemoteRenameWorkspace(
        uint64_t workspace, std::string_view label) override;
    [[nodiscard]] contour::session::RemoteRouting requestRemoteMoveWorkspace(uint64_t workspace,
                                                                             uint32_t position) override;
    /// SessionFactory: titles the daemon's tab holding @p session (proto::EditTab), mirrored here or not.
    [[nodiscard]] contour::session::RemoteRouting requestRemoteTabTitle(uint64_t session,
                                                                        std::string_view title) override;
    [[nodiscard]] contour::session::RemoteRouting requestRemoteCloseWorkspace(uint64_t workspace) override;

    /// SessionFactory: the toggle of a split's axis in a daemon-hosted window is made by the daemon, whose
    /// layout push brings it in (a local flip would be reverted by the next one). Local when the daemon does
    /// not serve MoveNode, so an older daemon keeps the local toggle, and while the reconciler applies a
    /// push; Refused when the pane is bound to no session.
    [[nodiscard]] contour::session::RemoteRouting requestRemoteSplitOrientation(
        vtpty::Pty const* anchorPty, vtworkspace::SplitState axis) override;

    /// moveRefused()'s reason for a move that never reached the daemon: no connection was up to send it.
    /// Outside every vthost::NodeMoveError value, which is a byte.
    static constexpr int MoveNotSent = -1;

    /// The moves requestRemoteMove() asked for whose pushes have ended since the last call, so the GUI can
    /// tell its user where they landed once it has reconciled them. Taken before reading the settled
    /// layouts, which are then at least as new as each push named here. Any thread.
    /// @return Their request numbers, oldest first.
    [[nodiscard]] std::vector<uint64_t> takeLandedMoves();

    /// A select() in flight.
    struct SelectRequest
    {
        uint64_t request = 0; ///< Its request number, as select() returned it.
        uint64_t asked = 0;   ///< The workspace it asked for; 0 for a new one.
        bool operator==(SelectRequest const&) const = default;
    };

    /// The selections select() asked for whose answering pushes have ended since the last call: one that
    /// states another workspace than it asked for was not taken (the daemon has no such workspace, or could
    /// not make a new one). Any thread.
    /// @return The answers, oldest first.
    [[nodiscard]] std::vector<SelectAnswer> takeSelectAnswers();

    /// What the panes of the workspaces this connection does not select raised -- a bell, a desktop
    /// notification, a command that finished -- while this connection held the raise role
    /// (proto::RaiseRole::Raise) as each arrived: the window rings and notifies for them. Each is handed out
    /// once. Any thread.
    /// @return Their summaries, carrying only what was raised, oldest first.
    [[nodiscard]] std::vector<vthost::proto::PaneSummary> takeRaised();

    /// The moves in flight, by the serial each went out under: each move's request number, as
    /// requestRemoteMove() numbers it.
    using RequestsInFlight = std::map<uint64_t, uint64_t>;

    /// Takes out of @p inFlight the requests a push ended with @p causeSerial completes. The daemon names one
    /// cause per push, the last request it served before the push's end was sent, and serials only grow per
    /// connection: every request with a serial up to it was served by then, so it is complete as well -- one
    /// that pushed nothing would otherwise wait for the next reconnect.
    /// @param inFlight    The requests in flight, by serial; the completed ones are erased.
    /// @param causeSerial LayoutBatchEnd::causeSerial; 0 when no request of this client caused the push.
    /// @return What each request stands for, oldest first.
    template <typename Request>
    [[nodiscard]] static std::vector<Request> takeCompletedRequests(std::map<uint64_t, Request>& inFlight,
                                                                    uint64_t causeSerial)
    {
        auto completed = std::vector<Request> {};
        if (causeSerial == 0)
            return completed;
        auto const end = inFlight.upper_bound(causeSerial);
        for (auto const& request: std::ranges::subrange(inFlight.begin(), end) | std::views::values)
            completed.push_back(request);
        inFlight.erase(inFlight.begin(), end);
        return completed;
    }

    /// SessionFactory: the run of the daemon the last accepted connection reached.
    /// @return The daemon's instance; std::nullopt before a connection was accepted, and from a daemon that
    ///         states none.
    [[nodiscard]] std::optional<uint64_t> daemonInstance() const noexcept override;

    /// SessionFactory: the daemon session @p pty mirrors.
    /// @return Its id; std::nullopt for a pty not bound to one.
    [[nodiscard]] std::optional<uint64_t> hostSessionIdOf(vtpty::Pty const& pty) const noexcept override
    {
        return sessionForPty(&pty);
    }

    /// SessionFactory: the user moved a divider, so the daemon's model — what a re-attaching client
    /// rebuilds from — is told the new ratio. @see proto::ResizeSplit.
    void reportSplitRatio(vtpty::Pty const* firstPty, vtpty::Pty const* secondPty, double ratio) override;

    /// SessionFactory: the user changed how a tab is presented, so the daemon's model -- and every other
    /// client -- is told: one proto::EditTab per field that differs from the last LayoutState this
    /// controller holds for the tab (vthost::client::tabEditsToReport). Dropped while realizing a layout,
    /// whose changes came from the daemon, and for a tab the daemon has not pushed.
    void reportTabPresentation(contour::session::LocalTabPresentation const& presentation) override;

    /// SessionFactory: the user ended the pane backed by @p pty, so close it on the daemon too
    /// rather than leave the session running there with nothing showing it. The counterpart of
    /// `unbind()`, which merely forgets a binding and cannot tell why the pty went away.
    void requestRemoteClose(vtpty::Pty const* pty) override;

    /// SessionFactory: the pane backed by @p pty is detached while its daemon session runs on -- its window
    /// switched to another workspace, or its workspace's layout no longer names it, as a move took it to
    /// another -- so the binding goes now, and, unlike the pty's own end (unbind()), leaves no tombstone. A
    /// session still bound, or tombstoned, is not realized again, and the pane's session may be destroyed
    /// only after a switch back, or the switch that follows the move, has realized the workspace that holds
    /// it. GUI thread; takes _mutex. A pty that is not one of this controller's is left alone.
    void releaseRemoteSession(vtpty::Pty const* pty) override;

    /// SessionFactory: every pane's terminal mirrors one the daemon hosts, so an operation on the engine is
    /// sent there (proto::PerformOperation) and the mirror follows through its deltas. A pty that is not
    /// bound to a daemon session runs it here; without a connection every operation is Unreachable.
    [[nodiscard]] contour::session::HostedOperationRoute requestHostedOperation(
        vtpty::Pty const* pty, contour::session::HostedOperationRequest const& request) override;

    /// SessionFactory: the terminal built around one of our ptys arrived, so the session's mirror
    /// can be created and primed with whatever the daemon has already told us about it.
    /// The mirror also reports the command a restart stopped in the daemon's session to @p interruptions,
    /// posted to its context's (the session's, the GUI) thread.
    void bindTerminal(vtpty::Pty const* pty,
                      vtbackend::Terminal& terminal,
                      contour::session::InterruptionSink const& interruptions) override;

    /// @return Why the last connection ended for good, as the connection said it; empty when it did not (or
    ///         was stopped). Any thread.
    [[nodiscard]] std::string failure() const
    {
        auto const lock = std::lock_guard { _mutex };
        return _failure;
    }

    /// SessionFactory: Supported -- this controller is the attach to a daemon a pane's Reconnect seeks again.
    [[nodiscard]] contour::session::ReconnectSupport reconnectSupport() const noexcept override
    {
        return contour::session::ReconnectSupport::Supported;
    }

    /// SessionFactory: starts a new supervised run once the last one ended (refused, failed, or stopped),
    /// resuming the workspaces followed. The outcome arrives as connectionStateChanged() and, when it ends
    /// for good again, connectionClosed(). GUI thread.
    /// @return Nothing once started; AlreadyConnected while a run is up or being sought, NotAttached when
    ///         this controller never connected.
    [[nodiscard]] std::expected<void, contour::session::ReconnectError> reconnect() override;

    /// SessionFactory: when the layout the daemon pushed names the remote session @p endedPty mirrored, makes
    /// the next createPty() bind to it again. GUI thread.
    [[nodiscard]] contour::session::ReconnectBinding bindForReconnect(vtpty::Pty const* endedPty) override;

    /// SessionFactory: forgets the session bindForReconnect() named, when no pty was made for it. GUI thread.
    void cancelReconnectBinding() override;

  signals:
    /// A remote session appeared that has no local tab yet (fires on the
    /// reactor thread; connect queued).
    void remoteSessionDiscovered();

    /// The connection to the daemon ended for good: the first connect failed, a reconnect was refused, or
    /// stop() ended it. A connection that is lost and sought again does not end it (@see
    /// connectionStateChanged).
    void connectionClosed();

    /// connectionState() changed (fires on the reactor thread; connect queued).
    void connectionStateChanged();
    /// The attach has something new for whoever polls it (RemoteController::pollConnected): the connection
    /// phase settled, or the transport returned its connection. Fires on the reactor thread; connect queued.
    void attachProgressed();

    /// A pty this controller made was destroyed, whether or not it still bound its remote session (fires on
    /// whichever thread destroyed it; connect queued). @see bindsPanes
    void paneUnbound();

    /// After a reconnect: daemon workspaces this client showed that the daemon no longer has for it -- it
    /// restarted, or another client closed them -- for the GUI to close (fires on the reactor thread once the
    /// layouts of the workspaces that came instead are settled (settledLayouts()), ahead of their
    /// layoutChanged(); connect queued).
    /// @param daemonWorkspaces The lost workspaces' ids, as the layouts the GUI last settled named them: the
    ///        ids its windows are bound to, whichever connection pushed them.
    void workspacesLost(std::vector<uint64_t> daemonWorkspaces);

    /// The daemon says it restored its saved workspaces before this client attached, and how much came back
    /// (fires on the reactor thread, once per connection whose hello carries it; connect queued). Only the
    /// first Attach connection after a restore is told, so a second client never raises it, nor does a
    /// reconnect to the same daemon run; a reconnect to a restarted daemon that itself restored does.
    /// @param workspaces How many workspaces came back.
    /// @param tabs How many tabs.
    void workspaceRestored(quint32 workspaces, quint32 tabs);

    /// The daemon's tab/pane layout changed (fires on the reactor thread; connect
    /// queued). The GUI reconstructs its tab/split tree from layout() (B2).
    void layoutChanged();

    /// What directory() holds changed: once per layout batch the daemon ended, and once per flush of the
    /// status that arrived since the last one -- never once per PDU (fires on the reactor thread for a layout
    /// batch and on the GUI thread for a status flush; connect queued).
    void directoryChanged();

    /// A daemon's status-line extension asks this client's user something (fires on the reactor thread;
    /// connect queued). The GUI takes the questions with takePermissionRequests().
    void extensionPermissionRequested();

    /// The daemon asks this client to switch the window showing one of its workspaces (fires on the reactor
    /// thread; connect queued). The GUI takes the requests with takeSwitchRequests().
    void switchRequested();

    /// The daemon refused a tab merge this client asked for, and changed nothing (fires on the reactor
    /// thread; connect queued). The GUI tells the user.
    void tabMergeRefused();

    /// The daemon refused a move this client asked for, and changed nothing -- or the move could not be sent
    /// at all (fires on the reactor thread; connect queued). The GUI tells the user.
    /// @param reason Why, as a vthost::NodeMoveError's value, carried, not judged; MoveNotSent for a move no
    ///               connection could take.
    /// @param modelReason When @p reason is NodeMoveError::Model, why the model refused, as a
    ///                    vtworkspace::MoveError's value; 0 otherwise.
    /// @param request The number requestRemoteMove() was given for the move; 0 for one this client cannot
    ///                match to a request.
    void moveRefused(int reason, int modelReason, quint64 request);

  private:
    /// One discovered remote session awaiting a local tab.
    struct PendingSession
    {
        uint64_t session = 0;
        uint32_t columns = 80;
        uint32_t lines = 25;
    };

    /// One remote session's local binding.
    ///
    /// The pty stays for INPUT and resize (keystrokes and geometry travel out through its sinks);
    /// remote output no longer flows through it, because the mirror writes the terminal's grid
    /// directly. The terminal is therefore absent until `bindTerminal` announces it — a pty exists
    /// before the terminal built around it does — and the mirror until the reactor primes it, since
    /// what it is built with (where the daemon runs) is the connection's, which lives there.
    struct Binding
    {
        /// Which binding this is, never reused: a reconnect to a restarted daemon detaches every binding,
        /// whose pty still lives until its window closes, and a new session may take the old one's id.
        /// What such a pty sends is matched by this, never by the session id alone.
        uint64_t serial = 0;
        vtpty::ChannelPty* pty = nullptr;
        vtbackend::Terminal* terminal = nullptr;
        std::unique_ptr<vthost::client::ScreenMirror> mirror;
        /// Where the session is told what a restart stopped in it; empty until bindTerminal().
        contour::session::InterruptionSink interruptions {};
        /// What @ref interruptions was last told, so only a change is posted. Reactor thread, under _mutex.
        std::string interruptionTold {};
    };

  protected:
    /// The reactor's whole lifetime: connect, serve, notify. Takes the loop
    /// by pointer (coroutine reference parameters can dangle).
    [[nodiscard]] core::async::Task<void> runClient(core::net::EventLoop* loop) override;

    /// Tells whoever polls the attach that there is something new to look at (attachProgressed()).
    void notePhaseChange() override { emit attachProgressed(); }

    // RemoteController hooks: the attach-specific half of the shared connect lifecycle.
    [[nodiscard]] bool detachOnReactor() override
    {
        if (_supervisor == nullptr)
            return false;
        _supervisor->stop();
        return true;
    }
    void closeReactorBindings() override { closeAllBindings(); }
    /// A reactor that died mid-run never reaches the supervisor's own Failed, so this reports it: without
    /// connectionClosed() the GUI never stop()s the controller, and Reconnect is refused for good.
    void reactorDied(std::string const& reason) override;
    [[nodiscard]] std::string connectTimeoutMessage() const override
    {
        return "timed out waiting for the daemon's snapshot";
    }
    [[nodiscard]] std::string connectClosedMessage() const override
    {
        return "connection closed during attach";
    }

  private:
    /// Reactor-side, _mutex held: posts what @p binding's mirror knows of a restart's stopped command to its
    /// session, when that differs from what the session was last told.
    /// @param binding The binding whose mirror was just applied.
    static void tellInterruption(Binding& binding);

    /// Reactor-side: builds the client of one connection.
    /// @param loop The reactor's loop.
    /// @param connection The connection it runs over.
    /// @param intent What it asks to follow.
    [[nodiscard]] std::unique_ptr<vthost::client::NativeClient> makeClient(
        core::net::EventLoop& loop,
        std::unique_ptr<vthost::Connection> connection,
        vthost::proto::AttachIntent intent);

    /// Reactor-side: the supervisor's state changed.
    /// @param state Where the connection stands now.
    /// @param reason Why, for a lost, failed or refused one.
    void onConnectionState(vthost::client::ConnectionState state, std::string const& reason);

    /// @return The preference the app shows now as Config::colorPreference answers, else what
    ///         requestHostedOperation() last stated; Unstated before either. Any thread.
    [[nodiscard]] vthost::proto::WireColorPreference shownColors() const;

    /// Reactor-side: states the light/dark preference the app shows, if any, for @p session to the daemon.
    /// @param client The connection.
    /// @param session The session to state it for.
    void stateColorPreference(vthost::client::NativeClient& client, uint64_t session) const;

    /// Reactor-side, under _mutex: a reconnect was accepted. Judges which workspaces were lost, and makes the
    /// bindings ready for the new connection's snapshots: restarted mirrors when the same daemon answered,
    /// detached bindings when it restarted. The panes closed while the connection was down are closed on
    /// the daemon now when it is the same one, and forgotten when it restarted.
    /// @param held The caller's lock of _mutex, asked for so that no call can be made without it: the
    ///             layouts, landed moves and tab pairings this clears are read by the GUI thread under it.
    void adoptReconnect(std::lock_guard<std::mutex> const& held);

    /// Lets the tests run code on the reactor thread, against the client of the moment, to stage a
    /// connection that is ending before its state change is delivered -- a window no daemon can be made to
    /// open on demand.
    friend struct NativeControllerTestAccess;

    /// Reactor-side: sends again the closes the client wrote nowhere because its connection was ending, for
    /// the panes @p layout still lists -- the daemon never saw them. Each goes out once.
    /// @param layout A workspace's layout the daemon just pushed.
    void replayUnsentCloses(vthost::proto::LayoutState const& layout);

    /// Reactor-side: hands a system sleep or wake to the connection that is up. While none is, a wake cuts
    /// the reconnect backoff short (ClientSupervisor::retryNow()) and a sleep is dropped, as the next
    /// connection starts with a fresh liveness verdict anyway.
    /// @param event What the system did.
    void notePowerEvent(core::platform::SystemPowerEvent event);

    /// Reactor-side: closes @p session's pane on the daemon, or remembers to when no connection can take it.
    /// @param session The session whose pane the user closed.
    void closePaneOnReactor(uint64_t session);

    /// Reactor-side: applies @p delta through the session's mirror (if bound).
    void onUpdate(vthost::client::RemoteScreen const& screen, vthost::proto::Delta const& delta);

    /// Reactor-side: an image's pixels arrived (or it was dropped), so the mirror can place it.
    void onImage(vthost::client::RemoteScreen const& screen, uint32_t imageId);

    /// Reactor-side: reproduces a bell / desktop notification / OSC 52 clipboard write on the
    /// session's mirror terminal, so the frontend's own handling and permissions apply.
    void onSessionEvent(vthost::client::RemoteScreen const& screen,
                        vthost::proto::SessionEventPdu const& event);

    /// Reactor-side: stores the daemon's latest tab/pane layout and signals the
    /// GUI to reconcile its own tree against it.
    void onLayout(vthost::proto::LayoutState const& layout);

    /// Reactor-side: the daemon refused a move (proto::MoveNodeRefused) it was sent under @p serial.
    void onMoveNodeRefused(vthost::proto::MoveNodeRefused const& refused, uint64_t serial);

    /// Reactor-side: the daemon ended a push (proto::LayoutBatchEnd); marks the layouts settled and signals
    /// the GUI to reconcile the push whole.
    void onLayoutBatchEnd(vthost::proto::LayoutBatchEnd const& end);

    /// Reactor-side: the daemon asks for this client's pane sizes again (proto::PanesReprojected, which only
    /// a daemon built while it was sent on departures sends); re-asserts the panes from the GUI thread.
    void onPanesReprojected();

    /// Reactor thread. Merges what the daemon says changed about a pane field by field into the status store.
    void onPaneSummary(vthost::proto::PaneSummary const& summary);

    /// Reactor thread. Takes the role the daemon gives this connection.
    void onRaiseRole(vthost::proto::RaiseRole role);

    /// Reactor thread. Merges what the daemon states of a workspace field by field into the metadata store.
    void onWorkspaceMeta(vthost::proto::WorkspaceMeta const& meta);

    /// _mutex held. Has the GUI thread announce directoryChanged() once for every change of
    /// the status store until then.
    void scheduleStatusFlush();

    /// GUI thread. Announces the status that arrived since the last flush.
    void flushStatus();

    /// _mutex held. Forgets the status of sessions no layout names any more.
    void pruneStatus();

    /// _mutex held. The panes of the selected workspace are shown, so they have no output unseen.
    void clearSelectedActivity();

    /// Fails the attach with @p reason and ends the supervisor, so that nothing of the daemon arrives after.
    void failAttach(std::string reason);

    /// @return Whether the daemon ends its pushes (Capability::LayoutBatch shared). Under _mutex.
    [[nodiscard]] bool layoutBatchShared() const;

    /// Under _mutex: a layout of the connection up settled. Takes the reconnect's losses, which the GUI may
    /// be told of now -- unless this connection has pushed no frame yet (a push that ended empty), when
    /// nothing came instead and they wait.
    /// @return The workspaces to announce lost (workspacesLost()); empty while they wait.
    [[nodiscard]] std::vector<uint64_t> settle(std::lock_guard<std::mutex> const& held);

    /// Reactor-side: keeps a daemon extension's question for the GUI, and tells it.
    void onPermissionRequest(vthost::proto::PermissionRequest const& request);

    /// Reactor-side: keeps a daemon's switch request for the GUI, and tells it.
    void onSwitchRequest(vthost::proto::SwitchRequest const& request);

    /// Reactor-side: builds the mirror of a binding whose terminal arrived, judging once whether the
    /// daemon runs on the terminal's machine, and feeds it its full replay if the session's screen is
    /// already known.
    void primeBinding(uint64_t session);

    /// GUI-side (from a pane's pty resize sink, on whichever thread resized it): records the grid
    /// @p session's pane is now rendered at and schedules ONE coalesced geometry report.
    ///
    /// Coalescing matters: a window resize or a divider drag resizes every pane in the same layout
    /// pass, and reporting per pane would make the daemon re-project (and answer with a forced full
    /// snapshot) once per pane — on a partially-updated tree, since the other panes have not
    /// reported yet.
    /// @param session The remote session whose pane resized.
    /// @param serial The binding of that pane; a detached one's report is dropped.
    /// @param cells The pane's new grid.
    void reportPaneGeometry(uint64_t session,
                            uint64_t serial,
                            vtpty::PageSize cells,
                            std::optional<vtpty::ImageSize> pixels);

    /// What a flushGeometry() may state.
    enum class GeometryFlush : uint8_t
    {
        Report = 0, ///< A pane reported a grid: the client area if it moved, and the panes that did.
        PanesOnly,  ///< A pending re-assertion: every pane, and never the client area, which panes a
                    ///< reconcile just created at their birth size do not compose yet.
    };

    /// Sends the pending geometry report (GUI thread): the composed client area if it moved (unless
    /// @p scope is PanesOnly), plus every bound pane whose grid the daemon does not already have, or every
    /// one while a re-assertion is pending and the panes compose to an area.
    /// @param scope What this flush may state.
    void flushGeometry(GeometryFlush scope);

    /// The client area composed from the pane tree the daemon last pushed and the grids its leaves
    /// are rendered at (@see vthost::client::composeClientArea). Prefers the tab holding the pane
    /// that reported last — it is the one on screen — and otherwise takes the first tab that fully
    /// resolves, in daemon-workspace then tab order, so the choice never depends on hash order.
    /// Must be called under BOTH _mutex (for the layouts) and _geometryMutex (for the pane grids).
    /// @return The composed area, or nullopt while no tab fully resolves.
    [[nodiscard]] std::optional<vtpty::PageSize> composedClientArea() const;

    /// GUI-side (from the pty's destructor): counts the pty gone, forgets its binding, and — while the
    /// connection is still live (not `_stopped`) — tombstones the session id so a remote session that
    /// outlives its local pane cannot resurrect it through the next delta. Only the binding @p serial names:
    /// a detached one's pty leaves alone the binding a new session of the same id has.
    ///
    /// It does NOT decide whether the remote session should be closed: a pty is destroyed by a pane
    /// close, a window close, an app quit and a lost connection alike, and only the first of those
    /// means "end this session". That intent is authored upstream by whoever holds it
    /// (@see requestRemoteClose, contour::SessionEnd).
    void unbind(uint64_t session, uint64_t serial);

    /// unbind() under _mutex: the pty is counted gone whether or not its binding is still there to forget.
    void forgetBinding(uint64_t session, uint64_t serial);

    /// Drops @p binding and what its pane owned of the composed geometry. Under _mutex.
    void dropBinding(std::unordered_map<uint64_t, Binding>::iterator binding);

    /// @return Whether @p session is bound, by the binding @p serial names. Takes _mutex.
    [[nodiscard]] bool isBinding(uint64_t session, uint64_t serial) const;

    /// Closes every bound pty (EOF to its session) — the disconnect path.
    void closeAllBindings();

    /// Resolves the session settings on the calling (GUI) thread, then posts @p create to the reactor
    /// thread, preceded by restating those settings when they changed, so the session it creates gets the
    /// configuration as it is now.
    /// @param create The creation verb to send.
    void postSessionCreation(std::function<void(vthost::client::NativeClient&)> create);

    // The reactor and the connect state machine (_reactor, _mutex, _connected, _state, _failure,
    // _stopped) live in RemoteController; the registry below is attach-specific.
    std::shared_ptr<vthost::ITransportFactory> _transport;
    vthost::proto::AttachIntent _intent; ///< What the first connection follows.
    /// @see Config::resumeFallback. Under _mutex: read on the reactor as a reconnect is made, and changed by
    /// resumeAsLaunch() on the GUI thread.
    ResumeFallback _resumeFallback;
    SwitchRequests _switchRequests;          ///< @see Config::switchRequests
    std::string _profileName;                ///< Stated in every ClientHello.
    std::filesystem::path _workingDirectory; ///< Stated in the first connection's ClientHello only.
    /// This client's profile as stated in the ClientHello; the daemon applies it to the sessions this
    /// connection creates. Under _mutex: restateSessionSettings() replaces it on the GUI thread, and
    /// makeClient() reads it on the reactor.
    std::optional<vtbackend::Settings> _attachSettings;
    /// This client's profile as it is now, restated before every session this connection creates.
    SessionSettingsSource _sessionSettings;
    /// The preference the app shows now, asked as every hello is built. @see Config::colorPreference
    ColorPreferenceSource _colorPreference;
    vthost::latency::StartupTimeline& _timeline;
    vthost::client::BackoffSchedule _backoff;
    /// Filled, under _mutex, by the first callback after each accepted connection's ServerHello. @see
    /// negotiated()
    std::optional<vthost::proto::Negotiated> _negotiated;
    std::string _daemonRelease; ///< @see daemonRelease(). Guarded by _mutex.
    /// Takes the handshake's outcome from the client once it has one. Call under _mutex.
    void captureHandshake();
    /// Forgets what the daemon said of each session's echo, and tells every bound pane it is Unknown. Call
    /// under _mutex, on the reactor thread, while the connection is down.
    void forgetInputEchoes();
    std::deque<PendingSession> _pending; ///< Discovered remote sessions without a local tab.
    std::unordered_map<uint64_t, Binding> _bindings;
    /// What the daemon last said each session's terminal does with typed text (SessionInputEcho), kept so a
    /// pane bound after it arrived starts from it. Guarded by _mutex.
    std::unordered_map<uint64_t, vtpty::InputEcho> _inputEchoes;
    /// The daemon's latest tab/pane tree per workspace id (B2/B4). Ordered so workspaceIds()
    /// yields a stable ascending order (the primary workspace is the lowest id).
    LayoutSnapshot _layouts;
    /// The layouts the GUI reconciles against (@see settledLayouts): _layouts as the last LayoutBatchEnd left
    /// them, or as each LayoutState left them from a daemon that does not end its pushes. Written by the
    /// reactor, read by the GUI thread: under _mutex.
    LayoutSnapshot _settledLayouts;
    /// What this client last reported for a tab, by the daemon's tab id, until the daemon's next layout
    /// push of that window says how it presents the tab now: a second report before then is compared with
    /// this, not with a layout that predates the first, so what was already sent is not sent again.
    /// Guarded by _mutex.
    std::map<uint64_t, vthost::client::TabPresentation> _reportedPresentations;
    /// The remote session the NEXT createPty() binds to, set by the layout
    /// executor before each pane's backing session is created (GUI thread).
    std::optional<uint64_t> _nextBindSession;
    /// The light/dark preference the app last stated through requestHostedOperation(), whether or not the
    /// daemon was reachable to hear it: what shownColors() falls back to when Config::colorPreference answers
    /// none, so a change made while the connection was down, or before a pane existed, is not lost. Atomic:
    /// written on the GUI thread only, read on the reactor.
    std::atomic<vthost::proto::WireColorPreference> _statedColors {
        vthost::proto::WireColorPreference::Unstated
    };
    bool _realizingLayout = false; ///< canCreateSession() is true while a layout is being realized.
    /// The request number of each move in flight, by the serial it went out under (@see requestRemoteMove).
    /// Reactor thread only.
    RequestsInFlight _moveRequests;
    /// The request numbers of the moves whose pushes ended, for takeLandedMoves(). Under _mutex.
    std::vector<uint64_t> _landedMoves;
    /// Each select() in flight, by the serial it went out under. Reactor thread only.
    std::map<uint64_t, SelectRequest> _selectRequests;
    /// The request number the next select() gets. Under _mutex.
    uint64_t _nextSelectRequest = 1;
    /// The answers to the selections whose pushes ended, for takeSelectAnswers(). Under _mutex.
    std::vector<SelectAnswer> _selectAnswers;
    /// The daemon tab each local tab shows, by local TabId value (@see pairTab).
    /// Written by the GUI thread, cleared by the connection thread when the daemon restarted (its tab ids
    /// count from 1 again): under _mutex.
    std::unordered_map<uint64_t, uint64_t> _wireTabOf;
    /// Session ids this client is done with: a pane the user closed or ended, released while still
    /// attached. Their remote sessions can outlive the local pane by a round trip — the daemon has
    /// not honored the `ClosePane` yet, or never will because the pane was only detached from — and
    /// they keep emitting deltas meanwhile; `onUpdate` ignores tombstoned ids so a released pane
    /// never reappears.
    ///
    /// Never pruned, and that is sound rather than a leak: session ids are monotonic (never reused),
    /// so a lingering tombstone can never suppress a genuinely new session, the set is bounded by
    /// this connection's lifetime at 8 bytes an entry, and a reattach is a fresh controller with an
    /// empty set. A reconnect to a restarted daemon, whose ids count from 1 again, empties it. The daemon's
    /// own removal notice arrives as a re-pushed `LayoutState`, which the reconciler acts on structurally
    /// rather than per id (@see applyRemoteLayout).
    std::unordered_set<uint64_t> _closedSessions;
    /// The sessions whose panes the user closed while no connection was up, in the order closed: their
    /// ClosePane goes out once the same daemon answers again, and is dropped with the tombstones when
    /// another run of it does (@see adoptReconnect). Reactor-owned.
    std::vector<uint64_t> _pendingCloses;
    /// The sessions whose ClosePane was issued while the connection was ending, so nothing was written: sent
    /// again when the daemon of the same run (a non-zero instance on both sides, @see adoptReconnect) pushes
    /// a layout still listing them, and dropped when another run, or a legacy daemon that names none,
    /// answers. Reactor-owned.
    std::vector<uint64_t> _unsentCloses;
    /// The daemon extensions' questions the GUI has not taken yet, oldest first. Guarded by _mutex.
    std::vector<vthost::proto::PermissionRequest> _permissionRequests;
    /// The daemon's switch requests the GUI has not taken yet, oldest first. Guarded by _mutex.
    std::vector<vthost::proto::SwitchRequest> _switchRequestQueue;
    /// Guards the geometry bookkeeping below, and ONLY that.
    ///
    /// A lock of its own because the resize sink is reached from inside a `_mutex`-holding frame:
    /// `onUpdate`/`primeBinding` hold `_mutex` while the mirror writes the pane's terminal, and a
    /// terminal write can resize the screen (`setStatusDisplay` does, when the status line appears or
    /// goes) — which fires the pty's resize sink straight back into `reportPaneGeometry`. With one
    /// mutex guarding both the bindings and the geometry, that is a self-deadlock on a non-recursive
    /// mutex; the reactor thread simply stops.
    ///
    /// Lock ORDER where both are needed (`flushGeometry`): `_mutex` first, then this. Never the
    /// reverse — `reportPaneGeometry` takes this one alone precisely so the callback cannot close a
    /// cycle.
    mutable std::mutex _geometryMutex;
    /// The grid each bound pane is rendered at, keyed by remote session — seeded with the pane's
    /// birth size and updated by every resize report. The client area is composed from ALL of them,
    /// so a single pane's resize still yields the whole content area (a pane's own grid is what the
    /// daemon mistook for the client area, halving it on every split).
    std::unordered_map<uint64_t, vtpty::PageSize> _paneSizes;
    /// The binding each of those panes belongs to (@see Binding::serial), so a detached pane's resize is not
    /// taken for the new session that has its id.
    std::unordered_map<uint64_t, uint64_t> _paneSerials;
    /// What was last sent upstream, so an unchanged report is dropped: a window resize fires the
    /// sink once per pane, and the daemon answers every accepted client area with a forced full
    /// snapshot per followed session.
    std::optional<vtpty::PageSize> _lastReportedArea;
    /// The pixel size of one cell, derived from the panes' reports (pixels / cells); the latest wins. What
    /// every ResizeRequest/ResizePane states and the next hello repeats. Under _geometryMutex.
    std::optional<vtpty::ImageSize> _cellPixelSize;
    /// The cell last stated to the daemon (a hello, or a report); a change of _cellPixelSize re-reports.
    std::optional<vtpty::ImageSize> _lastReportedCell;
    std::unordered_map<uint64_t, vtpty::PageSize> _lastReportedPaneSizes;
    /// The last area stated to any connection (a hello, or a resize request). Never reset, not even for a
    /// restarted daemon: it is what the window is, and the next hello states it while no layout composes one.
    std::optional<vtpty::PageSize> _lastStatedArea;
    /// The area this connection's hello stated; spent by the first flushGeometry() that composes it (which
    /// then does not state it again), or by a report that composes another.
    std::optional<vtpty::PageSize> _helloArea;
    /// Whether every pane's size is yet to be re-asserted after the daemon re-projected them.
    enum class GeometryReassert : uint8_t
    {
        Idle = 0,
        Pending, ///< Set by a reconnect or proto::PanesReprojected; spent by the first flush that composes.
    };
    GeometryReassert _geometryReassert = GeometryReassert::Idle;
    /// The session whose pane last reported a grid; anchors which tab the client area is composed
    /// from, @see vthost::client::composeAnchoredClientArea. Outlives the individual flush it was
    /// set for — a report is not "consumed" — but never its pane: `unbind` clears it, so it either
    /// names a live pane or nothing.
    std::optional<uint64_t> _geometryAnchor;
    bool _geometryFlushScheduled = false; ///< A coalesced flush is already queued.
    /// Runs the connections; set, under _mutex, while runClient() runs. Reactor-owned.
    vthost::client::ClientSupervisor* _supervisor = nullptr;
    /// The client of the connection that is up, and null whenever none is: every verb sent to the daemon
    /// goes through it, so nothing is written to a connection still in its handshake, or to none.
    /// Reactor-owned; set and cleared under _mutex.
    vthost::client::NativeClient* _client = nullptr;
    UserAttention _userAttention = UserAttention::NotNeeded; ///< Under _mutex.
    /// What the connection was before it said NeedsAttention; under _mutex.
    vthost::client::ConnectionState _stateUnderAttention = vthost::client::ConnectionState::Reconnecting;
    vthost::client::ConnectionState _connection =
        vthost::client::ConnectionState::Connecting; ///< Under _mutex.
    /// Which run of the daemon the last accepted connection reached; 0 for unknown. Under _mutex.
    uint64_t _daemonInstance = 0;
    /// How many connections were accepted so far; reactor-owned.
    uint64_t _acceptedConnections = 0;
    /// The connection input may be written to: its number among the accepted ones, or 0 while none is up.
    /// Read by every pty's write sink, on whichever thread writes, without a lock -- the sink can be reached
    /// while the reactor holds _mutex -- and checked again on the reactor, so input typed while no
    /// connection was up is dropped, never replayed into whatever shell answers after a reconnect.
    std::atomic<uint64_t> _inputConnection = 0;
    std::atomic<std::size_t> _droppedInputBytes = 0; ///< Since the connection was lost; for the log.
    /// The workspaces the last reconnect lost, announced as the first layout that came instead settles: with
    /// that frame, or at the end of its push when the daemon batches them (onLayoutBatchEnd) -- never at the
    /// end of a push with no frame in it. Under _mutex.
    std::vector<uint64_t> _lostWorkspaces;
    /// How far the connection up has come in giving the GUI layouts. Under _mutex.
    enum class ConnectionLayouts : uint8_t
    {
        Awaited =
            0,    ///< No frame yet: a push it ended was empty, and nothing has come instead of what was lost.
        Received, ///< Frames, none settled yet: the GUI still shows what an earlier connection settled.
        Settled,  ///< Settled: the GUI shows _settledLayouts.
    };
    ConnectionLayouts _connectionLayouts = ConnectionLayouts::Awaited;
    /// The workspaces the GUI's windows show, which a reconnect computes its losses against, and the run of
    /// the daemon whose ids they are: the settled layouts of the last connection that settled any. Taken as a
    /// connection that did is replaced (adoptReconnect()), and kept over one lost before anything it pushed
    /// settled: that one showed the GUI nothing, and its frames may name another run's workspaces. Under
    /// _mutex.
    struct ShownWorkspaces
    {
        std::vector<uint64_t> ids;
        uint64_t daemonInstance = 0; ///< 0 for unknown.
    };
    ShownWorkspaces _shownWorkspaces;
    /// The workspace the window selects, 0 for none: a stand-in for the first connection's resume
    /// (makeClient()); the client and the supervisor hold the live selection. Refreshed from the client with
    /// each connection and layout batch; forgotten for a restarted daemon, whose ids name other workspaces.
    /// Under _mutex.
    uint64_t _selected = 0;
    /// What is known of each pane of each workspace, by daemon session: the program-status records the daemon
    /// pushes for every session, and the fields of its PaneSummary for the panes of the workspaces this
    /// window does not select. Under _mutex.
    std::map<uint64_t, window::PaneStatus> _paneStatus;
    /// The last serial a pane flag took (window::DirectoryPane::activitySerial), one counter for every pane
    /// and kind so that a pane forgotten and met again cannot repeat one. Under _mutex.
    uint64_t _flagSerial = 0;
    /// What the daemon stated of each workspace (label, control id, git state), by daemon workspace id, as
    /// WorkspaceMeta merged it. Under _mutex.
    std::map<uint64_t, window::WorkspaceMetadata> _workspaceMeta;
    /// Whether a flush of _paneStatus is queued for the GUI thread. Under _mutex.
    enum class StatusFlush : uint8_t
    {
        Idle,
        Queued,
    };
    StatusFlush _statusFlush = StatusFlush::Idle;
    /// What the daemon last said this connection does for raising, as the reactor has it: what a summary is
    /// judged by as it arrives. Under _mutex.
    vthost::proto::RaiseRole _raiseRole = vthost::proto::RaiseRole::Quiet;
    /// The same, as the GUI thread has it: updated through the GUI thread's queue, behind the events posted
    /// before the change (@see raiseRole()). GUI thread only.
    vthost::proto::RaiseRole _guiRaiseRole = vthost::proto::RaiseRole::Quiet;
    /// What the panes this connection does not mirror raised while it held the raise role, for takeRaised():
    /// judged by the role as it stood when each arrived. Under _mutex.
    std::vector<vthost::proto::PaneSummary> _raised;
    /// The focus last reported, stated again to each connection that replaces a lost one. Under _mutex.
    vthost::proto::WindowFocus _focus = vthost::proto::WindowFocus::Unfocused;
    /// ResumeFallback::Nothing: the workspaces a reconnect resumed whose first layout is still to come. One
    /// that comes with no tab is lost, as Resume would have judged it: its session ended meanwhile. Under
    /// _mutex.
    std::set<uint64_t> _resumedAwaitingLayout;
    uint64_t _nextBindingSerial = 1; ///< @see Binding::serial. Under _mutex.
    /// The ptys createPty() bound that are not destroyed yet, still bound or detached: each captures this
    /// controller in its sinks and its destructor's callback, so the controller must outlive every one (@see
    /// bindsPanes). Not _bindings.size(): a restarted daemon's reconnect detaches every binding while the
    /// ptys live on. Under _mutex.
    std::size_t _livePtys = 0;
    /// Keeps the power monitor's handler, which posts each event to notePowerEvent() on the reactor; empty
    /// without a monitor. ~NativeController() stops and joins the reactor before members die, so a handler
    /// may still post onto the stopped loop until this unsubscribes: that is safe because
    /// ReactorThread::post() only queues (the lambda is destroyed with the loop, never run), not because of
    /// where this member sits. Unsubscribing waits out a handler that is running.
    core::platform::PowerSubscription _powerSubscription;
};

/// Gives @p live up when its connect failed: stops it, cuts its connections to @p owner and hands it to
/// @p abandoned rather than destroying it: the daemon start its transport began may still be under way -- a
/// first start copies this version's image -- and destroying the transport waits for that start to end
/// (UnixTransportFactory's destructor), which would hold the caller back all that while. The caller keeps
/// @p abandoned until it ends.
/// @param live The controller to give up; null afterwards.
/// @param abandoned Receives it.
/// @param owner Every connection from @p live to this object is cut.
void abandonConnect(std::unique_ptr<NativeController>& live,
                    std::unique_ptr<NativeController>& abandoned,
                    QObject const* owner);

} // namespace contour::remote
