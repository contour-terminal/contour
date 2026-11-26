// SPDX-License-Identifier: Apache-2.0
#include <contour/window/LocationHistory.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <ranges>
#include <utility>
#include <vector>

using namespace contour::window;

namespace
{

/// @return Workspace @p workspace's tab @p tab on daemon @p daemon's run @p instance.
[[nodiscard]] SidebarTarget place(char const* daemon, uint64_t workspace, uint64_t tab, uint64_t instance = 1)
{
    return SidebarTarget { .daemon = DaemonRef { daemon },
                           .instance = DaemonInstance { instance },
                           .workspace = workspace,
                           .tab = tab,
                           .pane = std::nullopt };
}

/// Every entry is still there.
[[nodiscard]] bool anyPlace(SidebarTarget const& /*entry*/)
{
    return true;
}

/// @return A section of @p daemon's run @p instance listing workspace @p workspace with tab @p tab.
[[nodiscard]] DaemonSection sectionOf(char const* daemon, uint64_t instance, uint64_t workspace, uint64_t tab)
{
    auto listed = DirectoryWorkspace { .id = workspace, .name = {}, .tabs = {} };
    auto listedTab = DirectoryTab {};
    listedTab.id = tab;
    listed.tabs.push_back(std::move(listedTab));
    auto section = DaemonSection {};
    section.daemon = DaemonRef { daemon };
    section.instance = DaemonInstance { instance };
    section.directory.workspaces.push_back(std::move(listed));
    return section;
}

auto const A = place("a", 1, 2);
auto const B = place("b", 1, 1);
auto const C = place("c", 3, 1);
auto const X = place("x", 1, 1);
auto const Y = place("y", 1, 1);
auto const Z = place("z", 1, 1);

} // namespace

TEST_CASE("LocationHistory: A→B→C, Back, Back lands on A", "[history]")
{
    auto history = LocationHistory { LocationHistoryCapacity };
    history.landed(A, B, HistoryCause::Jump);
    history.landed(B, C, HistoryCause::Jump);
    CHECK(history.back(std::nullopt, anyPlace) == B);
    CHECK(history.back(std::nullopt, anyPlace) == A);
    CHECK_FALSE(history.back(std::nullopt, anyPlace).has_value());
}

TEST_CASE("LocationHistory: Back's own landing is not recorded", "[history]")
{
    auto history = LocationHistory { LocationHistoryCapacity };
    history.landed(A, B, HistoryCause::Jump);
    history.landed(B, C, HistoryCause::Jump);
    REQUIRE(history.size() == 2);
    REQUIRE(history.back(std::nullopt, anyPlace) == B);
    history.landed(C, B, HistoryCause::Back);
    CHECK(history.size() == 1);
}

TEST_CASE("LocationHistory: a run of attention jumps is one entry", "[history]")
{
    auto history = LocationHistory { LocationHistoryCapacity };
    history.landed(A, X, HistoryCause::Attention);
    history.landed(X, Y, HistoryCause::Attention);
    history.landed(Y, Z, HistoryCause::Attention);
    CHECK(history.size() == 1);
    CHECK(history.back(std::nullopt, anyPlace) == A);
}

TEST_CASE("LocationHistory: a jump after a run ends it", "[history]")
{
    auto history = LocationHistory { LocationHistoryCapacity };
    history.landed(A, X, HistoryCause::Attention);
    history.landed(X, Y, HistoryCause::Attention);
    history.landed(Y, C, HistoryCause::Jump);
    CHECK(history.back(std::nullopt, anyPlace) == Y);
    CHECK(history.back(std::nullopt, anyPlace) == A);
}

TEST_CASE("LocationHistory: Back ends an attention run", "[history]")
{
    auto history = LocationHistory { LocationHistoryCapacity };
    history.landed(A, X, HistoryCause::Attention);
    REQUIRE(history.back(std::nullopt, anyPlace) == A);
    history.landed(X, A, HistoryCause::Back);
    history.landed(A, Y, HistoryCause::Attention);
    // The second run recorded its own origin.
    CHECK(history.size() == 1);
    CHECK(history.back(std::nullopt, anyPlace) == A);
}

TEST_CASE("LocationHistory: gone entries are skipped", "[history]")
{
    auto history = LocationHistory { LocationHistoryCapacity };
    history.landed(A, B, HistoryCause::Jump);
    history.landed(B, C, HistoryCause::Jump);
    CHECK(history.back(std::nullopt, [](SidebarTarget const& entry) { return entry != B; }) == A);
    // B is gone from the stack too.
    CHECK(history.size() == 0);
}

// Review Focus 4: a daemon that restarted lists other nodes under the old ids.
TEST_CASE("LocationHistory: an entry of a restarted daemon is gone", "[history]")
{
    auto history = LocationHistory { LocationHistoryCapacity };
    history.landed(A, place("b", 1, 1, 1), HistoryCause::Jump);
    history.landed(place("b", 1, 1, 1), C, HistoryCause::Jump);
    // A runs as before; B restarted (run 2) and lists a workspace and a tab of the same ids.
    auto const sections = std::vector { sectionOf("a", 1, A.workspace, *A.tab),
                                        sectionOf("b", 2, 1, 1),
                                        sectionOf("c", 1, C.workspace, *C.tab) };
    auto const stillExists = [&](SidebarTarget const& entry) {
        return placeStillExists(entry, sections);
    };
    CHECK_FALSE(placeStillExists(place("b", 1, 1, 1), sections));
    CHECK(history.back(std::nullopt, stillExists) == A);
}

TEST_CASE("LocationHistory: a place is still there only while its run lists its workspace and tab",
          "[history]")
{
    auto const sections = std::vector { sectionOf("a", 1, 1, 2) };
    CHECK(placeStillExists(place("a", 1, 2), sections));
    CHECK_FALSE(placeStillExists(place("a", 1, 3), sections));
    CHECK_FALSE(placeStillExists(place("a", 4, 2), sections));
    CHECK_FALSE(placeStillExists(place("b", 1, 2), sections));
    // A daemon listed before it stated its run: a jump there would be refused (jumpControlIdOf()).
    CHECK_FALSE(placeStillExists(place("a", 1, 2), std::vector { sectionOf("a", 0, 1, 2) }));
    auto tabless = place("a", 1, 2);
    tabless.tab = std::nullopt;
    CHECK(placeStillExists(tabless, sections));
}

TEST_CASE("LocationHistory: capacity drops the oldest", "[history]")
{
    auto history = LocationHistory { LocationHistoryCapacity };
    auto const landings = LocationHistoryCapacity + 1;
    for (auto const step: std::views::iota(std::size_t { 0 }, landings))
        history.landed(place("a", step + 1, 1), place("a", step + 2, 1), HistoryCause::Jump);
    CHECK(history.size() == LocationHistoryCapacity);
    auto popped = std::vector<SidebarTarget> {};
    while (auto const entry = history.back(std::nullopt, anyPlace))
        popped.push_back(*entry);
    CHECK(popped.size() == LocationHistoryCapacity);
    // The first origin never comes back.
    CHECK(std::ranges::find(popped, place("a", 1, 1)) == popped.end());
    CHECK(popped.back() == place("a", 2, 1));
}

TEST_CASE("LocationHistory: a landing that goes nowhere records nothing", "[history]")
{
    auto history = LocationHistory { LocationHistoryCapacity };
    // The same workspace and tab, another pane of it.
    auto pane = A;
    pane.pane = PaneKey { 7 };
    history.landed(A, pane, HistoryCause::Jump);
    // The same workspace re-selected, naming no tab.
    auto workspace = A;
    workspace.tab = std::nullopt;
    history.landed(A, workspace, HistoryCause::Jump);
    // From nowhere.
    history.landed(std::nullopt, B, HistoryCause::Jump);
    CHECK(history.size() == 0);
    // Another tab of the same workspace is somewhere else.
    history.landed(A, place("a", 1, 3), HistoryCause::Jump);
    CHECK(history.size() == 1);
}

// Phase 7 review M3: a tab-strip click back to a recorded place leaves it on the stack, where the window is.
TEST_CASE("LocationHistory: Back skips the place the window is on", "[history]")
{
    auto history = LocationHistory { LocationHistoryCapacity };
    auto const first = place("a", 1, 1);
    auto const second = place("a", 1, 2);
    history.landed(A, first, HistoryCause::Jump);
    history.landed(first, second, HistoryCause::Jump);
    // A tab-strip click from the second tab back to the first: unrecorded.
    CHECK(history.canGoBack(first, anyPlace));
    CHECK(history.back(first, anyPlace) == A);
    CHECK_FALSE(history.canGoBack(A, anyPlace));
}

TEST_CASE("LocationHistory: a workspace entry is where the window is on any tab of it", "[history]")
{
    auto history = LocationHistory { LocationHistoryCapacity };
    auto workspace = place("a", 1, 1);
    workspace.tab = std::nullopt;
    history.landed(A, B, HistoryCause::Jump);
    history.landed(workspace, C, HistoryCause::Jump);
    CHECK(history.back(place("a", 1, 9), anyPlace) == A);
}

TEST_CASE("LocationHistory: canGoBack pops nothing", "[history]")
{
    auto history = LocationHistory { LocationHistoryCapacity };
    CHECK_FALSE(history.canGoBack(std::nullopt, anyPlace));
    history.landed(A, B, HistoryCause::Jump);
    CHECK(history.canGoBack(B, anyPlace));
    CHECK_FALSE(history.canGoBack(B, [](SidebarTarget const&) { return false; }));
    CHECK(history.size() == 1);
}

TEST_CASE("LocationHistory: a Back that lands where it was ends a run all the same", "[history]")
{
    auto history = LocationHistory { LocationHistoryCapacity };
    history.landed(A, X, HistoryCause::Attention);
    history.landed(X, X, HistoryCause::Back);
    history.landed(X, Y, HistoryCause::Attention);
    // The second run recorded its own origin.
    CHECK(history.back(std::nullopt, anyPlace) == X);
    CHECK(history.back(std::nullopt, anyPlace) == A);
}

// Phase 7 review M5: a jump back that fails or is cancelled puts its place back.
TEST_CASE("LocationHistory: a restored place is the next Back's", "[history]")
{
    auto history = LocationHistory { LocationHistoryCapacity };
    history.landed(A, B, HistoryCause::Jump);
    history.landed(B, C, HistoryCause::Jump);
    auto const popped = history.back(std::nullopt, anyPlace);
    REQUIRE(popped == B);
    history.restore(*popped);
    CHECK(history.back(std::nullopt, anyPlace) == B);
    CHECK(history.back(std::nullopt, anyPlace) == A);
}
