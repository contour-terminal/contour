// SPDX-License-Identifier: Apache-2.0
#include <contour/platform/PortalAppearanceSources.hpp>

#include <string>
#include <utility>
#include <vector>

namespace contour::platform
{

namespace
{
    /// @return The settings of @p reading, or none when the portal gave none.
    [[nodiscard]] PortalSettings settingsOf(PortalReading const& reading)
    {
        return reading.value_or(PortalSettings {});
    }
} // namespace

PortalReadingCache::PortalReadingCache(PortalSettingsChannelFactory const& channels,
                                       std::span<std::string_view const> namespaces,
                                       Updated onUpdate):
    _onUpdate { std::move(onUpdate) },
    _channel { channels(std::vector<std::string>(namespaces.begin(), namespaces.end()),
                        [this](PortalSetting change) {
                            // A change proves a portal answers, even after a reply that said none does.
                            update(mergePortalSetting(settingsOf(_reading), std::move(change)));
                        }) }
{
    // A reply that runs inside readAll is the starting reading, not an update: hold the owner's callback
    // back until readAll has returned.
    auto held = std::exchange(_onUpdate, nullptr);
    _channel->readAll([this](PortalReading reading) {
        // A change that arrived first proved a portal answers: a failed reply (a timeout, say) keeps the
        // settings it brought rather than replacing them with the failure.
        if (!reading && _reading && !_reading->empty())
            return;
        update(std::move(reading));
    });
    _onUpdate = std::move(held);
}

void PortalReadingCache::update(PortalReading next)
{
    auto before = std::exchange(_reading, std::move(next));
    if (_onUpdate)
        _onUpdate(before);
}

PortalPlatformAppearance::PortalPlatformAppearance(PortalSettingsChannelFactory const& channels,
                                                   DesktopLayoutInputs desktop,
                                                   PlatformAppearanceChanged onChange):
    _desktop { std::move(desktop) },
    _onChange { std::move(onChange) },
    _cache { channels, PortalAppearanceNamespaces, [this](PortalReading const& before) {
                if (auto const next = current(); next != appearanceFromPortal(before, _desktop) && _onChange)
                    _onChange(next);
            } }
{
}

PlatformAppearance PortalPlatformAppearance::current() const
{
    return appearanceFromPortal(_cache.reading(), _desktop);
}

PortalAccessibilityPreferenceSource::PortalAccessibilityPreferenceSource(
    PortalSettingsChannelFactory const& channels, AccessibilityPreferencesChanged onChange):
    _onChange { std::move(onChange) },
    _cache { channels, PortalAccessibilityNamespaces, [this](PortalReading const& before) {
                if (auto const next = current();
                    next != accessibilityFromPortal(settingsOf(before)) && _onChange)
                    _onChange(next);
            } }
{
}

config::AccessibilityPreferences PortalAccessibilityPreferenceSource::current() const
{
    return accessibilityFromPortal(settingsOf(_cache.reading()));
}

} // namespace contour::platform
