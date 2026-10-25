// SPDX-License-Identifier: Apache-2.0
#include <contour/config/NewDefaultsOffer.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <optional>

using contour::config::DefaultsEdition;
using contour::config::DefaultsEditionSources;
using contour::config::effectiveEdition;
using contour::config::isOfferShown;
using contour::config::NewDefaultsOffer;
using contour::config::newDefaultsOffer;

namespace
{
/// One line of the decision table: what each document states, the offer, and the edition that applies.
struct OfferCase
{
    DefaultsEditionSources sources {};
    NewDefaultsOffer offer = NewDefaultsOffer::None;
    DefaultsEdition edition = DefaultsEdition::Classic;
};

constexpr auto Classic = DefaultsEdition::Classic;
constexpr auto Native = DefaultsEdition::Native2026;
constexpr auto Unstated = std::optional<DefaultsEdition> {};

constexpr auto OfferCases = std::array {
    // Neither states one: a configuration from before editions.
    OfferCase { .sources = { .mainFile = Unstated, .settingsFile = Unstated },
                .offer = NewDefaultsOffer::ExistingConfig,
                .edition = Classic },
    // contour.yml states the newest, and nothing or the newest over it.
    OfferCase { .sources = { .mainFile = Native, .settingsFile = Unstated },
                .offer = NewDefaultsOffer::None,
                .edition = Native },
    OfferCase { .sources = { .mainFile = Native, .settingsFile = Native },
                .offer = NewDefaultsOffer::None,
                .edition = Native },
    // A reset contour.yml under a settings.yml that keeps classic: the Reset Config case.
    OfferCase { .sources = { .mainFile = Native, .settingsFile = Classic },
                .offer = NewDefaultsOffer::SettingsOverride,
                .edition = Classic },
    // An explicit classic is a choice (R20).
    OfferCase { .sources = { .mainFile = Classic, .settingsFile = Unstated },
                .offer = NewDefaultsOffer::None,
                .edition = Classic },
    // settings.yml stating the newest: nothing to offer, whatever contour.yml says.
    OfferCase { .sources = { .mainFile = Unstated, .settingsFile = Native },
                .offer = NewDefaultsOffer::None,
                .edition = Native },
    OfferCase { .sources = { .mainFile = Classic, .settingsFile = Native },
                .offer = NewDefaultsOffer::None,
                .edition = Native },
    // settings.yml stating classic over nothing or classic: a choice.
    OfferCase { .sources = { .mainFile = Unstated, .settingsFile = Classic },
                .offer = NewDefaultsOffer::None,
                .edition = Classic },
    OfferCase { .sources = { .mainFile = Classic, .settingsFile = Classic },
                .offer = NewDefaultsOffer::None,
                .edition = Classic },
};
} // namespace

TEST_CASE("NewDefaultsOffer: the decision table", "[config][newdefaults]")
{
    for (auto const& [sources, offer, edition]: OfferCases)
    {
        INFO("contour.yml " << (sources.mainFile ? static_cast<int>(*sources.mainFile) : -1)
                            << ", settings.yml "
                            << (sources.settingsFile ? static_cast<int>(*sources.settingsFile) : -1));
        CHECK(newDefaultsOffer(sources) == offer);
        CHECK(effectiveEdition(sources) == edition);
    }
}

TEST_CASE("NewDefaultsOffer: a declined offer is hidden, another one is not", "[config][newdefaults]")
{
    auto const declinedExisting = std::array { NewDefaultsOffer::ExistingConfig };
    auto const declinedOverride = std::array { NewDefaultsOffer::SettingsOverride };
    CHECK(isOfferShown(NewDefaultsOffer::SettingsOverride, declinedExisting));
    CHECK_FALSE(isOfferShown(NewDefaultsOffer::SettingsOverride, declinedOverride));
    CHECK_FALSE(isOfferShown(NewDefaultsOffer::None, {}));
    CHECK(isOfferShown(NewDefaultsOffer::ExistingConfig, {}));
}

TEST_CASE("NewDefaultsOffer: the tokens", "[config][newdefaults]")
{
    using contour::config::configEnumFromToken;
    using contour::config::configEnumToken;
    CHECK(configEnumToken(NewDefaultsOffer::ExistingConfig) == "existing_config");
    CHECK(configEnumToken(NewDefaultsOffer::SettingsOverride) == "settings_override");
    CHECK(configEnumToken(NewDefaultsOffer::None).empty());
    CHECK(configEnumFromToken<NewDefaultsOffer>("settings_override") == NewDefaultsOffer::SettingsOverride);
}
