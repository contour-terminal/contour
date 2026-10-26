// SPDX-License-Identifier: Apache-2.0
//
// What the command palette publishes to QML. The ordering itself is command::listPalette()'s (tested in
// CommandPaletteTree_test); these cases pin the adapter: the roles, the drill-in level and the filter
// it keeps, and the promise that makes the MRU safe to persist — a remembered command that no longer
// EXISTS (its profile was deleted, its tab was closed) quietly stops appearing.

#include <contour/command/CommandCatalog.hpp>
#include <contour/command/CommandHistory.hpp>
#include <contour/command/CommandPaletteTree.hpp>
#include <contour/command/FuzzyFilter.hpp>
#include <contour/command/Shortcut.hpp>
#include <contour/config/Config.hpp>
#include <contour/test/GuiTestFixtures.hpp>
#include <contour/window/CommandPaletteModel.hpp>

#include <catch2/catch_test_macros.hpp>

#include <ranges>
#include <string>
#include <utility>
#include <vector>

#include <QtTest/QSignalSpy>

using namespace contour;
using namespace contour::window;

namespace
{
using Sources = std::vector<command::CommandSource const*>;

/// Reads one role off a row, the way the QML delegate does.
[[nodiscard]] QVariant roleAt(CommandPaletteModel const& model, int row, CommandPaletteModel::Roles role)
{
    return model.data(model.index(row, 0), static_cast<int>(role));
}

[[nodiscard]] std::string titleAt(CommandPaletteModel const& model, int row)
{
    return roleAt(model, row, CommandPaletteModel::Roles::TitleRole).toString().toStdString();
}

[[nodiscard]] std::string idAt(CommandPaletteModel const& model, int row)
{
    return roleAt(model, row, CommandPaletteModel::Roles::IdRole).toString().toStdString();
}

[[nodiscard]] std::string groupTitleAt(CommandPaletteModel const& model, int row)
{
    return roleAt(model, row, CommandPaletteModel::Roles::GroupTitleRole).toString().toStdString();
}

[[nodiscard]] int kindAt(CommandPaletteModel const& model, int row)
{
    return roleAt(model, row, CommandPaletteModel::Roles::KindRole).toInt();
}

/// The title character indices the current filter matched on a row, as the QML delegate reads them.
[[nodiscard]] std::vector<int> titleMatchesAt(CommandPaletteModel const& model, int row)
{
    auto result = std::vector<int> {};
    for (auto const& entry: roleAt(model, row, CommandPaletteModel::Roles::TitleMatchesRole).toList())
        result.push_back(entry.toInt());
    return result;
}

/// The row of the entry whose `commandId` is @p id, or -1 if it is not listed. Rows are looked up by
/// id, never by rank, so a ranking tweak cannot silently retarget a case.
[[nodiscard]] int rowOf(CommandPaletteModel const& model, std::string const& id)
{
    for (auto const row: std::views::iota(0, model.rowCount()))
        if (idAt(model, row) == id)
            return row;
    return -1;
}

/// The ids of the rows in the "Recently used" group, in order.
[[nodiscard]] std::vector<std::string> recentRows(CommandPaletteModel const& model)
{
    auto result = std::vector<std::string> {};
    if (model.rowCount() == 0 || groupTitleAt(model, 0) != command::RecentGroupTitle)
        return result;
    for (auto const row: std::views::iota(0, model.rowCount()))
    {
        if (row > 0 && !groupTitleAt(model, row).empty())
            break;
        result.push_back(idAt(model, row));
    }
    return result;
}

/// The non-empty group titles, top to bottom.
[[nodiscard]] std::vector<std::string> headersOf(CommandPaletteModel const& model)
{
    auto result = std::vector<std::string> {};
    for (auto const row: std::views::iota(0, model.rowCount()))
        if (auto title = groupTitleAt(model, row); !title.empty())
            result.push_back(std::move(title));
    return result;
}

/// A configuration whose only profiles are @p names (the default "main" is replaced, not added to).
[[nodiscard]] config::Config configWithProfiles(std::vector<std::string> const& names)
{
    auto config = config::Config {};
    auto profiles = std::remove_cvref_t<decltype(config.profiles.value())> {};
    for (auto const& name: names)
        profiles.insert_or_assign(name, config::TerminalProfile {});
    config.profiles.value() = std::move(profiles);
    return config;
}
} // namespace

TEST_CASE("With no filter, the palette groups its rows behind Recently used", "[contour][palette]")
{
    auto history = command::CommandHistory { 3 };
    history.record("SplitVertical");
    history.record("CreateNewTab"); // newest

    auto const actionCommands = command::ActionCommandSource {};
    auto model = CommandPaletteModel { history, Sources { &actionCommands }, command::ShortcutStyle::Text };
    model.refresh({});

    REQUIRE(model.rowCount() > 2);

    SECTION("the recent commands lead, newest first")
    {
        CHECK(groupTitleAt(model, 0) == command::RecentGroupTitle);
        CHECK(recentRows(model) == std::vector<std::string> { "CreateNewTab", "SplitVertical" });
    }

    SECTION("the first row after them opens the first group")
    {
        CHECK(groupTitleAt(model, 1).empty());
        CHECK(groupTitleAt(model, 2) == "Pane");
    }

    SECTION("exactly one row per group carries its title")
    {
        auto headers = headersOf(model);
        std::ranges::sort(headers);
        CHECK(std::ranges::unique(headers).empty());
    }

    SECTION("a recent command still appears in its group too")
    {
        // Recently used is a shortcut, not a move: a user browsing the Pane group must still find it.
        auto const inGroup = std::ranges::count_if(std::views::iota(2, model.rowCount()), [&](int row) {
            return idAt(model, row) == "SplitVertical";
        });
        CHECK(inGroup == 1);
    }
}

TEST_CASE("A remembered command that no longer exists quietly stops appearing", "[contour][palette]")
{
    // The self-healing property that makes the MRU safe to persist by id. The user switched to a tab,
    // then closed it; the stored "SwitchToTab:2" must not become a dead row that does nothing when
    // picked — it must simply be gone.
    auto tabs = test::StubTabs { { "zsh", "vim" } };
    auto const tabCommands = command::TabCommandSource { tabs };

    auto history = command::CommandHistory { 5 };
    history.record("SwitchToTab:2");

    auto model = CommandPaletteModel { history, Sources { &tabCommands }, command::ShortcutStyle::Text };
    model.refresh({});
    REQUIRE(recentRows(model) == std::vector<std::string> { "SwitchToTab:2" });
    // A child shows qualified by its parent outside its level.
    CHECK(titleAt(model, 0) == "Switch To Tab · 2: vim");

    // The second tab closes.
    tabs.setTitles({ "zsh" });
    model.refresh({});

    CHECK(recentRows(model).empty());
    CHECK(model.commandById("SwitchToTab:2") == nullptr);
}

TEST_CASE("Typing ranks across groups and keeps their headers", "[contour][palette]")
{
    auto history = command::CommandHistory { 3 };
    auto const actionCommands = command::ActionCommandSource {};

    auto model = CommandPaletteModel { history, Sources { &actionCommands }, command::ShortcutStyle::Text };
    model.refresh(command::shortcutIndex(config::defaultInputMappings(), command::ShortcutStyle::Text));
    auto const unfiltered = model.rowCount();

    SECTION("the best match leads, under its group's header, with its chord")
    {
        model.setFilter(QStringLiteral("SplitVertical"));
        REQUIRE(model.rowCount() > 0);
        CHECK(titleAt(model, 0) == "Split Vertical");
        CHECK(groupTitleAt(model, 0) == "Pane");
        CHECK(roleAt(model, 0, CommandPaletteModel::Roles::ShortcutRole).toString().toStdString()
              == "Ctrl+Shift+E");
    }

    SECTION("the commands the user meant lead, and the incidental match trails")
    {
        model.setFilter(QStringLiteral("spl"));
        CHECK(model.rowCount() < unfiltered);
        auto const horizontal = rowOf(model, "SplitHorizontal");
        auto const vertical = rowOf(model, "SplitVertical");
        auto const toggle = rowOf(model, "ToggleSplitOrientation");
        REQUIRE(horizontal >= 0);
        REQUIRE(vertical >= 0);
        REQUIRE(toggle >= 0);
        CHECK(horizontal < toggle);
        CHECK(vertical < toggle);
        // One of the two Splits is under the cursor when the popup opens.
        CHECK((idAt(model, 0) == "SplitHorizontal" || idAt(model, 0) == "SplitVertical"));

        for (auto const row: std::views::iota(0, model.rowCount()))
        {
            INFO("row " << row << ": " << titleAt(model, row));
            CHECK(command::fuzzyScore("spl", titleAt(model, row)).has_value());
        }
    }

    SECTION("clearing the filter restores the grouped list")
    {
        model.setFilter(QStringLiteral("spl"));
        model.setFilter(QString {});
        CHECK(model.rowCount() == unfiltered);
    }

    SECTION("a query matching nothing yields an empty list rather than everything")
    {
        model.setFilter(QStringLiteral("zzzznotacommand"));
        CHECK(model.rowCount() == 0);
    }
}

TEST_CASE("The palette reports which title characters the filter matched", "[contour][palette]")
{
    auto history = command::CommandHistory { 3 };
    auto const actionCommands = command::ActionCommandSource {};

    auto model = CommandPaletteModel { history, Sources { &actionCommands }, command::ShortcutStyle::Text };
    model.refresh({});

    SECTION("an unfiltered row highlights nothing")
    {
        REQUIRE(model.rowCount() > 0);
        CHECK(titleMatchesAt(model, 0).empty());
    }

    SECTION("a filtered row marks exactly the matched title characters")
    {
        model.setFilter(QStringLiteral("splitv"));
        auto const row = rowOf(model, "SplitVertical");
        REQUIRE(row >= 0);
        // "splitv" over "Split Vertical": S p l i t (0..4), then V (6) past the space.
        CHECK(titleMatchesAt(model, row) == std::vector<int> { 0, 1, 2, 3, 4, 6 });
    }

    SECTION("clearing the filter clears the highlight again")
    {
        model.setFilter(QStringLiteral("split"));
        auto const filtered = rowOf(model, "SplitVertical");
        REQUIRE(filtered >= 0);
        REQUIRE_FALSE(titleMatchesAt(model, filtered).empty());

        model.setFilter(QString {});
        auto const cleared = rowOf(model, "SplitVertical");
        REQUIRE(cleared >= 0);
        CHECK(titleMatchesAt(model, cleared).empty());
    }
}

TEST_CASE("Title-match highlight indices are UTF-16 code units, not UTF-8 byte offsets", "[contour][palette]")
{
    // The fuzzy filter reports UTF-8 byte offsets, but the QML delegate indexes the title as a UTF-16
    // string. A tab title can carry any UTF-8 (a shell sets it), so a multibyte character before a
    // match would otherwise shift every highlight after it. U+2192 (→) is 3 UTF-8 bytes, 1 UTF-16 unit.
    auto tabs = test::StubTabs { { "→zephyr" } };
    auto const tabCommands = command::TabCommandSource { tabs };

    auto history = command::CommandHistory { 3 };
    auto model = CommandPaletteModel { history, Sources { &tabCommands }, command::ShortcutStyle::Text };
    model.refresh({});

    model.setFilter(QStringLiteral("zephyr"));
    auto const row = rowOf(model, "SwitchToTab:1");
    REQUIRE(row >= 0);
    auto const title = QString::fromStdString(titleAt(model, row));
    auto const prefix = QString::fromUtf8("Switch To Tab · 1: →");
    REQUIRE(title == prefix + QStringLiteral("zephyr"));

    auto const matches = titleMatchesAt(model, row);
    auto expected = std::vector<int> {};
    for (auto const offset: std::views::iota(0, 6))
        expected.push_back(static_cast<int>(prefix.size()) + offset);
    CHECK(matches == expected);

    // Each index lands on the matched character in the UTF-16 string, the property QML relies on.
    auto const query = QStringLiteral("zephyr");
    for (auto const index: std::views::iota(std::size_t { 0 }, matches.size()))
        CHECK(title.at(matches[index]) == query.at(static_cast<qsizetype>(index)));
}

TEST_CASE("Title-match highlight indices are UTF-16 code units through a child's prefix",
          "[contour][palette]")
{
    auto const config = configWithProfiles({ "日本" });
    auto const profiles = command::ProfileCommandSource { config };
    auto history = command::CommandHistory { 3 };
    auto model = CommandPaletteModel { history, Sources { &profiles }, command::ShortcutStyle::Text };
    model.refresh({});

    model.setFilter(QString::fromUtf8("本"));
    auto const row = rowOf(model, "ChangeProfile:日本");
    REQUIRE(row >= 0);
    CHECK(titleMatchesAt(model, row)
          == std::vector<int> { static_cast<int>(QString::fromUtf8("Change Profile · ").size()) + 1 });
}

TEST_CASE("Recency breaks a tie between two equally good matches", "[contour][palette]")
{
    auto history = command::CommandHistory { 5 };
    history.record("ToggleStatusLine");

    auto const actionCommands = command::ActionCommandSource {};
    auto model = CommandPaletteModel { history, Sources { &actionCommands }, command::ShortcutStyle::Text };
    model.refresh({});
    model.setFilter(QStringLiteral("togglestatusline"));

    REQUIRE(model.rowCount() > 0);
    CHECK(idAt(model, 0) == "ToggleStatusLine");
}

TEST_CASE("The shortcut column advertises the chord that runs the command", "[contour][palette]")
{
    auto history = command::CommandHistory { 3 };
    auto const actionCommands = command::ActionCommandSource {};

    auto model = CommandPaletteModel { history, Sources { &actionCommands }, command::ShortcutStyle::Text };
    model.refresh(command::shortcutIndex(config::defaultInputMappings(), command::ShortcutStyle::Text));

    auto const split = rowOf(model, "SplitVertical");
    REQUIRE(split >= 0);
    CHECK(roleAt(model, split, CommandPaletteModel::Roles::ShortcutRole).toString().toStdString()
          == "Ctrl+Shift+E");

    SECTION("an unbound command shows an empty shortcut, not a stale or wrong one")
    {
        // ClearHistoryAndReset has no default binding.
        auto const clear = rowOf(model, "ClearHistoryAndReset");
        REQUIRE(clear >= 0);
        CHECK(roleAt(model, clear, CommandPaletteModel::Roles::ShortcutRole).toString().isEmpty());
    }
}

TEST_CASE("A zero recent_count turns the Recent section off entirely", "[contour][palette]")
{
    auto history = command::CommandHistory { 0 };
    history.record("SplitVertical");

    auto const actionCommands = command::ActionCommandSource {};
    auto model = CommandPaletteModel { history, Sources { &actionCommands }, command::ShortcutStyle::Text };
    model.refresh({});

    CHECK(recentRows(model).empty());
    // The full list is still there — only the recent group is gone.
    REQUIRE(model.rowCount() > 0);
    CHECK(groupTitleAt(model, 0) == "Pane");
}

TEST_CASE("A parameterised action is one drill-in row", "[contour][palette]")
{
    auto const config = configWithProfiles({ "dark", "light" });
    auto const profiles = command::ProfileCommandSource { config };
    auto const actionCommands = command::ActionCommandSource {};
    auto history = command::CommandHistory { 3 };
    auto model =
        CommandPaletteModel { history, Sources { &profiles, &actionCommands }, command::ShortcutStyle::Text };
    model.refresh(command::shortcutIndex(config::defaultInputMappings(), command::ShortcutStyle::Text));

    auto const drillIns = std::ranges::count_if(std::views::iota(0, model.rowCount()), [&](int row) {
        return kindAt(model, row) == model.drillInKind() && titleAt(model, row) == "Change Profile";
    });
    CHECK(drillIns == 1);

    auto const row = rowOf(model, "ChangeProfile");
    REQUIRE(row >= 0);
    CHECK(kindAt(model, row) == model.drillInKind());
    CHECK(titleAt(model, row) == "Change Profile");
    CHECK(roleAt(model, row, CommandPaletteModel::Roles::ShortcutRole).toString().isEmpty());
    CHECK(roleAt(model, row, CommandPaletteModel::Roles::DescriptionRole).toString().toStdString()
          == actions::documentation::ChangeProfile);
    CHECK(rowOf(model, "ChangeProfile:dark") < 0);
}

TEST_CASE("enter() lists the children, back() restores the filter the level was entered from",
          "[contour][palette]")
{
    auto const config = configWithProfiles({ "dark", "light" });
    auto const profiles = command::ProfileCommandSource { config };
    auto const actionCommands = command::ActionCommandSource {};
    auto history = command::CommandHistory { 3 };
    auto model =
        CommandPaletteModel { history, Sources { &profiles, &actionCommands }, command::ShortcutStyle::Text };
    model.refresh({});

    model.setFilter(QStringLiteral("prof"));
    auto const parentRow = rowOf(model, "ChangeProfile");
    REQUIRE(parentRow >= 0);

    auto levelSpy = QSignalSpy { &model, &CommandPaletteModel::levelChanged };
    auto filterSpy = QSignalSpy { &model, &CommandPaletteModel::filterChanged };
    auto order = std::vector<std::string> {};
    QObject::connect(&model, &CommandPaletteModel::levelChanged, [&] { order.emplace_back("level"); });
    QObject::connect(&model, &CommandPaletteModel::filterChanged, [&] { order.emplace_back("filter"); });

    model.enter(parentRow);

    CHECK(model.depth() == static_cast<int>(command::PaletteDepth::Level));
    CHECK(model.levelTitle() == QStringLiteral("Change Profile"));
    CHECK(model.filter().isEmpty());
    CHECK(model.rowCount() == 2);
    CHECK(model.anchorRow() == 0);
    CHECK(levelSpy.count() == 1);
    CHECK(filterSpy.count() == 1);
    CHECK(order == std::vector<std::string> { "level", "filter" });
    CHECK(titleAt(model, 0) == "dark");
    CHECK(idAt(model, 0) == "ChangeProfile:dark");
    CHECK(groupTitleAt(model, 0).empty());

    SECTION("a filter inside the level narrows only its children")
    {
        model.setFilter(QStringLiteral("li"));
        REQUIRE(model.rowCount() == 1);
        CHECK(titleAt(model, 0) == "light");
    }

    SECTION("back() returns to the top with the old query and the parent's row anchored")
    {
        model.setFilter(QStringLiteral("li"));
        order.clear();
        model.back();

        CHECK(model.depth() == model.topDepth());
        CHECK(model.levelTitle().isEmpty());
        CHECK(model.filter() == QStringLiteral("prof"));
        CHECK(model.anchorRow() == rowOf(model, "ChangeProfile"));
        CHECK(order == std::vector<std::string> { "level", "filter" });
    }

    SECTION("back() anchors the parent's row wherever the restored listing puts it")
    {
        // Unfiltered, the parent sits in the Profiles group, far below row 0.
        model.back();
        model.setFilter(QString {});
        auto const unfilteredParent = rowOf(model, "ChangeProfile");
        REQUIRE(unfilteredParent > 0);
        model.enter(unfilteredParent);
        model.back();
        CHECK(model.anchorRow() == unfilteredParent);
    }

    SECTION("enter() inside a level does nothing: levels do not nest")
    {
        model.enter(0);
        CHECK(levelSpy.count() == 1);
        CHECK(model.rowCount() == 2);
    }
}

TEST_CASE("enter() on a command row does nothing", "[contour][palette]")
{
    auto const actionCommands = command::ActionCommandSource {};
    auto history = command::CommandHistory { 3 };
    auto model = CommandPaletteModel { history, Sources { &actionCommands }, command::ShortcutStyle::Text };
    model.refresh({});

    auto levelSpy = QSignalSpy { &model, &CommandPaletteModel::levelChanged };
    auto filterSpy = QSignalSpy { &model, &CommandPaletteModel::filterChanged };

    auto const row = rowOf(model, "SplitVertical");
    REQUIRE(row >= 0);
    model.enter(row);
    model.enter(-1);
    model.enter(model.rowCount());
    model.back(); // at the top: nothing to go back to

    CHECK(model.depth() == model.topDepth());
    CHECK(levelSpy.count() == 0);
    CHECK(filterSpy.count() == 0);
}

TEST_CASE("Reopening the palette starts at the top", "[contour][palette]")
{
    auto const config = configWithProfiles({ "dark", "light" });
    auto const profiles = command::ProfileCommandSource { config };
    auto history = command::CommandHistory { 3 };
    auto model = CommandPaletteModel { history, Sources { &profiles }, command::ShortcutStyle::Text };
    model.refresh({});

    model.enter(rowOf(model, "ChangeProfile"));
    model.setFilter(QStringLiteral("d"));
    REQUIRE(model.depth() == static_cast<int>(command::PaletteDepth::Level));

    auto levelSpy = QSignalSpy { &model, &CommandPaletteModel::levelChanged };
    model.refresh({});

    CHECK(model.depth() == model.topDepth());
    CHECK(model.filter().isEmpty());
    CHECK(levelSpy.count() == 1);
    CHECK(rowOf(model, "ChangeProfile") >= 0);
}

TEST_CASE("Reopening the palette re-anchors at row 0 and says so", "[contour][palette]")
{
    // anchorRow's NOTIFY is levelChanged: a refresh that moves the anchor must emit it even at the top.
    auto const config = configWithProfiles({ "dark" });
    auto const profiles = command::ProfileCommandSource { config };
    auto const actionCommands = command::ActionCommandSource {};
    auto history = command::CommandHistory { 3 };
    auto model =
        CommandPaletteModel { history, Sources { &profiles, &actionCommands }, command::ShortcutStyle::Text };
    model.refresh({});

    model.enter(rowOf(model, "ChangeProfile"));
    model.back();
    REQUIRE(model.anchorRow() > 0);

    auto levelSpy = QSignalSpy { &model, &CommandPaletteModel::levelChanged };
    model.refresh({});

    CHECK(model.anchorRow() == 0);
    CHECK(levelSpy.count() == 1);
}

TEST_CASE("footerHints follows the row and the depth", "[contour][palette]")
{
    using command::PaletteDepth;
    using command::PaletteEntryKind;
    using command::paletteFooterHints;
    using command::ShortcutStyle;

    auto const config = configWithProfiles({ "dark" });
    auto const profiles = command::ProfileCommandSource { config };
    auto const actionCommands = command::ActionCommandSource {};
    auto history = command::CommandHistory { 3 };
    auto model = CommandPaletteModel { history, Sources { &profiles, &actionCommands }, ShortcutStyle::Text };
    model.refresh({});

    auto const text = [](PaletteEntryKind kind, PaletteDepth depth) {
        return QString::fromStdString(paletteFooterHints(kind, depth, ShortcutStyle::Text));
    };

    auto const hintsAt = [&](int row) {
        return roleAt(model, row, CommandPaletteModel::Roles::FooterHintsRole).toString();
    };
    CHECK(hintsAt(rowOf(model, "SplitVertical")) == text(PaletteEntryKind::Command, PaletteDepth::Top));
    CHECK(hintsAt(rowOf(model, "ChangeProfile")) == text(PaletteEntryKind::DrillIn, PaletteDepth::Top));
    CHECK(hintsAt(-1).isEmpty());
    CHECK(hintsAt(model.rowCount()).isEmpty());

    model.enter(rowOf(model, "ChangeProfile"));
    CHECK(hintsAt(0) == text(PaletteEntryKind::Command, PaletteDepth::Level));
}

TEST_CASE("A shown palette keeps its query and level through a refresh", "[contour][palette]")
{
    auto const config = configWithProfiles({ "dark", "light" });
    auto const profiles = command::ProfileCommandSource { config };
    auto history = command::CommandHistory { 3 };
    auto model = CommandPaletteModel { history, Sources { &profiles }, command::ShortcutStyle::Text };
    model.refresh({});
    model.enter(rowOf(model, "ChangeProfile"));
    model.setFilter(QStringLiteral("d"));

    model.setShown(true);
    model.refresh({});
    CHECK(model.depth() == static_cast<int>(command::PaletteDepth::Level));
    CHECK(model.filter() == QStringLiteral("d"));

    model.setShown(false);
    model.refresh({});
    CHECK(model.depth() == model.topDepth());
    CHECK(model.filter().isEmpty());
}
