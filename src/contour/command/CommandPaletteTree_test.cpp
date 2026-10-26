// SPDX-License-Identifier: Apache-2.0
//
// The palette's listing: its groups, the drill-in rows parameterised actions collapse into, and how a
// query ranks across a parent's children. Pure — no Qt; the model in window/ only adapts this.

#include <contour/command/CommandCatalog.hpp>
#include <contour/command/CommandPaletteTree.hpp>
#include <contour/command/Shortcut.hpp>
#include <contour/config/Config.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <format>
#include <ranges>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

using namespace contour;
using namespace contour::command;

namespace
{
/// What the configuration holds for one test: the names of its profiles, layouts and hosts.
struct Inventory
{
    std::vector<std::string> profiles {}; ///< Profile names; none means no ChangeProfile parent.
    std::vector<std::string> layouts {};  ///< Saved layout names.
    std::vector<std::string> hosts {};    ///< Saved host names.
};

/// A map of @p names to default values, assigned whole so a config default (the "main" profile)
/// cannot leak into the count.
template <typename Map>
[[nodiscard]] Map mapOf(std::vector<std::string> const& names)
{
    auto result = Map {};
    for (auto const& name: names)
        result.insert_or_assign(name, typename Map::mapped_type {});
    return result;
}

/// The tree the real sources build over @p inventory: every catalogue action plus the live rows.
[[nodiscard]] PaletteTree treeOf(Inventory const& inventory)
{
    auto config = config::Config {};
    config.profiles.value() =
        mapOf<std::remove_cvref_t<decltype(config.profiles.value())>>(inventory.profiles);
    config.layouts.value() = mapOf<std::remove_cvref_t<decltype(config.layouts.value())>>(inventory.layouts);
    config.hosts.value() = mapOf<std::remove_cvref_t<decltype(config.hosts.value())>>(inventory.hosts);

    auto const profiles = ProfileCommandSource { config };
    auto const layouts = LayoutCommandSource { config };
    auto const hosts = HostCommandSource { config };
    auto const actions = ActionCommandSource {};
    auto const sources = std::array<CommandSource const*, 4> { &profiles, &layouts, &hosts, &actions };
    return buildPaletteTree(collectCommands(sources));
}

/// The index into @p tree's parents of the parent named @p name; fails the test when absent.
[[nodiscard]] std::size_t parentIndex(PaletteTree const& tree, std::string_view name)
{
    auto const found = std::ranges::find(tree.parents, name, &PaletteParent::name);
    REQUIRE(found != tree.parents.end());
    return static_cast<std::size_t>(std::ranges::distance(tree.parents.begin(), found));
}

/// The titles of @p rows, in order.
[[nodiscard]] std::vector<std::string> titlesOf(std::vector<PaletteEntry> const& rows)
{
    auto result = std::vector<std::string> {};
    for (auto const& row: rows)
        result.push_back(row.title);
    return result;
}

/// The group headers of @p rows, in order.
[[nodiscard]] std::vector<std::string_view> headersOf(std::vector<PaletteEntry> const& rows)
{
    auto result = std::vector<std::string_view> {};
    for (auto const& row: rows)
        if (!row.groupTitle.empty())
            result.push_back(row.groupTitle);
    return result;
}

/// The header of the group row @p index of @p rows sits in.
[[nodiscard]] std::string_view groupOf(std::vector<PaletteEntry> const& rows, std::size_t index)
{
    for (auto const position: std::views::iota(std::size_t { 0 }, index + 1) | std::views::reverse)
        if (!rows[position].groupTitle.empty())
            return rows[position].groupTitle;
    return {};
}

/// The position in @p rows of the row titled @p title, or `rows.size()` when absent.
[[nodiscard]] std::size_t rowTitled(std::vector<PaletteEntry> const& rows, std::string_view title)
{
    return static_cast<std::size_t>(
        std::ranges::distance(rows.begin(), std::ranges::find(rows, title, &PaletteEntry::title)));
}

/// Whether row @p row of a listing is a child of a parameterised action (not a plain command).
[[nodiscard]] bool isChildRow(PaletteTree const& tree, PaletteEntry const& row)
{
    return row.kind == PaletteEntryKind::Command && actions::isParameterized(tree.commands[row.index].action);
}

/// The byte length of a child's qualifier, e.g. "Change Profile · " (the separator's dot is two bytes).
[[nodiscard]] int prefixOf(std::string_view parentTitle)
{
    return static_cast<int>(parentTitle.size() + DrillInSeparator.size());
}
} // namespace

TEST_CASE("paletteTree.groupsFollowTheTable", "[palettetree]")
{
    for (auto const value: std::views::iota(std::size_t { 0 }, CommandCategoryCount))
    {
        auto const category = static_cast<CommandCategory>(value);
        INFO("category " << value);
        CHECK(std::ranges::count(PaletteGroups, category, &PaletteGroupRow::category) == 1);
    }

    auto const titles = PaletteGroups | std::views::transform(&PaletteGroupRow::title);
    CHECK(std::ranges::equal(titles,
                             std::array<std::string_view, CommandCategoryCount> { "Pane",
                                                                                  "Tab",
                                                                                  "Window",
                                                                                  "Layouts",
                                                                                  "Blocks",
                                                                                  "Profiles",
                                                                                  "Hosts",
                                                                                  "Edit",
                                                                                  "View",
                                                                                  "Shell",
                                                                                  "Contour" }));
}

TEST_CASE("paletteTree.parameterisedActionsCollapse", "[palettetree]")
{
    auto const tree = treeOf({ .profiles = { "light", "dark" }, .layouts = {}, .hosts = {} });

    CHECK(std::ranges::count(tree.parents, std::string_view { "ChangeProfile" }, &PaletteParent::name) == 1);
    auto const& parent = tree.parents[parentIndex(tree, "ChangeProfile")];
    CHECK(parent.title == "Change Profile");
    CHECK(parent.category == CommandCategory::Profiles);
    CHECK(parent.description == actions::documentation::ChangeProfile);

    // Ordered by argument, whatever order the profile map hands them over in.
    auto arguments = std::vector<std::string> {};
    for (auto const index: parent.children)
        arguments.push_back(tree.commands[index].argument);
    CHECK(arguments == std::vector<std::string> { "dark", "light" });

    CHECK(std::ranges::none_of(tree.plain, [&](std::size_t index) {
        return std::holds_alternative<actions::ChangeProfile>(tree.commands[index].action);
    }));
    CHECK(tree.find("ChangeProfile:dark") != nullptr);
    CHECK(tree.find("ChangeProfile:gone") == nullptr);
}

TEST_CASE("paletteTree.unfilteredTopIsRecentThenGroups", "[palettetree]")
{
    auto const tree = treeOf({ .profiles = { "dark", "light" }, .layouts = {}, .hosts = {} });
    auto const recent = std::array<std::string, 2> { "ChangeProfile:dark", "SplitVertical" };
    auto const rows = listPalette(tree, PaletteQuery { .filter = {}, .level = {}, .recent = recent });
    REQUIRE(rows.size() > 3);

    auto const darkIndex = static_cast<std::size_t>(tree.find("ChangeProfile:dark") - tree.commands.data());
    CHECK(rows[0]
          == PaletteEntry { .kind = PaletteEntryKind::Command,
                            .index = darkIndex,
                            .title = "Change Profile · dark",
                            .titleMatches = {},
                            .groupTitle = RecentGroupTitle });
    CHECK(rows[1].title == "Split Vertical");
    CHECK(rows[1].groupTitle.empty());
    CHECK(rows[2].groupTitle == "Pane");

    SECTION("the groups follow the table, each once")
    {
        auto expected = std::vector<std::string_view> { RecentGroupTitle };
        for (auto const& group: PaletteGroups)
            if (group.category != CommandCategory::Hosts) // no hosts configured
                expected.push_back(group.title);
        CHECK(headersOf(rows) == expected);
    }

    SECTION("within a group, titles ascend")
    {
        auto previous = std::string {};
        for (auto const index: std::views::iota(std::size_t { 2 }, rows.size()))
        {
            if (groupOf(rows, index) != "Pane")
                break;
            INFO("'" << previous << "' then '" << rows[index].title << "'");
            CHECK(previous <= rows[index].title);
            previous = rows[index].title;
        }
    }

    SECTION("the parent is one drill-in row in its group, and no child is listed outside Recently used")
    {
        auto const parent = rowTitled(rows, "Change Profile");
        REQUIRE(parent < rows.size());
        CHECK(rows[parent].kind == PaletteEntryKind::DrillIn);
        CHECK(groupOf(rows, parent) == "Profiles");
        for (auto const index: std::views::iota(std::size_t { 2 }, rows.size()))
        {
            INFO("row " << index << ": " << rows[index].title);
            CHECK_FALSE(isChildRow(tree, rows[index]));
        }
    }
}

TEST_CASE("paletteTree.aStaleRecentIdDisappears", "[palettetree]")
{
    auto const tree = treeOf({ .profiles = { "dark" }, .layouts = {}, .hosts = {} });
    auto const recent = std::array<std::string, 1> { "ChangeProfile:gone" };
    auto const rows = listPalette(tree, PaletteQuery { .filter = {}, .level = {}, .recent = recent });

    REQUIRE_FALSE(rows.empty());
    CHECK_FALSE(std::ranges::contains(headersOf(rows), RecentGroupTitle));
    CHECK(rows[0].groupTitle == "Pane");
}

TEST_CASE("paletteTree.anEmptyGroupIsOmitted", "[palettetree]")
{
    auto const tree = treeOf({ .profiles = { "dark" }, .layouts = {}, .hosts = {} });
    auto const rows = listPalette(tree, PaletteQuery {});

    auto const headers = headersOf(rows);
    CHECK(std::ranges::find(headers, std::string_view { "Hosts" }) == headers.end());
    CHECK(std::ranges::find(tree.parents, std::string_view { "AttachToHost" }, &PaletteParent::name)
          == tree.parents.end());

    SECTION("and a configured host fills it")
    {
        auto const withHost = treeOf({ .profiles = { "dark" }, .layouts = {}, .hosts = { "starfive" } });
        auto const hostHeaders = headersOf(listPalette(withHost, PaletteQuery {}));
        CHECK(std::ranges::find(hostHeaders, std::string_view { "Hosts" }) != hostHeaders.end());
    }
}

TEST_CASE("paletteTree.aFilterSurfacesMatchingChildren", "[palettetree]")
{
    auto const tree = treeOf({ .profiles = { "dark" }, .layouts = { "split-dev", "logs" }, .hosts = {} });
    auto const rows = listPalette(tree, PaletteQuery { .filter = "split", .level = {}, .recent = {} });
    REQUIRE_FALSE(rows.empty());

    CHECK(rows[0].groupTitle == "Pane");
    CHECK(rowTitled(rows, "Split Vertical") < rows.size());
    CHECK(rowTitled(rows, "Split Horizontal") < rows.size());

    auto const child = rowTitled(rows, "Launch Layout · split-dev");
    REQUIRE(child < rows.size());
    CHECK(rows[child].kind == PaletteEntryKind::Command);
    CHECK(groupOf(rows, child) == "Layouts");
    auto const prefix = prefixOf("Launch Layout");
    CHECK(prefix == 17);
    CHECK(rows[child].titleMatches
          == std::vector<int> { prefix + 0, prefix + 1, prefix + 2, prefix + 3, prefix + 4 });

    CHECK(std::ranges::none_of(rows, [](auto const& row) { return row.title.contains("logs"); }));
}

TEST_CASE("paletteTree.childrenMatchOnTheirArgumentOnly", "[palettetree]")
{
    auto profiles = std::vector<std::string> {};
    for (auto const index: std::views::iota(0, 10))
        profiles.push_back(std::format("p{}", index));
    auto const tree = treeOf({ .profiles = profiles, .layouts = {}, .hosts = {} });

    auto const rows = listPalette(tree, PaletteQuery { .filter = "change", .level = {}, .recent = {} });

    CHECK(std::ranges::count_if(rows,
                                [](auto const& row) {
                                    return row.kind == PaletteEntryKind::DrillIn
                                           && row.title == "Change Profile";
                                })
          == 1);
    CHECK(std::ranges::none_of(rows, [&](auto const& row) { return isChildRow(tree, row); }));
}

TEST_CASE("paletteTree.groupsOrderByTheirBestMatch", "[palettetree]")
{
    auto const tree = treeOf({ .profiles = { "dark" }, .layouts = {}, .hosts = {} });
    auto const rows = listPalette(tree, PaletteQuery { .filter = "dark", .level = {}, .recent = {} });

    REQUIRE_FALSE(rows.empty());
    // Profiles sits sixth in the table, but its child equals the query outright.
    CHECK(rows[0].title == "Change Profile · dark");
    CHECK(rows[0].groupTitle == "Profiles");

    SECTION("even over a group above it in the table that also matches")
    {
        // "lane" lands scattered in the Pane group ("Close Pane"), and outright on the profile.
        auto const lane = treeOf({ .profiles = { "lane" }, .layouts = {}, .hosts = {} });
        auto const laneRows = listPalette(lane, PaletteQuery { .filter = "lane", .level = {}, .recent = {} });
        REQUIRE(std::ranges::contains(headersOf(laneRows), std::string_view { "Pane" }));
        CHECK(laneRows[0].title == "Change Profile · lane");
        CHECK(laneRows[0].groupTitle == "Profiles");
    }
}

TEST_CASE("paletteTree.aFilterInsideALevelRanksOnlyItsChildren", "[palettetree]")
{
    auto const tree = treeOf({ .profiles = { "light", "dim", "dark" }, .layouts = {}, .hosts = {} });
    auto const level = parentIndex(tree, "ChangeProfile");

    SECTION("filtered, only the matching children, titled by argument, without headers")
    {
        auto const rows = listPalette(tree, PaletteQuery { .filter = "d", .level = level, .recent = {} });
        CHECK(titlesOf(rows) == std::vector<std::string> { "dark", "dim" });
        CHECK(headersOf(rows).empty());
        for (auto const& row: rows)
            CHECK(std::holds_alternative<actions::ChangeProfile>(tree.commands[row.index].action));
    }

    SECTION("unfiltered, every child by argument")
    {
        auto const rows = listPalette(tree, PaletteQuery { .filter = {}, .level = level, .recent = {} });
        CHECK(titlesOf(rows) == std::vector<std::string> { "dark", "dim", "light" });
        CHECK(headersOf(rows).empty());
    }

    SECTION("unfiltered, the recently used first")
    {
        auto const recent = std::array<std::string, 1> { "ChangeProfile:light" };
        auto const rows = listPalette(tree, PaletteQuery { .filter = {}, .level = level, .recent = recent });
        CHECK(titlesOf(rows) == std::vector<std::string> { "light", "dark", "dim" });
    }
}

TEST_CASE("paletteTree.recencyBreaksATie", "[palettetree]")
{
    // Two commands score the same against the query; the one the user reached for last comes first.
    auto const tree = treeOf({});
    auto const recent = std::array<std::string, 1> { "ToggleStatusLine" };
    auto const rows =
        listPalette(tree, PaletteQuery { .filter = "togglestatusline", .level = {}, .recent = recent });

    REQUIRE_FALSE(rows.empty());
    CHECK(tree.commands[rows[0].index].id == "ToggleStatusLine");
}

TEST_CASE("paletteTree.idFallbackHighlightsNothing", "[palettetree]")
{
    // "clipboard:strip" is not in the title "Paste Clipboard (strip whitespace)" (no colon), only in the
    // id "PasteClipboard:strip", a default binding's command.
    auto const bound = BoundCommandSource { config::defaultInputMappings() };
    auto const sources = std::array<CommandSource const*, 1> { &bound };
    auto const tree = buildPaletteTree(collectCommands(sources));

    auto const rows =
        listPalette(tree, PaletteQuery { .filter = "clipboard:strip", .level = {}, .recent = {} });
    auto const found = std::ranges::find_if(
        rows, [&](auto const& row) { return tree.commands[row.index].id == "PasteClipboard:strip"; });
    REQUIRE(found != rows.end());
    CHECK(found->titleMatches.empty());

    SECTION("a title hit highlights the characters it landed on")
    {
        auto const split =
            listPalette(treeOf({}), PaletteQuery { .filter = "splitv", .level = {}, .recent = {} });
        auto const row = rowTitled(split, "Split Vertical");
        REQUIRE(row < split.size());
        CHECK(split[row].titleMatches == std::vector<int> { 0, 1, 2, 3, 4, 6 });
    }
}

TEST_CASE("paletteTree.twoHundredProfilesAreOneRow", "[palettetree]")
{
    auto profiles = std::vector<std::string> {};
    for (auto const index: std::views::iota(0, 200))
        profiles.push_back(std::format("p{:03}", index));
    auto const tree = treeOf({ .profiles = profiles, .layouts = {}, .hosts = {} });

    auto const top = listPalette(tree, PaletteQuery {});
    CHECK(std::ranges::count(top, std::string { "Change Profile" }, &PaletteEntry::title) == 1);
    CHECK(top[rowTitled(top, "Change Profile")].kind == PaletteEntryKind::DrillIn);
    CHECK(std::ranges::none_of(top, [&](auto const& row) { return isChildRow(tree, row); }));

    auto const level = listPalette(
        tree, PaletteQuery { .filter = {}, .level = parentIndex(tree, "ChangeProfile"), .recent = {} });
    CHECK(level.size() == profiles.size());

    auto const filtered = listPalette(tree, PaletteQuery { .filter = "p199", .level = {}, .recent = {} });
    REQUIRE_FALSE(filtered.empty());
    CHECK(filtered[0].title == "Change Profile · p199");
}

TEST_CASE("paletteTree.matchOffsetsLandOnTheChildArgument", "[palettetree]")
{
    auto const tree = treeOf({ .profiles = { "שלום" }, .layouts = {}, .hosts = {} });
    auto const rows = listPalette(tree, PaletteQuery { .filter = "של", .level = {}, .recent = {} });

    auto const row = rowTitled(rows, "Change Profile · שלום");
    REQUIRE(row < rows.size());
    // Fuzzy matching is per byte: two bytes per Hebrew letter, all four past the qualifier.
    auto const prefix = prefixOf("Change Profile");
    CHECK(rows[row].titleMatches == std::vector<int> { prefix + 0, prefix + 1, prefix + 2, prefix + 3 });
}

TEST_CASE("paletteTree.footerHints", "[palettetree]")
{
    using enum PaletteEntryKind;
    using enum PaletteDepth;

    CHECK(paletteFooterHints(Command, Top, ShortcutStyle::MacGlyphs) == "↩ Run · ⎋ Close");
    CHECK(paletteFooterHints(DrillIn, Top, ShortcutStyle::MacGlyphs) == "↩ Open · ⎋ Close");
    CHECK(paletteFooterHints(Command, Level, ShortcutStyle::MacGlyphs) == "↩ Run · ⎋ Back");
    CHECK(paletteFooterHints(DrillIn, Level, ShortcutStyle::MacGlyphs).empty());

    SECTION("spelled out, the keys read as shortcutText spells them")
    {
        auto const key = [](vtbackend::Key input) {
            return shortcutText(ShortcutChord { .modifiers = {}, .input = input }, ShortcutStyle::Text);
        };
        CHECK(paletteFooterHints(Command, Top, ShortcutStyle::Text)
              == std::format("{} Run · {} Close", key(vtbackend::Key::Enter), key(vtbackend::Key::Escape)));
        CHECK(paletteFooterHints(Command, Level, ShortcutStyle::Text)
              == std::format("{} Run · {} Back", key(vtbackend::Key::Enter), key(vtbackend::Key::Escape)));
        CHECK(paletteFooterHints(DrillIn, Level, ShortcutStyle::Text).empty());
    }
}
