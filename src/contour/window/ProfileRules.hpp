// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// What a profile allows in Settings -- editing, duplicating, renaming, deleting, making it the default --
/// decided by where it is defined and the lock, as one table (R-f). Qt-free.

#include <contour/config/Config.hpp>
#include <contour/config/SettingsEditSession.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <format>
#include <functional>
#include <ranges>
#include <string>
#include <string_view>

namespace contour::window
{

/// How far Settings may change a profile.
enum class ProfileEditability : uint8_t
{
    Editable = 0,    ///< A side file Settings owns.
    DefinedInConfig, ///< Defined in the hand-maintained contour.yml.
    BuiltIn,         ///< One Contour ships.
    Locked,          ///< `gui_config_locked` is set: nothing may change.
};

/// How many editabilities there are.
inline constexpr std::size_t ProfileEditabilityCount =
    static_cast<std::size_t>(ProfileEditability::Locked) + 1;

/// How far Settings may change a profile defined in @p origin while the configuration is @p lock.
[[nodiscard]] constexpr ProfileEditability profileEditability(config::SettingsOrigin origin,
                                                              config::SettingsEditability lock) noexcept
{
    if (lock == config::SettingsEditability::Locked)
        return ProfileEditability::Locked;
    switch (origin)
    {
        case config::SettingsOrigin::SideFile: return ProfileEditability::Editable;
        case config::SettingsOrigin::MainConfig: return ProfileEditability::DefinedInConfig;
        case config::SettingsOrigin::Builtin: return ProfileEditability::BuiltIn;
    }
    return ProfileEditability::BuiltIn;
}

/// Something the page can do to a profile.
enum class ProfileAction : uint8_t
{
    Edit = 0,
    Duplicate,
    Rename,
    Delete,
    SetDefault,
};

/// How many actions there are.
inline constexpr std::size_t ProfileActionCount = static_cast<std::size_t>(ProfileAction::SetDefault) + 1;

/// The name each action is published to QML under, in enumerator order.
inline constexpr auto ProfileActionTokens = std::array<std::string_view, ProfileActionCount> {
    "edit", "duplicate", "rename", "delete", "setDefault",
};

/// Whether an action is offered.
enum class ActionAvailability : uint8_t
{
    Offered = 0,
    Withheld,
};

/// What one editability allows, and the banner a profile page under it shows.
struct ProfileActionRule
{
    ProfileEditability editability = ProfileEditability::Editable; ///< The editability.
    std::array<ActionAvailability, ProfileActionCount> actions {}; ///< Per ProfileAction.
    std::string_view banner {};                                    ///< Empty for none.
};

/// One rule per ProfileEditability, in enumerator order; the actions in ProfileAction order (Edit,
/// Duplicate, Rename, Delete, SetDefault).
inline constexpr auto ProfileActionRules = std::array {
    ProfileActionRule { .editability = ProfileEditability::Editable,
                        .actions = { ActionAvailability::Offered,
                                     ActionAvailability::Offered,
                                     ActionAvailability::Offered,
                                     ActionAvailability::Offered,
                                     ActionAvailability::Offered },
                        .banner = {} },
    ProfileActionRule { .editability = ProfileEditability::DefinedInConfig,
                        .actions = { ActionAvailability::Withheld,
                                     ActionAvailability::Offered,
                                     ActionAvailability::Withheld,
                                     ActionAvailability::Withheld,
                                     ActionAvailability::Offered },
                        .banner = "Defined in contour.yml" },
    ProfileActionRule { .editability = ProfileEditability::BuiltIn,
                        .actions = { ActionAvailability::Withheld,
                                     ActionAvailability::Offered,
                                     ActionAvailability::Withheld,
                                     ActionAvailability::Withheld,
                                     ActionAvailability::Offered },
                        .banner = "Built in" },
    ProfileActionRule { .editability = ProfileEditability::Locked,
                        .actions = { ActionAvailability::Withheld,
                                     ActionAvailability::Withheld,
                                     ActionAvailability::Withheld,
                                     ActionAvailability::Withheld,
                                     ActionAvailability::Withheld },
                        .banner = "Settings are locked by gui_config_locked in contour.yml" },
};

static_assert(ProfileActionRules.size() == ProfileEditabilityCount, "one rule per ProfileEditability");
static_assert(std::ranges::all_of(std::views::iota(std::size_t { 0 }, ProfileEditabilityCount),
                                  [](std::size_t i) {
                                      return static_cast<std::size_t>(ProfileActionRules[i].editability) == i;
                                  }),
              "ProfileActionRules lists the editabilities in enumerator order");

/// The rule of @p editability.
[[nodiscard]] constexpr ProfileActionRule const& profileActionRule(ProfileEditability editability) noexcept
{
    return ProfileActionRules[static_cast<std::size_t>(editability)];
}

/// Whether @p rule offers @p action.
[[nodiscard]] constexpr bool offers(ProfileActionRule const& rule, ProfileAction action) noexcept
{
    return rule.actions[static_cast<std::size_t>(action)] == ActionAvailability::Offered;
}

/// The first of @p base, "@p base 2", "@p base 3", … that @p taken does not hold.
/// @param base The name to start from.
/// @param taken Whether a name is in use.
/// @return The first free name.
[[nodiscard]] inline std::string firstFreeName(std::string_view base,
                                               std::function<bool(std::string_view)> const& taken)
{
    if (!taken(base))
        return std::string { base };
    auto candidates =
        std::views::iota(2)
        | std::views::transform([base](int number) { return std::format("{} {}", base, number); })
        | std::views::filter([&taken](std::string const& name) { return !taken(name); });
    return *candidates.begin();
}

/// The name "Duplicate to Edit" gives a copy of @p base: "base copy", else "base copy 2", "base copy 3", ….
/// @param base The profile or scheme copied.
/// @param taken Whether a name is in use.
/// @return The first free name.
[[nodiscard]] inline std::string uniqueCopyName(std::string_view base,
                                                std::function<bool(std::string_view)> const& taken)
{
    return firstFreeName(std::format("{} copy", base), taken);
}

/// The name "+" gives a new profile: "New Profile", else "New Profile 2", ….
/// @param taken Whether a name is in use.
/// @return The first free name.
[[nodiscard]] inline std::string uniqueNewName(std::function<bool(std::string_view)> const& taken)
{
    constexpr auto NewProfileName = std::string_view { "New Profile" };
    return firstFreeName(NewProfileName, taken);
}

} // namespace contour::window
