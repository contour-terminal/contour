// SPDX-License-Identifier: Apache-2.0
#include <vthost/ServedSessions.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <optional>
#include <string_view>
#include <utility>

#include <vthost/Logging.hpp>
#include <vthost/SessionHost.hpp>

namespace vthost
{

namespace
{
    /// What a reveal does once the window that was asked to switch answered.
    enum class SwitchedReveal : uint8_t
    {
        Nothing = 0,  ///< The window did not go there: the user was asked, refused, or it failed.
        ToOrigin,     ///< The asked window shows the tab's workspace now: it is told.
        ToShowingNow, ///< Another window already showed it and was focused: that one is told.
    };

    /// One row of what a reveal does, by the asked window's answer.
    struct SwitchedRevealRow
    {
        proto::SwitchOutcome outcome;
        SwitchedReveal reveal;
    };

    inline constexpr auto SwitchedReveals = std::array {
        SwitchedRevealRow { .outcome = proto::SwitchOutcome::Switched, .reveal = SwitchedReveal::ToOrigin },
        SwitchedRevealRow { .outcome = proto::SwitchOutcome::FocusedExisting,
                            .reveal = SwitchedReveal::ToShowingNow },
        SwitchedRevealRow { .outcome = proto::SwitchOutcome::PickerShown, .reveal = SwitchedReveal::Nothing },
        SwitchedRevealRow { .outcome = proto::SwitchOutcome::Refused, .reveal = SwitchedReveal::Nothing },
        SwitchedRevealRow { .outcome = proto::SwitchOutcome::Failed, .reveal = SwitchedReveal::Nothing },
    };

    /// @return What a reveal does once the asked window answered @p outcome.
    [[nodiscard]] constexpr SwitchedReveal switchedRevealOf(proto::SwitchOutcome outcome) noexcept
    {
        auto const row = std::ranges::find(SwitchedReveals, outcome, &SwitchedRevealRow::outcome);
        return row != SwitchedReveals.end() ? row->reveal : SwitchedReveal::Nothing;
    }
} // namespace

/// What one device's sinks record. Shared by the sinks, which may outlive the ServedSessions that made them.
struct ServedSessions::DeviceState
{
    /// Whether the input is dropping what does not fit: one episode is logged once.
    enum class InputOverflow : std::uint8_t
    {
        None = 0, ///< The last chunk fit.
        Dropping, ///< The last chunk was dropped.
    };

    /// @param initial The size the device starts at.
    /// @param changed Tells the extension its tab changed; empty for a device no extension serves.
    DeviceState(vtbackend::PageSize initial, std::function<void()> changed):
        size { initial }, changed { std::move(changed) }
    {
    }

    /// Appends @p chunk to the input, whole, if it fits; drops it whole otherwise. The first chunk into an
    /// empty buffer tells the extension.
    /// @param chunk One write of the terminal's.
    void accept(std::string_view chunk)
    {
        if (chunk.size() > MaxServedInputBytes)
        {
            sessionLog()("a served tab's input of {} bytes can never fit its bound of {} bytes; dropped",
                         chunk.size(),
                         MaxServedInputBytes);
            return;
        }
        auto wasEmpty = false;
        {
            auto const lock = std::lock_guard { mutex };
            if (input.size() + chunk.size() > MaxServedInputBytes)
            {
                if (std::exchange(overflow, InputOverflow::Dropping) == InputOverflow::None)
                    sessionLog()("a served tab's input is full; whole chunks are dropped until it is read");
                return;
            }
            overflow = InputOverflow::None;
            wasEmpty = input.empty();
            input.append(chunk);
        }
        if (wasEmpty && changed)
            changed();
    }

    /// Records the device's new size, and tells the extension.
    void resized(vtbackend::PageSize cells)
    {
        {
            auto const lock = std::lock_guard { mutex };
            size = cells;
        }
        if (changed)
            changed();
    }

    mutable std::mutex mutex; ///< Guards everything below but `changed`.
    std::string input;
    vtbackend::PageSize size;
    InputOverflow overflow = InputOverflow::None;
    std::function<void()> changed; ///< Fixed at construction; called holding no lock.
};

ServedSessions::ServedSessions(std::shared_ptr<vtcontrol::UuidSource> ids,
                               IExtensionActions const& origins,
                               Raise raise,
                               OwnerPost post,
                               ServedTabHost& host):
    _ids { std::move(ids) },
    _origins { origins },
    _raise { std::move(raise) },
    _post { std::move(post) },
    _host { host }
{
}

ServedSessions::Tracked ServedSessions::track(vtbackend::PageSize size, std::function<void()> changed)
{
    auto device = std::make_shared<vtpty::ChannelPty>(size);
    auto state = std::make_shared<DeviceState>(size, std::move(changed));
    device->setWriteSink(
        [state](std::string_view chunk, vtpty::InputKind /*kind*/) { state->accept(chunk); });
    device->setResizeSink([state](vtbackend::PageSize cells, std::optional<vtpty::ImageSize> /*pixels*/) {
        state->resized(cells);
    });
    // The entries of devices nobody holds any more go as a new one comes.
    std::erase_if(_devices, [](Entry const& entry) { return entry.device.expired(); });
    _devices.push_back(Entry { .device = device, .state = state });
    return Tracked { .device = std::move(device), .state = std::move(state) };
}

std::shared_ptr<vtpty::ChannelPty> ServedSessions::makeDevice(vtbackend::PageSize size)
{
    auto const lock = std::lock_guard { _mutex };
    return track(size, {}).device;
}

std::string ServedSessions::inputOf(vtpty::ChannelPty const& device) const
{
    auto state = std::shared_ptr<DeviceState> {};
    {
        auto const lock = std::lock_guard { _mutex };
        auto const found = std::ranges::find_if(
            _devices, [&](Entry const& entry) { return entry.device.lock().get() == &device; });
        if (found == _devices.end())
            return {};
        state = found->state;
    }
    auto const lock = std::lock_guard { state->mutex };
    return state->input;
}

void ServedSessions::publishWorkspace(ConnectionId const& connection,
                                      std::optional<vtworkspace::WorkspaceId> selected)
{
    auto const lock = std::lock_guard { _mutex };
    std::erase_if(_selected, [&](auto const& entry) { return entry.first == connection; });
    if (selected)
        _selected.emplace_back(connection, *selected);
}

void ServedSessions::publishPageSize(vtbackend::PageSize size)
{
    auto const lock = std::lock_guard { _mutex };
    _pageSize = size;
}

void ServedSessions::publishAdmission(ServedAdmission admission)
{
    auto const lock = std::lock_guard { _mutex };
    _admission = admission;
}

ServedSessions::Tab* ServedSessions::ownedBy(std::string_view extension, std::string_view id)
{
    auto const found = std::ranges::find_if(_tabs, [&](Tab const& tab) {
        return tab.id == id && tab.extension == extension && tab.phase != TabPhase::Cancelled;
    });
    return found != _tabs.end() ? &*found : nullptr;
}

std::optional<vtworkspace::WorkspaceId> ServedSessions::selectedBy(ConnectionId const& connection) const
{
    auto const found =
        std::ranges::find(_selected, connection, &std::pair<ConnectionId, vtworkspace::WorkspaceId>::first);
    return found != _selected.end() ? std::optional { found->second } : std::nullopt;
}

void ServedSessions::forget(std::string_view id)
{
    std::erase_if(_tabs, [&](Tab const& tab) { return tab.id == id; });
}

std::expected<std::string, ServedTabError> ServedSessions::open(std::string_view extension,
                                                                std::string_view title,
                                                                std::string_view moduleDigest,
                                                                std::string_view scheme,
                                                                Origin origin)
{
    auto const connection = _origins.connectionOf(extension, origin);
    if (!connection)
        return std::unexpected(ServedTabError::Gone);
    auto id = std::string {};
    auto reveal = std::optional<std::pair<vtworkspace::SessionId, vtworkspace::WorkspaceId>> {};
    auto created = false;
    {
        auto const lock = std::lock_guard { _mutex };
        if (_admission == ServedAdmission::Closed)
            return std::unexpected(ServedTabError::Limit);
        auto const workspace = selectedBy(*connection);
        if (!workspace)
            return std::unexpected(ServedTabError::Gone);
        // One dashboard per extension and workspace: a second open, pending or not, is the first one.
        auto const existing = std::ranges::find_if(_tabs, [&](Tab const& tab) {
            return tab.extension == extension && tab.workspace == *workspace
                   && tab.phase != TabPhase::Cancelled;
        });
        if (existing != _tabs.end())
        {
            // A pending one is activated by its creation.
            if (existing->session)
                reveal.emplace(*existing->session, *workspace);
            id = existing->id;
        }
        else
        {
            id = vtcontrol::toString(_ids->next());
            auto tracked = track(_pageSize, [raise = _raise, name = std::string(extension)] {
                raise(name, ServedHint::Tabs);
            });
            _tabs.push_back(Tab { .id = id,
                                  .extension = std::string(extension),
                                  .scheme = std::string(scheme),
                                  .workspace = *workspace,
                                  .phase = TabPhase::Pending,
                                  .device = std::move(tracked.device),
                                  .state = std::move(tracked.state),
                                  .session = std::nullopt });
            created = true;
        }
    }
    if (created)
        _post([this, id, title = std::string(title), moduleDigest = std::string(moduleDigest)] {
            create(id, title, moduleDigest);
        });
    else if (reveal)
        _post([this, session = reveal->first, connection = *connection, workspace = reveal->second] {
            revealOnLoop(session, connection, workspace);
        });
    return id;
}

void ServedSessions::create(std::string const& id, std::string const& title, std::string const& moduleDigest)
{
    auto spawn = ServedSpawn {};
    auto workspace = vtworkspace::WorkspaceId {};
    {
        auto const lock = std::lock_guard { _mutex };
        auto const found = std::ranges::find(_tabs, id, &Tab::id);
        if (found == _tabs.end() || found->phase == TabPhase::Open)
            return;
        if (found->phase == TabPhase::Cancelled)
        {
            // Closed before it was created: nothing is.
            found->device->close();
            forget(id);
            return;
        }
        spawn = ServedSpawn { .extension = found->extension,
                              .scheme = found->scheme,
                              .moduleDigest = moduleDigest,
                              .device = found->device };
        workspace = found->workspace;
    }
    auto const pane = vtcontrol::parseUuid(id);
    auto const extension = spawn.extension;
    auto const device = spawn.device;
    auto const session =
        pane ? _host.createServedTab(
                   workspace,
                   SessionSpawnRequest { .presetIds = PresetObjectIds { .pane = *pane, .tabName = title },
                                         .served = std::move(spawn) })
             : std::nullopt;
    auto cancelled = false;
    {
        auto const lock = std::lock_guard { _mutex };
        auto const found = std::ranges::find(_tabs, id, &Tab::id);
        cancelled = found == _tabs.end() || found->phase == TabPhase::Cancelled;
        if (session && !cancelled)
        {
            found->phase = TabPhase::Open;
            found->session = session;
        }
        else
            forget(id);
    }
    if (!session)
    {
        // Refused: its workspace went, or a live upgrade runs. The extension finds it gone from its tabs.
        sessionLog()("the served tab {} of {} could not be created", id, extension);
        device->close();
        _raise(extension, ServedHint::Tabs);
        return;
    }
    if (cancelled)
    {
        // Closed while it was being created.
        _host.closeServed(*session, SessionEnd::Requested);
        return;
    }
    _raise(extension, ServedHint::Tabs);
}

void ServedSessions::sessionMoved(vtworkspace::SessionId session, vtworkspace::WorkspaceId workspace)
{
    auto const lock = std::lock_guard { _mutex };
    if (auto const found = std::ranges::find(_tabs, std::optional { session }, &Tab::session);
        found != _tabs.end())
        found->workspace = workspace;
}

void ServedSessions::sessionEnded(vtworkspace::SessionId session)
{
    auto extension = std::string {};
    {
        auto const lock = std::lock_guard { _mutex };
        auto const found = std::ranges::find(_tabs, std::optional { session }, &Tab::session);
        if (found == _tabs.end())
            return;
        extension = found->extension;
        _tabs.erase(found);
    }
    _raise(extension, ServedHint::Tabs);
}

std::expected<void, ServedTabError> ServedSessions::close(std::string_view extension, std::string_view tab)
{
    auto session = std::optional<vtworkspace::SessionId> {};
    {
        auto const lock = std::lock_guard { _mutex };
        auto* const found = ownedBy(extension, tab);
        if (found == nullptr)
            return std::unexpected(ServedTabError::NotOwned);
        if (!found->session)
        {
            found->phase = TabPhase::Cancelled;
            return {};
        }
        session = found->session;
        forget(tab);
    }
    _post([this, session = *session] { _host.closeServed(session, SessionEnd::Requested); });
    return {};
}

std::expected<void, ServedTabError> ServedSessions::reveal(std::string_view extension,
                                                           std::string_view tab,
                                                           Origin origin)
{
    auto const connection = _origins.connectionOf(extension, origin);
    auto session = vtworkspace::SessionId {};
    auto workspace = vtworkspace::WorkspaceId {};
    {
        auto const lock = std::lock_guard { _mutex };
        auto const* const found = ownedBy(extension, tab);
        if (found == nullptr)
            return std::unexpected(ServedTabError::NotOwned);
        // A pending tab is activated by its creation.
        if (!found->session)
            return {};
        auto const selected = connection ? selectedBy(*connection) : std::nullopt;
        if (!selected)
            return std::unexpected(ServedTabError::Gone);
        session = *found->session;
        workspace = *selected;
    }
    _post([this, session, connection = *connection, workspace] {
        revealOnLoop(session, connection, workspace);
    });
    return {};
}

void ServedSessions::revealOnLoop(vtworkspace::SessionId session,
                                  ConnectionId const& connection,
                                  vtworkspace::WorkspaceId origin)
{
    auto const where = _host.workspaceOf(session);
    if (!where)
        return;
    if (*where == origin)
    {
        _host.activate(session);
        _host.revealTo(connection, session);
        return;
    }
    // In another workspace: the origin's window is asked to go there first, as the workspace it leaves.
    _host.askToSwitch(connection,
                      origin,
                      proto::SwitchTargetWire { .kind = proto::SwitchDaemonKind::Same,
                                                .daemon = {},
                                                .workspace = _host.workspaceRef(*where) },
                      [this, session, connection, target = *where](proto::SwitchReply const& reply) {
                          auto const reveal = switchedRevealOf(reply.outcome);
                          if (reveal == SwitchedReveal::Nothing || _host.workspaceOf(session) != target)
                              return;
                          _host.activate(session);
                          switch (reveal)
                          {
                              case SwitchedReveal::ToOrigin: _host.revealTo(connection, session); break;
                              case SwitchedReveal::ToShowingNow:
                                  if (auto const shows = _host.showing(target); !shows.empty())
                                      _host.revealTo(shows.front(), session);
                                  break;
                              case SwitchedReveal::Nothing: break;
                          }
                      });
}

std::vector<ServedTabInfo> ServedSessions::list(std::string_view extension) const
{
    auto const lock = std::lock_guard { _mutex };
    auto listed = std::vector<ServedTabInfo> {};
    for (auto const& tab: _tabs)
    {
        if (tab.extension != extension || tab.phase == TabPhase::Cancelled)
            continue;
        auto const size = [&] {
            auto const stateLock = std::lock_guard { tab.state->mutex };
            return tab.state->size;
        }();
        listed.push_back(ServedTabInfo { .tab = tab.id,
                                         .scheme = tab.scheme,
                                         .cols = unbox<uint32_t>(size.columns),
                                         .rows = unbox<uint32_t>(size.lines),
                                         .state = tab.phase == TabPhase::Open ? ServedTabState::Open
                                                                              : ServedTabState::Pending });
    }
    return listed;
}

std::expected<std::string, ServedTabError> ServedSessions::read(std::string_view extension,
                                                                std::string_view tab)
{
    auto state = std::shared_ptr<DeviceState> {};
    {
        auto const lock = std::lock_guard { _mutex };
        auto const* const found = ownedBy(extension, tab);
        if (found == nullptr)
            return std::unexpected(ServedTabError::NotOwned);
        // A pending tab has no terminal to have written anything yet.
        if (!found->session)
            return std::string {};
        state = found->state;
    }
    auto const lock = std::lock_guard { state->mutex };
    state->overflow = DeviceState::InputOverflow::None;
    return std::exchange(state->input, {});
}

std::expected<void, ServedTabError> ServedSessions::write(std::string_view extension,
                                                          std::string_view tab,
                                                          std::string_view bytes)
{
    auto device = std::shared_ptr<vtpty::ChannelPty> {};
    {
        auto const lock = std::lock_guard { _mutex };
        auto const* const found = ownedBy(extension, tab);
        if (found == nullptr)
            return std::unexpected(ServedTabError::NotOwned);
        device = found->device;
    }
    if (bytes.size() > MaxServedInputBytes)
        return std::unexpected(ServedTabError::Limit);
    if (device->isClosed())
        return std::unexpected(ServedTabError::Gone);
    device->feed(bytes);
    return {};
}

void ServedSessions::closeAll(std::string_view extension)
{
    auto sessions = std::vector<vtworkspace::SessionId> {};
    {
        auto const lock = std::lock_guard { _mutex };
        for (auto& tab: _tabs)
        {
            if (tab.extension != extension)
                continue;
            if (tab.session)
                sessions.push_back(*tab.session);
            else
                tab.phase = TabPhase::Cancelled;
        }
        std::erase_if(_tabs, [&](Tab const& tab) { return tab.extension == extension && tab.session; });
    }
    if (!sessions.empty())
        _post([this, sessions = std::move(sessions)] {
            for (auto const session: sessions)
                _host.closeServed(session, SessionEnd::Requested);
        });
}

} // namespace vthost
