// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// `ServedSessions` -- the served tabs of one SessionHost: hosted sessions that run no program, whose screen
/// a daemon-scoped extension draws through a host-owned vtpty::ChannelPty, and whose keyboard and mouse input
/// the host buffers for it to read.

#include <vtbackend/core/Primitives.hpp>

#include <vtpty/ChannelPty.hpp>

#include <cstddef>
#include <cstdint>
#include <expected>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <vtcontrol/Uuid.hpp>
#include <vthost/ConnectionId.hpp>
#include <vthost/ExtensionHosting.hpp>
#include <vthost/SessionEnd.hpp>
#include <vthost/proto/Pdu.hpp>
#include <vtworkspace/Primitives.hpp>

namespace vthost
{

struct SessionSpawnRequest;

/// The most input one served tab buffers for its extension: what one read of it takes. A chunk the terminal
/// writes that does not fit whole is dropped whole, never cut, so that its reader never sees half a sequence.
inline constexpr std::size_t MaxServedInputBytes = std::size_t { 64 } * 1024;

/// Whether a host admits a served spawn.
enum class ServedAdmission : uint8_t
{
    Open = 0, ///< It does.
    Closed,   ///< A live upgrade runs: what it would have to close again is refused until it ends.
};

/// Told the outcome of a switch a window was asked for. @see ServedTabHost::askToSwitch
using SwitchCompletion = std::function<void(proto::SwitchReply const&)>;

/// What ServedSessions reaches of its host: the narrow loop-thread face SessionHost gives it. Every member is
/// called on the host's loop thread only.
class ServedTabHost
{
  public:
    ServedTabHost() = default;
    ServedTabHost(ServedTabHost const&) = delete;
    ServedTabHost(ServedTabHost&&) = delete;
    ServedTabHost& operator=(ServedTabHost const&) = delete;
    ServedTabHost& operator=(ServedTabHost&&) = delete;
    virtual ~ServedTabHost() = default;

    /// Creates a served tab in @p workspace, activated there as every new tab is.
    /// @param workspace Where.
    /// @param request The spawn: its `served` names the device.
    /// @return Its session; nullopt when it was refused (the workspace is gone, a live upgrade runs).
    [[nodiscard]] virtual std::optional<vtworkspace::SessionId> createServedTab(
        vtworkspace::WorkspaceId workspace, SessionSpawnRequest request) = 0;

    /// Makes @p session's tab the active one of its workspace, and its pane the tab's active pane.
    virtual void activate(vtworkspace::SessionId session) = 0;

    /// Ends @p session for @p cause.
    virtual void closeServed(vtworkspace::SessionId session, SessionEnd cause) = 0;

    /// Asks the window on @p connection to switch, as SessionHost::requestSwitch() does.
    /// @param connection The window's connection.
    /// @param leaving The workspace that window shows, and so leaves.
    /// @param target Where it should go.
    /// @param done Told the outcome, exactly once, on the loop thread.
    virtual void askToSwitch(ConnectionId const& connection,
                             vtworkspace::WorkspaceId leaving,
                             proto::SwitchTargetWire target,
                             SwitchCompletion done) = 0;

    /// Tells the window on @p connection to raise itself and focus @p session's pane (sends RevealPane).
    virtual void revealTo(ConnectionId const& connection, vtworkspace::SessionId session) = 0;

    /// @return The connections whose windows show @p workspace, the one focused last first.
    [[nodiscard]] virtual std::vector<ConnectionId> showing(vtworkspace::WorkspaceId workspace) const = 0;

    /// @return The workspace @p session's tab is in now; nullopt for a session the model does not hold.
    [[nodiscard]] virtual std::optional<vtworkspace::WorkspaceId> workspaceOf(
        vtworkspace::SessionId session) const = 0;

    /// @return The UUID @p workspace is known by, as a switch target names it.
    [[nodiscard]] virtual std::string workspaceRef(vtworkspace::WorkspaceId workspace) const = 0;
};

/// The served tabs of one SessionHost, and the production IServedTabs: makes the device each one runs on,
/// holds what each one's terminal wrote until its extension reads it, and keeps the mirror of every tab that
/// extensions reach from their own thread (@see IServedTabs for the thread rules).
///
/// Each extension serves at most one tab per workspace: opening it again for the same workspace -- open or
/// still pending, even within one batch of calls -- answers the same id and reveals it. The workspace is the
/// one the origin's connection selects as the mirror holds it: the loop publishes every attached
/// connection's selection (publishWorkspace()), so an open racing a switch of that window lands in the
/// workspace the user was looking at a moment before -- the switch that follows is the user's own.
class ServedSessions final: public IServedTabs
{
  public:
    /// Raises a hint for an extension, from any thread; never waits.
    using Raise = std::function<void(std::string_view extension, ServedHint hint)>;

    /// @param ids Where the UUIDs of served tabs come from; thread-safe.
    /// @param origins Which connection each origin stands for.
    /// @param raise Tells an extension its tabs changed: their input, their size, their number.
    /// @param post Hands a job to the host's loop.
    /// @param host The host's loop-thread face.
    ServedSessions(std::shared_ptr<vtcontrol::UuidSource> ids,
                   IExtensionActions const& origins,
                   Raise raise,
                   OwnerPost post,
                   ServedTabHost& host);

    ServedSessions(ServedSessions const&) = delete;
    ServedSessions(ServedSessions&&) = delete;
    ServedSessions& operator=(ServedSessions const&) = delete;
    ServedSessions& operator=(ServedSessions&&) = delete;
    ~ServedSessions() override = default;

    /// Makes the device of a served tab: what is fed to it is its screen's output; what its terminal writes
    /// is appended to its input in whole chunks while they fit (@see MaxServedInputBytes); a resize is
    /// recorded. Any thread.
    /// @param size The size the device starts at.
    /// @return The device, shared with the session that adopts it.
    [[nodiscard]] std::shared_ptr<vtpty::ChannelPty> makeDevice(vtbackend::PageSize size);

    /// @param device A device makeDevice() or open() made.
    /// @return What its terminal wrote and nobody read yet; empty for a device this did not make. Any thread.
    [[nodiscard]] std::string inputOf(vtpty::ChannelPty const& device) const;

    /// Records which workspace @p connection's window selects, which open() reads. Loop thread.
    /// @param connection An attached connection.
    /// @param selected Its selection; nullopt once it selects none, or detached.
    void publishWorkspace(ConnectionId const& connection, std::optional<vtworkspace::WorkspaceId> selected);

    /// Records the size new served tabs start at: the host's client area. Loop thread.
    void publishPageSize(vtbackend::PageSize size);

    /// Records whether a served tab may be opened: refused (Limit) while a live upgrade runs. Loop thread.
    void publishAdmission(ServedAdmission admission);

    /// Records that @p session's tab moved, whole, to @p workspace: the extension's dashboard of that
    /// workspace now, and no longer of the one it was opened in. Loop thread; a session that serves no tab
    /// is ignored.
    void sessionMoved(vtworkspace::SessionId session, vtworkspace::WorkspaceId workspace);

    /// Forgets @p session's tab once its session ended, and tells its extension, unless it closed it itself.
    /// Loop thread; a session that served no tab is ignored.
    void sessionEnded(vtworkspace::SessionId session);

    [[nodiscard]] std::expected<std::string, ServedTabError> open(std::string_view extension,
                                                                  std::string_view title,
                                                                  std::string_view moduleDigest,
                                                                  std::string_view scheme,
                                                                  Origin origin) override;
    [[nodiscard]] std::expected<void, ServedTabError> close(std::string_view extension,
                                                            std::string_view tab) override;
    [[nodiscard]] std::expected<void, ServedTabError> reveal(std::string_view extension,
                                                             std::string_view tab,
                                                             Origin origin) override;
    [[nodiscard]] std::vector<ServedTabInfo> list(std::string_view extension) const override;
    [[nodiscard]] std::expected<std::string, ServedTabError> read(std::string_view extension,
                                                                  std::string_view tab) override;
    [[nodiscard]] std::expected<void, ServedTabError> write(std::string_view extension,
                                                            std::string_view tab,
                                                            std::string_view bytes) override;
    void closeAll(std::string_view extension) override;

  private:
    struct DeviceState;

    /// One device, as long as anyone holds it, and what it buffered.
    struct Entry
    {
        std::weak_ptr<vtpty::ChannelPty const> device;
        std::shared_ptr<DeviceState> state;
    };

    /// Where one tab of the mirror stands.
    enum class TabPhase : uint8_t
    {
        Pending = 0, ///< Its creation is posted.
        Cancelled,   ///< Closed while pending: its creation closes the device and creates nothing.
        Open,        ///< Its session exists.
    };

    /// One served tab, as the mirror holds it.
    struct Tab
    {
        std::string id;                                ///< Its pane's UUID, as text.
        std::string extension;                         ///< Who serves it.
        std::string scheme;                            ///< What it was opened as.
        vtworkspace::WorkspaceId workspace;            ///< Where it is: opened, or moved to whole.
        TabPhase phase = TabPhase::Pending;            ///< Whether its session exists.
        std::shared_ptr<vtpty::ChannelPty> device;     ///< What it runs on.
        std::shared_ptr<DeviceState> state;            ///< What its device recorded.
        std::optional<vtworkspace::SessionId> session; ///< Its session, once Open.
    };

    /// A device and what its sinks record.
    struct Tracked
    {
        std::shared_ptr<vtpty::ChannelPty> device;
        std::shared_ptr<DeviceState> state;
    };

    /// Makes a device, and remembers it for inputOf(). Call holding _mutex.
    /// @param size The size it starts at.
    /// @param changed Tells its extension that its input or size changed; empty for none.
    [[nodiscard]] Tracked track(vtbackend::PageSize size, std::function<void()> changed);

    /// @return @p extension's tab @p id in the mirror, unless it was cancelled; null otherwise. Call holding
    ///         _mutex.
    [[nodiscard]] Tab* ownedBy(std::string_view extension, std::string_view id);

    /// @return The workspace @p connection selects, as published; nullopt for none. Call holding _mutex.
    [[nodiscard]] std::optional<vtworkspace::WorkspaceId> selectedBy(ConnectionId const& connection) const;

    /// Creates the session of the pending tab @p id, or drops it if it was cancelled meanwhile. Loop thread.
    void create(std::string const& id, std::string const& title, std::string const& moduleDigest);

    /// Activates @p session's tab and tells the window on @p connection, which shows @p origin; for a tab in
    /// another workspace, that window is asked to switch first. Loop thread.
    void revealOnLoop(vtworkspace::SessionId session,
                      ConnectionId const& connection,
                      vtworkspace::WorkspaceId origin);

    /// Removes @p id from the mirror. Call holding _mutex.
    void forget(std::string_view id);

    std::shared_ptr<vtcontrol::UuidSource> _ids; ///< Where the UUIDs of served tabs come from.
    IExtensionActions const& _origins;
    Raise _raise;
    OwnerPost _post;
    ServedTabHost& _host;
    mutable std::mutex _mutex; ///< Guards everything below.
    std::vector<Entry> _devices;
    std::vector<Tab> _tabs;
    std::vector<std::pair<ConnectionId, vtworkspace::WorkspaceId>> _selected;
    vtbackend::PageSize _pageSize { vtbackend::LineCount(24), vtbackend::ColumnCount(80) };
    ServedAdmission _admission = ServedAdmission::Open;
};

} // namespace vthost
