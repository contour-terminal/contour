// SPDX-License-Identifier: Apache-2.0
#include <contour/LayoutBuilder.h>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <map>
#include <optional>
#include <string>

#include <vtmux/ModelEvents.h>
#include <vtmux/SessionModel.h>

using namespace contour;
using contour::config::LayoutPane;

namespace
{
LayoutPane leaf(std::string cmd, double ratio = 1.0)
{
    LayoutPane p;
    p.command = std::move(cmd);
    p.ratio = ratio;
    return p;
}
LayoutPane split(vtmux::SplitState o, std::vector<LayoutPane> kids)
{
    LayoutPane p;
    p.orientation = o;
    p.children = std::move(kids);
    return p;
}

struct NullEvents: vtmux::ModelEvents
{
    void tabAdded(vtmux::WindowId, vtmux::TabId, int) override {}
    void tabClosed(vtmux::WindowId, vtmux::TabId, int) override {}
    void tabMoved(vtmux::WindowId, vtmux::TabId, int, int) override {}
    void activeTabChanged(vtmux::WindowId, vtmux::TabId, int) override {}
    void paneSplit(vtmux::TabId, vtmux::PaneId, vtmux::PaneId) override {}
    void paneClosed(vtmux::TabId, vtmux::PaneId, vtmux::PaneId) override {}
    void activePaneChanged(vtmux::TabId, vtmux::PaneId) override {}
    void paneRatioChanged(vtmux::TabId, vtmux::PaneId, double) override {}
    void tabTitleChanged(vtmux::TabId) override {}
    void tabColorChanged(vtmux::TabId) override {}
};

// A harness that pre-mints session ids and records the command each id was seeded with.
struct RealizeHarness
{
    NullEvents events;
    uint64_t nextSession = 1000;
    std::optional<vtmux::SessionId> pending;
    std::map<uint64_t, std::string> commandBySession;

    vtmux::SessionModel model { events, [this]() -> vtmux::SessionId {
                                   if (pending)
                                   {
                                       auto id = *pending;
                                       pending.reset();
                                       return id;
                                   }
                                   return vtmux::SessionId { nextSession++ };
                               } };

    PaneSeeder seeder()
    {
        return [this](config::LayoutPane const& leaf) {
            auto const id = vtmux::SessionId { nextSession++ };
            pending = id;
            commandBySession[id.value] = leaf.command.value_or("");
        };
    }
};
} // namespace

TEST_CASE("realizeLayoutTab: a single-pane tab creates one leaf with its command", "[layout][realize]")
{
    RealizeHarness h;
    auto* win = h.model.createWindow();

    config::LayoutTab tab;
    tab.title = "editor";
    tab.root.command = "nvim";

    auto* modelTab = realizeLayoutTab(h.model, win->id(), tab, h.seeder());
    REQUIRE(modelTab != nullptr);
    CHECK(modelTab->paneCount() == 1);
    CHECK(modelTab->runtimeTitle() == "editor");
    auto const session = modelTab->rootPane()->session();
    CHECK(h.commandBySession.at(session.value) == "nvim");
}

TEST_CASE("realizeLayoutTab: two side-by-side panes build a Vertical split", "[layout][realize]")
{
    RealizeHarness h;
    auto* win = h.model.createWindow();

    config::LayoutTab tab;
    tab.root.orientation = vtmux::SplitState::Vertical;
    tab.root.children = { [] {
                             config::LayoutPane p;
                             p.command = "left";
                             return p;
                         }(),
                          [] {
                              config::LayoutPane p;
                              p.command = "right";
                              return p;
                          }() };

    auto* modelTab = realizeLayoutTab(h.model, win->id(), tab, h.seeder());
    REQUIRE(modelTab->paneCount() == 2);
    auto* root = modelTab->rootPane();
    REQUIRE_FALSE(root->isLeaf());
    CHECK(root->splitState() == vtmux::SplitState::Vertical);
    CHECK(h.commandBySession.at(root->first()->session().value) == "left");
    CHECK(h.commandBySession.at(root->second()->session().value) == "right");
}

TEST_CASE("realizeLayoutTab: nested split produces the right tree and commands", "[layout][realize]")
{
    RealizeHarness h;
    auto* win = h.model.createWindow();

    auto mk = [](std::string c) {
        config::LayoutPane p;
        p.command = std::move(c);
        return p;
    };
    config::LayoutTab tab;
    tab.root.orientation = vtmux::SplitState::Vertical;
    config::LayoutPane nested;
    nested.orientation = vtmux::SplitState::Horizontal;
    nested.children = { mk("htop"), mk("logs") };
    tab.root.children = { mk("dev"), nested };

    auto* modelTab = realizeLayoutTab(h.model, win->id(), tab, h.seeder());
    REQUIRE(modelTab->paneCount() == 3);
    auto* root = modelTab->rootPane();
    CHECK(root->splitState() == vtmux::SplitState::Vertical);
    CHECK(h.commandBySession.at(root->first()->session().value) == "dev");
    auto* tail = root->second();
    REQUIRE_FALSE(tail->isLeaf());
    CHECK(tail->splitState() == vtmux::SplitState::Horizontal);
    CHECK(h.commandBySession.at(tail->first()->session().value) == "htop");
    CHECK(h.commandBySession.at(tail->second()->session().value) == "logs");
}

TEST_CASE("LayoutBuilder: leftmostLeaf descends first children", "[layout][builder]")
{
    auto tree = split(vtmux::SplitState::Vertical,
                      { split(vtmux::SplitState::Horizontal, { leaf("a"), leaf("b") }), leaf("c") });
    CHECK(*leftmostLeaf(tree).command == "a");
    CHECK(*leftmostLeaf(leaf("solo")).command == "solo");
}

TEST_CASE("LayoutBuilder: ratioForFirst splits weight of child0 vs the rest", "[layout][builder]")
{
    auto equalThree = split(vtmux::SplitState::Vertical, { leaf("a", 1.0), leaf("b", 1.0), leaf("c", 1.0) });
    CHECK(ratioForFirst(equalThree) == Catch::Approx(1.0 / 3.0));

    auto weighted = split(vtmux::SplitState::Vertical, { leaf("a", 0.6), leaf("b", 0.4) });
    CHECK(ratioForFirst(weighted) == Catch::Approx(0.6));
}

TEST_CASE("LayoutBuilder: tailGroup drops the first child", "[layout][builder]")
{
    auto three = split(vtmux::SplitState::Vertical, { leaf("a"), leaf("b"), leaf("c") });
    auto tail = tailGroup(three);
    REQUIRE_FALSE(tail.isLeaf());
    REQUIRE(tail.children.size() == 2);
    CHECK(*tail.children[0].command == "b");

    auto two = split(vtmux::SplitState::Vertical, { leaf("a"), leaf("b") });
    auto tail2 = tailGroup(two);
    CHECK(tail2.isLeaf()); // single remaining child collapses to that leaf
    CHECK(*tail2.command == "b");
}
