// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// The daemon's profile catalog over its own configuration: a profile a client names is resolved here, on
/// the host, so what runs is what the host's configuration says.

#include <contour/config/Config.hpp>

#include <vtbackend/core/ColorPalette.hpp>

#include <string>
#include <string_view>

#include <vthost/ProfileCatalog.hpp>

namespace contour::cli
{

/// Resolves profile names in a loaded configuration, and applies `daemon.history_limit` on top.
class ConfigProfileCatalog final: public vthost::ProfileCatalog
{
  public:
    /// @param config The host's configuration (owned: a reload builds a new catalog).
    /// @param defaultProfile The profile an empty or unknown name resolves to — the daemon's `--profile`;
    ///        empty for the configuration's `default_profile`.
    /// @param preference Which half of a dual color configuration the sessions get.
    ConfigProfileCatalog(config::Config config,
                         std::string defaultProfile,
                         vtbackend::ColorPreference preference);

    /// @param name The profile a client named.
    /// @return That profile when the configuration has it, else the default one with ProfileMatch::Default.
    [[nodiscard]] vthost::ResolvedProfile resolve(std::string_view name) const override;

  private:
    config::Config _config;
    std::string _defaultProfile;
    vtbackend::ColorPreference _preference;
};

} // namespace contour::cli
