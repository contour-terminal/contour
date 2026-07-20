// SPDX-License-Identifier: Apache-2.0
#include <vtpty/MockPty.h>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <format>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include <muxserver/SessionHost.h>
#include <net/EventLoop.h>
#include <net/testing/ScriptedEventSource.h>
#include <vtmux/Pane.h>
#include <vtmux/Tab.h>

using muxserver::SessionHost;
using vtmux::SplitState;

namespace
{

/// Records every fanned-out model event so tests can assert what subscribers saw.
struct RecordingEvents final: vtmux::ModelEvents
{
    std::vector<std::string> log;

    void tabAdded(vtmux::WindowId, vtmux::TabId, int index) override
    {
        log.push_back(std::format("tabAdded:{}", index));
    }
    void tabClosed(vtmux::WindowId, vtmux::TabId, int index) override
    {
        log.push_back(std::format("tabClosed:{}", index));
    }
    void tabMoved(vtmux::WindowId, vtmux::TabId, int from, int to) override
    {
        log.push_back(std::format("tabMoved:{}->{}", from, to));
    }
    void activeTabChanged(vtmux::WindowId, vtmux::TabId, int index) override
    {
        log.push_back(std::format("activeTabChanged:{}", index));
    }
    void paneSplit(vtmux::TabId, vtmux::PaneId, vtmux::PaneId) override { log.emplace_back("paneSplit"); }
    void paneClosed(vtmux::TabId, vtmux::PaneId, vtmux::PaneId) override { log.emplace_back("paneClosed"); }
    void activePaneChanged(vtmux::TabId, vtmux::PaneId) override { log.emplace_back("activePaneChanged"); }
    void paneRatioChanged(vtmux::TabId, vtmux::PaneId, double) override
    {
        log.emplace_back("paneRatioChanged");
    }
    void tabTitleChanged(vtmux::TabId) override { log.emplace_back("tabTitleChanged"); }
    void tabColorChanged(vtmux::TabId) override { log.emplace_back("tabColorChanged"); }

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

TEST_CASE("createTab seeds a backing session handed back by the allocator", "[muxserver][host]")
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

TEST_CASE("splitActivePane backs the new leaf with a fresh session", "[muxserver][host]")
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

TEST_CASE("a refused split reaps the orphaned backing session", "[muxserver][host]")
{
    auto h = HostHarness {};
    auto* tab = h.host.createTab();
    REQUIRE(tab != nullptr);
    REQUIRE(h.host.sessionCount() == 1);

    h.host.splitActivePane(vtmux::TabId { 4711 }, SplitState::Vertical, 0.5);

    // The unknown tab refused the split; the pre-spawned session must not leak.
    CHECK(h.host.sessionCount() == 1);
    CHECK(tab->paneCount() == 1);
}

TEST_CASE("a session exit prunes its pane and keeps the sibling", "[muxserver][host]")
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

TEST_CASE("the last pane's session exit closes the whole tab", "[muxserver][host]")
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

TEST_CASE("an unsubscribed observer stops receiving events", "[muxserver][host]")
{
    auto h = HostHarness {};
    h.host.unsubscribe(&h.recorder);

    REQUIRE(h.host.createTab() != nullptr);
    CHECK(h.recorder.log.empty());
}
