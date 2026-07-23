// SPDX-License-Identifier: Apache-2.0
#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <memory>
#include <utility>
#include <vector>

#include <muxserver/client/LayoutReconstruction.h>
#include <vtmux/LayoutTree.h>
#include <vtmux/ModelEvents.h>
#include <vtmux/Pane.h>
#include <vtmux/SessionModel.h>
#include <vtmux/Tab.h>

using muxserver::client::WireLayout;
using muxserver::client::wireToLayout;
namespace proto = muxserver::proto;

namespace
{

/// The minimal ModelEvents: overrides only the pure-virtual "completed change"
/// callbacks (the bracket hooks keep their no-op defaults).
struct NoopEvents: vtmux::ModelEvents
{
    void tabAdded(vtmux::WindowId, vtmux::TabId, int) override {}
    void tabClosed(vtmux::WindowId, vtmux::TabId, int) override {}
    void tabMoved(vtmux::WindowId, vtmux::TabId, int, int) override {}
    void tabMovedToWindow(vtmux::WindowId, vtmux::TabId, int, vtmux::WindowId, int) override {}
    void activeTabChanged(vtmux::WindowId, vtmux::TabId, int) override {}
    void paneSplit(vtmux::TabId, vtmux::PaneId, vtmux::PaneId) override {}
    void paneClosed(vtmux::TabId, vtmux::PaneId, vtmux::PaneId) override {}
    void activePaneChanged(vtmux::TabId, vtmux::PaneId) override {}
    void paneRatioChanged(vtmux::TabId, vtmux::PaneId, double) override {}
    void tabTitleChanged(vtmux::TabId) override {}
    void tabColorChanged(vtmux::TabId) override {}
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
void requireMatches(vtmux::Pane const& pane, proto::WirePane const& wire)
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
    std::unique_ptr<vtmux::SessionModel> model;
    std::vector<vtmux::Tab*> tabs;
};

std::unique_ptr<Rebuilt> realize(WireLayout const& wl)
{
    auto rebuilt = std::make_unique<Rebuilt>();
    auto pending = vtmux::SessionId {};
    rebuilt->model = std::make_unique<vtmux::SessionModel>(rebuilt->events, [&pending] { return pending; });
    auto const window = rebuilt->model->createWindow()->id();

    // The seeder stages each leaf's remote session so the model allocator hands it
    // back for that pane — exactly what a GUI seeder would do before binding.
    auto const seed = [&](vtmux::LayoutPane const& leafPane) {
        pending = vtmux::SessionId { wl.leafSession.at(&leafPane) };
        return true;
    };
    for (auto const& tab: wl.layout.tabs)
    {
        auto* modelTab = vtmux::realizeLayoutTab(*rebuilt->model, window, tab, seed);
        REQUIRE(modelTab != nullptr);
        rebuilt->tabs.push_back(modelTab);
    }
    return rebuilt;
}

} // namespace

TEST_CASE("wireToLayout realizes a single-pane tab", "[muxserver][layout]")
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

TEST_CASE("wireToLayout realizes a single split", "[muxserver][layout]")
{
    auto state = proto::LayoutState {};
    state.tabs.push_back(proto::WireTab { .root = split(2, 6000, leaf(100), leaf(101)) });

    auto const wl = wireToLayout(state);
    REQUIRE(wl.layout.tabs.size() == 1);
    auto const& root = wl.layout.tabs[0].root;
    REQUIRE_FALSE(root.isLeaf());
    CHECK(root.orientation == vtmux::SplitState::Vertical);
    REQUIRE(root.children.size() == 2);
    REQUIRE(root.children[0].ratio.has_value());
    CHECK(std::lround(*root.children[0].ratio * 10000.0) == 6000); // first child's share

    auto const rebuilt = realize(wl);
    requireMatches(*rebuilt->tabs[0]->rootPane(), state.tabs[0].root);
}

TEST_CASE("wireToLayout realizes a nested split tree", "[muxserver][layout]")
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

TEST_CASE("wireToLayout realizes multiple tabs", "[muxserver][layout]")
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

TEST_CASE("wireToLayout yields no tabs for an empty layout", "[muxserver][layout]")
{
    CHECK(wireToLayout(proto::LayoutState {}).layout.tabs.empty());
}

TEST_CASE("wireToLayout tolerates a malformed split node without reading out of bounds",
          "[muxserver][layout]")
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
