// SPDX-License-Identifier: Apache-2.0
#include <contour/window/DaemonDriftChoice.hpp>

#include <catch2/catch_test_macros.hpp>

using contour::window::DaemonLocality;
using contour::window::DriftOffer;
using contour::window::driftOfferFor;
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
