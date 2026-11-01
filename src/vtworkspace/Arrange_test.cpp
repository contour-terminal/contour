// SPDX-License-Identifier: Apache-2.0
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <expected>
#include <format>
#include <ranges>
#include <utility>
#include <vector>

#include <vtworkspace/Arrangement.hpp>
#include <vtworkspace/ModelFixture.hpp>
#include <vtworkspace/SessionModel.hpp>

using namespace vtworkspace;
using namespace vtworkspace::test;

namespace
{
/// @return A leaf of an arrangement, showing @p session.
[[nodiscard]] PaneArrangement leaf(uint64_t session)
{
    return PaneArrangement { .session = SessionId { session }, .children = {} };
}

/// @return A split of an arrangement along @p split, @p first taking @p ratio of the space.
[[nodiscard]] PaneArrangement split(SplitState split,
                                    double ratio,
                                    PaneArrangement first,
                                    PaneArrangement second)
{
    return PaneArrangement { .split = split,
                             .ratio = ratio,
                             .children = { std::move(first), std::move(second) } };
}

/// Tab `tab` of window `window` shows sessions 1000 | (1001 / 1002), with 1002 the active pane.
struct ArrangeFixture: Fixture
{
    Window* window = model.createWindow();
    Tab* tab = model.createTab(window->id());
    Pane* p1001 = model.splitActivePane(tab->id(), SplitState::Vertical, 0.4);
    Pane* p1002 = model.splitActivePane(tab->id(), SplitState::Horizontal, 0.7);
    TabId tabId = tab->id();
    PaneId leaf1000 = tab->rootPane()->findLeaf(SessionId { 1000 })->id();
    PaneId leaf1001 = tab->rootPane()->findLeaf(SessionId { 1001 })->id();
    PaneId leaf1002 = p1002->id();

    ArrangeFixture() { events.log.clear(); }

    /// @return The leaf PaneIds of the tab, in tree order.
    [[nodiscard]] std::vector<uint64_t> leafIds() const
    {
        return std::ranges::to<std::vector>(tab->leafSessions() | std::views::transform([this](SessionId s) {
                                                return tab->rootPane()->findLeaf(s)->id().value;
                                            }));
    }

    /// @return The tab's leaves' sessions, in tree order.
    [[nodiscard]] std::vector<uint64_t> sessions() const
    {
        return std::ranges::to<std::vector>(tab->leafSessions()
                                            | std::views::transform([](SessionId s) { return s.value; }));
    }

    /// A refusal changes nothing and fires nothing.
    void requireRefused(std::expected<void, ArrangeError> const& result, ArrangeError expected) const
    {
        REQUIRE_FALSE(result.has_value());
        CHECK(result.error() == expected);
        CHECK(events.log.empty());
        CHECK(sessions() == std::vector<uint64_t> { 1000, 1001, 1002 });
        CHECK(tab->rootPane()->splitState() == SplitState::Vertical);
        CHECK(tab->rootPane()->ratio() == Catch::Approx(0.4));
    }
};
} // namespace

TEST_CASE("leafSessions lists a tab's sessions in tree order", "[vtworkspace][model][arrange]")
{
    ArrangeFixture const f;
    CHECK(f.sessions() == std::vector<uint64_t> { 1000, 1001, 1002 });
}

TEST_CASE("arrangeTab lays the same leaves out as another tree", "[vtworkspace][model][arrange]")
{
    ArrangeFixture f;
    // (1000 | 1001) / 1002: the split nests the other way, and the axes trade.
    auto const shape = split(
        SplitState::Horizontal, 0.25, split(SplitState::Vertical, 0.6, leaf(1000), leaf(1001)), leaf(1002));

    REQUIRE(f.model.arrangeTab(f.tabId, shape).has_value());

    auto const* const root = f.tab->rootPane();
    CHECK(root->splitState() == SplitState::Horizontal);
    CHECK(root->ratio() == Catch::Approx(0.25));
    REQUIRE(root->first() != nullptr);
    CHECK(root->first()->splitState() == SplitState::Vertical);
    CHECK(root->first()->ratio() == Catch::Approx(0.6));
    CHECK(f.sessions() == std::vector<uint64_t> { 1000, 1001, 1002 });
    // The leaves are the panes the tab had, ids and all: nothing was spawned or closed.
    CHECK(f.leafIds() == std::vector<uint64_t> { f.leaf1000.value, f.leaf1001.value, f.leaf1002.value });
    CHECK(f.tab->activePane()->id() == f.leaf1002);
    CHECK(f.tab->paneCount() == 3);
    CHECK(f.events.log == std::vector<std::string> { std::format("restructured:{}", f.tabId.value) });
}

TEST_CASE("arrangeTab trades the places of two leaves", "[vtworkspace][model][arrange]")
{
    ArrangeFixture f;
    auto const shape = split(
        SplitState::Vertical, 0.4, leaf(1002), split(SplitState::Horizontal, 0.7, leaf(1001), leaf(1000)));

    REQUIRE(f.model.arrangeTab(f.tabId, shape).has_value());

    CHECK(f.sessions() == std::vector<uint64_t> { 1002, 1001, 1000 });
    // Each pane went with its session: the slot is the session's, not a position's.
    CHECK(f.leafIds() == std::vector<uint64_t> { f.leaf1002.value, f.leaf1001.value, f.leaf1000.value });
    CHECK(f.tab->activePane()->session() == SessionId { 1002 });
    CHECK(f.events.countPrefix("restructured:") == 1);
    CHECK(f.events.countPrefix("paneClosed:") == 0);
    CHECK(f.events.countPrefix("paneSplit:") == 0);
}

TEST_CASE("arrangeTab keeps the ids of the split nodes it rebuilds", "[vtworkspace][model][arrange]")
{
    ArrangeFixture f;
    auto const rootId = f.tab->rootPane()->id();
    auto const innerId = f.tab->rootPane()->second()->id();
    auto const shape = split(
        SplitState::Horizontal, 0.5, split(SplitState::Vertical, 0.5, leaf(1001), leaf(1000)), leaf(1002));

    REQUIRE(f.model.arrangeTab(f.tabId, shape).has_value());

    // Pre-order, root first: as many split nodes as before, under the ids they had.
    CHECK(f.tab->rootPane()->id() == rootId);
    CHECK(f.tab->rootPane()->first()->id() == innerId);
    CHECK(f.tab->rootPane()->first()->parent() == f.tab->rootPane());
    CHECK(f.tab->rootPane()->second()->parent() == f.tab->rootPane());
}

TEST_CASE("arrangeTab into the tree the tab already has changes nothing", "[vtworkspace][model][arrange]")
{
    ArrangeFixture f;
    auto const shape = split(
        SplitState::Vertical, 0.4, leaf(1000), split(SplitState::Horizontal, 0.7, leaf(1001), leaf(1002)));

    REQUIRE(f.model.arrangeTab(f.tabId, shape).has_value());
    CHECK(f.events.log.empty());

    SECTION("a ratio alone differing is an arrangement to make")
    {
        auto moved = shape;
        moved.ratio = 0.55;
        REQUIRE(f.model.arrangeTab(f.tabId, moved).has_value());
        CHECK(f.tab->rootPane()->ratio() == Catch::Approx(0.55));
        CHECK(f.events.countPrefix("restructured:") == 1);
    }
}

TEST_CASE("arrangeTab clears a zoom, as every reshaping does", "[vtworkspace][model][arrange]")
{
    ArrangeFixture f;
    f.model.toggleActivePaneZoom(f.tabId);
    REQUIRE(f.tab->isZoomed());
    f.events.log.clear();

    REQUIRE(f.model
                .arrangeTab(f.tabId,
                            split(SplitState::Horizontal,
                                  0.5,
                                  leaf(1002),
                                  split(SplitState::Vertical, 0.5, leaf(1000), leaf(1001))))
                .has_value());

    CHECK_FALSE(f.tab->isZoomed());
    CHECK(f.events.log
          == std::vector<std::string> { std::format("restructured:{}", f.tabId.value),
                                        std::format("zoom:{}:-", f.tabId.value),
                                        std::format("title:{}", f.tabId.value) });
}

TEST_CASE("arrangeTab of a one-pane tab into that pane changes nothing", "[vtworkspace][model][arrange]")
{
    Fixture f;
    auto* const window = f.model.createWindow();
    auto* const tab = f.model.createTab(window->id());
    f.events.log.clear();

    REQUIRE(f.model.arrangeTab(tab->id(), leaf(1000)).has_value());
    CHECK(f.events.log.empty());
    CHECK(tab->rootPane()->session() == SessionId { 1000 });
}

TEST_CASE("arrangeTab refuses what it cannot lay out", "[vtworkspace][model][arrange]")
{
    ArrangeFixture f;
    auto const whole = [](PaneArrangement second) {
        return split(SplitState::Vertical, 0.5, leaf(1000), std::move(second));
    };

    SECTION("an unknown tab")
    {
        f.requireRefused(f.model.arrangeTab(TabId { 4242 }, leaf(1000)), ArrangeError::UnknownTab);
    }
    SECTION("a split without two children")
    {
        auto lopsided = whole(split(SplitState::Horizontal, 0.5, leaf(1001), leaf(1002)));
        lopsided.children.pop_back();
        f.requireRefused(f.model.arrangeTab(f.tabId, lopsided), ArrangeError::Malformed);
    }
    SECTION("a leaf with children")
    {
        auto parent = leaf(1000);
        parent.children = { leaf(1001), leaf(1002) };
        f.requireRefused(f.model.arrangeTab(f.tabId, parent), ArrangeError::Malformed);
    }
    SECTION("a session the tab does not show")
    {
        f.requireRefused(
            f.model.arrangeTab(f.tabId, whole(split(SplitState::Horizontal, 0.5, leaf(1001), leaf(1999)))),
            ArrangeError::OtherSessions);
    }
    SECTION("a session of the tab left out")
    {
        f.requireRefused(f.model.arrangeTab(f.tabId, whole(leaf(1001))), ArrangeError::OtherSessions);
    }
    SECTION("a session named twice")
    {
        f.requireRefused(
            f.model.arrangeTab(f.tabId, whole(split(SplitState::Horizontal, 0.5, leaf(1001), leaf(1001)))),
            ArrangeError::OtherSessions);
    }
}

TEST_CASE("sideOf names the side a split axis and graft order make", "[vtworkspace][primitives]")
{
    for (auto const side: AllSplitSides)
        CHECK(sideOf(splitStateOf(side), graftOrderOf(side)) == side);
    CHECK_FALSE(sideOf(SplitState::None, GraftOrder::GraftFirst).has_value());
}

TEST_CASE("arrangeTab changes only the axis of a split", "[vtworkspace][model][arrange]")
{
    // [1000 | 1001] to 1000 / 1001: the same sessions, in the same order, along the other axis.
    Fixture f;
    auto* const window = f.model.createWindow();
    auto* const tab = f.model.createTab(window->id());
    (void) f.model.splitActivePane(tab->id(), SplitState::Vertical);
    f.events.log.clear();

    REQUIRE(f.model.arrangeTab(tab->id(), split(SplitState::Horizontal, 0.5, leaf(1000), leaf(1001)))
                .has_value());
    CHECK(tab->rootPane()->splitState() == SplitState::Horizontal);
    CHECK(tab->rootPane()->first()->session() == SessionId { 1000 });
    CHECK(tab->rootPane()->second()->session() == SessionId { 1001 });
    CHECK(tab->activePane()->session() == SessionId { 1001 });
    CHECK(f.events.countPrefix("restructured:") == 1);
}
