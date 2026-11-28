// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// Which daemons a window's sidebar lists, and where each one's link stands (spec §7, Which daemons are
/// listed). Pure, so every rule is a row of a table test: the presence service feeds it what discovery,
/// the configuration, the GUI state and the windows report, and lists what it returns.

#include <contour/remote/DaemonLinkHost.hpp>
#include <contour/remote/ObserveSupport.hpp>
#include <contour/remote/SwitchPlan.hpp>
#include <contour/window/DaemonDriftChoice.hpp>
#include <contour/window/DaemonLinkLook.hpp>
#include <contour/window/DaemonSection.hpp>

#include <cstdint>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include <vthost/Transport.hpp>
#include <vthost/client/ConnectionState.hpp>

namespace contour::remote
{

/// What the user and the configuration chose about listing daemons.
struct ListingSettings
{
    /// Every saved host by its name, with its `show_in_sidebar` setting, in the configuration's (name) order.
    std::vector<std::pair<std::string, window::SidebarListingChoice>> hosts;
    std::set<std::string> shownByUser;  ///< The @c DaemonRef values the user chose to show.
    std::set<std::string> hiddenByUser; ///< The @c DaemonRef values the user chose to hide.
};

/// Everything the listing is decided from.
struct PresenceInputs
{
    std::vector<std::string> runningLocal;  ///< The labels of the local daemons discovery finds serving now.
    std::set<std::string> seenLocalThisRun; ///< The labels of the local daemons listed earlier in this run.
    ListingSettings settings;               ///< What the user and the configuration chose.
    std::set<std::string> removedThisRun;   ///< The @c DaemonRef values the user removed for this run.
    std::vector<window::DaemonRef> windowsOn; ///< The daemons the windows of this process are on.
};

/// One daemon the sidebar lists.
struct ListedDaemon
{
    window::DaemonRef daemon;           ///< Which daemon.
    window::DaemonLocality locality {}; ///< Local for a `local:` daemon; Remote for a host or a link.

    bool operator==(ListedDaemon const&) const = default;
};

/// Decides which daemons the sidebar lists, in tree order.
///
/// Nothing is listed while no window is on a daemon. A daemon a window is on is always listed, a `link:`
/// one included. Otherwise a local daemon is listed while it runs or once it was listed this run, unless
/// the user hid or removed it; a saved host is listed when its configuration or the user's Show lists it,
/// unless the user hid it.
/// @param inputs What discovery, the configuration, the GUI state and the windows report.
/// @return The listed daemons, each once: `local:default`, the other local daemons by label, the saved
///         hosts by name, hosts no configuration names by ref, then links by id.
[[nodiscard]] std::vector<ListedDaemon> listedDaemons(PresenceInputs const& inputs);

/// What the user and the configuration chose for a daemon's listing, whether or not the sidebar can show it
/// now: the user's Hide wins, then the user's Show or the configuration's listing.
/// @param settings The listing settings.
/// @param daemon The daemon.
/// @param configured What the configuration alone chooses for it: Listed for a local daemon, a saved host's
///        `show_in_sidebar`, Unlisted for a host no configuration names.
/// @return Listed or Unlisted.
[[nodiscard]] window::SidebarListingChoice chosenListing(ListingSettings const& settings,
                                                         window::DaemonRef const& daemon,
                                                         window::SidebarListingChoice configured);

/// The daemons the sidebar's Hidden row names: the user hid each (ListingSettings::hiddenByUser), it is known
/// this run -- a configured saved host, or a local daemon running now or listed earlier this run -- and it
/// is not listed, as a window on it keeps it listed. A saved host the configuration does not list, with no
/// choice of the user's, is not hidden (the switcher's toggle offers it), nor is a daemon removed this run.
/// @param inputs What discovery, the configuration, the GUI state and the windows report.
/// @param listed What listedDaemons() lists for the same @p inputs.
/// @return Each once, in tree order (as listedDaemons() orders them), named by its label or host name; none
///         while no window is on a daemon.
[[nodiscard]] std::vector<window::HiddenDaemon> hiddenDaemons(PresenceInputs const& inputs,
                                                              std::vector<ListedDaemon> const& listed);

/// @param address The daemon a link reaches.
/// @param link    The link, for a daemon no switch can name.
/// @return `local:<name>` for a Name address, `host:<name>` for a Host, `link:<id>` for Same.
[[nodiscard]] window::DaemonRef daemonRefOf(DaemonAddress const& address, DaemonLinkId link);

/// Where a link stands, from its connection's state.
/// @param state       The connection's state.
/// @param lastFailure The last transport failure of the connection, if any.
/// @param observe     Whether the daemon can be observed.
/// @return TooOld whenever @p observe says the daemon is too old; for a Failed or Refused connection what
///         @p lastFailure says (@see linkStateOfFailure), else Failed; otherwise the state's own.
[[nodiscard]] window::DaemonLinkState linkStateOf(vthost::client::ConnectionState state,
                                                  std::optional<vthost::TransportErrorKind> lastFailure,
                                                  ObserveSupport observe) noexcept;

} // namespace contour::remote
