// SPDX-License-Identifier: Apache-2.0
#include <contour/window/TaskbarAttentionTally.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <optional>
#include <ranges>
#include <utility>
#include <vector>

using namespace contour::window;

TEST_CASE("taskbarAttention.tallyCountsBlockedTabsPerWindowAndInAll", "[taskbarattention]")
{
    auto const tabs = std::array {
        std::pair { uint64_t { 1 }, TabBadgeKind::Blocked },
        std::pair { uint64_t { 1 }, TabBadgeKind::Done },
        std::pair { uint64_t { 2 }, TabBadgeKind::Blocked },
        std::pair { uint64_t { 2 }, TabBadgeKind::Blocked },
    };
    auto const tally = tallyWaitingTabs(tabs);
    CHECK(tally.total == 3);
    CHECK(tally.perWindow == std::vector<std::pair<uint64_t, unsigned>> { { 1, 1 }, { 2, 2 } });
}

TEST_CASE("taskbarAttention.aBellCountsAsWaiting", "[taskbarattention]")
{
    auto const tabs = std::array {
        std::pair { uint64_t { 7 }, TabBadgeKind::Bell },
        std::pair { uint64_t { 7 }, TabBadgeKind::None },
        std::pair { uint64_t { 8 }, TabBadgeKind::Blocked },
    };
    auto const tally = tallyWaitingTabs(tabs);
    CHECK(tally.total == 2);
    CHECK(tally.perWindow == std::vector<std::pair<uint64_t, unsigned>> { { 7, 1 }, { 8, 1 } });
}

TEST_CASE("taskbarAttention.aWindowWithNoneWaitingIsListedWithZero", "[taskbarattention]")
{
    auto const tabs = std::array {
        std::pair { uint64_t { 7 }, TabBadgeKind::Done },
        std::pair { uint64_t { 7 }, TabBadgeKind::None },
        std::pair { uint64_t { 8 }, TabBadgeKind::Blocked },
    };
    auto const tally = tallyWaitingTabs(tabs);
    CHECK(tally.total == 1);
    CHECK(tally.perWindow == std::vector<std::pair<uint64_t, unsigned>> { { 7, 0 }, { 8, 1 } });
}

TEST_CASE("taskbarAttention.onlyNeedsYouWaits", "[taskbarattention]")
{
    for (auto const index: std::views::iota(size_t { 0 }, TabBadgeKindCount))
    {
        auto const kind = TabBadgeKind(index);
        auto const tabs = std::array { std::pair { uint64_t { 1 }, kind } };
        INFO("kind " << index);
        CHECK(tallyWaitingTabs(tabs).total == (urgencyOf(kind) == Urgency::NeedsYou ? 1u : 0u));
    }
}

TEST_CASE("taskbarAttention.noTabsNoTally", "[taskbarattention]")
{
    CHECK(tallyWaitingTabs({}) == AttentionTally {});
}

TEST_CASE("taskbarAttention.applicationScopeCountsOneDaemonOnceAcrossTwoWindows", "[taskbarattention]")
{
    constexpr auto daemonA = std::optional<uint64_t> { 10 };
    constexpr auto daemonB = std::optional<uint64_t> { 11 };

    SECTION("two windows on one daemon count it once, as the more recently focused one sees it")
    {
        auto const windows = std::array {
            WindowWaiting { .window = 1, .waiting = 2, .daemon = daemonA, .focusOrder = 5 },
            WindowWaiting { .window = 2, .waiting = 3, .daemon = daemonA, .focusOrder = 9 },
        };
        CHECK(applicationWaiting(windows) == 3);
    }
    SECTION("the order of the list does not decide it")
    {
        auto const windows = std::array {
            WindowWaiting { .window = 2, .waiting = 3, .daemon = daemonA, .focusOrder = 9 },
            WindowWaiting { .window = 1, .waiting = 2, .daemon = daemonA, .focusOrder = 5 },
        };
        CHECK(applicationWaiting(windows) == 3);
    }
    SECTION("other daemons and in-process windows add their own")
    {
        auto const windows = std::array {
            WindowWaiting { .window = 1, .waiting = 2, .daemon = daemonA, .focusOrder = 5 },
            WindowWaiting { .window = 2, .waiting = 3, .daemon = daemonA, .focusOrder = 9 },
            WindowWaiting { .window = 3, .waiting = 4, .daemon = daemonB, .focusOrder = 1 },
            WindowWaiting { .window = 4, .waiting = 1 },
            WindowWaiting { .window = 5, .waiting = 1 },
        };
        CHECK(applicationWaiting(windows) == 3 + 4 + 1 + 1);
    }
    SECTION("no windows wait for nothing")
    {
        CHECK(applicationWaiting({}) == 0);
    }
}

TEST_CASE("taskbarAttention.aDirectoryCountsItsBlockedTabsButTheDisplayedWorkspace", "[taskbarattention]")
{
    using contour::window::DirectoryPane;
    using contour::window::DirectoryTab;
    using contour::window::DirectoryWorkspace;
    auto const pane = [](vtbackend::ProgramState state) {
        auto record = vtbackend::ProgramStatusRecord {};
        record.state = state;
        record.generation = 1;
        return DirectoryPane {
            .key = {}, .title = {}, .cwd = {}, .status = { record }, .activity = {}, .finished = {}
        };
    };
    auto const tab = [](std::vector<DirectoryPane> panes) {
        return DirectoryTab { .id = 0, .title = {}, .label = {}, .panes = std::move(panes) };
    };
    auto const blocked = pane(vtbackend::ProgramState::Blocked);
    auto const working = pane(vtbackend::ProgramState::Working);
    auto const directory = contour::window::WorkspaceDirectory {
        .workspaces = { DirectoryWorkspace {
                            .id = 1, .label = {}, .tabs = { tab({ blocked }), tab({ working }) } },
                        // A split tab with one blocked pane waits once.
                        DirectoryWorkspace { .id = 2, .label = {}, .tabs = { tab({ working, blocked }) } },
                        DirectoryWorkspace { .id = 3, .label = {}, .tabs = { tab({ blocked }) } } },
        .selected = 1,
    };

    CHECK(contour::window::tabsWaitingIn(directory, std::nullopt) == 3);
    CHECK(contour::window::tabsWaitingIn(directory, uint64_t { 1 }) == 2);
    CHECK(contour::window::tabsWaitingIn(directory, uint64_t { 3 }) == 2);
    CHECK(contour::window::tabsWaitingIn({}, std::nullopt) == 0);
}
