// SPDX-License-Identifier: Apache-2.0
#include <contour/cli/DaemonStart.hpp>

#include <vtpty/PtyAdoption.hpp>

#include <format>

namespace contour::cli
{

std::expected<void, std::string> DaemonStartFailures::fail(std::string reason)
{
    switch (_continuity)
    {
        case vthost::StartContinuity::Fresh: return std::unexpected { std::move(reason) };
        case vthost::StartContinuity::HandedOver:
        case vthost::StartContinuity::Recovered: break;
    }
    _kept.push_back(std::move(reason));
    return {};
}

std::expected<config::Config, std::string> loadDaemonConfig(std::string_view configPath,
                                                            DaemonStartFailures& failures)
{
    auto loaded = config::tryLoadConfigNamed(configPath);
    if (loaded)
        return loaded;
    if (auto refused =
            failures.fail(std::format("Contour's configuration could not be loaded when the daemon took over "
                                      "its running sessions ({}); the daemon runs on its "
                                      "built-in configuration until the file loads again",
                                      loaded.error()));
        !refused)
        return std::unexpected { std::move(refused.error()) };
    auto fallback = config::Config {};
    fallback.configFile = config::configFileNamed(configPath);
    return fallback;
}

std::expected<DaemonProfile, std::string> resolveDaemonProfile(config::Config& hostConfig,
                                                               std::string const& profileName,
                                                               DaemonStartFailures& failures)
{
    auto resolved = config::resolvedSessionConfigFor(hostConfig, profileName);
    if (resolved)
        return DaemonProfile { .name = profileName, .resolved = *std::move(resolved) };
    if (auto refused = failures.fail(std::format(
            "Contour's configuration no longer resolves the daemon's profile ({}); its default profile "
            "serves new sessions instead",
            resolved.error()));
        !refused)
        return std::unexpected { std::move(refused.error()) };

    if (auto fallback = config::resolvedSessionConfigFor(hostConfig, std::string {}))
        return DaemonProfile { .name = std::string {}, .resolved = *std::move(fallback) };
    // Not even the configuration's default profile resolves: the built-in configuration's does, always.
    auto builtin = config::Config {};
    builtin.configFile = hostConfig.configFile;
    hostConfig = std::move(builtin);
    auto const defaulted = config::resolvedSessionConfigFor(hostConfig, std::string {});
    return DaemonProfile { .name = std::string {}, .resolved = defaulted.value() };
}

vthost::handover::ReadinessReport handoverReadiness(std::string release,
                                                    std::string_view configPath,
                                                    std::string const& profileName)
{
    auto report = vthost::handover::ownReadiness(
        std::move(release), vthost::handover::reportedAdoption(vtpty::pseudoconsoleAdoptability()));
    if (configPath.empty() && profileName.empty())
        return report;
    // What loadDaemonConfig() and resolveDaemonProfile() refuse a fresh daemon on, in the loader's own words:
    // nothing here falls back.
    auto problem = config::tryLoadConfigNamed(configPath).and_then([&](config::Config const& loaded) {
        return config::resolvedSessionConfigFor(loaded, profileName).transform([](auto const&) {});
    });
    if (!problem)
        report.configProblem = std::move(problem.error());
    return report;
}

} // namespace contour::cli
