// SPDX-License-Identifier: Apache-2.0
#include <contour/window/DaemonDriftChoice.hpp>

#include <catch2/catch_test_macros.hpp>

using contour::window::ConnectedDaemon;
using contour::window::DaemonLocality;
using contour::window::DaemonUpkeep;
using contour::window::describe;
using contour::window::DriftOffer;
using contour::window::driftOfferFor;
using contour::window::UpgradeVerdict;
using contour::window::upgradeVerdictFor;
using vthost::client::DriftKind;
using vthost::proto::Capability;
using vthost::proto::CapabilitySet;

TEST_CASE("a local daemon with LiveUpgrade offers upgrade and restart", "[contour][drift]")
{
    auto const capabilities = CapabilitySet::of({ Capability::DaemonControl, Capability::LiveUpgrade });
    for (auto const drift: { DriftKind::DaemonOlder, DriftKind::Differs, DriftKind::DaemonUnknown })
        CHECK(driftOfferFor(drift, DaemonLocality::Local, capabilities) == DriftOffer::UpgradeAndRestart);
}

TEST_CASE("a remote daemon offers nothing", "[contour][drift]")
{
    auto const capabilities = CapabilitySet::of({ Capability::DaemonControl, Capability::LiveUpgrade });
    CHECK(driftOfferFor(DriftKind::DaemonOlder, DaemonLocality::Remote, capabilities) == DriftOffer::None);
    CHECK(driftOfferFor(DriftKind::DaemonOlder, DaemonLocality::Remote, CapabilitySet {})
          == DriftOffer::None);
}

TEST_CASE("an old daemon offers restart only", "[contour][drift]")
{
    auto const capabilities = CapabilitySet::of({ Capability::DaemonControl });
    CHECK(driftOfferFor(DriftKind::DaemonOlder, DaemonLocality::Local, capabilities)
          == DriftOffer::RestartOnly);
    CHECK(driftOfferFor(DriftKind::DaemonUnknown, DaemonLocality::Local, CapabilitySet {})
          == DriftOffer::RestartOnly);
}

TEST_CASE("matching releases offer nothing", "[contour][drift]")
{
    auto const capabilities = CapabilitySet::of({ Capability::LiveUpgrade });
    CHECK(driftOfferFor(DriftKind::None, DaemonLocality::Local, capabilities) == DriftOffer::None);
}

TEST_CASE("a daemon newer than the client is offered a restart, not an upgrade", "[contour][drift]")
{
    auto const capabilities = CapabilitySet::of({ Capability::DaemonControl, Capability::LiveUpgrade });
    CHECK(driftOfferFor(DriftKind::DaemonNewer, DaemonLocality::Local, capabilities)
          == DriftOffer::RestartOnly);
}

namespace
{
ConnectedDaemon connectedWith(DriftKind drift)
{
    return { .drift = drift,
             .daemonCapabilities =
                 CapabilitySet::of({ Capability::DaemonControl, Capability::LiveUpgrade }) };
}

ConnectedDaemon connectedWithoutLiveUpgrade(DriftKind drift)
{
    return { .drift = drift, .daemonCapabilities = CapabilitySet::of({ Capability::DaemonControl }) };
}
} // namespace

TEST_CASE("a commanded upgrade goes ahead for every drift but a newer daemon, the same release included",
          "[contour][drift]")
{
    for (auto const drift:
         { DriftKind::None, DriftKind::DaemonOlder, DriftKind::Differs, DriftKind::DaemonUnknown })
        CHECK(upgradeVerdictFor(DaemonLocality::Local, DaemonUpkeep::ThisClient, connectedWith(drift))
              == UpgradeVerdict::Upgrade);
    CHECK(describe(UpgradeVerdict::Upgrade).empty());
}

TEST_CASE("a commanded upgrade is refused with a reason for each way it cannot go ahead", "[contour][drift]")
{
    CHECK(upgradeVerdictFor(DaemonLocality::Remote, DaemonUpkeep::None, connectedWith(DriftKind::None))
          == UpgradeVerdict::NotThisClients);
    CHECK(upgradeVerdictFor(DaemonLocality::Local, DaemonUpkeep::None, connectedWith(DriftKind::None))
          == UpgradeVerdict::NotThisClients);
    // Whose daemon it is is judged before the connection: a daemon of another's is not ours while down
    // either.
    CHECK(upgradeVerdictFor(DaemonLocality::Remote, DaemonUpkeep::None, std::nullopt)
          == UpgradeVerdict::NotThisClients);
    CHECK(upgradeVerdictFor(DaemonLocality::Local, DaemonUpkeep::ThisClient, std::nullopt)
          == UpgradeVerdict::NotConnected);
    CHECK(upgradeVerdictFor(
              DaemonLocality::Local, DaemonUpkeep::ThisClient, connectedWith(DriftKind::DaemonNewer))
          == UpgradeVerdict::WouldDowngrade);
    CHECK(upgradeVerdictFor(
              DaemonLocality::Local, DaemonUpkeep::ThisClient, connectedWithoutLiveUpgrade(DriftKind::None))
          == UpgradeVerdict::NoLiveUpgrade);
    for (auto const& row: contour::window::UpgradeVerdictRows)
        if (row.verdict != UpgradeVerdict::Upgrade)
            CHECK_FALSE(describe(row.verdict).empty());
}

TEST_CASE("a daemon reached by label is this client's only when it serves the launch's socket",
          "[contour][drift]")
{
    using contour::window::upkeepOfReachedDaemon;
    CHECK(upkeepOfReachedDaemon("/run/contour/mux.sock", "/run/contour/mux.sock")
          == DaemonUpkeep::ThisClient);
    CHECK(upkeepOfReachedDaemon("/run/contour/mux.sock", "/run/contour/./mux.sock")
          == DaemonUpkeep::ThisClient);
    // --socket or $CONTOUR_MUX moved the launch's daemon off the label's socket.
    CHECK(upkeepOfReachedDaemon("/tmp/custom.sock", "/run/contour/mux.sock") == DaemonUpkeep::None);
    // A launch with no daemon of its own upkeeps none.
    CHECK(upkeepOfReachedDaemon("", "/run/contour/mux.sock") == DaemonUpkeep::None);
    CHECK(upkeepOfReachedDaemon("", "") == DaemonUpkeep::None);
}
