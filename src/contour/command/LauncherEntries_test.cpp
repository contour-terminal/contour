// SPDX-License-Identifier: Apache-2.0
#include <contour/command/LauncherEntries.hpp>

#include <catch2/catch_test_macros.hpp>

#include <format>
#include <map>
#include <ranges>
#include <string>
#include <vector>

using contour::command::launcherEntries;
using contour::command::LauncherEntry;
using contour::command::LauncherSurface;
using contour::config::HostEntry;

namespace
{
using Args = std::vector<std::string>;

[[nodiscard]] std::map<std::string, HostEntry> savedHosts()
{
    auto hosts = std::map<std::string, HostEntry> {};
    hosts["starfive"].url = contour::config::parseHostUrl("ssh://me@starfive").value();
    hosts["build-box"].url = contour::config::parseHostUrl("tls://10.0.0.7:7297").value();
    return hosts;
}
} // namespace

TEST_CASE("Launcher: the Dock menu opens the picker, then lists the saved hosts",
          "[contour][connect][launcher]")
{
    auto const entries = launcherEntries(LauncherSurface::DockMenu, savedHosts(), "");
    REQUIRE(entries.size() == 3);
    CHECK(entries[0] == LauncherEntry { "New Window to…", "", Args { "connect" } });
    CHECK(entries[1]
          == LauncherEntry { "build-box", "tls://10.0.0.7:7297", Args { "connect", "--", "build-box" } });
    CHECK(entries[2]
          == LauncherEntry { "starfive", "ssh://me@starfive", Args { "connect", "--", "starfive" } });
}

TEST_CASE("Launcher: the jump list words its picker task Connect…", "[contour][connect][launcher]")
{
    auto const entries = launcherEntries(LauncherSurface::JumpList, savedHosts(), "");
    REQUIRE(entries.size() == 3);
    CHECK(entries[0].title == "Connect…");
    CHECK(entries[0].arguments == Args { "connect" });
    CHECK(entries[2].arguments == Args { "connect", "--", "starfive" });
}

TEST_CASE("Launcher: no saved hosts leaves just the picker", "[contour][connect][launcher]")
{
    for (auto const surface: { LauncherSurface::DockMenu, LauncherSurface::JumpList })
        CHECK(launcherEntries(surface, {}, "").size() == 1);
}

TEST_CASE("Launcher: the configuration file reaches every command line", "[contour][connect][launcher]")
{
    auto const entries = launcherEntries(LauncherSurface::DockMenu, savedHosts(), "/c/contour.yml");
    CHECK(entries[0].arguments == Args { "connect", "--config", "/c/contour.yml" });
    CHECK(entries[2].arguments == Args { "connect", "--config", "/c/contour.yml", "--", "starfive" });
}

TEST_CASE("Launcher: a surface lists no more hosts than it has room for", "[contour][connect][launcher]")
{
    auto hosts = std::map<std::string, HostEntry> {};
    for (auto const index: std::views::iota(0, 30))
        hosts[std::format("host{:02}", index)].url = contour::config::parseHostUrl("ssh://h").value();
    for (auto const& row: contour::command::LauncherSurfaceRows)
        CHECK(launcherEntries(row.surface, hosts, "").size() == row.maxHosts + 1);
}
