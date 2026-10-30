// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// The seam between the portal-backed sources and the bus: a channel asks the XDG Settings portal for
/// its values and reports its changes. Qt-free, so the sources and their tests never see D-Bus; the
/// Linux adapter is DBusPortalSettingsChannel.

#include <contour/platform/PortalSettings.hpp>

#include <expected>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace contour::platform
{

/// Receives the answer to IPortalSettingsChannel::readAll.
using PortalSettingsReply = std::function<void(std::expected<PortalSettings, PortalSettingsError>)>;

/// Receives one changed setting, as the portal's SettingChanged signal reports it.
using PortalSettingChanged = std::function<void(PortalSetting)>;

/// Talks to the XDG Settings portal for the namespaces it was built for.
class IPortalSettingsChannel
{
  public:
    virtual ~IPortalSettingsChannel() = default;

    /// Asks for every setting of the channel's namespaces, and returns without waiting for the answer.
    ///
    /// The reply runs at most once, on the GUI thread, and never after the channel is destroyed. It may
    /// run before this returns (the null channel's does), so a caller must be ready for it. Changes
    /// arrive through the PortalSettingChanged the channel was built with, under the same rules.
    /// @param onReply Called with the settings, or with why there are none.
    virtual void readAll(PortalSettingsReply onReply) = 0;
};

/// The channel where there is no portal to talk to: it answers NoPortal at once and never reports a
/// change.
class NullPortalSettingsChannel final: public IPortalSettingsChannel
{
  public:
    /// Replies NoPortal before returning.
    /// @param onReply Called once, with PortalSettingsError::NoPortal.
    void readAll(PortalSettingsReply onReply) override
    {
        onReply(std::unexpected(PortalSettingsError::NoPortal));
    }
};

/// Builds a channel for @p namespaces that reports changes to @p onChange.
using PortalSettingsChannelFactory = std::function<std::unique_ptr<IPortalSettingsChannel>(
    std::vector<std::string> namespaces, PortalSettingChanged onChange)>;

/// @return A factory building NullPortalSettingsChannels.
[[nodiscard]] inline PortalSettingsChannelFactory nullPortalSettingsChannels()
{
    return [](std::vector<std::string> const& /*namespaces*/,
              PortalSettingChanged const& /*onChange*/) -> std::unique_ptr<IPortalSettingsChannel> {
        return std::make_unique<NullPortalSettingsChannel>();
    };
}

} // namespace contour::platform
