// SPDX-License-Identifier: Apache-2.0
#include <contour/Config.h>
#include <contour/LayoutBuilder.h>
#include <contour/test/GuiTestFixtures.h>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>

#include <vtmux/ModelEvents.h>
#include <vtmux/SessionModel.h>

using namespace contour;
using contour::config::LayoutPane;
using contour::test::loadConfigFromYaml;
using namespace std::string_view_literals;

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

TEST_CASE("realizeLayoutTab: an unknown window is refused before any session is seeded", "[layout][realize]")
{
    // Seeding spawns a REAL backing session, so the window must be validated first: seeding and
    // then failing to create the tab would orphan that session (it would outlive every reference
    // to it, still holding its PTY).
    RealizeHarness h;

    config::LayoutTab tab;
    tab.root.command = "nvim";

    auto* modelTab = realizeLayoutTab(h.model, vtmux::WindowId { 4711 }, tab, h.seeder());
    CHECK(modelTab == nullptr);
    CHECK(h.commandBySession.empty()); // nothing was seeded
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

TEST_CASE("LayoutBuilder: ratioForFirst weighs a tail group of siblings", "[layout][builder]")
{
    // Realization splits child0 off, then recurses on the REST of the same children (a subspan).
    // Each step's ratio must be the first child's share of the siblings still to be placed.
    auto three = split(vtmux::SplitState::Vertical, { leaf("a", 0.5), leaf("b", 0.3), leaf("c", 0.2) });
    auto const children = std::span<config::LayoutPane const> { three.children };

    CHECK(ratioForFirst(children) == Catch::Approx(0.5));            // a | (b, c)
    CHECK(ratioForFirst(children.subspan(1)) == Catch::Approx(0.6)); // b | c, within the 0.5 left
    CHECK(ratioForFirst(children.subspan(2)) == Catch::Approx(1.0)); // c alone
    CHECK(ratioForFirst(std::span<config::LayoutPane const> {}) == Catch::Approx(0.5)); // no children
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

    std::unordered_map<std::string, config::Layout> const layouts { { "work", work } };
    auto const yaml = emitLayoutsYaml(layouts);

    auto const cfg = loadConfigFromYaml(yaml);
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

TEST_CASE("emitLayoutsYaml: escapes embedded double-quotes in a command so it round-trips", "[layout][save]")
{
    // A command containing an embedded double-quote (e.g. `echo "hi"`) must survive byte-for-byte
    // through emit -> parse: unescaped, it would either be misinterpreted mid-scalar or terminate
    // the YAML string early, producing invalid or silently-truncated YAML.
    config::LayoutTab tab;
    tab.root.command = std::string { "echo \"hi\"" };
    tab.root.directory = std::filesystem::path { "/tmp/some dir" };
    tab.root.profile = std::string { "dev-profile" };

    config::Layout work;
    work.tabs = { tab };
    std::unordered_map<std::string, config::Layout> const layouts { { "work", work } };
    auto const yaml = emitLayoutsYaml(layouts);

    auto const cfg = loadConfigFromYaml(yaml);
    REQUIRE(cfg.layouts.value().contains("work"));
    auto const& parsed = cfg.layouts.value().at("work");
    REQUIRE(parsed.tabs.size() == 1);
    REQUIRE(parsed.tabs[0].root.command.has_value());
    CHECK(*parsed.tabs[0].root.command == "echo \"hi\"");
    REQUIRE(parsed.tabs[0].root.directory.has_value());
    CHECK(parsed.tabs[0].root.directory->generic_string() == "/tmp/some dir");
    REQUIRE(parsed.tabs[0].root.profile.has_value());
    CHECK(*parsed.tabs[0].root.profile == "dev-profile");
}

TEST_CASE("emitLayoutsYaml: round-trips an asymmetric split ratio through save", "[layout][save]")
{
    // serializePane sets children[0].ratio / children[1].ratio from the live split position, but
    // the emitter used to drop them entirely, so an asymmetric split silently reset to 50/50 on
    // every SaveLayout. Assert the ratio survives emit -> parse -> ratioForFirst reconstruction.
    auto mk = [](std::string c, double ratio) {
        config::LayoutPane p;
        p.command = std::move(c);
        p.ratio = ratio;
        return p;
    };

    config::LayoutTab tab;
    tab.root.orientation = vtmux::SplitState::Vertical;
    tab.root.children = { mk("left", 0.7), mk("right", 0.3) };

    config::Layout work;
    work.tabs = { tab };
    std::unordered_map<std::string, config::Layout> const layouts { { "work", work } };
    auto const yaml = emitLayoutsYaml(layouts);

    auto const cfg = loadConfigFromYaml(yaml);
    REQUIRE(cfg.layouts.value().contains("work"));
    auto const& parsedRoot = cfg.layouts.value().at("work").tabs.at(0).root;
    REQUIRE_FALSE(parsedRoot.isLeaf());
    REQUIRE(parsedRoot.children.size() == 2);
    CHECK(ratioForFirst(parsedRoot) == Catch::Approx(0.7));
}

TEST_CASE("LayoutBuilder: an explicit ratio is a fraction; unspecified siblings share the rest",
          "[layout][builder]")
{
    // The documented example: `ratio: 0.6` next to an unspecified sibling means a 60/40 split —
    // not 0.6 weighted against an implicit default weight.
    auto const documented = split(vtmux::SplitState::Vertical, { leaf("dev", 0.6), leaf("htop", 0.0) });
    auto asUnset = documented;
    asUnset.children[1].ratio.reset();
    CHECK(ratioForFirst(asUnset) == Catch::Approx(0.6));

    // All-unspecified children share equally.
    auto equal = split(vtmux::SplitState::Vertical, { leaf("a"), leaf("b") });
    for (auto& child: equal.children)
        child.ratio.reset();
    CHECK(ratioForFirst(equal) == Catch::Approx(0.5));

    // Degenerate: the specified fractions over-commit the space; the unspecified sibling still
    // gets a minimal share instead of collapsing to zero (and no division blows up).
    auto overCommitted = split(vtmux::SplitState::Vertical, { leaf("a", 1.0), leaf("b", 0.0) });
    overCommitted.children[1].ratio.reset();
    CHECK(ratioForFirst(overCommitted) > 0.9);
    CHECK(ratioForFirst(overCommitted) < 1.0);
}

TEST_CASE("emitLayoutsYaml: a YAML-significant layout name is quoted and round-trips", "[layout][save]")
{
    // The layout name is free-form user input (the SaveLayout action's `name:`). Emitted raw as
    // a map key, "foo: bar" would corrupt the whole document and destroy every OTHER saved
    // layout on the next load.
    config::LayoutTab tab;
    tab.root.command = std::string { "top" };
    config::Layout work;
    work.tabs = { tab };

    config::LayoutTab otherTab;
    otherTab.root.command = std::string { "bash" };
    config::Layout other;
    other.tabs = { otherTab };

    std::unordered_map<std::string, config::Layout> const layouts { { "foo: bar", work },
                                                                    { "plain", other } };
    auto const yaml = emitLayoutsYaml(layouts);

    auto const cfg = loadConfigFromYaml(yaml);
    REQUIRE(cfg.layouts.value().contains("foo: bar"));
    CHECK(*cfg.layouts.value().at("foo: bar").tabs.at(0).root.command == "top");
    // The innocent sibling survives.
    REQUIRE(cfg.layouts.value().contains("plain"));
    CHECK(*cfg.layouts.value().at("plain").tabs.at(0).root.command == "bash");
}

TEST_CASE("emitLayoutsYaml: a fully-default tab terminates its line", "[layout][save]")
{
    // A tab with nothing to say (default shell, no title/color/profile) emits a bare dash; that
    // dash must not swallow the FOLLOWING tab (or the next layout) onto its own line.
    config::Layout work;
    work.tabs.emplace_back(); // fully-default tab
    config::LayoutTab second;
    second.title = "real";
    second.root.command = std::string { "top" };
    work.tabs.push_back(second);

    std::unordered_map<std::string, config::Layout> const layouts { { "work", work } };
    auto const yaml = emitLayoutsYaml(layouts);

    auto const cfg = loadConfigFromYaml(yaml);
    REQUIRE(cfg.layouts.value().contains("work"));
    auto const& parsed = cfg.layouts.value().at("work");
    REQUIRE(parsed.tabs.size() == 2);
    CHECK_FALSE(parsed.tabs[0].root.command.has_value());
    CHECK(parsed.tabs[1].title == "real");
    CHECK(*parsed.tabs[1].root.command == "top");

    // Same for a default child pane inside a split.
    config::LayoutTab splitTab;
    splitTab.root.orientation = vtmux::SplitState::Vertical;
    splitTab.root.children = { config::LayoutPane {}, config::LayoutPane {} };
    splitTab.root.children[1].command = std::string { "htop" };
    config::Layout dev;
    dev.tabs = { splitTab };
    auto const cfg2 = loadConfigFromYaml(emitLayoutsYaml({ { "dev", dev } }));
    auto const& parsedRoot = cfg2.layouts.value().at("dev").tabs.at(0).root;
    REQUIRE_FALSE(parsedRoot.isLeaf());
    REQUIRE(parsedRoot.children.size() == 2);
    CHECK(*parsedRoot.children[1].command == "htop");
}

TEST_CASE("emitLayoutsYaml: control characters in a title round-trip", "[layout][save]")
{
    // YAML folds a raw newline inside a double-quoted scalar into a plain space; the emitter must
    // escape it so the title survives byte-for-byte. The control characters are concatenated in
    // rather than written inside a word-bearing literal, because the repository's spell check
    // scans source text and would read an escape sequence and the word after it as one token.
    auto const title = std::string { "first" } + "\n" + "second" + "\t" + "third";
    config::LayoutTab tab;
    tab.title = title;
    tab.root.command = std::string { "top" };
    config::Layout work;
    work.tabs = { tab };

    auto const cfg = loadConfigFromYaml(emitLayoutsYaml({ { "work", work } }));
    REQUIRE(cfg.layouts.value().contains("work"));
    CHECK(*cfg.layouts.value().at("work").tabs.at(0).title == title);
}

TEST_CASE("emitLayoutsYaml: literal ${...} text survives the save/load round trip", "[layout][save]")
{
    // The parser expands ${NAME} from the environment; a saved command containing literal ${...}
    // (sed or template tooling) must be escaped on emit so it is NOT re-expanded on load.
    config::LayoutTab tab;
    tab.root.command = std::string { "sed" };
    tab.root.arguments = { "s/${VERSION}/1.0/" };
    config::Layout work;
    work.tabs = { tab };

    auto const cfg = loadConfigFromYaml(emitLayoutsYaml({ { "work", work } }));
    auto const& parsed = cfg.layouts.value().at("work").tabs.at(0).root;
    REQUIRE(parsed.command.has_value());
    CHECK(*parsed.command == "sed");
    REQUIRE(parsed.arguments.size() == 1);
    CHECK(parsed.arguments[0] == "s/${VERSION}/1.0/");
}

TEST_CASE("emitLayoutsYaml: an engaged-but-empty command is not emitted", "[layout][save]")
{
    // A directory-only pane records a launched command with an EMPTY program (it runs the
    // profile's default shell); emitting it would produce a junk `command: "''"` entry.
    config::LayoutTab tab;
    tab.root.command = std::string {};
    tab.root.directory = std::filesystem::path { "/tmp" };
    config::Layout work;
    work.tabs = { tab };

    auto const yaml = emitLayoutsYaml({ { "work", work } });
    CHECK(!yaml.contains("command:"));

    auto const cfg = loadConfigFromYaml(yaml);
    auto const& parsed = cfg.layouts.value().at("work").tabs.at(0).root;
    CHECK_FALSE(parsed.command.has_value());
    REQUIRE(parsed.directory.has_value());
    CHECK(parsed.directory->generic_string() == "/tmp");
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
    spec.title = "server";
    spec.root.orientation = vtmux::SplitState::Vertical;
    spec.root.children = { mk("dev"), mk("logs") };
    auto* tab = realizeLayoutTab(h.model, win->id(), spec, h.seeder());
    REQUIRE(tab != nullptr);

    // Only the "dev" leaf carries a profile override, so serializePane must capture it there and
    // leave the "logs" leaf's profile unset — a fake stand-in for
    // TerminalSession::profileName() ONLY being non-empty for a real per-pane override.
    auto resolve = [&](vtmux::SessionId id) {
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
    CHECK(out.root.orientation == vtmux::SplitState::Vertical);
    REQUIRE(out.root.children.size() == 2);
    CHECK(*out.root.children[0].command == "dev");
    CHECK(out.root.children[0].directory->string() == "/work");
    REQUIRE(out.root.children[0].profile.has_value());
    CHECK(*out.root.children[0].profile == "dev-profile");
    CHECK(*out.root.children[1].command == "logs");
    CHECK_FALSE(out.root.children[1].profile.has_value());
}
