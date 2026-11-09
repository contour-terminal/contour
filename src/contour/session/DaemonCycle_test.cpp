// SPDX-License-Identifier: Apache-2.0
#include <contour/session/DaemonCycle.hpp>

#include <catch2/catch_test_macros.hpp>

#include <string>
#include <vector>

using contour::actions::CycleDirection;
using contour::cli::DiscoveredDaemon;
using contour::session::nextDaemon;
using contour::session::SwitchTarget;
using vthost::proto::SwitchDaemonKind;

namespace
{
std::vector<DiscoveredDaemon> daemons(std::vector<std::string> const& labels)
{
    auto result = std::vector<DiscoveredDaemon> {};
    for (auto const& label: labels)
        result.push_back({ .label = label, .socket = {}, .workspaces = std::nullopt });
    return result;
}

SwitchTarget label(std::string name)
{
    return { .kind = SwitchDaemonKind::Label, .daemon = std::move(name), .workspace = {} };
}

SwitchTarget host(std::string name)
{
    return { .kind = SwitchDaemonKind::Host, .daemon = std::move(name), .workspace = {} };
}
} // namespace

TEST_CASE("nextDaemon walks local labels, then shown hosts, alphabetically, wrapping", "[daemon-cycle]")
{
    auto const local = daemons({ "work", "admin", "default" });
    auto const hosts = std::vector<std::string> { "studio", "attic" };

    // The ring: admin, default, work, attic, studio.
    CHECK(nextDaemon(local, hosts, label("admin"), CycleDirection::Next) == label("default"));
    CHECK(nextDaemon(local, hosts, label("work"), CycleDirection::Next) == host("attic"));
    CHECK(nextDaemon(local, hosts, host("attic"), CycleDirection::Next) == host("studio"));
    CHECK(nextDaemon(local, hosts, host("studio"), CycleDirection::Next) == label("admin"));

    CHECK(nextDaemon(local, hosts, label("admin"), CycleDirection::Previous) == host("studio"));
    CHECK(nextDaemon(local, hosts, host("attic"), CycleDirection::Previous) == label("work"));
}

TEST_CASE("nextDaemon skips a current host no window shows, and a ring of only the current one",
          "[daemon-cycle]")
{
    auto const local = daemons({ "admin", "default" });
    auto const hosts = std::vector<std::string> { "studio" };

    // The window's own host is not shown by any window (it just left it): the step starts at the ring's end.
    CHECK(nextDaemon(local, hosts, host("gone"), CycleDirection::Next) == label("admin"));
    CHECK(nextDaemon(local, hosts, host("gone"), CycleDirection::Previous) == host("studio"));

    // A label and a host of one name are different entries.
    CHECK(nextDaemon(local, std::vector<std::string> { "admin" }, label("admin"), CycleDirection::Next)
          == label("default"));

    CHECK_FALSE(nextDaemon(daemons({ "default" }), {}, label("default"), CycleDirection::Next).has_value());
    CHECK_FALSE(nextDaemon({}, {}, label("default"), CycleDirection::Previous).has_value());
}
