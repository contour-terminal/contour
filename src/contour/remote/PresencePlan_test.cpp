// SPDX-License-Identifier: Apache-2.0
#include <contour/remote/NativeController.hpp>
#include <contour/remote/PresencePlan.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <optional>
#include <string>
#include <vector>

using contour::remote::DaemonAddress;
using contour::remote::DaemonLinkId;
using contour::remote::ListedDaemon;
using contour::remote::ObserveSupport;
using contour::remote::PresenceInputs;
using contour::window::DaemonLinkState;
using contour::window::DaemonLocality;
using contour::window::DaemonRef;
using contour::window::hostDaemonRef;
using contour::window::linkDaemonRef;
using contour::window::localDaemonRef;
using contour::window::SidebarListingChoice;
using vthost::TransportErrorKind;
using vthost::client::ConnectionState;

namespace
{

/// @return The refs of the listed daemons, in order.
[[nodiscard]] std::vector<std::string> refsOf(std::vector<ListedDaemon> const& listed)
{
    auto refs = std::vector<std::string> {};
    for (auto const& daemon: listed)
        refs.push_back(daemon.daemon.value);
    return refs;
}

/// @return Inputs with a window on the local default daemon, which is running.
[[nodiscard]] PresenceInputs onDefault()
{
    auto inputs = PresenceInputs {};
    inputs.runningLocal = { "default" };
    inputs.windowsOn = { localDaemonRef("default") };
    return inputs;
}

using Refs = std::vector<std::string>;

} // namespace

TEST_CASE("nothing is listed while no window is on a daemon", "[presence]")
{
    auto inputs = PresenceInputs {};
    inputs.runningLocal = { "default", "work" };
    inputs.seenLocalThisRun = { "old" };
    inputs.settings.hosts = { { "starfive", SidebarListingChoice::Listed } };
    CHECK(listedDaemons(inputs).empty());
}

TEST_CASE("running local daemons are listed, default first, then alphabetically", "[presence]")
{
    auto inputs = onDefault();
    inputs.runningLocal = { "zeta", "alpha", "default", "beta" };
    auto const listed = listedDaemons(inputs);
    CHECK(refsOf(listed) == Refs { "local:default", "local:alpha", "local:beta", "local:zeta" });
    for (auto const& daemon: listed)
        CHECK(daemon.locality == DaemonLocality::Local);
}

TEST_CASE("a listed local daemon that stops stays listed for the run", "[presence]")
{
    auto inputs = onDefault();
    inputs.seenLocalThisRun = { "work" };
    CHECK(refsOf(listedDaemons(inputs)) == Refs { "local:default", "local:work" });
}

TEST_CASE("a local daemon not running when discovered is not listed", "[presence]")
{
    // Discovery found "idle"'s socket, but no daemon serving it: it is in neither running nor seen.
    auto inputs = onDefault();
    CHECK(refsOf(listedDaemons(inputs)) == Refs { "local:default" });
}

TEST_CASE("a removed daemon is not listed this run, unless a window is on it", "[presence]")
{
    auto inputs = onDefault();
    inputs.seenLocalThisRun = { "work" };
    inputs.removedThisRun = { "local:work", "local:default" };
    CHECK(refsOf(listedDaemons(inputs)) == Refs { "local:default" });
}

TEST_CASE("a saved host is listed only with show_in_sidebar or the user's Show", "[presence]")
{
    auto inputs = onDefault();
    inputs.settings.hosts = {
        { "listed", SidebarListingChoice::Listed },
        { "shown", SidebarListingChoice::Unlisted },
        { "unlisted", SidebarListingChoice::Unlisted },
    };
    inputs.settings.shownByUser = { "host:shown" };
    auto const listed = listedDaemons(inputs);
    CHECK(refsOf(listed) == Refs { "local:default", "host:listed", "host:shown" });
    CHECK(listed.at(1).locality == DaemonLocality::Remote);
}

TEST_CASE("Hide unlists a daemon unless a window is on it", "[presence]")
{
    auto inputs = onDefault();
    inputs.runningLocal = { "default", "work" };
    inputs.settings.hosts = {
        { "starfive", SidebarListingChoice::Listed },
        { "vega", SidebarListingChoice::Unlisted },
    };
    inputs.settings.shownByUser = { "host:vega" };
    inputs.settings.hiddenByUser = { "local:default", "local:work", "host:starfive", "host:vega" };
    CHECK(refsOf(listedDaemons(inputs)) == Refs { "local:default" });

    inputs.windowsOn.push_back(hostDaemonRef("vega"));
    CHECK(refsOf(listedDaemons(inputs)) == Refs { "local:default", "host:vega" });
}

namespace
{
/// @return The refs of the hidden daemons, in order.
[[nodiscard]] std::vector<std::string> refsOf(std::vector<contour::window::HiddenDaemon> const& hidden)
{
    auto refs = std::vector<std::string> {};
    for (auto const& daemon: hidden)
        refs.push_back(daemon.daemon.value);
    return refs;
}
} // namespace

TEST_CASE("the hidden daemons are the user's hides that are known and not listed", "[presence]")
{
    auto inputs = onDefault();
    inputs.runningLocal = { "default", "work" };
    inputs.settings.hosts = {
        { "starfive", SidebarListingChoice::Listed },
        { "quiet", SidebarListingChoice::Unlisted },
    };

    SECTION("a running local daemon, hidden, is named")
    {
        inputs.settings.hiddenByUser = { "local:work" };
        auto const hidden = hiddenDaemons(inputs, listedDaemons(inputs));
        CHECK(refsOf(hidden) == Refs { "local:work" });
        CHECK(hidden.at(0).name == "work");
    }
    SECTION("a local daemon never seen this run is not")
    {
        inputs.settings.hiddenByUser = { "local:gone" };
        CHECK(hiddenDaemons(inputs, listedDaemons(inputs)).empty());
    }
    SECTION("a configured host, hidden, is named")
    {
        inputs.settings.hiddenByUser = { "host:starfive" };
        auto const hidden = hiddenDaemons(inputs, listedDaemons(inputs));
        CHECK(refsOf(hidden) == Refs { "host:starfive" });
        CHECK(hidden.at(0).name == "starfive");
    }
    SECTION("a host its configuration does not list, with no choice, is not")
    {
        CHECK(hiddenDaemons(inputs, listedDaemons(inputs)).empty());
    }
    SECTION("a removed daemon is not")
    {
        inputs.settings.hiddenByUser = { "local:work" };
        inputs.removedThisRun = { "local:work" };
        CHECK(hiddenDaemons(inputs, listedDaemons(inputs)).empty());
    }
    SECTION("a hidden daemon a window is on is listed, not hidden")
    {
        inputs.settings.hiddenByUser = { "local:default", "host:starfive" };
        inputs.windowsOn.push_back(hostDaemonRef("starfive"));
        CHECK(hiddenDaemons(inputs, listedDaemons(inputs)).empty());
    }
}

TEST_CASE("hidden daemons follow tree order", "[presence]")
{
    auto inputs = onDefault();
    inputs.runningLocal = { "zeta", "default" };
    inputs.seenLocalThisRun = { "alpha" };
    inputs.settings.hosts = {
        { "vega", SidebarListingChoice::Listed },
        { "starfive", SidebarListingChoice::Unlisted },
    };
    inputs.settings.hiddenByUser = { "host:vega", "local:zeta", "host:starfive", "local:alpha" };
    CHECK(refsOf(hiddenDaemons(inputs, listedDaemons(inputs)))
          == Refs { "local:alpha", "local:zeta", "host:starfive", "host:vega" });
}

TEST_CASE("hosts follow the local daemons in name order, unconfigured hosts and links last", "[presence]")
{
    auto inputs = PresenceInputs {};
    inputs.runningLocal = { "work", "default" };
    inputs.settings.hosts = {
        { "zulu", SidebarListingChoice::Listed },
        { "alpha", SidebarListingChoice::Listed },
    };
    inputs.windowsOn = {
        linkDaemonRef(10),        hostDaemonRef("adhoc-b"),  linkDaemonRef(9),
        hostDaemonRef("adhoc-a"), localDaemonRef("stopped"),
    };
    auto const listed = listedDaemons(inputs);
    CHECK(refsOf(listed)
          == Refs { "local:default",
                    "local:stopped",
                    "local:work",
                    "host:alpha",
                    "host:zulu",
                    "host:adhoc-a",
                    "host:adhoc-b",
                    "link:9",
                    "link:10" });
    CHECK(listed.at(1).locality == DaemonLocality::Local);
    CHECK(listed.back().locality == DaemonLocality::Remote);
}

TEST_CASE("each daemon is listed once", "[presence]")
{
    auto inputs = PresenceInputs {};
    inputs.runningLocal = { "default", "default" };
    inputs.seenLocalThisRun = { "default" };
    inputs.settings.hosts = { { "starfive", SidebarListingChoice::Listed } };
    inputs.settings.shownByUser = { "host:starfive" };
    inputs.windowsOn = { localDaemonRef("default"), localDaemonRef("default"), hostDaemonRef("starfive"),
                         hostDaemonRef("starfive"), linkDaemonRef(3),          linkDaemonRef(3) };
    CHECK(refsOf(listedDaemons(inputs)) == Refs { "local:default", "host:starfive", "link:3" });
}

TEST_CASE("daemonRefOf spells local, host and link", "[presence]")
{
    using vthost::proto::SwitchDaemonKind;
    auto const link = DaemonLinkId(7);
    CHECK(daemonRefOf(DaemonAddress { .kind = SwitchDaemonKind::Name, .name = "work" }, link)
          == localDaemonRef("work"));
    CHECK(daemonRefOf(DaemonAddress { .kind = SwitchDaemonKind::Host, .name = "work" }, link)
          == hostDaemonRef("work"));
    CHECK(daemonRefOf(DaemonAddress { .kind = SwitchDaemonKind::Same, .name = {} }, link)
          == linkDaemonRef(7));
}

TEST_CASE("a connection's state maps to a link state", "[presence]")
{
    struct Row
    {
        ConnectionState connection;
        DaemonLinkState state;
    };
    // Without a transport failure and with an observable daemon: one row per ConnectionState.
    constexpr auto Rows = std::to_array<Row>({
        { .connection = ConnectionState::Connecting, .state = DaemonLinkState::Connecting },
        { .connection = ConnectionState::Connected, .state = DaemonLinkState::Connected },
        { .connection = ConnectionState::Reconnecting, .state = DaemonLinkState::Reconnecting },
        { .connection = ConnectionState::Failed, .state = DaemonLinkState::Failed },
        { .connection = ConnectionState::Refused, .state = DaemonLinkState::Failed },
        { .connection = ConnectionState::Stopped, .state = DaemonLinkState::NotConnected },
        { .connection = ConnectionState::Upgrading, .state = DaemonLinkState::Reconnecting },
        { .connection = ConnectionState::NeedsAttention, .state = DaemonLinkState::NeedsInput },
    });
    static_assert(Rows.size() == vthost::client::ConnectionStateCount);
    for (auto const& row: Rows)
    {
        INFO("connection state " << static_cast<int>(row.connection));
        CHECK(linkStateOf(row.connection, std::nullopt, ObserveSupport::Supported) == row.state);
        CHECK(linkStateOf(row.connection, std::nullopt, ObserveSupport::Unknown) == row.state);
        CHECK(linkStateOf(row.connection, TransportErrorKind::DaemonAbsent, ObserveSupport::TooOld)
              == DaemonLinkState::TooOld);
    }

    SECTION("a failed connection says what its transport failure says")
    {
        for (auto const connection: { ConnectionState::Failed, ConnectionState::Refused })
        {
            CHECK(linkStateOf(connection, TransportErrorKind::AuthenticationRequired, ObserveSupport::Unknown)
                  == DaemonLinkState::NeedsInput);
            CHECK(linkStateOf(connection, TransportErrorKind::DaemonAbsent, ObserveSupport::Unknown)
                  == DaemonLinkState::NotRunning);
            CHECK(linkStateOf(connection, TransportErrorKind::HostKeyChanged, ObserveSupport::Unknown)
                  == DaemonLinkState::Failed);
            // A failure that would be retried says nothing more: the connection gave up, so it failed.
            CHECK(linkStateOf(connection, TransportErrorKind::Unreachable, ObserveSupport::Unknown)
                  == DaemonLinkState::Failed);
        }
    }

    SECTION("a live connection ignores a stale transport failure")
    {
        CHECK(linkStateOf(
                  ConnectionState::Connected, TransportErrorKind::DaemonAbsent, ObserveSupport::Supported)
              == DaemonLinkState::Connected);
    }
}

TEST_CASE("chosenListing: the user's Hide wins, then the user's Show or the configuration's listing",
          "[presence]")
{
    using contour::remote::chosenListing;
    auto const settings = contour::remote::ListingSettings {
        .hosts = {}, .shownByUser = { "host:vega" }, .hiddenByUser = { "local:work", "host:starfive" }
    };
    CHECK(chosenListing(settings, localDaemonRef("default"), SidebarListingChoice::Listed)
          == SidebarListingChoice::Listed);
    CHECK(chosenListing(settings, localDaemonRef("work"), SidebarListingChoice::Listed)
          == SidebarListingChoice::Unlisted);
    CHECK(chosenListing(settings, hostDaemonRef("starfive"), SidebarListingChoice::Listed)
          == SidebarListingChoice::Unlisted);
    CHECK(chosenListing(settings, hostDaemonRef("vega"), SidebarListingChoice::Unlisted)
          == SidebarListingChoice::Listed);
    CHECK(chosenListing(settings, hostDaemonRef("rigel"), SidebarListingChoice::Unlisted)
          == SidebarListingChoice::Unlisted);
}
