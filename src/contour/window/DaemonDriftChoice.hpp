// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// What the daemon-drift chip offers the user: a decision made from facts only, so it is testable without a
/// window, a daemon or a connection.

#include <array>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <ranges>
#include <string_view>
#include <utility>

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

/// Whether this client restarts and upgrades a daemon from its drift badge: a local daemon is not enough, for
/// a restart relaunches this process with its own arguments, which come back on the daemon it launched on.
enum class DaemonUpkeep : uint8_t
{
    None = 0, ///< Not this client's to restart or upgrade: a remote daemon, or a local one a switch reached.
    ThisClient, ///< The daemon this client launched on (and started, when none served): local by definition.
};

/// @param locality Where the daemon runs.
/// @param upkeep Whether it is the daemon this client launched on.
/// @return Whether the daemon is this client's to restart or upgrade: a local one it launched on.
[[nodiscard]] constexpr bool isThisClientsDaemon(DaemonLocality locality, DaemonUpkeep upkeep) noexcept
{
    return locality == DaemonLocality::Local && upkeep == DaemonUpkeep::ThisClient;
}

/// @param drift How the daemon's release stands against the client's.
/// @return Whether upgrading would replace a newer daemon with this older release: a downgrade, which only
///         the restart, with its confirmation, is for.
[[nodiscard]] constexpr bool wouldDowngrade(vthost::client::DriftKind drift) noexcept
{
    return drift == vthost::client::DriftKind::DaemonNewer;
}

/// @param daemonCapabilities What the daemon and this client both offered at the handshake.
/// @return Whether the daemon announced vthost::proto::Capability::LiveUpgrade.
[[nodiscard]] inline bool canLiveUpgrade(vthost::proto::CapabilitySet const& daemonCapabilities)
{
    return daemonCapabilities.contains(vthost::proto::Capability::LiveUpgrade);
}

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
    auto const upgrades = !wouldDowngrade(drift) && canLiveUpgrade(daemonCapabilities);
    return upgrades ? DriftOffer::UpgradeAndRestart : DriftOffer::RestartOnly;
}

/// What a daemon connection is known to be at the time an upgrade is asked for by command.
struct ConnectedDaemon
{
    vthost::client::DriftKind drift;                 ///< How its release stands against the client's.
    vthost::proto::CapabilitySet daemonCapabilities; ///< What the daemon and this client both offered.
};

/// Whether an upgrade asked for by command (the `UpgradeDaemon` action) goes ahead, and if not, why not.
/// Unlike DriftOffer it needs no drift: the same release upgrades too, to pick up a rebuilt binary.
enum class UpgradeVerdict : uint8_t
{
    Upgrade = 0,    ///< Nothing refuses it.
    NotThisClients, ///< A remote daemon, or a local one this client did not launch on.
    NotConnected,   ///< The window's connection to the daemon is down.
    WouldDowngrade, ///< The daemon runs a newer release than this client.
    NoLiveUpgrade,  ///< The daemon cannot upgrade in place (it lacks vthost::proto::Capability::LiveUpgrade).
};

/// One verdict and what the user is told when it refuses.
struct UpgradeVerdictRow
{
    UpgradeVerdict verdict;
    std::string_view reason; ///< Empty for UpgradeVerdict::Upgrade.
};

/// One row per verdict, in enumerator order.
inline constexpr auto UpgradeVerdictRows = std::array {
    UpgradeVerdictRow { .verdict = UpgradeVerdict::Upgrade, .reason = "" },
    UpgradeVerdictRow { .verdict = UpgradeVerdict::NotThisClients,
                        .reason = "this window's daemon is not the one this client launched on" },
    UpgradeVerdictRow { .verdict = UpgradeVerdict::NotConnected,
                        .reason = "this window is not connected to its daemon" },
    UpgradeVerdictRow { .verdict = UpgradeVerdict::WouldDowngrade,
                        .reason = "the daemon runs a newer release than this client" },
    UpgradeVerdictRow { .verdict = UpgradeVerdict::NoLiveUpgrade,
                        .reason = "the daemon is too old to upgrade in place" },
};

static_assert(UpgradeVerdictRows.size() == std::to_underlying(UpgradeVerdict::NoLiveUpgrade) + 1U,
              "UpgradeVerdictRows must have a row for every verdict");
static_assert(
    [] {
        for (auto const index: std::views::iota(std::size_t { 0 }, UpgradeVerdictRows.size()))
            if (std::to_underlying(UpgradeVerdictRows[index].verdict) != index)
                return false;
        return true;
    }(),
    "UpgradeVerdictRows must list every verdict in enumerator order");

/// What a commanded upgrade is refused with while a restart or an upgrade of the daemon already runs: not
/// a verdict, as it depends on what runs now rather than on whose daemon it is.
inline constexpr std::string_view UpgradeAlreadyUnderWay =
    "the daemon is already being upgraded or restarted";

/// @param verdict A verdict.
/// @return Why it refuses, in words for the user; empty for UpgradeVerdict::Upgrade.
[[nodiscard]] constexpr std::string_view describe(UpgradeVerdict verdict) noexcept
{
    return UpgradeVerdictRows[std::to_underlying(verdict)].reason;
}

/// @param locality Where the daemon runs.
/// @param upkeep Whether it is the daemon this client launched on.
/// @param daemon What the connection knows of the daemon; none while it is down.
/// @return Whether to upgrade. The refusals are checked in the order of UpgradeVerdict: whose daemon it is
///         comes before anything the connection says of it.
[[nodiscard]] inline UpgradeVerdict upgradeVerdictFor(DaemonLocality locality,
                                                      DaemonUpkeep upkeep,
                                                      std::optional<ConnectedDaemon> const& daemon)
{
    if (!isThisClientsDaemon(locality, upkeep))
        return UpgradeVerdict::NotThisClients;
    if (!daemon)
        return UpgradeVerdict::NotConnected;
    if (wouldDowngrade(daemon->drift))
        return UpgradeVerdict::WouldDowngrade;
    if (!canLiveUpgrade(daemon->daemonCapabilities))
        return UpgradeVerdict::NoLiveUpgrade;
    return UpgradeVerdict::Upgrade;
}

/// @param launchControlPath The control socket of the daemon the application launched on (--socket and
///        $CONTOUR_MUX honoured); empty when the launch attached to none it may upkeep.
/// @param reachedControlPath The control socket of a daemon reached some other way, by its label.
/// @return ThisClient when both name the same socket -- the only daemon the application's restart and
///         upgrade act on -- else None.
[[nodiscard]] inline DaemonUpkeep upkeepOfReachedDaemon(std::filesystem::path const& launchControlPath,
                                                        std::filesystem::path const& reachedControlPath)
{
    if (launchControlPath.empty()
        || launchControlPath.lexically_normal() != reachedControlPath.lexically_normal())
        return DaemonUpkeep::None;
    return DaemonUpkeep::ThisClient;
}

} // namespace contour::window
