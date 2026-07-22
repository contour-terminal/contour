// SPDX-License-Identifier: Apache-2.0
#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <deque>
#include <memory>
#include <utility>
#include <vector>

#include <muxserver/client/LayoutReconstruction.h>
#include <vtmux/ModelEvents.h>
#include <vtmux/Pane.h>
#include <vtmux/SessionModel.h>
#include <vtmux/Tab.h>

using muxserver::client::planReconstruction;
using muxserver::client::ReconstructStep;
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

/// Replays a reconstruction plan against a fresh model whose session allocator
/// hands out the plan's sessions in NewTab/Split order — so the rebuilt leaves
/// carry the very ids the layout named.
struct Rebuilt
{
    NoopEvents events;
    std::unique_ptr<vtmux::SessionModel> model;
    std::vector<vtmux::Tab*> tabs;
};

std::unique_ptr<Rebuilt> replay(std::vector<ReconstructStep> const& steps)
{
    auto queue = std::make_shared<std::deque<uint64_t>>();
    for (auto const& step: steps)
        if (step.kind == ReconstructStep::Kind::NewTab || step.kind == ReconstructStep::Kind::Split)
            queue->push_back(step.session);

    auto rebuilt = std::make_unique<Rebuilt>();
    rebuilt->model = std::make_unique<vtmux::SessionModel>(rebuilt->events, [queue] {
        auto const id = queue->front();
        queue->pop_front();
        return vtmux::SessionId { id };
    });
    auto const window = rebuilt->model->createWindow()->id();

    auto* current = static_cast<vtmux::Tab*>(nullptr);
    for (auto const& step: steps)
    {
        switch (step.kind)
        {
            case ReconstructStep::Kind::NewTab:
                current = rebuilt->model->createTab(window);
                rebuilt->tabs.push_back(current);
                break;
            case ReconstructStep::Kind::Split:
                rebuilt->model->splitActivePane(
                    current->id(), static_cast<vtmux::SplitState>(step.orientation), step.ratio / 10000.0);
                break;
            case ReconstructStep::Kind::Activate: {
                auto* leafPane = current->rootPane()->findLeaf(vtmux::SessionId { step.session });
                REQUIRE(leafPane != nullptr);
                rebuilt->model->setActivePane(current->id(), leafPane->id());
                break;
            }
        }
    }
    return rebuilt;
}

} // namespace

TEST_CASE("planReconstruction rebuilds a single-pane tab", "[muxserver][layout]")
{
    auto layout = proto::LayoutState {};
    layout.tabs.push_back(proto::WireTab { .root = leaf(100) });

    auto const steps = planReconstruction(layout);
    REQUIRE(steps.size() == 1);
    CHECK(steps[0].kind == ReconstructStep::Kind::NewTab);
    CHECK(steps[0].session == 100);

    auto const rebuilt = replay(steps);
    REQUIRE(rebuilt->tabs.size() == 1);
    requireMatches(*rebuilt->tabs[0]->rootPane(), layout.tabs[0].root);
}

TEST_CASE("planReconstruction rebuilds a single split", "[muxserver][layout]")
{
    auto layout = proto::LayoutState {};
    layout.tabs.push_back(proto::WireTab { .root = split(2, 6000, leaf(100), leaf(101)) });

    auto const steps = planReconstruction(layout);
    REQUIRE(steps.size() == 2);
    CHECK(steps[0] == ReconstructStep { .kind = ReconstructStep::Kind::NewTab, .session = 100 });
    CHECK(steps[1]
          == ReconstructStep {
              .kind = ReconstructStep::Kind::Split, .session = 101, .orientation = 2, .ratio = 6000 });

    auto const rebuilt = replay(steps);
    REQUIRE(rebuilt->tabs.size() == 1);
    requireMatches(*rebuilt->tabs[0]->rootPane(), layout.tabs[0].root);
}

TEST_CASE("planReconstruction rebuilds a nested split tree (re-activation path)", "[muxserver][layout]")
{
    // root = H-split( V-split(leaf 1, leaf 2), leaf 3 ) — the left child is itself
    // a split, which forces the plan to re-activate the left leaf and recurse.
    auto layout = proto::LayoutState {};
    layout.tabs.push_back(
        proto::WireTab { .root = split(1, 4000, split(2, 7000, leaf(1), leaf(2)), leaf(3)) });

    auto const rebuilt = replay(planReconstruction(layout));
    REQUIRE(rebuilt->tabs.size() == 1);
    requireMatches(*rebuilt->tabs[0]->rootPane(), layout.tabs[0].root);
}

TEST_CASE("planReconstruction rebuilds multiple tabs", "[muxserver][layout]")
{
    auto layout = proto::LayoutState {};
    layout.tabs.push_back(proto::WireTab { .root = leaf(10) });
    layout.tabs.push_back(proto::WireTab { .root = split(2, 5000, leaf(20), leaf(21)) });
    layout.tabs.push_back(proto::WireTab { .root = leaf(30) });

    auto const rebuilt = replay(planReconstruction(layout));
    REQUIRE(rebuilt->tabs.size() == 3);
    requireMatches(*rebuilt->tabs[0]->rootPane(), layout.tabs[0].root);
    requireMatches(*rebuilt->tabs[1]->rootPane(), layout.tabs[1].root);
    requireMatches(*rebuilt->tabs[2]->rootPane(), layout.tabs[2].root);
}

TEST_CASE("planReconstruction yields nothing for an empty layout", "[muxserver][layout]")
{
    CHECK(planReconstruction(proto::LayoutState {}).empty());
}
