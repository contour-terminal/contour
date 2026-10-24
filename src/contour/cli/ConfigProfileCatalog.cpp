// SPDX-License-Identifier: Apache-2.0
#include <contour/cli/ConfigProfileCatalog.hpp>

#include <utility>

namespace contour::cli
{

ConfigProfileCatalog::ConfigProfileCatalog(config::Config config,
                                           std::string defaultProfile,
                                           vtbackend::ColorPreference preference):
    _config { std::move(config) },
    _defaultProfile { defaultProfile.empty() ? _config.defaultProfileName.value()
                                             : std::move(defaultProfile) },
    _preference { preference }
{
}

vthost::ResolvedProfile ConfigProfileCatalog::resolve(std::string_view name) const
{
    auto const asked = std::string { name };
    auto const* named = asked.empty() ? nullptr : _config.findProfile(asked);
    auto const match = named != nullptr ? vthost::ProfileMatch::Named : vthost::ProfileMatch::Default;
    auto const& resolvedName = named != nullptr ? asked : _defaultProfile;
    // The daemon refuses to start under a default profile its configuration lacks, and a reload that lost
    // it still runs the built-in defaults rather than refusing a session -- those of the configuration's own
    // edition, so a Classic configuration does not fall back to the Native2026 look.
    auto const builtIn = config::TerminalProfile { .defaultsEdition = _config.defaultsEdition };
    auto const* profile = named != nullptr ? named : _config.findProfile(_defaultProfile);
    if (profile == nullptr)
        profile = &builtIn;

    auto session = config::resolvedSessionConfig(_config, *profile, resolvedName, _preference);
    // The host's scrollback, when it states one, wins over the profile's: the daemon keeps the lines.
    if (auto const& limit = _config.daemon.value().historyLimit)
        session.settings.historyLimits = vtbackend::HistoryLimits::plain(*limit);
    return vthost::ResolvedProfile { .name = resolvedName,
                                     .settings = std::move(session.settings),
                                     .shell = std::move(session.shell),
                                     .match = match,
                                     .sandbox = session.escapeSandbox ? vthost::SandboxEscape::Escapes
                                                                      : vthost::SandboxEscape::Contained };
}

} // namespace contour::cli
