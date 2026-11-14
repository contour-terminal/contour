// SPDX-License-Identifier: Apache-2.0
#include <contour/cli/KeeperVerb.hpp>

#include <catch2/catch_test_macros.hpp>

#include <string>
#include <vector>

using contour::cli::DaemonLaunch;
using contour::cli::DaemonVerbOptions;
using contour::cli::OptionFlag;
using vthost::keeper::LaunchContinuity;

TEST_CASE("only a daemon restarted after a crash drops the options meant for one process",
          "[contour][cli][daemon]")
{
    auto given = DaemonVerbOptions {};
    given.handover = "fd=9";
    given.background = OptionFlag::Given;
    given.stdio = OptionFlag::Given;
    given.token = "secret";
    auto const launch = DaemonLaunch::restore(given, "/tmp/contour-test/keeper.sock");

    CHECK(contour::cli::launchFor(launch, LaunchContinuity::Fresh) == launch);
    CHECK(contour::cli::launchFor(launch, LaunchContinuity::HandedOver) == launch);
    auto const recovered = contour::cli::launchFor(launch, LaunchContinuity::Recovered);
    CHECK(recovered == launch.relaunched());
    CHECK(recovered.given().handover.empty());
    CHECK(recovered.given().background == OptionFlag::Absent);
    CHECK(recovered.given().stdio == OptionFlag::Absent);
    CHECK(recovered.given().token == "secret");
}

TEST_CASE("daemon.keeper.restart_hung becomes what the daemon tells its keeper", "[contour][cli][daemon]")
{
    CHECK(contour::cli::keeperSettingsOf({ .restartHung = true }).hungRestart
          == vthost::keeper::HungRestart::On);
    CHECK(contour::cli::keeperSettingsOf({ .restartHung = false }).hungRestart
          == vthost::keeper::HungRestart::Off);
    CHECK(contour::cli::keeperSettingsOf({}).heartbeatTimeout == vthost::KeeperSettings {}.heartbeatTimeout);
}

#ifndef _WIN32
TEST_CASE("the keeper starts the daemon with the frozen command line", "[contour][cli][daemon]")
{
    CHECK(contour::cli::keeperServeArguments()
          == std::vector<std::string> { "daemon", "keeper-serve", "--keeper-channel=3" });
}
#endif
