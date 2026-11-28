// SPDX-License-Identifier: Apache-2.0
#include <contour/window/DaemonSection.hpp>

#include <catch2/catch_test_macros.hpp>

#include <optional>
#include <utility>

using namespace contour::window;

TEST_CASE("local x and host x have different refs", "[daemon-link]")
{
    CHECK(localDaemonRef("x") != hostDaemonRef("x"));
    CHECK(localDaemonRef("x").value == "local:x");
    CHECK(hostDaemonRef("x").value == "host:x");
    CHECK(linkDaemonRef(7).value == "link:7");
}

TEST_CASE("a ref parses back to its kind and name", "[daemon-link]")
{
    auto const local = localDaemonRef("work");
    auto const host = hostDaemonRef("starfive");
    auto const link = linkDaemonRef(42);
    CHECK(parseDaemonRef(local) == std::pair { DaemonRefKind::Local, std::string_view { "work" } });
    CHECK(parseDaemonRef(host) == std::pair { DaemonRefKind::Host, std::string_view { "starfive" } });
    CHECK(parseDaemonRef(link) == std::pair { DaemonRefKind::Link, std::string_view { "42" } });
    CHECK(parseDaemonRef(DaemonRef { "work" }) == std::nullopt);
    CHECK(parseDaemonRef(DaemonRef { "remote:work" }) == std::nullopt);
    CHECK(parseDaemonRef(DaemonRef { "host:" }) == std::nullopt);
    CHECK(parseDaemonRef(DaemonRef { "link:4x" }) == std::nullopt);
}
