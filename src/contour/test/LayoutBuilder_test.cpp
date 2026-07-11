// SPDX-License-Identifier: Apache-2.0
#include <contour/Config.h>
#include <contour/LayoutBuilder.h>

#include <QtCore/QTemporaryDir>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>

#include <vtmux/ModelEvents.h>
#include <vtmux/SessionModel.h>

using namespace contour;
using contour::config::LayoutPane;
using namespace std::string_view_literals;

namespace
{

/// Writes @p yaml to a fresh `contour.yml` inside a QTemporaryDir and loads it through the
/// production config file loader. Mirrors Config_test.cpp's `loadFromYaml` helper.
[[nodiscard]] contour::config::Config loadFromYamlString(std::string_view yaml)
{
    QTemporaryDir dir;
    auto const path = std::filesystem::path(dir.path().toStdString()) / "contour.yml";
    {
        auto out = std::ofstream(path);
        out << yaml;
    }
    auto config = contour::config::Config {};
    contour::config::loadConfigFromFile(config, path);
    return config;
}

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

TEST_CASE("emitLayoutsYaml: round-trips a leaf + bare + split layout through the parser",
          "[layout][save][builder]")
{
    auto mk = [](std::string c) {
        config::LayoutPane p;
        p.command = std::move(c);
        return p;
    };

    config::Layout work;

    // t0: a leaf tab with title + color + command + arguments + directory.
    config::LayoutTab t0;
    t0.title = "editor";
    t0.color = vtbackend::RGBColor { "#D75F00" };
    t0.root = mk("nvim");
    t0.root.arguments = { "-u", "NONE" };
    t0.root.directory = std::filesystem::path { "/tmp" };

    // t1: a bare leaf tab (command only, no tab-level keys at all).
    config::LayoutTab t1;
    t1.root = mk("claude");

    // t2: a tab with a nested split: vertical { leaf, horizontal { leaf, leaf } }.
    config::LayoutTab t2;
    t2.title = "servers";
    config::LayoutPane nested;
    nested.orientation = vtmux::SplitState::Horizontal;
    nested.children = { mk("htop"), mk("journalctl -f") };
    t2.root.orientation = vtmux::SplitState::Vertical;
    t2.root.children = { mk("npm run dev"), nested };

    work.tabs = { t0, t1, t2 };

    std::unordered_map<std::string, config::Layout> layouts { { "work", work } };
    auto const yaml = emitLayoutsYaml(layouts);

    auto const cfg = loadFromYamlString(yaml);
    REQUIRE(cfg.layouts.value().contains("work"));
    auto const& parsed = cfg.layouts.value().at("work");
    REQUIRE(parsed.tabs.size() == 3);

    auto const& p0 = parsed.tabs[0];
    CHECK(p0.title == "editor");
    REQUIRE(p0.color.has_value());
    CHECK(*p0.color == vtbackend::RGBColor { "#D75F00" });
    CHECK(p0.root.isLeaf());
    REQUIRE(p0.root.command.has_value());
    CHECK(*p0.root.command == "nvim");
    REQUIRE(p0.root.arguments.size() == 2);
    CHECK(p0.root.arguments[0] == "-u");
    CHECK(p0.root.arguments[1] == "NONE");
    REQUIRE(p0.root.directory.has_value());
    CHECK(p0.root.directory->generic_string() == "/tmp");

    auto const& p1 = parsed.tabs[1];
    CHECK_FALSE(p1.title.has_value());
    CHECK_FALSE(p1.color.has_value());
    CHECK(p1.root.isLeaf());
    REQUIRE(p1.root.command.has_value());
    CHECK(*p1.root.command == "claude");

    auto const& p2 = parsed.tabs[2];
    CHECK(p2.title == "servers");
    REQUIRE_FALSE(p2.root.isLeaf());
    CHECK(p2.root.orientation == vtmux::SplitState::Vertical);
    REQUIRE(p2.root.children.size() == 2);
    CHECK(*p2.root.children[0].command == "npm run dev");
    auto const& nestedParsed = p2.root.children[1];
    REQUIRE_FALSE(nestedParsed.isLeaf());
    CHECK(nestedParsed.orientation == vtmux::SplitState::Horizontal);
    REQUIRE(nestedParsed.children.size() == 2);
    CHECK(*nestedParsed.children[0].command == "htop");
    CHECK(*nestedParsed.children[1].command == "journalctl -f");
}

TEST_CASE("serializeTab: reproduces the pane tree with resolved commands", "[layout][save]")
{
    RealizeHarness h;
    auto* win = h.model.createWindow();
    auto mk = [](std::string c) {
        config::LayoutPane p;
        p.command = std::move(c);
        return p;
    };
    config::LayoutTab spec;
    spec.title = "srv";
    spec.root.orientation = vtmux::SplitState::Vertical;
    spec.root.children = { mk("dev"), mk("logs") };
    auto* tab = realizeLayoutTab(h.model, win->id(), spec, h.seeder());
    REQUIRE(tab != nullptr);

    auto resolve = [&](vtmux::SessionId id) {
        return PaneLeafData { .command = h.commandBySession.at(id.value),
                              .arguments = {},
                              .directory = std::string { "/work" } };
    };
    auto const out = serializeTab(*tab, resolve);
    CHECK(out.title == "srv");
    REQUIRE_FALSE(out.root.isLeaf());
    CHECK(out.root.orientation == vtmux::SplitState::Vertical);
    REQUIRE(out.root.children.size() == 2);
    CHECK(*out.root.children[0].command == "dev");
    CHECK(out.root.children[0].directory->string() == "/work");
    CHECK(*out.root.children[1].command == "logs");
}
