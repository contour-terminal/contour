// SPDX-License-Identifier: Apache-2.0
#include <contour/window/SettingsSections.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <ranges>
#include <set>
#include <string_view>

using contour::window::groupsOf;
using contour::window::SettingsGroup;
using contour::window::SettingsGroupCount;
using contour::window::settingsGroupRow;
using contour::window::SettingsGroupRows;
using contour::window::SettingsScope;
using contour::window::SettingsSection;
using contour::window::SettingsSectionCount;
using contour::window::SettingsSectionRows;

TEST_CASE("SettingsSections: every group appears exactly once, in enumerator order", "[settingssections]")
{
    REQUIRE(SettingsGroupRows.size() == SettingsGroupCount);
    for (auto const index: std::views::iota(std::size_t { 0 }, SettingsGroupCount))
    {
        auto const group = static_cast<SettingsGroup>(index);
        INFO(index);
        CHECK(std::ranges::count(SettingsGroupRows, group, &contour::window::SettingsGroupRow::group) == 1);
        CHECK(settingsGroupRow(group).group == group);
        CHECK_FALSE(settingsGroupRow(group).title.empty());
    }
}

TEST_CASE("SettingsSections: the sections are the spec's five, in order", "[settingssections]")
{
    REQUIRE(SettingsSectionRows.size() == SettingsSectionCount);
    auto const titles = std::array<std::string_view, SettingsSectionCount> {
        "General", "Appearance", "Keyboard", "Shell Integration", "Profiles"
    };
    for (auto const index: std::views::iota(std::size_t { 0 }, SettingsSectionCount))
    {
        INFO(index);
        CHECK(SettingsSectionRows.at(index).section == static_cast<SettingsSection>(index));
        CHECK(SettingsSectionRows.at(index).title == titles.at(index));
        CHECK_FALSE(SettingsSectionRows.at(index).id.empty());
    }
    CHECK(contour::window::EditConfigEntryTitle == "Edit contour.yml…");
}

TEST_CASE("SettingsSections: every section has a group, and only Profiles' are per profile",
          "[settingssections]")
{
    for (auto const& row: SettingsSectionRows)
    {
        INFO(row.title);
        CHECK_FALSE(std::ranges::empty(groupsOf(row.section)));
        auto titles = std::set<std::string_view> {};
        for (auto const& group: groupsOf(row.section))
        {
            CHECK(group.section == row.section);
            CHECK((group.scope == SettingsScope::Profile) == (row.section == SettingsSection::Profiles));
            CHECK(titles.insert(group.title).second); // unique within the section
        }
    }
}

TEST_CASE("SettingsSections: the canvas's group titles", "[settingssections]")
{
    CHECK(settingsGroupRow(SettingsGroup::ProfileFont).title == "Text");
    CHECK(settingsGroupRow(SettingsGroup::GeneralWindows).title == "Windows");
    CHECK(settingsGroupRow(SettingsGroup::ProfileHistory).title == "History & scrollbar");
}
