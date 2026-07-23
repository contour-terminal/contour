// SPDX-License-Identifier: Apache-2.0
#include <vtpty/MockPty.h>

#include <crispy/BufferObject.h>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <format>
#include <functional>
#include <memory>
#include <ranges>
#include <string>
#include <string_view>
#include <thread>
#include <tuple>
#include <vector>

#include <coro/Task.hpp>
#include <vthost/SessionHost.h>
#include <vthost/TappingPty.h>
#include <net/EventLoop.h>
#include <net/PollEventSource.h>
#include <net/testing/ScriptedEventSource.h>
#include <vtworkspace/Pane.h>
#include <vtworkspace/Tab.h>

using vthost::SessionHost;
using vtworkspace::SplitState;

namespace
{

/// Records every fanned-out model event so tests can assert what subscribers saw.
struct RecordingEvents final: vtworkspace::ModelEvents
{
    std::vector<std::string> log;

    void tabAdded(vtworkspace::WindowId, vtworkspace::TabId, int index) override
    {
        log.push_back(std::format("tabAdded:{}", index));
    }
    void tabClosed(vtworkspace::WindowId, vtworkspace::TabId, int index) override
    {
        log.push_back(std::format("tabClosed:{}", index));
    }
    void tabMoved(vtworkspace::WindowId, vtworkspace::TabId, int from, int to) override
    {
        log.push_back(std::format("tabMoved:{}->{}", from, to));
    }
    void activeTabChanged(vtworkspace::WindowId, vtworkspace::TabId, int index) override
    {
        log.push_back(std::format("activeTabChanged:{}", index));
    }
    void paneSplit(vtworkspace::TabId, vtworkspace::PaneId, vtworkspace::PaneId) override { log.emplace_back("paneSplit"); }
    void paneClosed(vtworkspace::TabId, vtworkspace::PaneId, vtworkspace::PaneId) override { log.emplace_back("paneClosed"); }
    void activePaneChanged(vtworkspace::TabId, vtworkspace::PaneId) override { log.emplace_back("activePaneChanged"); }
    void paneRatioChanged(vtworkspace::TabId, vtworkspace::PaneId, double) override
    {
        log.emplace_back("paneRatioChanged");
    }
    void tabTitleChanged(vtworkspace::TabId) override { log.emplace_back("tabTitleChanged"); }
    void tabColorChanged(vtworkspace::TabId) override { log.emplace_back("tabColorChanged"); }

    [[nodiscard]] bool saw(std::string_view needle) const
    {
        return std::ranges::any_of(log, [&](auto const& entry) { return entry.starts_with(needle); });
    }
};

/// A host over MockPty sessions with pump threads disabled (tests drive the
/// model on the calling thread, which stands in for the loop thread).
struct HostHarness
{
    net::testing::ScriptedEventSource source;
    net::EventLoop loop { source };
    RecordingEvents recorder;
    SessionHost host { loop,
                       [](vtbackend::PageSize size) { return std::make_unique<vtpty::MockPty>(size); },
                       vtbackend::Settings {},
                       /*startPumps=*/false };

    HostHarness() { host.subscribe(&recorder); }
};

} // namespace

TEST_CASE("createTab seeds a backing session handed back by the allocator", "[vthost][host]")
{
    auto h = HostHarness {};

    auto* tab = h.host.createTab();
    REQUIRE(tab != nullptr);
    REQUIRE(h.host.sessionCount() == 1);

    // The pane's session id is the pre-minted one, and it maps to a live terminal.
    auto const session = tab->rootPane()->session();
    REQUIRE(h.host.terminal(session) != nullptr);
    CHECK(h.recorder.saw("tabAdded"));
}

TEST_CASE("splitActivePane backs the new leaf with a fresh session", "[vthost][host]")
{
    auto h = HostHarness {};
    auto* tab = h.host.createTab();
    REQUIRE(tab != nullptr);

    h.host.splitActivePane(tab->id(), SplitState::Vertical, 0.5);

    REQUIRE(tab->paneCount() == 2);
    REQUIRE(h.host.sessionCount() == 2);
    CHECK(h.recorder.saw("paneSplit"));

    // Both leaves resolve to distinct live terminals.
    auto* first = h.host.terminal(tab->rootPane()->first()->session());
    auto* second = h.host.terminal(tab->rootPane()->second()->session());
    REQUIRE(first != nullptr);
    REQUIRE(second != nullptr);
    CHECK(first != second);
}

TEST_CASE("a refused split reaps the orphaned backing session", "[vthost][host]")
{
    auto h = HostHarness {};
    auto* tab = h.host.createTab();
    REQUIRE(tab != nullptr);
    REQUIRE(h.host.sessionCount() == 1);

    h.host.splitActivePane(vtworkspace::TabId { 4711 }, SplitState::Vertical, 0.5);

    // The unknown tab refused the split; the pre-spawned session must not leak.
    CHECK(h.host.sessionCount() == 1);
    CHECK(tab->paneCount() == 1);
}

TEST_CASE("a session exit prunes its pane and keeps the sibling", "[vthost][host]")
{
    auto h = HostHarness {};
    auto* tab = h.host.createTab();
    REQUIRE(tab != nullptr);
    h.host.splitActivePane(tab->id(), SplitState::Horizontal, 0.5);
    REQUIRE(h.host.sessionCount() == 2);

    auto const exited = tab->rootPane()->first()->session();
    auto const surviving = tab->rootPane()->second()->session();

    h.host.handleSessionExit(exited);

    CHECK(h.host.sessionCount() == 1);
    CHECK(h.host.terminal(exited) == nullptr);
    CHECK(h.host.terminal(surviving) != nullptr);
    CHECK(tab->paneCount() == 1);
    CHECK(h.recorder.saw("paneClosed"));
}

TEST_CASE("the last pane's session exit closes the whole tab", "[vthost][host]")
{
    auto h = HostHarness {};
    auto* tab = h.host.createTab();
    REQUIRE(tab != nullptr);
    auto const session = tab->rootPane()->session();

    h.host.handleSessionExit(session);

    CHECK(h.host.sessionCount() == 0);
    CHECK(h.host.model().window(h.host.windowId())->tabCount() == 0);
    CHECK(h.recorder.saw("tabClosed"));
}

TEST_CASE("an unsubscribed observer stops receiving events", "[vthost][host]")
{
    auto h = HostHarness {};
    h.host.unsubscribe(&h.recorder);

    REQUIRE(h.host.createTab() != nullptr);
    CHECK(h.recorder.log.empty());
}

TEST_CASE("applyClientSize reprojects the leaves onto the new client area", "[vthost][host]")
{
    auto h = HostHarness {};
    auto* tab = h.host.createTab();
    REQUIRE(tab != nullptr);

    h.host.applyClientSize(
        vtpty::PageSize { .lines = vtpty::LineCount(40), .columns = vtpty::ColumnCount(100) });

    // The sole full-area leaf now spans the whole client width (columns are
    // unaffected by the status-line height, so this is exact).
    auto* terminal = h.host.terminal(tab->rootPane()->session());
    REQUIRE(terminal != nullptr);
    CHECK(terminal->totalPageSize().columns.value == 100);
}

TEST_CASE("applyClientSize is race-free against a concurrent terminal writer",
          "[vthost][host][concurrency]")
{
    auto h = HostHarness {};
    auto* tab = h.host.createTab();
    REQUIRE(tab != nullptr);
    h.host.splitActivePane(tab->id(), SplitState::Vertical, 0.5);
    REQUIRE(h.host.sessionCount() == 2);

    auto* firstTerm = h.host.terminal(tab->rootPane()->first()->session());
    REQUIRE(firstTerm != nullptr);

    // The writer mutates a leaf's grid under _stateMutex (writeToScreen); reproject
    // now resizes the same terminal under that lock too (the fix). Under TSan an
    // unlocked resizeScreen would be flagged racing this writer's grid mutation.
    auto stop = std::atomic<bool> { false };
    auto writer = std::thread { [&] {
        for (auto i = 0; !stop.load(std::memory_order_relaxed); ++i)
            firstTerm->writeToScreen(std::format("row-{}\r\n", i));
    } };

    for (auto const i: std::views::iota(0, 300))
        h.host.applyClientSize(vtpty::PageSize { .lines = vtpty::LineCount(20 + (i % 20)),
                                                 .columns = vtpty::ColumnCount(60 + (i % 40)) });

    stop.store(true, std::memory_order_relaxed);
    writer.join();

    CHECK(h.host.sessionCount() == 2); // no state torn: both sessions intact
}

namespace
{

/// Records the stream fan-out one attached client would receive.
struct StreamRecorder final: vthost::SessionStreamEvents
{
    std::vector<uint64_t> screens;
    std::vector<std::string> output;

    void sessionScreenUpdated(vtworkspace::SessionId session) override { screens.push_back(session.value); }
    void sessionOutput(vtworkspace::SessionId session, std::string const& bytes) override
    {
        output.push_back(std::format("{}:{}", session.value, bytes));
    }
};

coro::Task<void> waitFor(net::EventLoop* loop, std::function<bool()> ready)
{
    using namespace std::chrono_literals;
    for (auto i = 0; i < 2000 && !ready(); ++i)
        co_await loop->delay(1ms);
}

} // namespace

TEST_CASE("stream events fan out to every subscriber independently", "[vthost][host]")
{
    auto source = net::PollEventSource {};
    auto loop = net::EventLoop { source };
    auto host = SessionHost { loop,
                              [](vtbackend::PageSize size) { return std::make_unique<vtpty::MockPty>(size); },
                              vtbackend::Settings {},
                              /*startPumps=*/false };
    auto first = StreamRecorder {};
    auto second = StreamRecorder {};
    host.subscribeStream(&first);
    host.subscribeStream(&second);

    REQUIRE(host.createTab() != nullptr);
    auto const session = host.model().window(host.windowId())->activeTab()->rootPane()->session();
    auto* terminal = host.terminal(session);
    REQUIRE(terminal != nullptr);

    // A screen update reaches BOTH subscribers.
    terminal->writeToScreen("hello");
    loop.blockOn(waitFor(&loop, [&] { return !first.screens.empty() && !second.screens.empty(); }));
    REQUIRE(!first.screens.empty());
    REQUIRE(!second.screens.empty());
    CHECK(first.screens.front() == session.value);
    CHECK(second.screens.front() == session.value);

    // The raw byte tap fans out too: drive one read through the TappingPty on
    // this thread (standing in for the session's pump thread).
    auto& tapped = dynamic_cast<vthost::TappingPty&>(terminal->device());
    auto& mock = dynamic_cast<vtpty::MockPty&>(tapped.inner());
    mock.appendStdOutBuffer("raw-bytes");
    auto pool = crispy::buffer_object_pool<char> { 4096 };
    auto const storage = pool.allocateBufferObject();
    std::ignore = tapped.read(*storage, std::nullopt, 4096);
    loop.blockOn(waitFor(&loop, [&] { return !first.output.empty() && !second.output.empty(); }));
    auto const expected = std::format("{}:raw-bytes", session.value);
    REQUIRE(!first.output.empty());
    REQUIRE(!second.output.empty());
    CHECK(first.output.front() == expected);
    CHECK(second.output.front() == expected);

    // Unsubscribing one observer must NOT silence the other — the regression
    // the single-slot handlers had (a disconnecting client nulled the shared
    // slot, muting every remaining client).
    host.unsubscribeStream(&first);
    auto const firstScreensSeen = first.screens.size();
    auto const secondScreensSeen = second.screens.size();
    terminal->writeToScreen("again");
    loop.blockOn(waitFor(&loop, [&] { return second.screens.size() > secondScreensSeen; }));
    CHECK(second.screens.size() > secondScreensSeen);
    CHECK(first.screens.size() == firstScreensSeen);
}
