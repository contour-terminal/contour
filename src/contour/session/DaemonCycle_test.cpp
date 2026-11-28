// SPDX-License-Identifier: Apache-2.0
#include <contour/session/DaemonCycle.hpp>

#include <catch2/catch_test_macros.hpp>

#include <string>
#include <vector>

using contour::actions::CycleDirection;
using contour::session::nextDaemon;
using contour::session::SwitchTarget;
using vthost::proto::SwitchDaemonKind;

namespace
{
SwitchTarget label(std::string name)
{
    return { .kind = SwitchDaemonKind::Name, .daemon = std::move(name), .workspace = {} };
}

SwitchTarget host(std::string name)
{
    return { .kind = SwitchDaemonKind::Host, .daemon = std::move(name), .workspace = {} };
}
} // namespace

TEST_CASE("CycleDaemon walks the listed daemons in tree order", "[daemon-cycle]")
{
    // The sidebar's order: local:default first, the other local daemons, then the saved hosts.
    auto const ring =
        std::vector<SwitchTarget> { label("default"), label("work"), host("studio"), host("attic") };

    CHECK(nextDaemon(ring, label("default"), CycleDirection::Next) == label("work"));
    CHECK(nextDaemon(ring, host("studio"), CycleDirection::Next) == host("attic"));
    CHECK(nextDaemon(ring, host("attic"), CycleDirection::Next) == label("default"));
    CHECK(nextDaemon(ring, label("default"), CycleDirection::Previous) == host("attic"));

    // A label and a host of one name are different entries.
    auto const sameName = std::vector<SwitchTarget> { label("admin"), host("admin"), label("default") };
    CHECK(nextDaemon(sameName, label("admin"), CycleDirection::Next) == host("admin"));
    CHECK(nextDaemon(sameName, host("admin"), CycleDirection::Next) == label("default"));

    // A daemon no longer listed starts at the ring's end; a ring of only the current one goes nowhere.
    CHECK(nextDaemon(ring, label("gone"), CycleDirection::Next) == label("default"));
    CHECK(nextDaemon(ring, label("gone"), CycleDirection::Previous) == host("attic"));
    CHECK_FALSE(
        nextDaemon(std::vector { label("default") }, label("default"), CycleDirection::Next).has_value());
    CHECK_FALSE(nextDaemon(std::vector<SwitchTarget> {}, label("default"), CycleDirection::Next).has_value());
}
