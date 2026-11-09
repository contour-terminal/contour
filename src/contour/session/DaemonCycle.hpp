// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// Which daemon the CycleDaemon action moves a window to. A pure decision over the cached discovery, so the
/// ring's order and its wrap-around are table-tested without a daemon or a window.

#include <contour/cli/DaemonDiscovery.hpp>
#include <contour/config/Actions.hpp>
#include <contour/session/WindowSwitching.hpp>

#include <algorithm>
#include <iterator>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <vector>

namespace contour::session
{

/// The daemon to show after (or before) @p current, in the ring of local labels then hosts.
///
/// The ring is the local daemons by label, alphabetically, followed by the saved hosts some window shows,
/// alphabetically; stepping past either end wraps. A @p current that is not in the ring (a host no window
/// shows any more, or a daemon that stopped) steps to the first entry going Next and to the last going
/// Previous.
///
/// @param local The local daemons discovered.
/// @param hostsShownHere The saved hosts some window of this process shows.
/// @param current The daemon the window is on now: a Label or Host target (Same is not in the ring).
/// @param direction Which way to step.
/// @return The daemon target, with no workspace named; none when the ring holds nothing but @p current.
[[nodiscard]] inline std::optional<SwitchTarget> nextDaemon(std::span<cli::DiscoveredDaemon const> local,
                                                            std::span<std::string const> hostsShownHere,
                                                            SwitchTarget const& current,
                                                            actions::CycleDirection direction)
{
    auto ring = std::vector<SwitchTarget> {};
    ring.reserve(local.size() + hostsShownHere.size());

    auto labels = std::vector<std::string> {};
    std::ranges::transform(local, std::back_inserter(labels), &cli::DiscoveredDaemon::label);
    std::ranges::sort(labels);
    for (auto const& label: labels)
        ring.push_back({ .kind = vthost::proto::SwitchDaemonKind::Label, .daemon = label, .workspace = {} });

    auto hosts = std::vector<std::string>(hostsShownHere.begin(), hostsShownHere.end());
    std::ranges::sort(hosts);
    for (auto const& host: hosts)
        ring.push_back({ .kind = vthost::proto::SwitchDaemonKind::Host, .daemon = host, .workspace = {} });

    auto const isCurrent = [&](SwitchTarget const& entry) {
        return entry.kind == current.kind && entry.daemon == current.daemon;
    };
    auto const found = std::ranges::find_if(ring, isCurrent);

    auto const forward = direction == actions::CycleDirection::Next;
    auto position = size_t {};
    if (found == ring.end())
        position = forward ? 0 : ring.size() - 1; // underflows on an empty ring, which is checked below
    else
    {
        auto const at = static_cast<size_t>(std::distance(ring.begin(), found));
        position = forward ? (at + 1) % ring.size() : (at + ring.size() - 1) % ring.size();
    }

    if (ring.empty() || isCurrent(ring[position]))
        return std::nullopt;
    return ring[position];
}

} // namespace contour::session
