// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// `OwnedPanes` -- the agent panes daemon-scoped extensions open in a SessionHost: which pane is whose, kept
/// in a mutex-guarded mirror the extensions' lane reads and writes, with every model change posted to the
/// host's loop; persisted per extension (OwnedPaneStore) so that a live upgrade or a keeper recovery keeps
/// it.

#include <vtbackend/vt/ProgramStatus.hpp>

#include <vtpty/Process.hpp>

#include <array>
#include <atomic>
#include <cstdint>
#include <expected>
#include <functional>
#include <mutex>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <vtcontrol/Uuid.hpp>
#include <vthost/ConnectionWorkspaces.hpp>
#include <vthost/ExtensionHosting.hpp>
#include <vthost/OwnedPaneStore.hpp>
#include <vthost/ServedSessions.hpp>
#include <vtworkspace/Primitives.hpp>

namespace vthost
{

/// The status records a pane's status is read from, most trusted first: the first one present wins, never
/// the newest. The Claude Code hooks' record (`contour agent setup`, origin Integration), then the root
/// record Claude Code reports itself (origin Program), which says working and done but not reliably blocked.
inline constexpr auto PaneStatusSources = std::array<std::string_view, 2> { "claude-code", "" };

/// @param records A session's status records.
/// @return The state and message of the first PaneStatusSources row that names a present record; nullopt for
///         none.
[[nodiscard]] std::optional<PaneStatus> paneStatusOf(std::span<vtbackend::ProgramStatusRecord const> records);

/// The model calls OwnedPanes posts, which SessionHost implements (as ServedTabHost for served tabs). Every
/// member is called on the host's loop thread only.
class OwnedPaneHost
{
  public:
    OwnedPaneHost() = default;
    OwnedPaneHost(OwnedPaneHost const&) = delete;
    OwnedPaneHost(OwnedPaneHost&&) = delete;
    OwnedPaneHost& operator=(OwnedPaneHost const&) = delete;
    OwnedPaneHost& operator=(OwnedPaneHost&&) = delete;
    virtual ~OwnedPaneHost() = default;

    /// Creates the tab of an agent pane in @p workspace, in the background (vtworkspace::TabActivation): @p
    /// spawn's program, started in its directory or not at all, its pane known by @p pane.
    /// @return Its session; nullopt when the spawn failed or the workspace is gone.
    [[nodiscard]] virtual std::optional<vtworkspace::SessionId> createOwnedTab(
        OwnedPaneSpawn const& spawn, vtcontrol::Uuid pane, vtworkspace::WorkspaceId workspace) = 0;

    /// Ends @p session, as its pane's close asked.
    virtual void closeOwnedSession(vtworkspace::SessionId session) = 0;

    /// @return Whether an agent's spawn can start its program in a directory held open: decided as the spawn
    ///         decides its sandbox policy, so that no policy reaches a spawn that would fail on it.
    [[nodiscard]] virtual vtpty::HeldDirectorySpawn heldDirectorySpawn() const = 0;

    /// @return The host's first workspace; nullopt when it holds none.
    [[nodiscard]] virtual std::optional<vtworkspace::WorkspaceId> firstWorkspace() const = 0;

    /// Unlike the calls above, safe on any thread: it reads only what is fixed at construction.
    /// @return Whether @p spawn's program is found where its spawn will look it up.
    [[nodiscard]] virtual bool findsProgram(OwnedPaneSpawn const& spawn) const = 0;
};

/// What OwnedPanes reaches of its host: every collaborator its rules need, at construction.
struct OwnedPanesServices
{
    IOwnedPaneStore& store;     ///< The registry files.
    vtcontrol::UuidSource& ids; ///< Mints pane UUIDs; thread-safe.
    OwnedPaneHost& host;        ///< The loop-side model calls.
    IServedTabLookup const&
        servedTabs;             ///< `nearTab` -> its tab's workspace; who typed or clicked into its tabs now.
    IExtensionActions& origins; ///< origin -> ConnectionId, claimed by a pane-reveal.
    IConnectionWorkspaces const&
        workspaces;                    ///< ConnectionId -> selected workspace; the most recently focused.
    IServedAdmission const& admission; ///< The upgrade latch.
    ServedTabHost& tabHost;            ///< The reveal walk's loop-side calls (revealSession()).
    OwnerPost post;                    ///< Posts to the loop.
    std::function<void(std::string_view extension)> panesChanged; ///< Raises ServedHint::Panes; any thread.
};

/// A pane the host holds after a restore, as reconcile() is told it.
struct HostedPane
{
    vtcontrol::Uuid pane;                ///< Its UUID.
    vtworkspace::SessionId session;      ///< Its session.
    std::optional<PaneStatus> status {}; ///< Its status now (paneStatusOf()).
};

/// Whether the registry has been checked against the panes the host holds.
enum class Reconciliation : uint8_t
{
    Unreconciled = 0, ///< Read from the files only: before the host restored its sessions.
    Reconciled,       ///< Checked: every entry names a pane the host holds.
};

/// The production IOwnedPanes (@see IOwnedPanes for the thread rules).
///
/// It is built in SessionHost's member-initializer list, before the host restores a live upgrade's or a
/// keeper's sessions, or a snapshot -- and before the daemon world, whose boot extensions may list their
/// panes at once. So it starts from the raw files, Unreconciled: list() and open() answer NotReconciled
/// ("retry later") until reconcile() ran on the loop after every restore path, which drops the entries whose
/// pane is not there (a cold restart empties the registry; an upgrade or a recovery keeps it), saves, and
/// raises Panes for every extension that had entries or was told to retry.
class OwnedPanes final: public IOwnedPanes
{
  public:
    /// Loads the registry files; Reconciliation::Unreconciled.
    /// @param services What it reaches of its host.
    explicit OwnedPanes(OwnedPanesServices services);

    OwnedPanes(OwnedPanes const&) = delete;
    OwnedPanes(OwnedPanes&&) = delete;
    OwnedPanes& operator=(OwnedPanes const&) = delete;
    OwnedPanes& operator=(OwnedPanes&&) = delete;
    ~OwnedPanes() override = default;

    [[nodiscard]] std::expected<std::string, OwnedPaneError> open(OwnedPaneSpawn spawn) override;
    [[nodiscard]] std::expected<void, OwnedPaneError> close(std::string_view extension,
                                                            std::string_view pane) override;
    [[nodiscard]] std::expected<std::vector<OwnedPaneInfo>, OwnedPaneError> list(
        std::string_view extension) const override;
    [[nodiscard]] std::expected<void, OwnedPaneError> reveal(std::string_view extension,
                                                             std::string_view pane,
                                                             Origin origin) override;

    /// Checks the registry against the panes the host holds once it restored them. Loop thread, once.
    /// @param hosted Every pane the host holds.
    void reconcile(std::span<HostedPane const> hosted);

    /// Forgets @p session's pane once its session ended, saves, and raises Panes. Loop thread; a session no
    /// extension owns is ignored.
    void sessionEnded(vtworkspace::SessionId session);

    /// Records that @p session's program exited while its session is still hosted. Loop thread.
    void programExited(vtworkspace::SessionId session);

    /// Asks the host again whether an agent can start in a directory held open, after what decides it -- its
    /// profiles -- was reloaded. Loop thread.
    void heldDirectoriesChanged();

    /// Records @p session's status, raising Panes when its state or message changed. Loop thread; a session
    /// no extension owns is ignored.
    void statusChanged(vtworkspace::SessionId session, std::optional<PaneStatus> status);

    /// @return Whether @p session runs an extension's pane. Any thread.
    [[nodiscard]] bool owns(vtworkspace::SessionId session) const;

    /// @return Whether @p pane is an extension's, as the registry holds it -- before reconcile() too. Any
    ///         thread.
    [[nodiscard]] bool ownsPane(vtcontrol::Uuid const& pane) const;

  private:
    /// One owned pane, as the mirror holds it.
    struct Entry
    {
        std::string extension;
        vtcontrol::Uuid pane;
        std::string tag;
        OwnedPaneState state = OwnedPaneState::Pending;
        std::optional<vtworkspace::SessionId> session {};
        std::optional<PaneStatus> status {};
        PaneLiveness program = PaneLiveness::Alive;
    };

    /// Creates the pending pane @p pane, or drops it if it was cancelled meanwhile. Loop thread.
    void create(vtcontrol::Uuid pane, OwnedPaneSpawn const& spawn);

    /// @return Where a pane opened near @p nearTab opens. Loop thread.
    [[nodiscard]] std::optional<vtworkspace::WorkspaceId> workspaceFor(
        std::string_view extension, std::optional<std::string> const& nearTab) const;

    /// Writes @p extension's open panes to its registry file. Loop thread.
    void save(std::string_view extension);

    /// @return The entry of @p session; end() for none. Call holding _mutex.
    [[nodiscard]] std::vector<Entry>::iterator entryOf(vtworkspace::SessionId session);

    /// Notes that @p extension was told NotReconciled, so that the reconciliation raises Panes for it.
    /// Call holding _mutex.
    void askedTooEarly(std::string_view extension) const;

    OwnedPanesServices _services;
    /// OwnedPaneHost::heldDirectorySpawn(), as the loop last asked it: what the lane's open() refuses by.
    std::atomic<vtpty::HeldDirectorySpawn> _heldDirectories;
    mutable std::mutex _mutex; ///< Guards everything below.
    std::vector<Entry> _entries;
    Reconciliation _reconciliation = Reconciliation::Unreconciled;
    /// The extensions told NotReconciled: each hears Panes once reconciled, entries or not.
    mutable std::set<std::string, std::less<>> _askedTooEarly;
};

} // namespace vthost
