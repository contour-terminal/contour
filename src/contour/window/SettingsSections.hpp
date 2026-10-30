// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// The Settings window's sections and the groups of fields in each, as one table: which section a group
/// belongs to, whether it is per profile, and its title. Qt-free.

#include <array>
#include <cstddef>
#include <cstdint>
#include <ranges>
#include <string_view>

namespace contour::window
{

/// A section of the Settings sidebar.
enum class SettingsSection : uint8_t
{
    General = 0,
    Appearance,
    Keyboard,
    ShellIntegration,
    Profiles,
    Hosts,
};

/// How many sections there are.
inline constexpr std::size_t SettingsSectionCount = static_cast<std::size_t>(SettingsSection::Hosts) + 1;

/// Whether a group's fields are the application's or a profile's.
enum class SettingsScope : uint8_t
{
    Global = 0, ///< settings.yml over contour.yml.
    Profile,    ///< A profile's side file.
    Host,       ///< A saved host's side file.
};

/// A titled group of fields, as a section's page shows them.
enum class SettingsGroup : uint8_t
{
    GeneralWindows = 0,
    GeneralQuickTerminal,
    GeneralPalette,
    GeneralAdvanced,
    AppearanceChrome,
    AppearanceTabBar,
    KeyboardBindings,
    ShellGutter,
    ShellCommandBlocks,
    ShellFolding,
    ProfileShell,
    ProfileColors,
    ProfileWindow,
    ProfileFont,
    ProfileScrolling,
    ProfileBehaviour,
    ProfileTerminal,
    ProfileKeyboard,
    ProfileHistory,
    ProfileCommandBlocks,
    ProfileMouse,
    ProfileVisualEffects,
    ProfileCursor,
    ProfileBell,
    ProfileBackground,
    ProfileLinks,
    ProfileStatusLine,
    HostConnection,
    HostSsh,
    HostTls,
    HostSessions,
};

/// How many groups there are.
inline constexpr std::size_t SettingsGroupCount = static_cast<std::size_t>(SettingsGroup::HostSessions) + 1;

/// The icon a sidebar row shows.
enum class SettingsIcon : uint8_t
{
    Gear = 0,
    Paintbrush,
    Keyboard,
    Terminal,
    Person,
    Document,
    Network,
};

/// One section of the sidebar.
struct SettingsSectionRow
{
    SettingsSection section = SettingsSection::General; ///< The section.
    std::string_view title {};                          ///< Its sidebar title.
    std::string_view id {};                             ///< Its page id, stable across releases.
    SettingsIcon icon = SettingsIcon::Gear;             ///< Its sidebar icon.
};

/// The sections, in sidebar order.
inline constexpr auto SettingsSectionRows = std::array {
    SettingsSectionRow { .section = SettingsSection::General,
                         .title = "General",
                         .id = "general",
                         .icon = SettingsIcon::Gear },
    SettingsSectionRow { .section = SettingsSection::Appearance,
                         .title = "Appearance",
                         .id = "appearance",
                         .icon = SettingsIcon::Paintbrush },
    SettingsSectionRow { .section = SettingsSection::Keyboard,
                         .title = "Keyboard",
                         .id = "keyboard",
                         .icon = SettingsIcon::Keyboard },
    SettingsSectionRow { .section = SettingsSection::ShellIntegration,
                         .title = "Shell Integration",
                         .id = "shell-integration",
                         .icon = SettingsIcon::Terminal },
    SettingsSectionRow { .section = SettingsSection::Profiles,
                         .title = "Profiles",
                         .id = "profiles",
                         .icon = SettingsIcon::Person },
    SettingsSectionRow {
        .section = SettingsSection::Hosts, .title = "Hosts", .id = "hosts", .icon = SettingsIcon::Network },
};

/// One group of fields: the section it is shown in, whose fields it holds, and its title.
struct SettingsGroupRow
{
    SettingsGroup group = SettingsGroup::GeneralWindows; ///< The group.
    SettingsSection section = SettingsSection::General;  ///< The section whose page shows it.
    SettingsScope scope = SettingsScope::Global;         ///< Whose fields it holds.
    std::string_view title {};                           ///< Its title on the page.
};

/// Every group, in page order. Adding a group is adding a row here and an enumerator above.
inline constexpr auto SettingsGroupRows = std::array {
    SettingsGroupRow { .group = SettingsGroup::GeneralWindows,
                       .section = SettingsSection::General,
                       .scope = SettingsScope::Global,
                       .title = "Windows" },
    SettingsGroupRow { .group = SettingsGroup::GeneralQuickTerminal,
                       .section = SettingsSection::General,
                       .scope = SettingsScope::Global,
                       .title = "Quick Terminal" },
    SettingsGroupRow { .group = SettingsGroup::GeneralPalette,
                       .section = SettingsSection::General,
                       .scope = SettingsScope::Global,
                       .title = "Command palette" },
    SettingsGroupRow { .group = SettingsGroup::GeneralAdvanced,
                       .section = SettingsSection::General,
                       .scope = SettingsScope::Global,
                       .title = "Advanced" },
    SettingsGroupRow { .group = SettingsGroup::AppearanceChrome,
                       .section = SettingsSection::Appearance,
                       .scope = SettingsScope::Global,
                       .title = "Window chrome" },
    SettingsGroupRow { .group = SettingsGroup::AppearanceTabBar,
                       .section = SettingsSection::Appearance,
                       .scope = SettingsScope::Global,
                       .title = "Tab bar" },
    SettingsGroupRow { .group = SettingsGroup::KeyboardBindings,
                       .section = SettingsSection::Keyboard,
                       .scope = SettingsScope::Global,
                       .title = "Key bindings" },
    SettingsGroupRow { .group = SettingsGroup::ShellGutter,
                       .section = SettingsSection::ShellIntegration,
                       .scope = SettingsScope::Global,
                       .title = "Gutter" },
    SettingsGroupRow { .group = SettingsGroup::ShellCommandBlocks,
                       .section = SettingsSection::ShellIntegration,
                       .scope = SettingsScope::Global,
                       .title = "Command blocks" },
    SettingsGroupRow { .group = SettingsGroup::ShellFolding,
                       .section = SettingsSection::ShellIntegration,
                       .scope = SettingsScope::Global,
                       .title = "Folding" },
    SettingsGroupRow { .group = SettingsGroup::ProfileShell,
                       .section = SettingsSection::Profiles,
                       .scope = SettingsScope::Profile,
                       .title = "Shell" },
    SettingsGroupRow { .group = SettingsGroup::ProfileColors,
                       .section = SettingsSection::Profiles,
                       .scope = SettingsScope::Profile,
                       .title = "Colors" },
    SettingsGroupRow { .group = SettingsGroup::ProfileWindow,
                       .section = SettingsSection::Profiles,
                       .scope = SettingsScope::Profile,
                       .title = "Window" },
    SettingsGroupRow { .group = SettingsGroup::ProfileFont,
                       .section = SettingsSection::Profiles,
                       .scope = SettingsScope::Profile,
                       .title = "Text" },
    SettingsGroupRow { .group = SettingsGroup::ProfileScrolling,
                       .section = SettingsSection::Profiles,
                       .scope = SettingsScope::Profile,
                       .title = "Scrolling" },
    SettingsGroupRow { .group = SettingsGroup::ProfileBehaviour,
                       .section = SettingsSection::Profiles,
                       .scope = SettingsScope::Profile,
                       .title = "Behaviour" },
    SettingsGroupRow { .group = SettingsGroup::ProfileTerminal,
                       .section = SettingsSection::Profiles,
                       .scope = SettingsScope::Profile,
                       .title = "Terminal" },
    SettingsGroupRow { .group = SettingsGroup::ProfileKeyboard,
                       .section = SettingsSection::Profiles,
                       .scope = SettingsScope::Profile,
                       .title = "Keyboard" },
    SettingsGroupRow { .group = SettingsGroup::ProfileHistory,
                       .section = SettingsSection::Profiles,
                       .scope = SettingsScope::Profile,
                       .title = "History & scrollbar" },
    SettingsGroupRow { .group = SettingsGroup::ProfileCommandBlocks,
                       .section = SettingsSection::Profiles,
                       .scope = SettingsScope::Profile,
                       .title = "Command blocks" },
    SettingsGroupRow { .group = SettingsGroup::ProfileMouse,
                       .section = SettingsSection::Profiles,
                       .scope = SettingsScope::Profile,
                       .title = "Mouse & permissions" },
    SettingsGroupRow { .group = SettingsGroup::ProfileVisualEffects,
                       .section = SettingsSection::Profiles,
                       .scope = SettingsScope::Profile,
                       .title = "Visual effects" },
    SettingsGroupRow { .group = SettingsGroup::ProfileCursor,
                       .section = SettingsSection::Profiles,
                       .scope = SettingsScope::Profile,
                       .title = "Cursor" },
    SettingsGroupRow { .group = SettingsGroup::ProfileBell,
                       .section = SettingsSection::Profiles,
                       .scope = SettingsScope::Profile,
                       .title = "Bell" },
    SettingsGroupRow { .group = SettingsGroup::ProfileBackground,
                       .section = SettingsSection::Profiles,
                       .scope = SettingsScope::Profile,
                       .title = "Background" },
    SettingsGroupRow { .group = SettingsGroup::ProfileLinks,
                       .section = SettingsSection::Profiles,
                       .scope = SettingsScope::Profile,
                       .title = "Links" },
    SettingsGroupRow { .group = SettingsGroup::ProfileStatusLine,
                       .section = SettingsSection::Profiles,
                       .scope = SettingsScope::Profile,
                       .title = "Status line" },
    SettingsGroupRow { .group = SettingsGroup::HostConnection,
                       .section = SettingsSection::Hosts,
                       .scope = SettingsScope::Host,
                       .title = "Connection" },
    SettingsGroupRow { .group = SettingsGroup::HostSsh,
                       .section = SettingsSection::Hosts,
                       .scope = SettingsScope::Host,
                       .title = "SSH" },
    SettingsGroupRow { .group = SettingsGroup::HostTls,
                       .section = SettingsSection::Hosts,
                       .scope = SettingsScope::Host,
                       .title = "TLS" },
    SettingsGroupRow { .group = SettingsGroup::HostSessions,
                       .section = SettingsSection::Hosts,
                       .scope = SettingsScope::Host,
                       .title = "Sessions" },
};

static_assert(SettingsSectionRows.size() == SettingsSectionCount, "one row per SettingsSection");
static_assert(SettingsGroupRows.size() == SettingsGroupCount, "one row per SettingsGroup");
static_assert(
    [] {
        for (auto const index: std::views::iota(std::size_t { 0 }, SettingsSectionCount))
            if (SettingsSectionRows.at(index).section != static_cast<SettingsSection>(index))
                return false;
        for (auto const index: std::views::iota(std::size_t { 0 }, SettingsGroupCount))
            if (SettingsGroupRows.at(index).group != static_cast<SettingsGroup>(index))
                return false;
        return true;
    }(),
    "the rows are in enumerator order");

/// @param group A group.
/// @return Its row.
[[nodiscard]] constexpr SettingsGroupRow const& settingsGroupRow(SettingsGroup group) noexcept
{
    return SettingsGroupRows[static_cast<std::size_t>(group)];
}

/// @param section A section.
/// @return Its row.
[[nodiscard]] constexpr SettingsSectionRow const& settingsSectionRow(SettingsSection section) noexcept
{
    return SettingsSectionRows[static_cast<std::size_t>(section)];
}

/// @param section A section.
/// @return The rows of the groups its page shows, in page order.
[[nodiscard]] constexpr auto groupsOf(SettingsSection section)
{
    return SettingsGroupRows
           | std::views::filter([section](SettingsGroupRow const& row) { return row.section == section; });
}

/// A page that is not a group of fields, which a section's page links to.
enum class SettingsPageTarget : uint8_t
{
    DefaultProfileStatusLine = 0, ///< The status line page of the default profile.
};

/// How many page targets there are.
inline constexpr std::size_t SettingsPageTargetCount =
    static_cast<std::size_t>(SettingsPageTarget::DefaultProfileStatusLine) + 1;

/// One link a section's page shows to a page of its own.
struct SettingsPageLink
{
    SettingsSection section = SettingsSection::Appearance; ///< The section whose page shows the link.
    std::string_view title {};                             ///< The link's row title, and what search finds.
    std::string_view id {};                                ///< The target's id, stable across releases.
    SettingsPageTarget target = SettingsPageTarget::DefaultProfileStatusLine; ///< The page it opens.
};

/// Every link, in enumerator order of its target. Adding a page is adding a row here and an enumerator.
inline constexpr auto SettingsPageLinks = std::array {
    SettingsPageLink { .section = SettingsSection::Appearance,
                       .title = "Status Line",
                       .id = "default-profile-status-line",
                       .target = SettingsPageTarget::DefaultProfileStatusLine },
};

static_assert(SettingsPageLinks.size() == SettingsPageTargetCount, "one row per SettingsPageTarget");
static_assert(
    [] {
        for (auto const index: std::views::iota(std::size_t { 0 }, SettingsPageTargetCount))
            if (SettingsPageLinks.at(index).target != static_cast<SettingsPageTarget>(index))
                return false;
        return true;
    }(),
    "the links are in enumerator order");

/// @param section A section.
/// @return The links its page shows, in page order.
[[nodiscard]] constexpr auto linksOf(SettingsSection section)
{
    return SettingsPageLinks
           | std::views::filter([section](SettingsPageLink const& row) { return row.section == section; });
}

/// The sidebar's last entry, which opens contour.yml in the OS's editor.
inline constexpr std::string_view EditConfigEntryTitle = "Edit contour.yml…";

} // namespace contour::window
