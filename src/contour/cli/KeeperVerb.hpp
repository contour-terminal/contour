// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// `contour daemon` behind a PTY keeper: the keeper's half (`contour daemon` itself, which keeps the daemon)
/// and the daemon's half (the hidden `contour daemon keeper-serve`, which the keeper starts). POSIX only.

#include <contour/cli/DaemonVerbOptions.hpp>
#include <contour/config/Config.hpp>

#include <expected>
#include <filesystem>
#include <iosfwd>
#include <memory>
#include <string>
#include <vector>

#include <vthost/KeeperSettings.hpp>
#include <vthost/keeper/Messages.hpp>
#include <vthost/keeper/Ports.hpp>
#include <vthost/keeper/RestartPolicy.hpp>
#include <vthost/keeperlink/KeeperAttachment.hpp>

namespace contour::cli
{

/// The daemon's command line after its image, as its keeper starts it. Frozen: what the daemon needs travels
/// in the Launch record, so the keeper's call into the daemon never changes (`--keeper-channel=3` is
/// keeper::DaemonChannelDescriptor).
[[nodiscard]] std::vector<std::string> keeperServeArguments();

/// @return This process's environment, each variable as `NAME=value`: what the keeper hands the daemon and
///         every shell it starts.
[[nodiscard]] std::vector<std::string> processEnvironmentEntries();

/// What `contour daemon` keeps a daemon with.
struct KeeperVerbInputs
{
    std::filesystem::path daemonImage;    ///< The image the daemon is started, and restarted, from.
    DaemonLaunch launch;                  ///< What it launches: the options record the keeper carries.
    std::vector<std::string> environment; ///< The daemon's environment, `NAME=value`.
    /// The crash-loop numbers it keeps the daemon by (`daemon.keeper`, DaemonKeeperConfig::restartPolicy).
    vthost::keeper::RestartPolicyConfig policy {};
    /// What a crashed daemon is restarted from, and the lease on it; null for daemonImage, always.
    std::unique_ptr<vthost::keeper::RestartImages> restartImages = nullptr;
};

/// Keeps a daemon: the keeper's half of `contour daemon`. Blocks until the keeper ends.
/// @param inputs What to keep.
/// @param errors Where a keeper that does not start says why.
/// @return The status the process exits with: the keeper's (@see vthost::keeper::Keeper::exitStatus), or
///         EXIT_FAILURE when it did not start, another keeper holding the label among the reasons.
[[nodiscard]] int runKeeperVerb(KeeperVerbInputs inputs, std::ostream& errors);

/// @param launch The launch the keeper's record holds.
/// @param continuity How this daemon process starts.
/// @return What it launches with: a daemon restarted after a crash repeats nothing meant for one process
///         (DaemonLaunch::relaunched()); any other launches as given.
[[nodiscard]] DaemonLaunch launchFor(DaemonLaunch const& launch, vthost::keeper::LaunchContinuity continuity);

/// What the hidden `keeper-serve` verb starts the daemon with.
struct KeeperServeStart
{
    DaemonLaunch launch;                             ///< From the Launch record, for this continuity.
    vthost::keeperlink::KeeperAttachment attachment; ///< The keeper's channel, past the handshake.
};

/// The daemon's half of `contour daemon`: the handshake with the keeper on @p channel, and the launch it
/// hands over.
/// @param channel The descriptor `--keeper-channel` names.
/// @return What the daemon starts with; or why it cannot.
[[nodiscard]] std::expected<KeeperServeStart, std::string> attachKeeperServe(int channel);

/// @param config The configuration's `daemon.keeper`.
/// @return What the daemon tells its keeper, the Config boundary bool converted to keeper::HungRestart.
[[nodiscard]] vthost::KeeperSettings keeperSettingsOf(config::DaemonKeeperConfig const& config);

} // namespace contour::cli
