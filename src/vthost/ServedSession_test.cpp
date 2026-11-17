// SPDX-License-Identifier: Apache-2.0
#include <core/Environment.hpp>
#include <core/net/EventLoop.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <exception>
#include <memory>
#include <optional>
#include <ranges>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include <vthost/SessionHost.hpp>
#include <vthost/handover/HandoverCapture.hpp>
#include <vthost/handover/HandoverCodec.hpp>
#include <vthost/handover/HandoverRestore.hpp>
#include <vthost/handover/HandoverToken.hpp>
#include <vthost/handover/ImageHandoff.hpp>
#include <vthost/handover/LiveUpgrade.hpp>
#include <vthost/keeper/Messages.hpp>
#include <vthost/keeper/UniqueFd.hpp>
#include <vthost/keeperlink/Recovery.hpp>
#include <vthost/keeperlink/Structure.hpp>
#include <vthost/testing/FakeHandoverMemory.hpp>
#include <vthost/testing/FakeProcessImageReplacer.hpp>
#include <vthost/testing/HandoverTesting.hpp>
#include <vthost/testing/LiveUpgradeTesting.hpp>
#include <vthost/testing/ServedTesting.hpp>

using namespace vthost;
using namespace vthost::handover;
using namespace std::chrono_literals;
using vthost::testing::LiveHost;
using vthost::testing::openServedTab;
using vthost::testing::releaseForAdoption;
using vthost::testing::ServedText;
using vtworkspace::SessionId;

namespace
{

/// Thrown from the fake replacer where the exec succeeds: it ends run() as the exec ends the old image.
struct ImageReplaced: std::exception
{
};

/// @return The echoing shell on a PTY of its own.
[[nodiscard]] std::unique_ptr<vtpty::Pty> catDevice(vtpty::PageSize size)
{
    return vthost::testing::shellProcess(size);
}

/// Records why each session ended.
class EndRecorder final: public SessionStreamEvents
{
  public:
    [[nodiscard]] bool mirrorsScreens() const noexcept override { return false; }
    void sessionEnded(SessionId session, SessionEnd cause) override { ends.emplace_back(session, cause); }

    std::vector<std::pair<SessionId, SessionEnd>> ends;
};

/// A daemon about to be upgraded on POSIX, its exec a fake's.
struct ServedUpgrade: vthost::testing::UpgradeBench
{
    ServedUpgrade(): UpgradeBench { catDevice } {}

    vthost::testing::FakeProcessImageReplacer replacer;
    vthost::testing::FakeHandoverMemory memory;
    ExecHandoff handoff { replacer, memory };
};

/// The sessions of a host with two shells side by side in one tab, and a served tab beside it.
struct ServedLayout
{
    std::vector<SessionId> shells;
    SessionId served {};
};

/// Spawns two shells side by side in one tab of @p live, and opens a served tab beside it.
/// @return The sessions, once every screen is drawn.
[[nodiscard]] ServedLayout spawnServedLayout(LiveHost& live)
{
    live.spawnTwo();
    auto layout = ServedLayout { .shells = live.host.sessionIds() };
    auto const opened = openServedTab(live.host);
    REQUIRE(opened.has_value());
    layout.served = *opened;
    REQUIRE(live.eventually([&] {
        return std::ranges::all_of(layout.shells,
                                   [&](SessionId id) { return live.pageText(id).contains("ready"); })
               && live.pageText(layout.served).contains(ServedText);
    }));
    return layout;
}

/// @return Whether @p host holds exactly the sessions @p ids, none of them served.
[[nodiscard]] bool holdsExactly(SessionHost const& host, std::vector<SessionId> const& ids)
{
    return host.sessionIds() == ids
           && std::ranges::none_of(ids, [&](SessionId id) { return host.servedBy(id) != nullptr; });
}

/// Opens a served tab in @p live, as the extension reopening it would, and waits for its screen.
void reopenServed(LiveHost& live)
{
    auto const reopened = openServedTab(live.host);
    REQUIRE(reopened.has_value());
    CHECK(live.host.servedBy(*reopened) != nullptr);
    REQUIRE(live.eventually([&] { return live.pageText(*reopened).contains(ServedText); }));
}

/// @return The ids of @p sessions.
[[nodiscard]] std::vector<SessionId> idsOf(std::vector<HandoverSession> const& sessions)
{
    return std::ranges::to<std::vector<SessionId>>(sessions | std::views::transform(&HandoverSession::id));
}

} // namespace

TEST_CASE(
    "(a) a live upgrade with a served tab open completes; the tab is gone, every other session survives",
    "[daemon][served][upgrade]")
{
    auto u = ServedUpgrade {};
    auto recorder = EndRecorder {};
    auto subscription = std::optional<ScopedStreamSubscription> {};
    subscription.emplace(
        u.live->host, recorder, &SessionHost::subscribeStream, &SessionHost::unsubscribeStream);
    auto const old = spawnServedLayout(*u.live);

    // Admission stays closed for the whole run: while the clients go, and up to the exec.
    auto refusedWhileClosing = std::optional<bool> {};
    u.hooks.onClose = [&] {
        refusedWhileClosing = !openServedTab(u.live->host).has_value();
    };
    auto next = std::unique_ptr<LiveHost> {};
    auto image = std::optional<HandoverImage> {};
    u.replacer.onReplace = [&] {
        CHECK(u.live->host.servedAdmission() == ServedAdmission::Closed);
        CHECK(u.hooks.served == std::vector<std::string> { "close" });
        auto const& [program, argv] = u.replacer.replacements.back();
        auto const fd = handoverTokenOf(argv.back()).and_then(parseHandoverDescriptor);
        REQUIRE(fd.has_value());
        auto decoded = readHandover(u.memory, *fd);
        REQUIRE(decoded.has_value());
        image = *decoded;
        releaseForAdoption(u.live->host);
        auto config = LiveHost::sequentialIds();
        config.handover = *std::move(decoded);
        next = std::make_unique<LiveHost>(catDevice, std::move(config));
        throw ImageReplaced {};
    };
    auto upgrade = u.orchestrator(u.handoff);
    REQUIRE_THROWS_AS(std::ignore = u.run(upgrade, UpgradeTarget { .imageId = u.imageId }), ImageReplaced);

    // The served tab was closed by the upgrade, before anything was captured; the image holds the shells.
    REQUIRE(image.has_value());
    CHECK(idsOf(image->sessions) == old.shells);
    CHECK(std::ranges::find(recorder.ends, std::pair { old.served, SessionEnd::Upgrade })
          != recorder.ends.end());
    REQUIRE_FALSE(u.hooks.served.empty());
    CHECK(u.hooks.served.front() == "close");
    CHECK(refusedWhileClosing == std::optional { true });

    subscription.reset();
    u.acceptor.reset();
    u.live.reset();

    // The new image runs the same shells, and the extension opens its tab again there.
    REQUIRE(next != nullptr);
    CHECK(next->host.handoverOutcome().has_value());
    CHECK(holdsExactly(next->host, old.shells));
    for (auto const id: old.shells)
        next->host.writeInput(id, "after-upgrade");
    REQUIRE(next->eventually([&] {
        return std::ranges::all_of(
            old.shells, [&](SessionId id) { return next->pageText(id).contains("after-upgrade"); });
    }));
    reopenServed(*next);
}

TEST_CASE("(b) a keeper recovery drops a served tab, keeps every other session, and it can be reopened",
          "[daemon][served][keeper]")
{
    auto live = std::make_unique<LiveHost>(catDevice);
    auto const old = spawnServedLayout(*live);

    // The structure the dead daemon pushed to its keeper: the served session runs on no keeper child.
    auto structure = keeperlink::captureStructure(
        live->host,
        keeperlink::StructureExtras {
            .daemonInstance = 0xC0FFEE, .release = "0.9.0", .upgradeGeneration = 0, .listeners = {} });
    auto const servedEntry = std::ranges::find(structure.sessions, old.served, &HandoverSession::id);
    REQUIRE(servedEntry != structure.sessions.end());
    CHECK(servedEntry->device == SessionDevice::PtyOnly);

    // The keeper held the two shells: the test plays it, naming each by a keeper session of its own and
    // handing its PTY over as the inventory does.
    auto kept = std::vector<keeper::InventorySession> {};
    auto masters = std::vector<int> {};
    for (auto const id: old.shells)
    {
        auto const entry = std::ranges::find(structure.sessions, id, &HandoverSession::id);
        REQUIRE(entry != structure.sessions.end());
        auto const& hosted = *live->host.hostedSession(id);
        auto const pty = hosted.pty().handover();
        REQUIRE(pty.has_value());
        entry->device = SessionDevice::KeeperChild;
        entry->keeperSession = kept.size() + 1;
        kept.push_back(keeper::InventorySession { .session = entry->keeperSession,
                                                  .pid = hosted.process()->crossing().child->pid,
                                                  .exited = std::nullopt,
                                                  .placed = keeper::Placement::Placed,
                                                  .descriptors = { keeper::KeptDescriptor::Master } });
        masters.push_back(pty->masterFd);
    }
    auto memory = vthost::testing::FakeHandoverMemory {};
    auto const handle = memory.write(keeperlink::encodeStructure(structure));
    REQUIRE(handle.has_value());
    auto inventory = std::vector<keeperlink::ReceivedMessage> {};
    inventory.push_back(
        { .message = keeper::InventoryBegin { .daemonImagePath = {}, .sessionCount = 2, .listenerCount = 0 },
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
    CHECK(idsOf(recovery.image.sessions) == old.shells);

    auto config = LiveHost::sequentialIds();
    config.handover = std::move(recovery.image);
    auto const next = std::make_unique<LiveHost>(catDevice, std::move(config));
    CHECK(holdsExactly(next->host, old.shells));
    next->host.writeInput(old.shells.front(), "recovered");
    REQUIRE(next->eventually([&] { return next->pageText(old.shells.front()).contains("recovered"); }));
    reopenServed(*next);
}

TEST_CASE("(d) a capture and restore round trip with a served session present", "[daemon][served][handover]")
{
    auto live = std::make_unique<LiveHost>(catDevice);
    auto const old = spawnServedLayout(*live);
    auto const uuids = std::ranges::to<std::vector<std::optional<vtcontrol::Uuid>>>(
        old.shells | std::views::transform([&](SessionId id) { return live->host.objects().uuidOf(id); }));

    // Without the close the capture refuses it, as it refuses every device no new image could adopt.
    auto const refused = checkCapturable(live->host);
    REQUIRE_FALSE(refused.has_value());
    CHECK(refused.error() == HandoverError::UnsupportedSession);

    live->host.closeServedSessions();
    REQUIRE(checkCapturable(live->host).has_value());
    live->settle();
    auto const captured = live->capture();
    REQUIRE(captured.has_value());
    auto decoded = decodeHandover(encodeHandover(*captured));
    REQUIRE(decoded.has_value());
    CHECK(idsOf(decoded->sessions) == old.shells);

    releaseForAdoption(live->host);
    live.reset();
    auto config = LiveHost::sequentialIds();
    config.handover = *std::move(decoded);
    auto const next = std::make_unique<LiveHost>(catDevice, std::move(config));
    CHECK(next->host.handoverOutcome().has_value());
    CHECK(holdsExactly(next->host, old.shells));
    for (auto const [id, uuid]: std::views::zip(old.shells, uuids))
        CHECK(next->host.objects().uuidOf(id) == uuid);
}
