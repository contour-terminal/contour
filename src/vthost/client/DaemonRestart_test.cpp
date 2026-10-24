// SPDX-License-Identifier: Apache-2.0
#include <catch2/catch_test_macros.hpp>

#include <optional>
#include <vector>

#include <vthost/client/DaemonRestart.hpp>

namespace
{
using vthost::client::ControlError;
using vthost::client::ControlErrorCode;

class FakeControlPort final: public vthost::client::DaemonControlPort
{
  public:
    [[nodiscard]] std::expected<vthost::client::DaemonStatus, ControlError> status() override
    {
        return vthost::client::DaemonStatus {};
    }

    [[nodiscard]] std::expected<void, ControlError> stop(vthost::proto::StopReason reason) override
    {
        stops.push_back(reason);
        if (stopError)
            return std::unexpected(*stopError);
        return {};
    }

    std::vector<vthost::proto::StopReason> stops;
    std::optional<ControlError> stopError;
};

class FakeStarter final: public vthost::DaemonStarter
{
  public:
    [[nodiscard]] std::expected<void, std::string> start() override
    {
        ++starts;
        if (startError)
            return std::unexpected(*startError);
        return {};
    }

    int starts = 0;
    std::optional<std::string> startError;
};
} // namespace

TEST_CASE("restartDaemon stops for upgrade, then starts", "[vthost][restart]")
{
    auto control = FakeControlPort {};
    auto starter = FakeStarter {};
    REQUIRE(vthost::client::restartDaemon(control, starter).has_value());
    CHECK(control.stops == std::vector { vthost::proto::StopReason::UpgradeRestart });
    CHECK(starter.starts == 1);
}

TEST_CASE("a daemon already gone is restarted; a refused stop is not", "[vthost][restart]")
{
    auto control = FakeControlPort {};
    auto starter = FakeStarter {};

    SECTION("not running counts as stopped")
    {
        control.stopError = ControlError { .code = ControlErrorCode::NotRunning, .detail = "gone" };
        CHECK(vthost::client::restartDaemon(control, starter).has_value());
        CHECK(starter.starts == 1);
    }

    SECTION("a timeout is a failed stop and starts nothing")
    {
        control.stopError = ControlError { .code = ControlErrorCode::Timeout, .detail = "slow" };
        auto const result = vthost::client::restartDaemon(control, starter);
        REQUIRE_FALSE(result.has_value());
        CHECK(result.error().code == vthost::client::DaemonRestartError::StopFailed);
        CHECK(starter.starts == 0);
    }

    SECTION("a failing start carries the starter's text")
    {
        starter.startError = "no binary";
        auto const result = vthost::client::restartDaemon(control, starter);
        REQUIRE_FALSE(result.has_value());
        CHECK(result.error().code == vthost::client::DaemonRestartError::StartFailed);
        CHECK(result.error().detail == "no binary");
    }
}
