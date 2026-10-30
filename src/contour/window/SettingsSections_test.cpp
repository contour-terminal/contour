// SPDX-License-Identifier: Apache-2.0
#include <contour/window/SettingsSections.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <ranges>
#include <set>
#include <string_view>
#include <vector>

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

TEST_CASE("SettingsSections: the sections are the spec's six, in order", "[settingssections]")
{
    REQUIRE(SettingsSectionRows.size() == SettingsSectionCount);
    auto const titles =
        std::array<std::string_view, SettingsSectionCount> { "General",           "Appearance", "Keyboard",
                                                             "Shell Integration", "Profiles",   "Hosts" };
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

TEST_CASE("SettingsSections: every page link names its own target, in order", "[settingssections]")
{
    using contour::window::SettingsPageLinks;
    using contour::window::SettingsPageTarget;
    using contour::window::SettingsPageTargetCount;
    REQUIRE(SettingsPageLinks.size() == SettingsPageTargetCount);
    auto ids = std::set<std::string_view> {};
    for (auto const index: std::views::iota(std::size_t { 0 }, SettingsPageTargetCount))
    {
        auto const& link = SettingsPageLinks.at(index);
        INFO(link.title);
        CHECK(link.target == static_cast<SettingsPageTarget>(index));
        CHECK_FALSE(link.title.empty());
        CHECK(ids.insert(link.id).second);
    }
    auto appearance = contour::window::linksOf(SettingsSection::Appearance);
    REQUIRE(std::ranges::distance(appearance) == 1);
    CHECK(appearance.front().title == "Status Line");
    CHECK(appearance.front().target == SettingsPageTarget::DefaultProfileStatusLine);
    CHECK(std::ranges::empty(contour::window::linksOf(SettingsSection::General)));
}

TEST_CASE("SettingsSections: the quick terminal's group is in General, after Windows", "[settingssections]")
{
    auto const& row = settingsGroupRow(SettingsGroup::GeneralQuickTerminal);
    CHECK(row.title == "Quick Terminal");
    CHECK(row.section == SettingsSection::General);
    CHECK(row.scope == SettingsScope::Global);
    CHECK(static_cast<std::size_t>(SettingsGroup::GeneralQuickTerminal)
          == static_cast<std::size_t>(SettingsGroup::GeneralWindows) + 1);
}

TEST_CASE("SettingsSections: the Shell group is a profile's, ahead of the other profile groups",
          "[settingssections]")
{
    auto const& row = settingsGroupRow(SettingsGroup::ProfileShell);
    CHECK(row.title == "Shell");
    CHECK(row.section == SettingsSection::Profiles);
    CHECK(row.scope == SettingsScope::Profile);
    CHECK(static_cast<std::size_t>(SettingsGroup::ProfileShell)
          < static_cast<std::size_t>(SettingsGroup::ProfileWindow));
}

TEST_CASE("SettingsSections: the Hosts section's groups are a host's, in the board's order",
          "[settingssections]")
{
    auto titles = std::vector<std::string_view> {};
    for (auto const& group: groupsOf(SettingsSection::Hosts))
    {
        CHECK(group.scope == SettingsScope::Host);
        titles.push_back(group.title);
    }
    CHECK(titles == std::vector<std::string_view> { "Connection", "SSH", "TLS", "Sessions" });
}
