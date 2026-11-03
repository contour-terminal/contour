// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// What `contour daemon` does about what it cannot start as configured: a daemon that starts afresh refuses
/// to start, and one that carries on from a live upgrade serves on without it, since ending would hang up
/// every program the upgrade handed it.

#include <contour/config/Config.hpp>

#include <cstdint>
#include <expected>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <vthost/StartContinuity.hpp>
#include <vthost/handover/Readiness.hpp>

namespace contour::cli
{

/// Collects what a daemon could not start as configured, and decides what each costs: a fresh daemon refuses
/// to start, which tells the user at once and loses nothing; a handed-over one serves on without it, and
/// keeps the reason for the first client to be told (vthost::DaemonConfig::degradations).
class DaemonStartFailures
{
  public:
    /// @param continuity Whether the daemon carries on from a handover.
    explicit DaemonStartFailures(vthost::StartContinuity continuity) noexcept: _continuity { continuity } {}

    /// Reports that something could not be started as configured.
    /// @param reason What, and what the daemon does instead, for a person.
    /// @return @p reason as the refusal, for a fresh daemon; nothing for a handed-over one, which keeps it
    ///         and goes on.
    [[nodiscard]] std::expected<void, std::string> fail(std::string reason);

    /// @return Whether the daemon carries on from a handover.
    [[nodiscard]] vthost::StartContinuity continuity() const noexcept { return _continuity; }

    /// Hands over every reason fail() kept, oldest first; empty for a fresh daemon.
    [[nodiscard]] std::vector<std::string> take() { return std::exchange(_kept, {}); }

  private:
    vthost::StartContinuity _continuity;
    std::vector<std::string> _kept;
};

/// Loads the daemon's configuration. A file that does not load refuses a fresh daemon; a handed-over one runs
/// on the built-in configuration instead, watching the same file, so that the configuration reload applies it
/// as soon as it loads again.
/// @param configPath The file the command line names; empty for the default one.
/// @param failures Where a failure is reported.
/// @return The configuration; or why a fresh daemon refuses to start.
[[nodiscard]] std::expected<config::Config, std::string> loadDaemonConfig(std::string_view configPath,
                                                                          DaemonStartFailures& failures);

/// The daemon's own profile, resolved.
struct DaemonProfile
{
    std::string name;                       ///< The profile it runs: the one asked for, or the default.
    config::ResolvedSessionConfig resolved; ///< What it resolves to.
};

/// Resolves the profile the daemon's own sessions run. A name the configuration no longer has refuses a fresh
/// daemon; a handed-over one runs the configuration's default profile instead, else the built-in one's.
/// @param hostConfig The loaded configuration; replaced by the built-in one when even its default profile
///        does not resolve, keeping its file.
/// @param profileName The profile the command line names; empty for the default.
/// @param failures Where a failure is reported.
/// @return The profile; or why a fresh daemon refuses to start.
[[nodiscard]] std::expected<DaemonProfile, std::string> resolveDaemonProfile(config::Config& hostConfig,
                                                                             std::string const& profileName,
                                                                             DaemonStartFailures& failures);

/// What `contour daemon handover-info` answers a daemon that asks whether this binary can take its handover
/// over: the formats it reads, and whether it can run as the daemon's configuration says -- the file loads,
/// and names the profile -- loaded as a fresh daemon would, so that a configuration the new release rejects
/// is refused while the old daemon still serves.
/// @param release This binary's release.
/// @param configPath The configuration file the daemon runs on; with @p profileName empty too, nothing is
///        loaded.
/// @param profileName The profile the daemon's sessions run; empty for the default.
/// @return The readiness report, ReadinessReport::configProblem saying what does not load or resolve.
[[nodiscard]] vthost::handover::ReadinessReport handoverReadiness(std::string release,
                                                                  std::string_view configPath,
                                                                  std::string const& profileName);

} // namespace contour::cli
