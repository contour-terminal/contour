// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// What the daemon-drift chip offers the user: a decision made from facts only, so it is testable without a
/// window, a daemon or a connection.

#include <cstdint>

#include <vthost/client/ReleaseDrift.hpp>
#include <vthost/proto/Negotiation.hpp>

namespace contour::window
{

/// Whether the daemon runs on this machine, so that this client may replace its binary.
enum class DaemonLocality : uint8_t
{
    Remote = 0, ///< Another machine's daemon: not this client's to restart or upgrade.
    Local,      ///< This machine's own daemon.
};

/// What the drift chip offers.
enum class DriftOffer : uint8_t
{
    None = 0,          ///< Nothing: the releases match, or the daemon is not ours to touch.
    RestartOnly,       ///< Restart the daemon, which ends the programs it hosts.
    UpgradeAndRestart, ///< Upgrade the daemon in place first, keeping every program; restart as the fallback.
};

/// @param drift How the daemon's release stands against the client's.
/// @param locality Where the daemon runs.
/// @param daemonCapabilities What the daemon and this client both offered at the handshake.
/// @return What to offer. A live upgrade is offered only for a local daemon that announced
///         vthost::proto::Capability::LiveUpgrade; an older one is offered a restart, and a remote one
///         nothing.
[[nodiscard]] inline DriftOffer driftOfferFor(vthost::client::DriftKind drift,
                                              DaemonLocality locality,
                                              vthost::proto::CapabilitySet const& daemonCapabilities)
{
    if (drift == vthost::client::DriftKind::None || locality == DaemonLocality::Remote)
        return DriftOffer::None;
    // A newer daemon would be replaced by this older release: that is a downgrade, which only the restart,
    // with its confirmation, is for.
    auto const upgrades = drift != vthost::client::DriftKind::DaemonNewer
                          && daemonCapabilities.contains(vthost::proto::Capability::LiveUpgrade);
    return upgrades ? DriftOffer::UpgradeAndRestart : DriftOffer::RestartOnly;
}

} // namespace contour::window
