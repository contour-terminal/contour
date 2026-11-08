// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// One daemon connection and the windows it serves.

#include <contour/remote/DaemonLinkHost.hpp>
#include <contour/remote/NativeController.hpp>
#include <contour/remote/RemoteLayout.hpp>
#include <contour/session/RestoreSummaryPolicy.hpp>
#include <contour/session/SessionFactory.hpp>
#include <contour/session/WindowBinding.hpp>
#include <contour/window/DaemonDriftChoice.hpp>
#include <contour/window/DaemonDriftModel.hpp>
#include <contour/window/NoticeQueue.hpp>

#include <vtbackend/screen/Settings.hpp>

#include <core/platform/Clock.hpp>
#include <core/platform/SystemPower.hpp>

#include <QtCore/QObject>

#include <chrono>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include <boxed-cpp/boxed.hpp>
#include <vthost/Transport.hpp>
#include <vthost/client/ClientSupervisor.hpp>
#include <vthost/latency/StartupTimeline.hpp>
#include <vthost/proto/Pdu.hpp>
#include <vtworkspace/Primitives.hpp>

namespace vtextension
{
class IGrantStore;
}

namespace contour::session
{
class TerminalSessionManager;
}

namespace contour::window
{
class WindowController;
}

namespace contour::remote
{

namespace detail::tags
{
    struct DaemonLinkId
    {
    };
} // namespace detail::tags

/// Whether a freshly spawned window was taken by a link to show one of its daemon workspaces.
enum class WindowAdoption : uint8_t
{
    Declined = 0, ///< Not the link's: the window creates its own first tab.
    Adopted,      ///< The link fills it from a daemon workspace: it must create no tab of its own.
};

/// Everything a DaemonLink is built with.
struct DaemonLinkConfig
{
    /// How the link reaches its daemon.
    std::shared_ptr<vthost::ITransportFactory> transport;
    /// Which daemon workspaces it follows.
    vthost::proto::AttachIntent intent;
    /// The remote host, as `contour client --host` named it; empty for a local daemon and `--connect-tcp`.
    std::string hostLabel;
    /// Whether the daemon is this machine's, which this client may restart and upgrade.
    window::DaemonLocality locality = window::DaemonLocality::Remote;
    /// The profile the hello names.
    std::string profileName;
    /// The absolute directory the session the first connection creates starts in, stated in that hello only;
    /// empty for the profile's.
    std::filesystem::path workingDirectory;
    /// The emulation settings the hello states; none to have the daemon use its own profile.
    std::optional<vtbackend::Settings> attachSettings;
    /// Asked before each session the controller creates.
    NativeController::SessionSettingsSource sessionSettings;
    /// The light/dark preference the first hello states.
    std::optional<vtbackend::ColorPreference> colorPreference;
    /// This client's release, which the daemon's is judged against for the drift badge.
    std::string clientRelease;
    /// What the connect budget is measured on. Borrowed, never null; it must outlive the link.
    core::platform::IClock const* clock = nullptr;
    /// What the controller stamps. Borrowed, never null; it must outlive the link.
    vthost::latency::StartupTimeline* timeline = nullptr;
    /// Where the controller hears of system sleep and wake. Borrowed; may be null.
    core::platform::ISystemPowerMonitor* powerMonitor = nullptr;
    /// How long its reconnects wait after a loss.
    vthost::client::BackoffSchedule backoff {};
};

/// One daemon connection and the windows it serves.
///
/// Owns the NativeController and the attach bookkeeping: which daemon workspace each OS window shows, the
/// boot window's adoption, the window being spawned, the drift badge and the answers to the daemon's
/// extension questions. One link may serve several windows. GUI thread only, apart from what the controller
/// does on its own reactor.
class DaemonLink final: public session::WindowBinding
{
  public:
    /// Names a link among the application's.
    using Id = boxed::boxed<uint32_t, detail::tags::DaemonLinkId>;

    /// Builds the link and its controller, not yet connecting.
    /// @param id The link's name among the application's.
    /// @param config What it connects with.
    /// @param host The application. Borrowed; it must outlive the link.
    /// @param sessions The manager the link's windows live in. Borrowed; the link must not touch it once it
    ///        is gone, so stop() the link before the manager is destroyed.
    DaemonLink(Id id,
               DaemonLinkConfig config,
               IDaemonLinkHost& host,
               session::TerminalSessionManager& sessions);
    ~DaemonLink() override;

    DaemonLink(DaemonLink const&) = delete;
    DaemonLink& operator=(DaemonLink const&) = delete;
    DaemonLink(DaemonLink&&) = delete;
    DaemonLink& operator=(DaemonLink&&) = delete;

    /// @return The link's name among the application's.
    [[nodiscard]] Id id() const noexcept { return _id; }

    /// @return The controller, until the attach was given up (awaitAttached()); null after.
    [[nodiscard]] NativeController* liveController() noexcept { return _controller.get(); }

    /// @return Whether awaitAttached() succeeded: the attach settled connected, and was not given up.
    [[nodiscard]] bool isAttached() const noexcept { return _state == AttachState::Attached; }

    /// @return The controller as the factory of the link's sessions; the given-up one after awaitAttached()
    ///         gave up, which creates nothing.
    [[nodiscard]] session::SessionFactory& sessions() noexcept override;

    /// @return The remote host the link is attached to; empty for a local daemon, `--connect-tcp`, and until
    ///         awaitAttached() succeeded.
    [[nodiscard]] std::string_view hostLabel() const noexcept override;

    /// Forgets @p window as one the link serves. Its daemon workspace's mapping is kept, so that the window
    /// its user closed does not open again (@see reconcileAttachWindows()).
    /// @param window The closed window.
    void windowClosed(vtworkspace::WorkspaceId window) override;

    /// @return The OS windows the link serves (@see bindPendingWindow) and has not been told closed, in
    ///         binding order.
    [[nodiscard]] std::span<vtworkspace::WorkspaceId const> windows() const noexcept { return _windows; }

    /// @return What the link's drift badge shows.
    [[nodiscard]] window::DaemonDriftModel& drift() noexcept { return _drift; }

    /// Wires the controller's signals to the link's windows, installs the attach-window binder on the
    /// manager, and starts the connect on the controller's reactor. Call once.
    void start();

    /// Waits for the connect start() began, with what remains of @p budget; gives the controller up when it
    /// does not connect in time (@see awaitConnectedOrAbandon()).
    /// @param budget How long the connect may take since start().
    /// @return Nothing once attached; why not otherwise.
    [[nodiscard]] std::expected<void, std::string> awaitAttached(std::chrono::milliseconds budget);

    /// Shows the drift badge when the daemon runs another release than this client, and wires its actions to
    /// the host's upgrade and restart, for a local daemon. Follows the daemon through an upgrade: the badge
    /// reads "Upgrading daemon…" while the connection is held, and shows the drift anew once it is back.
    /// No-op once given up.
    void offerDaemonRestartIfDrifted();

    /// Ends the connection ahead of a restart of its daemon: every pane of its windows notes the transport
    /// lost, then the controller stops, so that a reconnect does not start a daemon of its own. No-op once
    /// given up.
    void stopForDaemonRestart();

    /// Stops the controller, and serves no window any more: what they make from then on is local
    /// (@see session::TerminalSessionManager::releaseBinding). Idempotent.
    void stop();

    /// Brings the local OS windows in line with the daemon's workspaces: the primary daemon workspace maps
    /// onto the boot window, every other one holding a tab gets an OS window of its own (openWindowFor()),
    /// and each window's tab/pane tree is reconciled. Idempotent; re-run on every layout change. No-op once
    /// given up.
    void reconcile();

    /// Called from a freshly spawned window's Main.qml (through the manager's attach-window binder): binds it
    /// to the daemon workspace staged for it, or, while the boot window is still to adopt one, to the
    /// primary daemon workspace. Every window it is called for is served by the link from then on
    /// (session::TerminalSessionManager::bindWindow), adopted or not: one link serves every window of the
    /// process it attached. No-op once given up.
    /// @param window The freshly created controller of the spawned OS window.
    /// @return Whether the link adopted the window.
    [[nodiscard]] WindowAdoption bindPendingWindow(window::WindowController& window);

    /// Records the daemon-workspace → OS-window mapping and reconciles that daemon workspace into it at once:
    /// the one place the "a mapping always implies a reconcile" invariant lives.
    /// @param daemonWorkspace The daemon workspace id.
    /// @param local The local workspace of the OS window that mirrors it.
    void bindDaemonWorkspace(uint64_t daemonWorkspace, vtworkspace::WorkspaceId local);

    /// Forgets a restore summary no window took, once a layout has been reconciled and no window of it is
    /// still to be bound: a toast about a restore must not surface in a window opened later.
    void expireRestoreSummaryAfterLayout();

  private:
    /// Whether the attach is still to settle, succeeded, or was given up.
    enum class AttachState : uint8_t
    {
        Connecting = 0,
        Attached,
        GivenUp,
    };

    void connectReconnectSignals();
    void closeLostWindows(std::vector<uint64_t> const& daemonWorkspaces);

    /// Serves @p window: binds it to this link on the manager, and records it among windows().
    /// @param window A window of the manager.
    void serve(vtworkspace::WorkspaceId window);
    void showDaemonDrift();
    void askHostedExtensions();
    void requireGuiThread() const;

    Id _id;
    IDaemonLinkHost& _host;
    session::TerminalSessionManager& _sessions;
    std::string _hostLabel;
    window::DaemonDriftModel _drift; ///< Declared before the controllers: destroyed after them.
    window::DaemonLocality _locality;
    std::string _clientRelease;
    AttachState _state = AttachState::Connecting;

    /// The live controller; null once given up. Remote-backed sessions hold ptys that unregister from it.
    std::unique_ptr<NativeController> _controller;
    /// The controller given up on, stopped: kept rather than destroyed, because its destruction waits for a
    /// daemon start still under way (@see awaitConnectedOrAbandon()). Null otherwise.
    std::unique_ptr<NativeController> _givenUp;

    /// Where the answers to the daemon's extensions are recorded, and so sent; made with the first question.
    /// Declared after the controller, which it sends through. Its deleter is chosen where it is made, so that
    /// this class is laid out the same without CONTOUR_EXTENSIONS, which never makes one.
    std::unique_ptr<vtextension::IGrantStore, void (*)(vtextension::IGrantStore*)> _grants { nullptr,
                                                                                             nullptr };

    /// Each daemon workspace mapped to the OS window that mirrors it.
    AttachWindowMap _attachWindowMap;
    /// The OS windows bound to a daemon workspace, and not told closed (@see windows()).
    std::vector<vtworkspace::WorkspaceId> _windows;
    /// The daemon workspace whose OS window is being spawned right now: reconcile() stages it just before
    /// openWindowFor(), and that window claims it through bindPendingWindow(). Single-slot: the QML load is
    /// synchronous, so a stage is always consumed before the next is made.
    std::optional<uint64_t> _pendingAttachWorkspace;
    /// The layouts of the reconcile under way, one snapshot for every window of it -- one whose OS window
    /// opens meanwhile included -- so that all of them are brought to the same push. Null between reconciles.
    LayoutSnapshot const* _reconcilingLayouts = nullptr;
    /// Whether the boot window has adopted the daemon's primary workspace yet. Not "the map is empty": after
    /// a reconnect lost every window, the windows that came instead are new OS windows.
    session::AttachBoot _attachBoot = session::AttachBoot::Pending;
    /// The daemon release the drift badge was last assessed for, to notice a daemon that came back changed.
    std::string _shownDaemonRelease;

    /// The receiver of every controller connection the link makes; declared last, so that it is destroyed
    /// first and no queued signal reaches a link being torn down.
    QObject _receiver;
};

/// The daemon workspace the boot (first) OS window should adopt when attaching, rather than authoring
/// a fresh tab. In attach mode the boot window mirrors the daemon's primary (lowest-id) workspace; a
/// later OS window is not a boot window (@p anyWindowMapped is then true). Pure so the boot-adoption
/// rule is unit-testable without the QML boot flow.
/// @param anyWindowMapped True if at least one OS window is already bound to a daemon workspace.
/// @param daemonWorkspaceIds The daemon's known workspace ids, ascending (workspaceIds() ordering).
/// @return The daemon workspace to adopt, or nullopt if this is not the boot window, or the daemon has
///         not reported a workspace yet.
[[nodiscard]] std::optional<std::uint64_t> primaryDaemonWorkspaceToAdopt(
    bool anyWindowMapped, std::vector<std::uint64_t> const& daemonWorkspaceIds);

} // namespace contour::remote
