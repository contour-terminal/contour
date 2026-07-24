// SPDX-License-Identifier: Apache-2.0
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <map>
#include <optional>
#include <span>
#include <string>

#include <vtworkspace/LayoutTree.h>
#include <vtworkspace/ModelEvents.h>
#include <vtworkspace/SessionModel.h>

using namespace vtworkspace;

namespace
{

LayoutPane leaf(std::string cmd, double ratio = 1.0)
{
    LayoutPane p;
    p.command = std::move(cmd);
    p.ratio = ratio;
    return p;
}
LayoutPane split(SplitState o, std::vector<LayoutPane> kids)
{
    LayoutPane p;
    p.orientation = o;
    p.children = std::move(kids);
    return p;
}

struct NullEvents: ModelEvents
{
    void tabAdded(WindowId, TabId, int) override {}
    void tabClosed(WindowId, TabId, int) override {}
    void tabMoved(WindowId, TabId, int, int) override {}
    void activeTabChanged(WindowId, TabId, int) override {}
    void paneSplit(TabId, PaneId, PaneId) override {}
    void paneClosed(TabId, PaneId, PaneId) override {}
    void activePaneChanged(TabId, PaneId) override {}
    void paneRatioChanged(TabId, PaneId, double) override {}
    void tabTitleChanged(TabId) override {}
    void tabColorChanged(TabId) override {}
};

// A harness that pre-mints session ids and records the command each id was seeded with.
struct RealizeHarness
{
    NullEvents events;
    uint64_t nextSession = 1000;
    std::optional<SessionId> pending;
    std::map<uint64_t, std::string> commandBySession;
    std::size_t seedCount = 0;            ///< How many seeds have been accepted so far.
    std::optional<std::size_t> seedLimit; ///< If set, seeds beyond this many are refused.

    SessionModel model { events, [this]() -> SessionId {
                            if (pending)
                            {
                                auto id = *pending;
                                pending.reset();
                                return id;
                            }
                            return SessionId { nextSession++ };
                        } };

    PaneSeeder seeder()
    {
        return [this](LayoutPane const& leaf) -> bool {
            // Model a factory that can back only seedLimit sessions: once exhausted, refuse
            // (without staging a pending id), exactly like NativeController's drained pool.
            if (seedLimit && seedCount >= *seedLimit)
                return false;
            ++seedCount;
            auto const id = SessionId { nextSession++ };
            pending = id;
            commandBySession[id.value] = leaf.command.value_or("");
            return true;
        };
    }
};
} // namespace

TEST_CASE("realizeLayoutTab: a single-pane tab creates one leaf with its command", "[layout][realize]")
{
    RealizeHarness h;
    auto* win = h.model.createWindow();

    LayoutTab tab;
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

    LayoutTab tab;
    tab.root.orientation = SplitState::Vertical;
    tab.root.children = { [] {
                             LayoutPane p;
                             p.command = "left";
                             return p;
                         }(),
                          [] {
                              LayoutPane p;
                              p.command = "right";
                              return p;
                          }() };

    auto* modelTab = realizeLayoutTab(h.model, win->id(), tab, h.seeder());
    REQUIRE(modelTab->paneCount() == 2);
    auto* root = modelTab->rootPane();
    REQUIRE_FALSE(root->isLeaf());
    CHECK(root->splitState() == SplitState::Vertical);
    CHECK(h.commandBySession.at(root->first()->session().value) == "left");
    CHECK(h.commandBySession.at(root->second()->session().value) == "right");
}

TEST_CASE("realizeLayoutTab: a refused seed mid-tree stops without allocating an unbacked pane",
          "[layout][realize]")
{
    RealizeHarness h;
    h.seedLimit = 1; // the factory can back the first pane only, then runs dry
    auto* win = h.model.createWindow();

    LayoutTab tab;
    tab.root.orientation = SplitState::Vertical;
    tab.root.children = { [] {
                             LayoutPane p;
                             p.command = "left";
                             return p;
                         }(),
                          [] {
                              LayoutPane p;
                              p.command = "right";
                              return p;
                          }() };

    auto* modelTab = realizeLayoutTab(h.model, win->id(), tab, h.seeder());
    REQUIRE(modelTab != nullptr);
    // The sibling's seed was refused, so no second (permanently blank) pane was allocated.
    CHECK(modelTab->paneCount() == 1);
    CHECK(modelTab->rootPane()->isLeaf());
    // Exactly one session was seeded, and it backs the single surviving pane.
    CHECK(h.commandBySession.size() == 1);
    CHECK(h.commandBySession.at(modelTab->rootPane()->session().value) == "left");
}

TEST_CASE("realizeLayoutTab: a refused first seed creates no tab at all", "[layout][realize]")
{
    RealizeHarness h;
    h.seedLimit = 0; // the factory can back nothing
    auto* win = h.model.createWindow();

    LayoutTab tab;
    tab.root.command = "nvim";

    auto* modelTab = realizeLayoutTab(h.model, win->id(), tab, h.seeder());
    CHECK(modelTab == nullptr);
    CHECK(h.commandBySession.empty()); // nothing was seeded, nothing orphaned
}

TEST_CASE("realizeLayoutTab: nested split produces the right tree and commands", "[layout][realize]")
{
    RealizeHarness h;
    auto* win = h.model.createWindow();

    auto mk = [](std::string c) {
        LayoutPane p;
        p.command = std::move(c);
        return p;
    };
    LayoutTab tab;
    tab.root.orientation = SplitState::Vertical;
    LayoutPane nested;
    nested.orientation = SplitState::Horizontal;
    nested.children = { mk("htop"), mk("logs") };
    tab.root.children = { mk("dev"), nested };

    auto* modelTab = realizeLayoutTab(h.model, win->id(), tab, h.seeder());
    REQUIRE(modelTab->paneCount() == 3);
    auto* root = modelTab->rootPane();
    CHECK(root->splitState() == SplitState::Vertical);
    CHECK(h.commandBySession.at(root->first()->session().value) == "dev");
    auto* tail = root->second();
    REQUIRE_FALSE(tail->isLeaf());
    CHECK(tail->splitState() == SplitState::Horizontal);
    CHECK(h.commandBySession.at(tail->first()->session().value) == "htop");
    CHECK(h.commandBySession.at(tail->second()->session().value) == "logs");
}

TEST_CASE("realizeLayoutTab: an unknown window is refused before any session is seeded", "[layout][realize]")
{
    // Seeding spawns a REAL backing session, so the window must be validated first: seeding and
    // then failing to create the tab would orphan that session (it would outlive every reference
    // to it, still holding its PTY).
    RealizeHarness h;

    LayoutTab tab;
    tab.root.command = "nvim";

    auto* modelTab = realizeLayoutTab(h.model, WindowId { 4711 }, tab, h.seeder());
    CHECK(modelTab == nullptr);
    CHECK(h.commandBySession.empty()); // nothing was seeded
}

TEST_CASE("LayoutTree: leftmostLeaf descends first children", "[layout][builder]")
{
    auto tree =
        split(SplitState::Vertical, { split(SplitState::Horizontal, { leaf("a"), leaf("b") }), leaf("c") });
    CHECK(*leftmostLeaf(tree).command == "a");
    CHECK(*leftmostLeaf(leaf("solo")).command == "solo");
}

TEST_CASE("LayoutTree: ratioForFirst splits weight of child0 vs the rest", "[layout][builder]")
{
    auto equalThree = split(SplitState::Vertical, { leaf("a", 1.0), leaf("b", 1.0), leaf("c", 1.0) });
    CHECK(ratioForFirst(equalThree) == Catch::Approx(1.0 / 3.0));

    auto weighted = split(SplitState::Vertical, { leaf("a", 0.6), leaf("b", 0.4) });
    CHECK(ratioForFirst(weighted) == Catch::Approx(0.6));
}

TEST_CASE("LayoutTree: ratioForFirst weighs a tail group of siblings", "[layout][builder]")
{
    // Realization splits child0 off, then recurses on the REST of the same children (a subspan).
    // Each step's ratio must be the first child's share of the siblings still to be placed.
    auto three = split(SplitState::Vertical, { leaf("a", 0.5), leaf("b", 0.3), leaf("c", 0.2) });
    auto const children = std::span<LayoutPane const> { three.children };

    CHECK(ratioForFirst(children) == Catch::Approx(0.5));                       // a | (b, c)
    CHECK(ratioForFirst(children.subspan(1)) == Catch::Approx(0.6));            // b | c, within the 0.5 left
    CHECK(ratioForFirst(children.subspan(2)) == Catch::Approx(1.0));            // c alone
    CHECK(ratioForFirst(std::span<LayoutPane const> {}) == Catch::Approx(0.5)); // no children
}

TEST_CASE("LayoutTree: an explicit ratio is a fraction; unspecified siblings share the rest",
          "[layout][builder]")
{
    // The documented example: `ratio: 0.6` next to an unspecified sibling means a 60/40 split —
    // not 0.6 weighted against an implicit default weight.
    auto const documented = split(SplitState::Vertical, { leaf("dev", 0.6), leaf("htop", 0.0) });
    auto asUnset = documented;
    asUnset.children[1].ratio.reset();
    CHECK(ratioForFirst(asUnset) == Catch::Approx(0.6));

    // All-unspecified children share equally.
    auto equal = split(SplitState::Vertical, { leaf("a"), leaf("b") });
    for (auto& child: equal.children)
        child.ratio.reset();
    CHECK(ratioForFirst(equal) == Catch::Approx(0.5));

    // Degenerate: the specified fractions over-commit the space; the unspecified sibling still
    // gets a minimal share instead of collapsing to zero (and no division blows up).
    auto overCommitted = split(SplitState::Vertical, { leaf("a", 1.0), leaf("b", 0.0) });
    overCommitted.children[1].ratio.reset();
    CHECK(ratioForFirst(overCommitted) > 0.9);
    CHECK(ratioForFirst(overCommitted) < 1.0);
}

TEST_CASE("serializeTab: reproduces the pane tree with resolved commands", "[layout][save]")
{
    RealizeHarness h;
    auto* win = h.model.createWindow();
    auto mk = [](std::string c) {
        LayoutPane p;
        p.command = std::move(c);
        return p;
    };
    LayoutTab spec;
    spec.title = "server";
    spec.root.orientation = SplitState::Vertical;
    spec.root.children = { mk("dev"), mk("logs") };
    auto* tab = realizeLayoutTab(h.model, win->id(), spec, h.seeder());
    REQUIRE(tab != nullptr);

    // Only the "dev" leaf carries a profile override, so serializePane must capture it there and
    // leave the "logs" leaf's profile unset — a fake stand-in for a session-side profile lookup
    // ONLY being non-empty for a real per-pane override.
    auto resolve = [&](SessionId id) {
        auto const command = h.commandBySession.at(id.value);
        return PaneLeafData { .command = command,
                              .arguments = {},
                              .directory = std::string { "/work" },
                              .profile = command == "dev" ? std::optional<std::string> { "dev-profile" }
                                                          : std::nullopt };
    };
    auto const out = serializeTab(*tab, resolve);
    CHECK(out.title == "server");
    REQUIRE_FALSE(out.root.isLeaf());
    CHECK(out.root.orientation == SplitState::Vertical);
    REQUIRE(out.root.children.size() == 2);
    CHECK(*out.root.children[0].command == "dev");
    REQUIRE(out.root.children[0].directory.has_value());
    CHECK(*out.root.children[0].directory == "/work");
    REQUIRE(out.root.children[0].profile.has_value());
    CHECK(*out.root.children[0].profile == "dev-profile");
    CHECK(*out.root.children[1].command == "logs");
    CHECK_FALSE(out.root.children[1].profile.has_value());
}
