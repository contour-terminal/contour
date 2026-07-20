// SPDX-License-Identifier: Apache-2.0
#include <contour/Config.h>
#include <contour/LayoutBuilder.h>
#include <contour/test/GuiTestFixtures.h>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <string>
#include <unordered_map>

#include <vtmux/LayoutTree.h>

// The pure layout tree model (structs, realize, serialize, ratio math) is tested in
// src/vtmux/LayoutTree_test.cpp; this file covers only the contour-side YAML emission
// (emitLayoutsYaml) and its round trip through the config parser.

using namespace contour;
using contour::test::loadConfigFromYaml;

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
    t0.root.directory = std::string { "/tmp" };

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
    CHECK(*p0.root.directory == "/tmp");

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
    tab.root.directory = std::string { "/tmp/some dir" };
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
    CHECK(*parsed.tabs[0].root.directory == "/tmp/some dir");
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
    tab.root.directory = std::string { "/tmp" };
    config::Layout work;
    work.tabs = { tab };

    auto const yaml = emitLayoutsYaml({ { "work", work } });
    CHECK(!yaml.contains("command:"));

    auto const cfg = loadConfigFromYaml(yaml);
    auto const& parsed = cfg.layouts.value().at("work").tabs.at(0).root;
    CHECK_FALSE(parsed.command.has_value());
    REQUIRE(parsed.directory.has_value());
    CHECK(*parsed.directory == "/tmp");
}
