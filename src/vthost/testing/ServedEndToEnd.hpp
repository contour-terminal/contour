// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// What the end-to-end cases of served tabs share on POSIX: a daemon upgraded up to its exec, a keeper
/// recovery played in process, and the checks of what crossed either.

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstdint>
#include <exception>
#include <memory>
#include <ranges>
#include <utility>
#include <vector>

#include <vthost/SessionHost.hpp>
#include <vthost/handover/HandoverCodec.hpp>
#include <vthost/handover/ImageHandoff.hpp>
#include <vthost/keeper/Messages.hpp>
#include <vthost/keeperlink/Recovery.hpp>
#include <vthost/keeperlink/Structure.hpp>
#include <vthost/testing/FakeHandoverMemory.hpp>
#include <vthost/testing/FakeProcessImageReplacer.hpp>
#include <vthost/testing/HandoverTesting.hpp>
#include <vthost/testing/LiveUpgradeTesting.hpp>

namespace vthost::testing
{

/// Thrown from the fake replacer where the exec succeeds: it ends run() as the exec ends the old image.
struct ImageReplaced: std::exception
{
};

/// Records why each session ended.
class EndRecorder final: public SessionStreamEvents
{
  public:
    [[nodiscard]] bool mirrorsScreens() const noexcept override { return false; }
    void sessionEnded(vtworkspace::SessionId session, SessionEnd cause) override
    {
        ends.emplace_back(session, cause);
    }

    std::vector<std::pair<vtworkspace::SessionId, SessionEnd>> ends;
};

/// A daemon about to be upgraded on POSIX, its exec a fake's.
struct ServedUpgrade: UpgradeBench
{
    /// @param config How the daemon's host is set up.
    explicit ServedUpgrade(SessionHostConfig config = LiveHost::sequentialIds()):
        UpgradeBench { [](vtpty::PageSize size) { return shellProcess(size); }, {}, std::move(config) }
    {
    }

    /// @param ptys Makes the device of each session, given the program a spawn names instead of the shell.
    /// @param config How the daemon's host is set up.
    ServedUpgrade(PtyFactory ptys, SessionHostConfig config):
        UpgradeBench { std::move(ptys), std::move(config) }
    {
    }

    FakeProcessImageReplacer replacer;
    FakeHandoverMemory memory;
    handover::ExecHandoff handoff { replacer, memory };
};

/// @return Whether @p host holds exactly the sessions @p ids, none of them served.
[[nodiscard]] inline bool holdsExactly(SessionHost const& host,
                                       std::vector<vtworkspace::SessionId> const& ids)
{
    return host.sessionIds() == ids && std::ranges::none_of(ids, [&](vtworkspace::SessionId id) {
               return host.servedBy(id) != nullptr;
           });
}

/// @return The ids of @p sessions.
[[nodiscard]] inline std::vector<vtworkspace::SessionId> idsOf(
    std::vector<handover::HandoverSession> const& sessions)
{
    return std::ranges::to<std::vector<vtworkspace::SessionId>>(
        sessions | std::views::transform(&handover::HandoverSession::id));
}

/// Plays a keeper recovery of @p live's daemon, whose keeper held @p shells while it served @p served itself:
/// the daemon dies, and what the keeper kept becomes the image its successor takes over.
/// @param live The daemon, gone on return.
/// @param shells Its sessions on the keeper, in order.
/// @param served The served session it hosted.
/// @return The image the keeper's inventory recovers.
[[nodiscard]] inline handover::HandoverImage recoverFromKeeper(
    std::unique_ptr<LiveHost>& live,
    std::vector<vtworkspace::SessionId> const& shells,
    vtworkspace::SessionId served)
{
    // The structure the dead daemon pushed to its keeper: the served session runs on no keeper child.
    auto structure = keeperlink::captureStructure(
        live->host,
        keeperlink::StructureExtras {
            .daemonInstance = 0xC0FFEE, .release = "0.9.0", .upgradeGeneration = 0, .listeners = {} });
    auto const servedEntry = std::ranges::find(structure.sessions, served, &handover::HandoverSession::id);
    REQUIRE(servedEntry != structure.sessions.end());
    CHECK(servedEntry->device == handover::SessionDevice::PtyOnly);

    // The keeper held the shells: the test plays it, naming each by a keeper session of its own and
    // handing its PTY over as the inventory does.
    auto kept = std::vector<keeper::InventorySession> {};
    auto masters = std::vector<int> {};
    for (auto const id: shells)
    {
        auto const entry = std::ranges::find(structure.sessions, id, &handover::HandoverSession::id);
        REQUIRE(entry != structure.sessions.end());
        auto const& hosted = *live->host.hostedSession(id);
        auto const pty = hosted.pty().handover();
        REQUIRE(pty.has_value());
        entry->device = handover::SessionDevice::KeeperChild;
        entry->keeperSession = kept.size() + 1;
        kept.push_back(keeper::InventorySession { .session = entry->keeperSession,
                                                  .pid = hosted.process()->crossing().child->pid,
                                                  .exited = std::nullopt,
                                                  .placed = keeper::Placement::Placed,
                                                  .descriptors = { keeper::KeptDescriptor::Master } });
        masters.push_back(pty->masterFd);
    }
    auto memory = FakeHandoverMemory {};
    auto const handle = memory.write(keeperlink::encodeStructure(structure));
    REQUIRE(handle.has_value());
    auto inventory = std::vector<keeperlink::ReceivedMessage> {};
    inventory.push_back(
        { .message = keeper::InventoryBegin { .daemonImagePath = {},
                                              .sessionCount = static_cast<uint32_t>(shells.size()),
                                              .listenerCount = 0 },
          .fds = {} });
    inventory.back().fds.emplace_back(static_cast<int>(handle->value));
    for (auto const& [session, master]: std::views::zip(kept, masters))
    {
        inventory.push_back({ .message = session, .fds = {} });
        inventory.back().fds.emplace_back(master);
    }
    // The dead daemon let go of its PTYs and programs without closing either.
    releaseForAdoption(live->host);
    live.reset();

    auto recovery = keeperlink::imageFromInventory(inventory, memory);
    CHECK(recovery.problems.empty());
    return std::move(recovery.image);
}

} // namespace vthost::testing
