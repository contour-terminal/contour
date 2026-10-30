// SPDX-License-Identifier: Apache-2.0
#include <contour/window/ProfileRules.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
#include <ranges>
#include <set>
#include <string>
#include <string_view>

using contour::config::SettingsEditability;
using contour::config::SettingsOrigin;
using contour::window::ActionAvailability;
using contour::window::ProfileAction;
using contour::window::ProfileActionCount;
using contour::window::profileActionRule;
using contour::window::ProfileActionRules;
using contour::window::ProfileEditability;
using contour::window::profileEditability;
using contour::window::ProfileEditabilityCount;
using contour::window::uniqueCopyName;
using contour::window::uniqueNewName;

namespace
{
[[nodiscard]] ActionAvailability availabilityOf(ProfileEditability editability, ProfileAction action)
{
    return profileActionRule(editability).actions.at(static_cast<std::size_t>(action));
}

constexpr auto AllOrigins =
    std::array { SettingsOrigin::Builtin, SettingsOrigin::MainConfig, SettingsOrigin::SideFile };
} // namespace

TEST_CASE("ProfileRules: the read-only profile rules", "[profilerules]")
{
    SECTION("a side file is editable and offers every action")
    {
        auto const editability = profileEditability(SettingsOrigin::SideFile, SettingsEditability::Editable);
        CHECK(editability == ProfileEditability::Editable);
        for (auto const index: std::views::iota(std::size_t { 0 }, ProfileActionCount))
            CHECK(availabilityOf(editability, static_cast<ProfileAction>(index))
                  == ActionAvailability::Offered);
        CHECK(profileActionRule(editability).banner.empty());
    }

    SECTION("a contour.yml profile offers Duplicate and Set as Default only")
    {
        auto const editability =
            profileEditability(SettingsOrigin::MainConfig, SettingsEditability::Editable);
        CHECK(editability == ProfileEditability::DefinedInConfig);
        CHECK(availabilityOf(editability, ProfileAction::Edit) == ActionAvailability::Withheld);
        CHECK(availabilityOf(editability, ProfileAction::Rename) == ActionAvailability::Withheld);
        CHECK(availabilityOf(editability, ProfileAction::Delete) == ActionAvailability::Withheld);
        CHECK(availabilityOf(editability, ProfileAction::Duplicate) == ActionAvailability::Offered);
        CHECK(availabilityOf(editability, ProfileAction::SetDefault) == ActionAvailability::Offered);
        CHECK(profileActionRule(editability).banner == "Defined in contour.yml");
    }

    SECTION("a built-in profile is read-only too")
    {
        auto const editability = profileEditability(SettingsOrigin::Builtin, SettingsEditability::Editable);
        CHECK(editability == ProfileEditability::BuiltIn);
        CHECK(availabilityOf(editability, ProfileAction::Edit) == ActionAvailability::Withheld);
        CHECK(availabilityOf(editability, ProfileAction::Duplicate) == ActionAvailability::Offered);
        CHECK(profileActionRule(editability).banner == "Built in");
    }

    SECTION("a locked configuration offers nothing, whatever the origin")
    {
        for (auto const origin: AllOrigins)
        {
            auto const editability = profileEditability(origin, SettingsEditability::Locked);
            CHECK(editability == ProfileEditability::Locked);
            for (auto const index: std::views::iota(std::size_t { 0 }, ProfileActionCount))
                CHECK(availabilityOf(editability, static_cast<ProfileAction>(index))
                      == ActionAvailability::Withheld);
        }
        CHECK(profileActionRule(ProfileEditability::Locked).banner
              == "Settings are locked by gui_config_locked in contour.yml");
    }

    SECTION("one rule per editability, in enumerator order")
    {
        REQUIRE(ProfileActionRules.size() == ProfileEditabilityCount);
        for (auto const index: std::views::iota(std::size_t { 0 }, ProfileEditabilityCount))
            CHECK(ProfileActionRules.at(index).editability == static_cast<ProfileEditability>(index));
    }
}

TEST_CASE("ProfileRules: a copy and a new profile take the first free name", "[profilerules]")
{
    auto const takenOf = [](std::set<std::string> names) {
        return [names = std::move(names)](std::string_view name) {
            return names.contains(std::string { name });
        };
    };
    CHECK(uniqueCopyName("main", takenOf({})) == "main copy");
    CHECK(uniqueCopyName("main", takenOf({ "main copy" })) == "main copy 2");
    CHECK(uniqueCopyName("main", takenOf({ "main copy", "main copy 2" })) == "main copy 3");
    CHECK(uniqueNewName(takenOf({})) == "New Profile");
    CHECK(uniqueNewName(takenOf({ "New Profile" })) == "New Profile 2");
}
