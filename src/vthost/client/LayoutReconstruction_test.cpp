// SPDX-License-Identifier: Apache-2.0
#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <cstdint>
#include <memory>
#include <optional>
#include <unordered_map>
#include <utility>
#include <vector>

#include <vthost/client/LayoutReconstruction.h>
#include <vtworkspace/LayoutTree.h>
#include <vtworkspace/ModelEvents.h>
#include <vtworkspace/Pane.h>
#include <vtworkspace/PaneLayout.h>
#include <vtworkspace/SessionModel.h>
#include <vtworkspace/Tab.h>

using vthost::client::composeClientArea;
using vthost::client::WireLayout;
using vthost::client::wireToLayout;
namespace proto = vthost::proto;

namespace
{

/// The minimal ModelEvents: overrides only the pure-virtual "completed change"
/// callbacks (the bracket hooks keep their no-op defaults).
struct NoopEvents: vtworkspace::ModelEvents
{
    void tabAdded(vtworkspace::WindowId, vtworkspace::TabId, int) override {}
    void tabClosed(vtworkspace::WindowId, vtworkspace::TabId, int) override {}
    void tabMoved(vtworkspace::WindowId, vtworkspace::TabId, int, int) override {}
    void tabMovedToWindow(vtworkspace::WindowId, vtworkspace::TabId, int, vtworkspace::WindowId, int) override
    {
    }
    void activeTabChanged(vtworkspace::WindowId, vtworkspace::TabId, int) override {}
    void paneSplit(vtworkspace::TabId, vtworkspace::PaneId, vtworkspace::PaneId) override {}
    void paneClosed(vtworkspace::TabId, vtworkspace::PaneId, vtworkspace::PaneId) override {}
    void activePaneChanged(vtworkspace::TabId, vtworkspace::PaneId) override {}
    void paneRatioChanged(vtworkspace::TabId, vtworkspace::PaneId, double) override {}
    void tabTitleChanged(vtworkspace::TabId) override {}
    void tabColorChanged(vtworkspace::TabId) override {}
};

/// A wire leaf carrying @p session.
proto::WirePane leaf(uint64_t session)
{
    return proto::WirePane { .split = 0, .session = session };
}

/// A wire split node (orientation 1 horizontal / 2 vertical) over two children.
proto::WirePane split(uint8_t orientation, uint16_t ratio, proto::WirePane first, proto::WirePane second)
{
    return proto::WirePane { .split = orientation,
                             .ratio = ratio,
                             .children = { std::move(first), std::move(second) } };
}

/// Asserts a model pane subtree reproduces a wire pane subtree exactly.
void requireMatches(vtworkspace::Pane const& pane, proto::WirePane const& wire)
{
    if (wire.split == 0)
    {
        REQUIRE(pane.isLeaf());
        CHECK(pane.session().value == wire.session);
        return;
    }
    REQUIRE_FALSE(pane.isLeaf());
    CHECK(std::to_underlying(pane.splitState()) == wire.split);
    CHECK(std::lround(pane.ratio() * 10000.0) == wire.ratio);
    REQUIRE(pane.first() != nullptr);
    REQUIRE(pane.second() != nullptr);
    requireMatches(*pane.first(), wire.children[0]);
    requireMatches(*pane.second(), wire.children[1]);
}

/// Realizes a WireLayout against a fresh model via the shared realizeLayoutTab,
/// binding each leaf to the remote session `wireToLayout` recorded — so the
/// rebuilt leaves carry the very ids the layout named. Returns the model plus its
/// realized tabs.
struct Rebuilt
{
    NoopEvents events;
    std::unique_ptr<vtworkspace::SessionModel> model;
    std::vector<vtworkspace::Tab*> tabs;
};

std::unique_ptr<Rebuilt> realize(WireLayout const& wl)
{
    auto rebuilt = std::make_unique<Rebuilt>();
    auto pending = vtworkspace::SessionId {};
    rebuilt->model =
        std::make_unique<vtworkspace::SessionModel>(rebuilt->events, [&pending] { return pending; });
    auto const window = rebuilt->model->createWindow()->id();

    // The seeder stages each leaf's remote session so the model allocator hands it
    // back for that pane — exactly what a GUI seeder would do before binding.
    auto const seed = [&](vtworkspace::LayoutPane const& leafPane) {
        pending = vtworkspace::SessionId { wl.leafSession.at(&leafPane) };
        return true;
    };
    for (auto const& tab: wl.layout.tabs)
    {
        auto* modelTab = vtworkspace::realizeLayoutTab(*rebuilt->model, window, tab, seed);
        REQUIRE(modelTab != nullptr);
        rebuilt->tabs.push_back(modelTab);
    }
    return rebuilt;
}

} // namespace

TEST_CASE("wireToLayout realizes a single-pane tab", "[vthost][layout]")
{
    auto state = proto::LayoutState {};
    state.tabs.push_back(proto::WireTab { .root = leaf(100) });

    auto const wl = wireToLayout(state);
    REQUIRE(wl.layout.tabs.size() == 1);
    CHECK(wl.layout.tabs[0].root.isLeaf());

    auto const rebuilt = realize(wl);
    REQUIRE(rebuilt->tabs.size() == 1);
    requireMatches(*rebuilt->tabs[0]->rootPane(), state.tabs[0].root);
}

TEST_CASE("wireToLayout realizes a single split", "[vthost][layout]")
{
    auto state = proto::LayoutState {};
    state.tabs.push_back(proto::WireTab { .root = split(2, 6000, leaf(100), leaf(101)) });

    auto const wl = wireToLayout(state);
    REQUIRE(wl.layout.tabs.size() == 1);
    auto const& root = wl.layout.tabs[0].root;
    REQUIRE_FALSE(root.isLeaf());
    CHECK(root.orientation == vtworkspace::SplitState::Vertical);
    REQUIRE(root.children.size() == 2);
    REQUIRE(root.children[0].ratio.has_value());
    CHECK(std::lround(*root.children[0].ratio * 10000.0) == 6000); // first child's share

    auto const rebuilt = realize(wl);
    requireMatches(*rebuilt->tabs[0]->rootPane(), state.tabs[0].root);
}

TEST_CASE("wireToLayout realizes a nested split tree", "[vthost][layout]")
{
    // root = H-split( V-split(leaf 1, leaf 2), leaf 3 ) — the left child is itself
    // a split, exercising realizeLayoutTab's return-to-first-child recursion.
    auto state = proto::LayoutState {};
    state.tabs.push_back(
        proto::WireTab { .root = split(1, 4000, split(2, 7000, leaf(1), leaf(2)), leaf(3)) });

    auto const rebuilt = realize(wireToLayout(state));
    REQUIRE(rebuilt->tabs.size() == 1);
    requireMatches(*rebuilt->tabs[0]->rootPane(), state.tabs[0].root);
}

TEST_CASE("wireToLayout realizes multiple tabs", "[vthost][layout]")
{
    auto state = proto::LayoutState {};
    state.tabs.push_back(proto::WireTab { .root = leaf(10) });
    state.tabs.push_back(proto::WireTab { .root = split(2, 5000, leaf(20), leaf(21)) });
    state.tabs.push_back(proto::WireTab { .root = leaf(30) });

    auto const rebuilt = realize(wireToLayout(state));
    REQUIRE(rebuilt->tabs.size() == 3);
    requireMatches(*rebuilt->tabs[0]->rootPane(), state.tabs[0].root);
    requireMatches(*rebuilt->tabs[1]->rootPane(), state.tabs[1].root);
    requireMatches(*rebuilt->tabs[2]->rootPane(), state.tabs[2].root);
}

TEST_CASE("wireToLayout yields no tabs for an empty layout", "[vthost][layout]")
{
    CHECK(wireToLayout(proto::LayoutState {}).layout.tabs.empty());
}

TEST_CASE("wireToLayout tolerates a malformed split node without reading out of bounds", "[vthost][layout]")
{
    // The wire decoder rejects a split with the wrong child count (see Pdu_test), but
    // the converter must be robust on its own too: a split node missing a child
    // collapses to a leaf rather than indexing children[0]/[1] out of bounds.
    auto rootWith = [](std::vector<proto::WirePane> children) {
        auto root = proto::WirePane { .paneId = 1, .split = 2, .session = 5 };
        root.children = std::move(children);
        return root;
    };

    SECTION("a split with no children")
    {
        auto state = proto::LayoutState {};
        state.tabs.push_back(proto::WireTab { .root = rootWith({}) });
        auto const wl = wireToLayout(state); // must not read OOB
        REQUIRE(wl.layout.tabs.size() == 1);
        CHECK(wl.layout.tabs[0].root.isLeaf());
    }
    SECTION("a split with a single child")
    {
        auto state = proto::LayoutState {};
        state.tabs.push_back(proto::WireTab { .root = rootWith({ leaf(9) }) });
        auto const wl = wireToLayout(state); // must not read OOB
        REQUIRE(wl.layout.tabs.size() == 1);
        CHECK(wl.layout.tabs[0].root.isLeaf());
    }
}

namespace
{

/// A page size in the (columns, lines) order these tests read in.
vtpty::PageSize cells(int columns, int lines)
{
    return vtpty::PageSize { .lines = vtpty::LineCount(lines), .columns = vtpty::ColumnCount(columns) };
}

/// Resolves leaf extents from a session -> size table; an absent session is an unrealized leaf.
vthost::client::LeafCellSize sizesOf(std::unordered_map<uint64_t, vtpty::PageSize> table)
{
    return [table = std::move(table)](uint64_t session) -> std::optional<vtpty::PageSize> {
        auto const it = table.find(session);
        return it != table.end() ? std::optional { it->second } : std::nullopt;
    };
}

} // namespace

TEST_CASE("composeClientArea sums a split's axis and spans the other", "[vthost][layout]")
{
    SECTION("a lone leaf is the whole area")
    {
        CHECK(composeClientArea(leaf(100), sizesOf({ { 100, cells(80, 25) } })) == cells(80, 25));
    }

    SECTION("a side-by-side split adds the divider column and keeps the height")
    {
        auto const root = split(2, 5000, leaf(100), leaf(101));
        auto const sizes = sizesOf({ { 100, cells(50, 30) }, { 101, cells(49, 30) } });
        CHECK(composeClientArea(root, sizes) == cells(100, 30));
    }

    SECTION("a stacked split adds the divider line and keeps the width")
    {
        auto const root = split(1, 5000, leaf(100), leaf(101));
        auto const sizes = sizesOf({ { 100, cells(80, 12) }, { 101, cells(80, 11) } });
        CHECK(composeClientArea(root, sizes) == cells(80, 24));
    }

    SECTION("nested splits compose per level")
    {
        // 160 columns wide: left 80 | divider | right 79, the right half stacked 24 over 25.
        auto const root = split(2, 5000, leaf(100), split(1, 5000, leaf(101), leaf(102)));
        auto const sizes =
            sizesOf({ { 100, cells(80, 50) }, { 101, cells(79, 24) }, { 102, cells(79, 25) } });
        CHECK(composeClientArea(root, sizes) == cells(160, 50));
    }

    SECTION("an unresolved leaf yields no area at all")
    {
        auto const root = split(2, 5000, leaf(100), leaf(101));
        CHECK_FALSE(composeClientArea(root, sizesOf({ { 100, cells(50, 30) } })).has_value());
    }
}

TEST_CASE("composeClientArea inverts layoutInCells", "[vthost][layout]")
{
    // The round trip that matters: project an area onto a tree, compose the resulting leaf extents
    // back, and the area must reappear — so a client reporting the composition gets its own panes
    // back from the daemon's projection rather than a re-divided approximation.
    auto state = proto::LayoutState {};
    state.tabs.push_back(
        proto::WireTab { .root = split(2, 6000, leaf(100), split(1, 4000, leaf(101), leaf(102))) });

    auto const area = cells(160, 50);
    auto const rebuilt = realize(wireToLayout(state));
    auto const projected = vtworkspace::layoutInCells(*rebuilt->tabs[0]->rootPane(), area);
    REQUIRE(projected.size() == 3);

    auto table = std::unordered_map<uint64_t, vtpty::PageSize> {};
    for (auto const& rect: projected)
    {
        auto const* pane = rebuilt->tabs[0]->rootPane()->findPane(rect.pane);
        REQUIRE(pane != nullptr);
        table.emplace(pane->session().value, cells(rect.width, rect.height));
    }
    // Moved OUTSIDE the assertion: CHECK re-evaluates its expression while building the failure
    // message, which would move from `table` twice.
    auto const composed = composeClientArea(state.tabs[0].root, sizesOf(std::move(table)));
    CHECK(composed == area);
}

TEST_CASE("paneTreeHosts finds a session anywhere in the tree", "[vthost][layout]")
{
    auto const root = split(2, 5000, leaf(100), split(1, 5000, leaf(101), leaf(102)));
    CHECK(vthost::client::paneTreeHosts(root, 100));
    CHECK(vthost::client::paneTreeHosts(root, 102));
    CHECK_FALSE(vthost::client::paneTreeHosts(root, 999));
    CHECK_FALSE(vthost::client::paneTreeHosts(root, 0)); // a split node's own (unset) session

    // A malformed split collapses to a leaf here exactly as it does in wireToLayoutPane, so the
    // two never disagree about which session a node carries.
    auto malformed = proto::WirePane { .paneId = 1, .split = 2, .session = 7 };
    CHECK(vthost::client::paneTreeHosts(malformed, 7));
}
