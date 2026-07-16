// SPDX-License-Identifier: Apache-2.0
//
// The ordering the command palette presents. Two orderings have to be right, and they are the whole
// user-visible behaviour of the feature:
//
//   - no filter: recently used first (newest first, capped), then everything alphabetically;
//   - a filter:  one flat list, best match first.
//
// Plus the promise that makes the MRU safe to persist: a remembered command that no longer EXISTS
// (its profile was deleted, its tab was closed) quietly stops appearing, instead of leaving a dead row.

#include <contour/CommandCatalog.h>
#include <contour/CommandHistory.h>
#include <contour/CommandPaletteModel.h>
#include <contour/Config.h>
#include <contour/FuzzyFilter.h>
#include <contour/Shortcut.h>
#include <contour/test/GuiTestFixtures.h>

#include <catch2/catch_test_macros.hpp>

#include <string>
#include <vector>

using namespace contour;

namespace
{
/// Reads one role off a row, the way the QML delegate does.
[[nodiscard]] QVariant roleAt(CommandPaletteModel const& model, int row, int role)
{
    return model.data(model.index(row, 0), role);
}

[[nodiscard]] std::string titleAt(CommandPaletteModel const& model, int row)
{
    return roleAt(model, row, CommandPaletteModel::TitleRole).toString().toStdString();
}

[[nodiscard]] std::string idAt(CommandPaletteModel const& model, int row)
{
    return roleAt(model, row, CommandPaletteModel::IdRole).toString().toStdString();
}

[[nodiscard]] int sectionAt(CommandPaletteModel const& model, int row)
{
    return roleAt(model, row, CommandPaletteModel::SectionRole).toInt();
}

/// The title character indices the current filter matched on a row, as the QML delegate reads them.
[[nodiscard]] std::vector<int> titleMatchesAt(CommandPaletteModel const& model, int row)
{
    auto result = std::vector<int> {};
    for (auto const& entry: roleAt(model, row, CommandPaletteModel::TitleMatchesRole).toList())
        result.push_back(entry.toInt());
    return result;
}

/// The row index of the command with @p id, or -1 if it is not present.
[[nodiscard]] int rowOf(CommandPaletteModel const& model, std::string const& id)
{
    for (auto row = 0; row < model.rowCount(); ++row)
        if (idAt(model, row) == id)
            return row;
    return -1;
}

/// The ids of the rows in the Recent section, in order.
[[nodiscard]] std::vector<std::string> recentRows(CommandPaletteModel const& model)
{
    auto result = std::vector<std::string> {};
    for (auto row = 0; row < model.rowCount(); ++row)
        if (sectionAt(model, row) == static_cast<int>(CommandPaletteModel::Section::Recent))
            result.push_back(idAt(model, row));
    return result;
}
} // namespace

TEST_CASE("With no filter, the palette shows Recent first, then everything alphabetically",
          "[contour][palette]")
{
    auto history = CommandHistory { 3 };
    history.record("SplitVertical");
    history.record("CreateNewTab"); // newest

    auto const actionCommands = ActionCommandSource {};
    auto model = CommandPaletteModel { history };
    model.setSources({ &actionCommands });
    model.refresh();

    REQUIRE(model.rowCount() > 0);
    CHECK(model.sectioned());

    SECTION("the recent commands lead, newest first")
    {
        CHECK(recentRows(model) == std::vector<std::string> { "CreateNewTab", "SplitVertical" });
        CHECK(sectionAt(model, 0) == static_cast<int>(CommandPaletteModel::Section::Recent));
        CHECK(idAt(model, 0) == "CreateNewTab");
    }

    SECTION("the first row of each section is marked, so QML draws exactly one header for it")
    {
        CHECK(roleAt(model, 0, CommandPaletteModel::SectionStartRole).toBool());
        CHECK_FALSE(roleAt(model, 1, CommandPaletteModel::SectionStartRole).toBool());
        // Row 2 is the first of the "All" section (two recent rows precede it).
        CHECK(roleAt(model, 2, CommandPaletteModel::SectionStartRole).toBool());
        CHECK(sectionAt(model, 2) == static_cast<int>(CommandPaletteModel::Section::All));
    }

    SECTION("the All section is alphabetical by title")
    {
        auto previous = std::string {};
        for (auto row = 0; row < model.rowCount(); ++row)
        {
            if (sectionAt(model, row) != static_cast<int>(CommandPaletteModel::Section::All))
                continue;
            auto const title = titleAt(model, row);
            INFO("row " << row << ": '" << previous << "' then '" << title << "'");
            CHECK(previous <= title);
            previous = title;
        }
    }

    SECTION("a recent command still appears in the All list too")
    {
        // The Recent section is a shortcut, not a move: a user browsing the alphabetical list must
        // still find Split Vertical under S.
        auto found = false;
        for (auto row = 0; row < model.rowCount(); ++row)
            if (sectionAt(model, row) == static_cast<int>(CommandPaletteModel::Section::All)
                && idAt(model, row) == "SplitVertical")
                found = true;
        CHECK(found);
    }
}

TEST_CASE("A remembered command that no longer exists quietly stops appearing", "[contour][palette]")
{
    // The self-healing property that makes the MRU safe to persist by id. The user switched to a tab,
    // then closed it; the stored "SwitchToTab:2" must not become a dead row that does nothing when
    // picked — it must simply be gone.
    auto tabs = test::StubTabs { { "zsh", "vim" } };
    auto const tabCommands = TabCommandSource { tabs };

    auto history = CommandHistory { 5 };
    history.record("SwitchToTab:2");

    auto model = CommandPaletteModel { history };
    model.setSources({ &tabCommands });
    model.refresh();
    REQUIRE(recentRows(model) == std::vector<std::string> { "SwitchToTab:2" });

    // The second tab closes.
    tabs.setTitles({ "zsh" });
    model.refresh();

    CHECK(recentRows(model).empty());
    CHECK(model.commandById("SwitchToTab:2") == nullptr);
}

TEST_CASE("Typing collapses the sections into one ranked list", "[contour][palette]")
{
    auto history = CommandHistory { 3 };
    auto const actionCommands = ActionCommandSource {};

    auto model = CommandPaletteModel { history };
    model.setSources({ &actionCommands });
    model.refresh();

    auto const unfiltered = model.rowCount();
    model.setFilter(QStringLiteral("spl"));

    SECTION("the list is filtered down and no longer sectioned")
    {
        CHECK(model.rowCount() < unfiltered);
        CHECK(model.rowCount() > 0);
        CHECK_FALSE(model.sectioned());
    }

    SECTION("the commands the user meant lead, and the incidental match trails")
    {
        // "spl" is genuinely ambiguous between the two Split commands — they score identically, and the
        // tie breaks alphabetically. What must NOT happen is "Toggle Split Orientation" (which merely
        // contains the letters, mid-word) coming up before either of them.
        auto rank = [&](std::string const& id) {
            for (auto row = 0; row < model.rowCount(); ++row)
                if (idAt(model, row) == id)
                    return row;
            return -1;
        };

        auto const horizontal = rank("SplitHorizontal");
        auto const vertical = rank("SplitVertical");
        auto const toggle = rank("ToggleSplitOrientation");

        REQUIRE(horizontal >= 0);
        REQUIRE(vertical >= 0);
        REQUIRE(toggle >= 0);
        CHECK(horizontal < toggle);
        CHECK(vertical < toggle);
        // One of the two Splits is under the cursor when the popup opens.
        CHECK((idAt(model, 0) == "SplitHorizontal" || idAt(model, 0) == "SplitVertical"));
    }

    SECTION("every surviving row actually matches")
    {
        for (auto row = 0; row < model.rowCount(); ++row)
        {
            INFO("row " << row << ": " << titleAt(model, row));
            CHECK(fuzzyScore("spl", titleAt(model, row)).has_value());
        }
    }

    SECTION("clearing the filter restores the sectioned list")
    {
        model.setFilter(QString {});
        CHECK(model.sectioned());
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
    // These indices are what QML bolds; they must point at the characters the filter actually landed on.
    auto history = CommandHistory { 3 };
    auto const actionCommands = ActionCommandSource {};

    auto model = CommandPaletteModel { history };
    model.setSources({ &actionCommands });
    model.refresh();

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
        REQUIRE(titleAt(model, row) == "Split Vertical");
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
    // Regression guard: the fuzzy filter reports matched positions as UTF-8 byte offsets, but the QML
    // delegate indexes the title as a UTF-16 string. A tab title can carry any UTF-8 (a shell sets it), so
    // a multibyte character before a match would otherwise shift — or drop — every highlight after it.
    // U+2192 (→) is 3 UTF-8 bytes but a single UTF-16 code unit, so raw byte offsets run two ahead of the
    // indices QML needs.
    auto tabs = test::StubTabs { { "→zephyr" } }; // → then a run of letters unique to the title
    auto const tabCommands = TabCommandSource { tabs };

    auto history = CommandHistory { 3 };
    auto model = CommandPaletteModel { history };
    model.setSources({ &tabCommands });
    model.refresh();

    model.setFilter(QStringLiteral("zephyr"));
    auto const row = rowOf(model, "SwitchToTab:1");
    REQUIRE(row >= 0);
    auto const title = QString::fromStdString(titleAt(model, row));
    REQUIRE(title == QString::fromUtf8("Switch To Tab 1: →zephyr"));

    // "zephyr" occurs exactly once, contiguously, right after the → at UTF-16 index 18. As raw byte
    // offsets these would be 20..25 (two higher, and 25 is past the string's end).
    auto const matches = titleMatchesAt(model, row);
    CHECK(matches == std::vector<int> { 18, 19, 20, 21, 22, 23 });

    // Each reported index must land on the matched character in the UTF-16 string — the property the QML
    // highlighter relies on. Byte offsets would index the wrong characters (and run off the end) here.
    auto const query = QStringLiteral("zephyr");
    for (auto i = 0; i < static_cast<int>(matches.size()); ++i)
        CHECK(title.at(matches[static_cast<std::size_t>(i)]) == query.at(i));
}

TEST_CASE("Recency breaks a tie between two equally good matches", "[contour][palette]")
{
    // Two commands score the same against "toggle"; the one the user reached for last time should be
    // the one under the cursor.
    auto history = CommandHistory { 5 };
    history.record("ToggleStatusLine");

    auto const actionCommands = ActionCommandSource {};
    auto model = CommandPaletteModel { history };
    model.setSources({ &actionCommands });
    model.refresh();
    model.setFilter(QStringLiteral("togglestatusline"));

    REQUIRE(model.rowCount() > 0);
    CHECK(idAt(model, 0) == "ToggleStatusLine");
}

TEST_CASE("The shortcut column advertises the chord that runs the command", "[contour][palette]")
{
    auto history = CommandHistory { 3 };
    auto const actionCommands = ActionCommandSource {};

    auto model = CommandPaletteModel { history };
    model.setSources({ &actionCommands });
    model.setShortcuts(shortcutIndex(config::defaultInputMappings));
    model.refresh();

    model.setFilter(QStringLiteral("SplitVertical"));
    REQUIRE(model.rowCount() > 0);
    REQUIRE(idAt(model, 0) == "SplitVertical");
    CHECK(roleAt(model, 0, CommandPaletteModel::ShortcutRole).toString().toStdString() == "Ctrl+Shift+E");

    SECTION("an unbound command shows an empty shortcut, not a stale or wrong one")
    {
        // ClearHistoryAndReset has no default binding.
        model.setFilter(QStringLiteral("ClearHistoryAndReset"));
        REQUIRE(model.rowCount() > 0);
        REQUIRE(idAt(model, 0) == "ClearHistoryAndReset");
        CHECK(roleAt(model, 0, CommandPaletteModel::ShortcutRole).toString().isEmpty());
    }
}

TEST_CASE("A zero recent_count turns the Recent section off entirely", "[contour][palette]")
{
    auto history = CommandHistory { 0 };
    history.record("SplitVertical");

    auto const actionCommands = ActionCommandSource {};
    auto model = CommandPaletteModel { history };
    model.setSources({ &actionCommands });
    model.refresh();

    CHECK(recentRows(model).empty());
    // The full list is still there — only the pinned section is gone.
    CHECK(model.rowCount() > 0);
    CHECK(sectionAt(model, 0) == static_cast<int>(CommandPaletteModel::Section::All));
}
