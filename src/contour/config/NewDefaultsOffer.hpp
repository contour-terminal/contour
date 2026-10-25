// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <contour/config/ConfigEnumInfo.hpp>
#include <contour/config/DefaultsEdition.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <optional>
#include <span>

namespace contour::config
{

/// Why the GUI offers the newest defaults, or that it does not.
///
/// The offer is made once per reason: a user who keeps their configuration is not asked again for the
/// same reason, yet a later Reset Config, which raises the other reason, is offered afresh.
enum class NewDefaultsOffer : uint8_t
{
    None = 0,         //!< Nothing to offer: the newest defaults apply, or the user chose the classic ones.
    ExistingConfig,   //!< A configuration from before editions existed, which states none.
    SettingsOverride, //!< A reset `contour.yml` states the newest edition, but `settings.yml` keeps classic.
};

namespace detail
{
    /// The offer spellings, as the GUI state file records a dismissed offer. None has no token: it is
    /// never dismissed. Once shipped, a token is fixed.
    inline constexpr auto NewDefaultsOfferTable = std::array {
        ConfigEnumInfo<NewDefaultsOffer> {
            NewDefaultsOffer::ExistingConfig, "existing_config", "Existing configuration" },
        ConfigEnumInfo<NewDefaultsOffer> {
            NewDefaultsOffer::SettingsOverride, "settings_override", "Settings override" },
    };

    /// One combination of statements that raises an offer.
    struct NewDefaultsOfferRule
    {
        std::optional<DefaultsEdition> mainFile {};      ///< What `contour.yml` states.
        std::optional<DefaultsEdition> settingsFile {};  ///< What `settings.yml` states.
        NewDefaultsOffer offer = NewDefaultsOffer::None; ///< The offer it raises.
    };

    /// The combinations that raise an offer; every other combination raises none. An explicit
    /// `classic`, in either file and with nothing newer over it, is a choice and never offered against.
    inline constexpr auto NewDefaultsOfferRules = std::array {
        NewDefaultsOfferRule { .mainFile = std::nullopt,
                               .settingsFile = std::nullopt,
                               .offer = NewDefaultsOffer::ExistingConfig },
        NewDefaultsOfferRule { .mainFile = DefaultsEdition::Native2026,
                               .settingsFile = DefaultsEdition::Classic,
                               .offer = NewDefaultsOffer::SettingsOverride },
    };
} // namespace detail

template <>
constexpr std::span<ConfigEnumInfo<NewDefaultsOffer> const> configEnumValues() noexcept
{
    return detail::NewDefaultsOfferTable;
}

/// The offer the documents' edition statements raise.
/// @param sources What `contour.yml` and `settings.yml` each state.
/// @return The matching rule's offer, or NewDefaultsOffer::None.
[[nodiscard]] constexpr NewDefaultsOffer newDefaultsOffer(DefaultsEditionSources sources) noexcept
{
    auto const* const rule = std::ranges::find_if(
        detail::NewDefaultsOfferRules, [&](detail::NewDefaultsOfferRule const& candidate) {
            return candidate.mainFile == sources.mainFile && candidate.settingsFile == sources.settingsFile;
        });
    return rule != detail::NewDefaultsOfferRules.end() ? rule->offer : NewDefaultsOffer::None;
}

/// Whether @p offer is to be shown.
/// @param offer The offer the configuration raises.
/// @param dismissed The offers the user has already declined.
/// @return True for an offer other than None that the user has not declined.
[[nodiscard]] constexpr bool isOfferShown(NewDefaultsOffer offer,
                                          std::span<NewDefaultsOffer const> dismissed) noexcept
{
    return offer != NewDefaultsOffer::None && !std::ranges::contains(dismissed, offer);
}

} // namespace contour::config
