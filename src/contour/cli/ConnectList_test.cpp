// SPDX-License-Identifier: Apache-2.0
#include <contour/cli/ConnectList.hpp>

#include <core/Utils.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <map>
#include <ranges>
#include <string>
#include <vector>

using contour::cli::ConnectGroup;
using contour::cli::ConnectRow;
using contour::cli::connectRows;
using contour::cli::DaemonRunState;
using contour::cli::describeLocalDaemon;
using contour::cli::initialSelection;
using contour::cli::ListFormat;
using contour::cli::LocalDaemonState;
using contour::cli::renderTargetList;
using contour::cli::rowForQuickSelect;
using contour::config::HostEntry;

namespace
{
[[nodiscard]] std::map<std::string, HostEntry> savedHosts()
{
    auto hosts = std::map<std::string, HostEntry> {};
    hosts["starfive"].url = contour::config::parseHostUrl("ssh://me@starfive").value();
    hosts["build-box"].url = contour::config::parseHostUrl("tls://10.0.0.7:7297").value();
    return hosts;
}

[[nodiscard]] std::vector<std::string> titlesOf(std::vector<ConnectRow> const& rows)
{
    return std::ranges::to<std::vector>(rows | std::views::transform(&ConnectRow::title));
}

constexpr auto Running3 = LocalDaemonState { .run = DaemonRunState::Running, .sessions = 3 };
} // namespace

TEST_CASE("ConnectList.unfiltered lists this computer, then the saved hosts by name", "[contour][connect]")
{
    auto const hosts = savedHosts();
    auto const rows = connectRows({ hosts, Running3 }, "");
    CHECK(titlesOf(rows)
          == std::vector<std::string> { "Persistent sessions", "This window only", "build-box", "starfive" });
    CHECK(rows[0].group == ConnectGroup::ThisComputer);
    CHECK(rows[1].group == ConnectGroup::ThisComputer);
    CHECK(rows[2].group == ConnectGroup::SavedHosts);
    CHECK(rows[0].detail == "Local daemon · running · 3 sessions");
    CHECK(rows[2].tag == "TLS");
    CHECK(rows[3].tag == "SSH");
    CHECK(rows[3].detail == "ssh://me@starfive");
    CHECK(rows[3].text == "starfive");
}

TEST_CASE("ConnectList.daemon status reads running, not running and a singular session", "[contour][connect]")
{
    CHECK(describeLocalDaemon({ DaemonRunState::NotRunning, std::nullopt }, "")
          == "Local daemon · not running · starts on connect");
    CHECK(describeLocalDaemon({ DaemonRunState::Probing, std::nullopt }, "") == "Local daemon · checking…");
    CHECK(describeLocalDaemon({ DaemonRunState::Running, 1 }, "") == "Local daemon · running · 1 session");
    CHECK(describeLocalDaemon({ DaemonRunState::Running, std::nullopt }, "") == "Local daemon · running");
    CHECK(describeLocalDaemon({ DaemonRunState::Running, 2 }, "dev") == "Daemon dev · running · 2 sessions");
}

TEST_CASE("ConnectList.rows are numbered one to nine and filtering renumbers", "[contour][connect]")
{
    auto const hosts = savedHosts();
    auto const all = connectRows({ hosts, Running3 }, "");
    for (auto const [index, row]: core::views::enumerate(all))
        CHECK(row.quick == static_cast<std::size_t>(index) + 1);

    auto const filtered = connectRows({ hosts, Running3 }, "star");
    REQUIRE(filtered.size() == 1);
    CHECK(filtered[0].title == "starfive");
    CHECK(filtered[0].quick == 1);
    CHECK(rowForQuickSelect(filtered, 1) == 0);
    CHECK_FALSE(rowForQuickSelect(filtered, 2).has_value());
}

TEST_CASE("ConnectList.only nine rows carry a digit", "[contour][connect]")
{
    auto hosts = std::map<std::string, HostEntry> {};
    for (auto const name: { "a", "b", "c", "d", "e", "f", "g", "h", "i", "j" })
        hosts[name].url = contour::config::parseHostUrl("ssh://host").value();
    auto const rows = connectRows({ hosts, {} }, "");
    REQUIRE(rows.size() == 12);
    CHECK(rows[8].quick == 9);
    CHECK_FALSE(rows[9].quick.has_value());
    CHECK_FALSE(rowForQuickSelect(rows, 10).has_value());
}

TEST_CASE("ConnectList.filter matches the address and several words", "[contour][connect]")
{
    auto const hosts = savedHosts();
    CHECK(titlesOf(connectRows({ hosts, Running3 }, "10.0.0")) == std::vector<std::string> { "build-box" });
    CHECK(titlesOf(connectRows({ hosts, Running3 }, "STAR")) == std::vector<std::string> { "starfive" });
    CHECK(titlesOf(connectRows({ hosts, Running3 }, "this window"))
          == std::vector<std::string> { "This window only" });
    CHECK(titlesOf(connectRows({ hosts, Running3 }, "tls box")) == std::vector<std::string> { "build-box" });
    CHECK(connectRows({ hosts, Running3 }, "zzz").empty());
}

TEST_CASE("ConnectList.a typed address adds a leading connect row", "[contour][connect]")
{
    auto const hosts = savedHosts();
    auto const rows = connectRows({ hosts, Running3 }, " ssh://me@pi4.lan ");
    REQUIRE(rows.size() == 1);
    CHECK(rows[0].group == ConnectGroup::Typed);
    CHECK(rows[0].title == "Connect to ssh://me@pi4.lan");
    CHECK(rows[0].detail == "Not saved");
    CHECK(rows[0].quick == 1);
    CHECK(rows[0].text == "ssh://me@pi4.lan");

    // Saved hosts that match the address follow it.
    auto const withMatch = connectRows({ hosts, Running3 }, "ssh://me@starfive");
    REQUIRE(withMatch.size() == 2);
    CHECK(withMatch[0].group == ConnectGroup::Typed);
    CHECK(withMatch[1].title == "starfive");
}

TEST_CASE("ConnectList.a half-typed or refused address adds no row", "[contour][connect]")
{
    auto const hosts = savedHosts();
    for (auto const* const typed: { "ssh://", "tls://host", "ssh://-bad", "ftp://x" })
    {
        INFO(typed);
        auto const rows = connectRows({ hosts, Running3 }, typed);
        CHECK(std::ranges::none_of(rows,
                                   [](ConnectRow const& row) { return row.group == ConnectGroup::Typed; }));
    }
}

TEST_CASE("ConnectList.the selection starts on the last used target", "[contour][connect]")
{
    auto const hosts = savedHosts();
    auto const rows = connectRows({ hosts, Running3 }, "");
    CHECK(initialSelection(rows, "starfive") == 3);
    CHECK(initialSelection(rows, "local") == 1);
    CHECK(initialSelection(rows, "") == 0);
    CHECK(initialSelection(rows, "gone-host") == 0);
}

TEST_CASE("ConnectList.the quick-select key reads ⌘ on macOS and Alt+ elsewhere", "[contour][connect]")
{
    CHECK(contour::cli::quickSelectLabel(contour::cli::QuickSelectKey::Command, 3) == "⌘3");
    CHECK(contour::cli::quickSelectLabel(contour::cli::QuickSelectKey::Alt, 9) == "Alt+9");
}

TEST_CASE("ConnectList.render prints text and JSON", "[contour][connect]")
{
    auto const hosts = savedHosts();
    auto const rows = connectRows({ hosts, { DaemonRunState::NotRunning, std::nullopt } }, "star");
    CHECK(renderTargetList(rows, ListFormat::Text) == "starfive\tssh://me@starfive\n");
    CHECK(renderTargetList(rows, ListFormat::Json)
          == "[{\"target\":\"starfive\",\"kind\":\"saved-host\",\"title\":\"starfive\","
             "\"detail\":\"ssh://me@starfive\",\"tag\":\"SSH\"}]\n");
    CHECK(renderTargetList({}, ListFormat::Json) == "[]\n");
}
