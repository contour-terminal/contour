// SPDX-License-Identifier: Apache-2.0
#include <vtpty/MockPty.hpp>

#include <core/Utils.hpp>
#include <core/async/Task.hpp>
#include <core/net/EventLoop.hpp>
#include <core/net/IoBackend.hpp>
#include <core/platform/Clock.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <future>
#include <memory>
#include <mutex>
#include <ranges>
#include <set>
#include <span>
#include <thread>
#include <tuple>
#include <vector>

#include <unistd.h>

#include <vtcontrol/testing/SequentialUuidSource.hpp>
#include <vthost/SessionHost.hpp>
#include <vthost/SessionSettings.hpp>
#include <vthost/handover/HandoverMemory.hpp>
#include <vthost/keeperlink/Structure.hpp>
#include <vthost/keeperlink/StructurePushPolicy.hpp>
#include <vthost/testing/FakeProfileCatalog.hpp>

using namespace std::chrono_literals;
using vthost::keeperlink::PushTiming;
using vthost::keeperlink::StructureError;
using vthost::keeperlink::StructureEvent;

namespace handover = vthost::handover;
namespace keeperlink = vthost::keeperlink;

namespace
{

/// Keeps every structure pushed, decoded.
struct RecordingSink final: keeperlink::StructureSink
{
    std::vector<handover::HandoverImage> pushed;

    void push(std::span<uint8_t const> blob) override
    {
        auto structure = keeperlink::decodeStructure(blob);
        REQUIRE(structure.has_value());
        pushed.push_back(*std::move(structure));
    }
};

/// A host on mock PTYs, on a loop whose clock the test drives, with profiles that know a light and a dark
/// palette.
struct StructureHarness
{
    std::unique_ptr<core::net::IoBackend> source = core::net::makeDefaultBackend();
    core::platform::ManualClock clock;
    core::net::EventLoop loop { *source, clock };
    vthost::SessionHost host { loop,
                               [](vtbackend::PageSize size, std::optional<vtpty::Process::ExecInfo> const&) {
                                   return std::make_unique<vtpty::MockPty>(size);
                               },
                               vtbackend::Settings {},
                               core::defaultEnvironment(),
                               vthost::SessionHostConfig {
                                   .pumping = vthost::SessionPumping::Manual,
                                   .profiles = std::make_shared<vthost::testing::FakeDualPaletteCatalog>(),
                                   .uuids = std::make_shared<vtcontrol::testing::SequentialUuidSource>() } };
    RecordingSink sink;
};

/// @return Whether the latest structure @p sink was pushed holds @p tab.
[[nodiscard]] bool latestHolds(RecordingSink const& sink, vtworkspace::TabId tab)
{
    if (sink.pushed.empty())
        return false;
    for (auto const& workspace: sink.pushed.back().tree.workspaces)
        for (auto const& known: workspace.tabs)
            if (known.id == tab)
                return true;
    return false;
}

/// Asks, on each tab added, whether the structure pushed by then holds the tab: as a client hears of it,
/// which is when its connection's write queue writes, from a flow the loop resumes (core::net::WriteQueue's
/// drain), never inside the event.
struct TabWatcher final: vtworkspace::ModelEvents
{
    RecordingSink const* sink = nullptr;
    core::net::EventLoop* loop = nullptr; ///< Where the "write" runs; none to ask at once.
    std::vector<bool> pushedBefore;       ///< Per tab added: whether the latest push held it.

    [[nodiscard]] static core::async::Task<void> hear(TabWatcher* watcher, vtworkspace::TabId tab)
    {
        watcher->pushedBefore.push_back(latestHolds(*watcher->sink, tab));
        co_return;
    }

    void tabAdded(vtworkspace::WorkspaceId /*workspace*/, vtworkspace::TabId tab, int /*index*/) override
    {
        if (loop != nullptr)
            loop->spawn(hear(this, tab));
        else
            pushedBefore.push_back(latestHolds(*sink, tab));
    }
    void tabClosed(vtworkspace::WorkspaceId /*workspace*/, vtworkspace::TabId /*tab*/, int /*index*/) override
    {
    }
    void tabMoved(vtworkspace::WorkspaceId /*workspace*/,
                  vtworkspace::TabId /*tab*/,
                  int /*from*/,
                  int /*to*/) override
    {
    }
    void activeTabChanged(vtworkspace::WorkspaceId /*workspace*/,
                          vtworkspace::TabId /*tab*/,
                          int /*index*/) override
    {
    }
    void paneSplit(vtworkspace::TabId /*tab*/,
                   vtworkspace::PaneId /*split*/,
                   vtworkspace::PaneId /*leaf*/) override
    {
    }
    void paneClosed(vtworkspace::TabId /*tab*/,
                    vtworkspace::PaneId /*closed*/,
                    vtworkspace::PaneId /*survivor*/) override
    {
    }
    void activePaneChanged(vtworkspace::TabId /*tab*/, vtworkspace::PaneId /*leaf*/) override {}
    void paneRatioChanged(vtworkspace::TabId /*tab*/,
                          vtworkspace::PaneId /*split*/,
                          double /*ratio*/) override
    {
    }
    void tabTitleChanged(vtworkspace::TabId /*tab*/) override {}
    void tabColorChanged(vtworkspace::TabId /*tab*/) override {}
};

/// @return A structure with something in every part the codec carries.
[[nodiscard]] handover::HandoverImage sampleStructure()
{
    auto structure = handover::HandoverImage {};
    structure.daemonInstance = 0xC0FFEE;
    structure.fromRelease = "1.2.3";
    structure.nextSessionId = vtworkspace::SessionId { 9 };
    structure.upgradeGeneration = 4;
    structure.defaultColorPreference = vtbackend::ColorPreference::Dark;
    structure.sessions.push_back(handover::HandoverSession {
        .id = vtworkspace::SessionId { 7 },
        .uuid = {},
        .label = "seven",
        .profileName = "main",
        .origin = vthost::SettingsOrigin::Client,
        .statedSettings = {},
        .pty = {},
        .child = {},
        .terminalState = {},
        .carried = {},
        .colorPreference = vtbackend::ColorPreference::Dark,
        .interruption = std::nullopt,
        .device = handover::SessionDevice::KeeperChild,
        .keeperSession = 42,
    });
    structure.listeners.push_back(handover::InheritedListener {
        .name = "native-tcp",
        .handle = {},
        .tlsIdentity = handover::TlsIdentity { .certPem = "CERT", .keyPem = "KEY" },
    });
    return structure;
}

} // namespace

TEST_CASE("every structure event has a push timing", "[keeper][structure]")
{
    static_assert(keeperlink::StructureEventCount == 21, "a new structure event needs a row and this count");
    auto const debounced = std::set { StructureEvent::PaneRatioChanged,
                                      StructureEvent::TabTitleChanged,
                                      StructureEvent::TabColorChanged,
                                      StructureEvent::ActiveTabChanged,
                                      StructureEvent::ActivePaneChanged };
    for (auto const index: std::views::iota(std::size_t { 0 }, keeperlink::StructureEventCount))
    {
        auto const event = static_cast<StructureEvent>(index);
        INFO("event " << index);
        CHECK(keeperlink::pushTimingOf(event)
              == (debounced.contains(event) ? PushTiming::Debounced : PushTiming::Synchronous));
    }
}

TEST_CASE("the structure round-trips; a newer structure version is refused", "[keeper][structure]")
{
    auto const blob = keeperlink::encodeStructure(sampleStructure());
    auto const decoded = keeperlink::decodeStructure(blob);
    REQUIRE(decoded.has_value());
    CHECK(decoded->daemonInstance == 0xC0FFEE);
    CHECK(decoded->fromRelease == "1.2.3");
    CHECK(decoded->nextSessionId == vtworkspace::SessionId { 9 });
    CHECK(decoded->upgradeGeneration == 4);
    REQUIRE(decoded->sessions.size() == 1);
    CHECK(decoded->sessions.front().id == vtworkspace::SessionId { 7 });
    CHECK(decoded->sessions.front().keeperSession == 42);
    CHECK(decoded->sessions.front().device == handover::SessionDevice::KeeperChild);

    SECTION("a newer version")
    {
        auto newer = blob;
        newer[keeperlink::StructureMagic.size()] = static_cast<uint8_t>(keeperlink::StructureVersion + 1);
        CHECK(keeperlink::decodeStructure(newer).error() == StructureError::NewerVersion);
    }
    SECTION("no structure at all")
    {
        auto foreign = blob;
        foreign.front() = 'X';
        CHECK(keeperlink::decodeStructure(foreign).error() == StructureError::BadMagic);
    }
    SECTION("cut short")
    {
        auto const cut = std::span { blob }.first(blob.size() - 1);
        CHECK(keeperlink::decodeStructure(cut).error() == StructureError::Malformed);
    }
}

TEST_CASE("tlsIdentity, statedSettings and colorPreference are in the structure", "[keeper][structure]")
{
    StructureHarness h;
    auto stated = vtbackend::Settings {};
    stated.maxImageRegisterCount = 77;
    REQUIRE(h.host.createTab(vthost::SessionSpawnRequest { .settings = stated }) != nullptr);
    auto const session = h.host.sessionIds().front();
    h.host.applyColorPreference(session, vtbackend::ColorPreference::Dark);

    auto const captured = keeperlink::captureStructure(
        h.host,
        keeperlink::StructureExtras {
            .daemonInstance = 5,
            .release = "r",
            .upgradeGeneration = 0,
            .listeners = { handover::InheritedListener {
                .name = "native-tcp",
                .handle = vtpty::CrossingHandle::of(3),
                .tlsIdentity = handover::TlsIdentity { .certPem = "CERT", .keyPem = "KEY" } } } });
    auto const structure = keeperlink::decodeStructure(keeperlink::encodeStructure(captured));
    REQUIRE(structure.has_value());

    REQUIRE(structure->listeners.size() == 1);
    CHECK(structure->listeners.front().handle.isNone()); // the keeper holds the listener itself
    REQUIRE(structure->listeners.front().tlsIdentity.has_value());
    CHECK(structure->listeners.front().tlsIdentity->certPem == "CERT");
    CHECK(structure->listeners.front().tlsIdentity->keyPem == "KEY");

    REQUIRE(structure->sessions.size() == 1);
    auto const& carried = structure->sessions.front();
    CHECK(carried.origin == vthost::SettingsOrigin::Client);
    CHECK(carried.statedSettings.maxImageRegisterCount == 77);
    CHECK(carried.statedSettings == vthost::toWireSessionSettings(vthost::hostedSessionSettings(stated)));
    CHECK(carried.colorPreference == vtbackend::ColorPreference::Dark);
    CHECK(structure->defaultColorPreference == vtbackend::ColorPreference::Dark);
    CHECK(carried.terminalState.empty());
}

TEST_CASE("an id-minting event is pushed before any client hears of it", "[keeper][structure]")
{
    StructureHarness h;
    auto watcher = TabWatcher {};
    watcher.sink = &h.sink;
    watcher.loop = &h.loop;
    h.host.subscribe(&watcher);
    auto const pusher = keeperlink::StructurePusher { h.loop, h.host, h.sink, {} };
    REQUIRE(h.sink.pushed.size() == 1); // the structure as it stands, as the pusher is built

    REQUIRE(h.host.createTab() != nullptr);
    std::ignore = h.loop.runOnce(0ms);
    // A flow queued ahead of the change -- it might be a connection's drain -- makes the push one at once.
    h.loop.spawn([]() -> core::async::Task<void> {
        co_return;
    }());
    REQUIRE(h.host.createTab() != nullptr);
    std::ignore = h.loop.runOnce(0ms);
    CHECK(watcher.pushedBefore == std::vector { true, true });
    h.host.unsubscribe(&watcher);
}

TEST_CASE("the id-minting changes of one turn are pushed once", "[keeper][structure]")
{
    StructureHarness h;
    auto const pusher = keeperlink::StructurePusher { h.loop, h.host, h.sink, {} };
    auto const initial = h.sink.pushed.size();

    // A tab, its session and its activation: three changes the keeper must hold before a client hears.
    auto* const tab = h.host.createTab();
    REQUIRE(tab != nullptr);
    h.host.splitActivePane(tab->id(), vtworkspace::SplitState::Vertical);
    CHECK(h.sink.pushed.size() == initial);
    std::ignore = h.loop.runOnce(0ms);
    REQUIRE(h.sink.pushed.size() == initial + 1);
    CHECK(latestHolds(h.sink, tab->id()));
    // Nothing more is owed.
    std::ignore = h.loop.runOnce(0ms);
    CHECK(h.sink.pushed.size() == initial + 1);
}

TEST_CASE("a split drag pushes once after a second", "[keeper][structure]")
{
    StructureHarness h;
    REQUIRE(h.host.createTab() != nullptr);
    auto pusher = keeperlink::StructurePusher { h.loop, h.host, h.sink, {} };
    auto const initial = h.sink.pushed.size();

    // A drag: a ratio per step, many steps within the second.
    for ([[maybe_unused]] auto const step: std::views::iota(0, 20))
    {
        pusher.paneRatioChanged(vtworkspace::TabId { 1 }, vtworkspace::PaneId { 1 }, 0.5);
        h.clock.advance(10ms);
        std::ignore = h.loop.runOnce(0ms);
    }
    CHECK(h.sink.pushed.size() == initial);

    // A deadline expires in one turn and its callback runs in the next.
    h.clock.advance(1s);
    std::ignore = h.loop.runOnce(0ms);
    std::ignore = h.loop.runOnce(0ms);
    CHECK(h.sink.pushed.size() == initial + 1);
    // And nothing more after.
    h.clock.advance(5s);
    std::ignore = h.loop.runOnce(0ms);
    std::ignore = h.loop.runOnce(0ms);
    CHECK(h.sink.pushed.size() == initial + 1);
}

TEST_CASE("a structure push locks no terminal", "[keeper][structure][threading]")
{
    StructureHarness h;
    REQUIRE(h.host.createTab() != nullptr);
    auto& terminal = h.host.hostedSession(h.host.sessionIds().front())->terminal();

    // Another thread holds the terminal, as a pump in a long batch does.
    auto lock = std::mutex {};
    auto condition = std::condition_variable {};
    auto held = false;
    auto release = false;
    auto holder = std::thread { [&] {
        auto const guard = std::lock_guard { terminal };
        {
            auto const signal = std::scoped_lock { lock };
            held = true;
        }
        condition.notify_all();
        auto wait = std::unique_lock { lock };
        condition.wait(wait, [&] { return release; });
    } };
    {
        auto wait = std::unique_lock { lock };
        condition.wait(wait, [&] { return held; });
    }

    auto captured = std::async(std::launch::async, [&] {
        return keeperlink::encodeStructure(
                   keeperlink::captureStructure(h.host, keeperlink::StructureExtras {}))
            .size();
    });
    CHECK(captured.wait_for(5s) == std::future_status::ready);

    {
        auto const signal = std::scoped_lock { lock };
        release = true;
    }
    condition.notify_all();
    holder.join();
    CHECK(captured.get() > 0);
}

#ifdef __linux__
TEST_CASE("a structure's memory object is sealed against writes", "[keeper][structure]")
{
    auto const memory = handover::makeNativeHandoverMemory();
    auto const blob = keeperlink::encodeStructure(sampleStructure());
    auto const object = memory->write(blob);
    REQUIRE(object.has_value());
    auto const byte = uint8_t { 0 };
    CHECK(::pwrite(object->native(), &byte, 1, 0) < 0);
    CHECK(errno == EPERM);
    CHECK(::ftruncate(object->native(), 1) < 0);
    auto const view = memory->read(*object);
    REQUIRE(view.has_value());
    CHECK(keeperlink::decodeStructure(view->bytes()).has_value());
    memory->discard(*object);
}
#endif

TEST_CASE("every structure pushed restores into a model", "[keeper][structure]")
{
    StructureHarness h;
    auto pusher = keeperlink::StructurePusher { h.loop, h.host, h.sink, {} };
    REQUIRE(h.host.createTab() != nullptr);
    auto* const second = h.host.createTab();
    REQUIRE(second != nullptr);
    std::ignore = h.loop.runOnce(0ms);
    h.host.splitActivePane(second->id(), vtworkspace::SplitState::Vertical);
    std::ignore = h.loop.runOnce(0ms);
    h.host.splitActivePane(second->id(), vtworkspace::SplitState::Horizontal);
    std::ignore = h.loop.runOnce(0ms);
    h.host.closeSession(h.host.sessionIds().back());
    std::ignore = h.loop.runOnce(0ms);
    h.host.closeSession(h.host.sessionIds().front());
    std::ignore = h.loop.runOnce(0ms);
    REQUIRE(h.sink.pushed.size() > 1);
    for (auto const& [index, structure]: core::views::enumerate(h.sink.pushed))
    {
        CAPTURE(index);
        auto events = TabWatcher {};
        events.sink = &h.sink;
        auto model = vtworkspace::SessionModel { events, [] { return vtworkspace::SessionId { 999 }; } };
        auto const restored = model.restore(structure.tree, structure.modelIds);
        CHECK(restored.error_or(vtworkspace::ModelRestoreError::DuplicateId)
              == vtworkspace::ModelRestoreError::DuplicateId);
        CHECK(restored.has_value());
    }
}
