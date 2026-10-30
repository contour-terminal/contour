// SPDX-License-Identifier: Apache-2.0
#include <contour/config/LaunchTarget.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <map>
#include <string>
#include <string_view>

using contour::config::AttachPlan;
using contour::config::attachPlanOf;
using contour::config::connectArguments;
using contour::config::HostEntry;
using contour::config::Hosting;
using contour::config::LaunchTarget;
using contour::config::LaunchTargetError;
using contour::config::LaunchTargetKind;
using contour::config::parseLaunchTarget;
using contour::config::toString;

namespace
{
[[nodiscard]] std::map<std::string, HostEntry> savedHosts()
{
    auto hosts = std::map<std::string, HostEntry> {};
    hosts["starfive"].url = contour::config::parseHostUrl("ssh://me@starfive").value();
    hosts["build-box"].url = contour::config::parseHostUrl("tls://10.0.0.7:7297").value();
    return hosts;
}

struct AcceptedRow
{
    std::string_view text;
    LaunchTargetKind kind;
    std::string_view name;
    std::string_view spelled; ///< What toString() reads back; the text, unless it was normalised.
};

constexpr auto AcceptedRows = std::array {
    AcceptedRow { "daemon", LaunchTargetKind::LocalDaemon, "", "daemon" },
    AcceptedRow { "daemon:build", LaunchTargetKind::LocalDaemon, "build", "daemon:build" },
    AcceptedRow { "local", LaunchTargetKind::InProcess, "", "local" },
    AcceptedRow { "starfive", LaunchTargetKind::SavedHost, "starfive", "starfive" },
    AcceptedRow { "build-box", LaunchTargetKind::SavedHost, "build-box", "build-box" },
    AcceptedRow { "ssh://me@pi4.lan", LaunchTargetKind::Address, "ssh://me@pi4.lan", "ssh://me@pi4.lan" },
    AcceptedRow {
        "SSH://pi4.lan:2222", LaunchTargetKind::Address, "ssh://pi4.lan:2222", "ssh://pi4.lan:2222" },
    AcceptedRow {
        "tls://10.0.0.9:7297", LaunchTargetKind::Address, "tls://10.0.0.9:7297", "tls://10.0.0.9:7297" },
};

struct RefusedRow
{
    std::string_view text;
    LaunchTargetError error;
};

constexpr auto RefusedRows = std::array {
    RefusedRow { "", LaunchTargetError::Empty },
    RefusedRow { "nowhere", LaunchTargetError::UnknownName },
    RefusedRow { "local:x", LaunchTargetError::LabelNotUsed },
    RefusedRow { "daemon:", LaunchTargetError::EmptyLabel },
    RefusedRow { "ftp://host", LaunchTargetError::BadAddress },
    RefusedRow { "tls://host", LaunchTargetError::BadAddress },
    RefusedRow { "ssh://-oProxyCommand=x", LaunchTargetError::BadAddress },
};
} // namespace

TEST_CASE("LaunchTarget.parse reads every kind of target", "[contour][connect]")
{
    auto const hosts = savedHosts();
    for (auto const& row: AcceptedRows)
    {
        INFO(row.text);
        auto const target = parseLaunchTarget(row.text, hosts);
        REQUIRE(target.has_value());
        CHECK(target->kind == row.kind);
        CHECK(target->name == row.name);
        CHECK(toString(*target) == row.spelled);
    }
}

TEST_CASE("LaunchTarget.parse refuses what names no target", "[contour][connect]")
{
    auto const hosts = savedHosts();
    for (auto const& row: RefusedRows)
    {
        INFO(row.text);
        auto const target = parseLaunchTarget(row.text, hosts);
        REQUIRE_FALSE(target.has_value());
        CHECK(target.error().error == row.error);
        CHECK_FALSE(target.error().describe().empty());
    }
}

TEST_CASE("LaunchTarget.toString is read back by parse", "[contour][connect]")
{
    auto const hosts = savedHosts();
    for (auto const& row: AcceptedRows)
    {
        auto const target = parseLaunchTarget(row.text, hosts).value();
        CHECK(parseLaunchTarget(toString(target), hosts).value() == target);
    }
}

TEST_CASE("LaunchTarget.a keyword shadows a saved host of the same name", "[contour][connect]")
{
    auto hosts = savedHosts();
    hosts["local"].url = contour::config::parseHostUrl("ssh://elsewhere").value();
    CHECK(parseLaunchTarget("local", hosts).value().kind == LaunchTargetKind::InProcess);
}

TEST_CASE("LaunchTarget.attachPlan maps each kind onto an attach path", "[contour][connect]")
{
    CHECK(attachPlanOf({ LaunchTargetKind::InProcess, {} }) == AttachPlan { .hosting = Hosting::InProcess });
    CHECK(attachPlanOf({ LaunchTargetKind::LocalDaemon, {} })
          == AttachPlan { .hosting = Hosting::LocalDaemon, .label = {}, .host = {} });
    CHECK(attachPlanOf({ LaunchTargetKind::LocalDaemon, "build" })
          == AttachPlan { .hosting = Hosting::LocalDaemon, .label = "build", .host = {} });
    CHECK(attachPlanOf({ LaunchTargetKind::SavedHost, "starfive" })
          == AttachPlan { .hosting = Hosting::RemoteHost, .label = {}, .host = "starfive" });
    CHECK(attachPlanOf({ LaunchTargetKind::Address, "ssh://me@pi4.lan" })
          == AttachPlan { .hosting = Hosting::RemoteHost, .label = {}, .host = "ssh://me@pi4.lan" });
}

TEST_CASE("LaunchTarget.connectArguments spells the connect verb", "[contour][connect]")
{
    using Args = std::vector<std::string>;
    CHECK(connectArguments({ LaunchTargetKind::SavedHost, "starfive" })
          == Args { "connect", "--", "starfive" });
    CHECK(connectArguments({ LaunchTargetKind::InProcess, {} }, "/c/contour.yml")
          == Args { "connect", "--config", "/c/contour.yml", "--", "local" });
}
