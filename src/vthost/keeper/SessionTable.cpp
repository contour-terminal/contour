// SPDX-License-Identifier: Apache-2.0
#include <vthost/keeper/SessionTable.hpp>

#include <algorithm>
#include <utility>

namespace vthost::keeper
{

KeeperSessionId SessionTable::add(int pid, Descriptors descriptors)
{
    auto const id = KeeperSessionId { _nextId++ };
    _sessions.emplace(id,
                      Session { .pid = pid,
                                .descriptors = std::move(descriptors),
                                .exited = std::nullopt,
                                .hold = Hold::Held,
                                .killAt = std::nullopt });
    return id;
}

void SessionTable::place(KeeperSessionId id)
{
    if (auto const session = _sessions.find(id); session != _sessions.end())
        session->second.descriptors[slotOf(KeptDescriptor::Gate)].reset();
}

void SessionTable::release(KeeperSessionId id, core::platform::SteadyTimePoint killAt)
{
    auto const found = _sessions.find(id);
    if (found == _sessions.end())
        return;
    auto& session = found->second;
    if (session.exited)
    {
        _sessions.erase(found);
        return;
    }
    // All but the pidfd, which the event source watches the child by until it is reaped, when the session is
    // forgotten with it.
    for (auto const role: core::codec::EnumWire<KeptDescriptor>::Values)
        if (role != KeptDescriptor::ProcessHandle)
            session.descriptors[slotOf(role)].reset();
    session.hold = Hold::Released;
    session.killAt = session.killAt ? std::min(*session.killAt, killAt) : killAt;
}

void SessionTable::releaseAll(core::platform::SteadyTimePoint killAt)
{
    auto ids = std::vector<KeeperSessionId> {};
    for (auto const& [id, session]: _sessions)
        ids.push_back(id);
    for (auto const id: ids)
        release(id, killAt);
}

std::optional<int> SessionTable::signallableChild(KeeperSessionId id) const
{
    auto const found = _sessions.find(id);
    if (found == _sessions.end() || found->second.exited || found->second.hold == Hold::Released)
        return std::nullopt;
    return found->second.pid;
}

std::optional<ReapedSession> SessionTable::reap(int pid, WireExitStatus status)
{
    auto const found =
        std::ranges::find_if(_sessions, [pid](auto const& entry) { return entry.second.pid == pid; });
    if (found == _sessions.end())
        return std::nullopt;
    auto const id = found->first;
    if (found->second.hold == Hold::Released)
    {
        _sessions.erase(found);
        return ReapedSession { .id = id, .fate = ReapedFate::Forgotten };
    }
    found->second.exited = status;
    return ReapedSession { .id = id, .fate = ReapedFate::Kept };
}

std::vector<int> SessionTable::takeDueKills(core::platform::SteadyTimePoint now)
{
    auto pids = std::vector<int> {};
    for (auto& [id, session]: _sessions)
    {
        if (session.killAt && *session.killAt <= now)
        {
            pids.push_back(session.pid);
            session.killAt.reset();
        }
    }
    return pids;
}

std::optional<core::platform::SteadyTimePoint> SessionTable::nextKill() const
{
    auto next = std::optional<core::platform::SteadyTimePoint> {};
    for (auto const& [id, session]: _sessions)
        if (session.killAt && (!next || *session.killAt < *next))
            next = session.killAt;
    return next;
}

std::vector<InventoryEntry> SessionTable::inventory() const
{
    auto entries = std::vector<InventoryEntry> {};
    for (auto const& [id, session]: _sessions)
    {
        if (session.hold == Hold::Released)
            continue;
        auto const& gate = session.descriptors[slotOf(KeptDescriptor::Gate)];
        auto entry = InventoryEntry {
            .message = InventorySession { .session = std::to_underlying(id),
                                          .pid = session.pid,
                                          .exited = session.exited,
                                          .placed = gate.isOpen() ? Placement::Parked : Placement::Placed,
                                          .descriptors = {} },
            .fds = {},
        };
        for (auto const role: core::codec::EnumWire<KeptDescriptor>::Values)
        {
            auto const& descriptor = session.descriptors[slotOf(role)];
            if (!descriptor.isOpen())
                continue;
            entry.message.descriptors.push_back(role);
            entry.fds.push_back(descriptor.get());
        }
        entries.push_back(std::move(entry));
    }
    return entries;
}

} // namespace vthost::keeper
