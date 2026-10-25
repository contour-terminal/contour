// SPDX-License-Identifier: Apache-2.0
#include <contour/window/NewDefaultsNotice.hpp>

#include <core/log/LogStore.hpp>

#include <utility>

namespace contour::window
{

NewDefaultsNotice::NewDefaultsNotice(std::function<config::Config const&()> currentConfig,
                                     std::shared_ptr<config::GuiConfigStore> settings,
                                     std::shared_ptr<config::GuiStateStore> state,
                                     std::function<std::expected<void, std::string>()> reload,
                                     QObject* parent):
    QObject(parent),
    _config { std::move(currentConfig) },
    _settings { std::move(settings) },
    _state { std::move(state) },
    _reload { std::move(reload) },
    _offer { config::newDefaultsOffer(_config().defaultsEditionSources) },
    _dismissed { dismissedIn(*_state, {}) }
{
}

bool NewDefaultsNotice::offered() const noexcept
{
    return config::isOfferShown(_offer, _dismissed);
}

std::expected<void, std::string> NewDefaultsNotice::switchToNewDefaults()
{
    // A copy of what settings.yml holds now, so the switch keeps every other setting the GUI recorded.
    auto settings = _config().guiManagedSettings;
    settings.globalOverrides[std::string { config::DefaultsEditionKey }] =
        std::string { config::configEnumToken(config::DefaultsEdition::Native2026) };
    return _settings->saveGuiSettings(settings).and_then([this] { return _reload(); });
}

void NewDefaultsNotice::useNew()
{
    if (auto const switched = switchToNewDefaults(); !switched)
        errorLog()("Could not switch to the new defaults: {}", switched.error());
}

void NewDefaultsNotice::keepMine()
{
    if (!offered())
        return;
    auto dismissed = dismissedIn(*_state, _dismissed);
    dismissed.push_back(_offer);
    if (auto const saved = _state->save(config::GuiState { .dismissedOffers = dismissed }); !saved)
        errorLog()("Could not remember declining the new defaults: {}", config::describe(saved.error()));
    // Hidden for this run either way: the user answered.
    apply(_offer, std::move(dismissed));
}

void NewDefaultsNotice::refresh()
{
    apply(config::newDefaultsOffer(_config().defaultsEditionSources), dismissedIn(*_state, _dismissed));
}

std::vector<config::NewDefaultsOffer> NewDefaultsNotice::dismissedIn(
    config::GuiStateStore const& store, std::vector<config::NewDefaultsOffer> fallback)
{
    auto loaded = store.load();
    if (loaded)
        return std::move(loaded->dismissedOffers);
    errorLog()("Could not read the GUI state: {}", config::describe(loaded.error()));
    return fallback;
}

void NewDefaultsNotice::apply(config::NewDefaultsOffer offer, std::vector<config::NewDefaultsOffer> dismissed)
{
    auto const wasOffered = offered();
    _offer = offer;
    _dismissed = std::move(dismissed);
    if (offered() != wasOffered)
        emit offeredChanged();
}

} // namespace contour::window
