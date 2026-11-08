// SPDX-License-Identifier: Apache-2.0
#include <catch2/catch_test_macros.hpp>

#include <system_error>

#include <vthost/DaemonControlWire.hpp>

TEST_CASE("a StopDaemon reason reads as an enumerator or as nothing", "[vthost][wire]")
{
    CHECK(vthost::stopReasonFrom(0) == vthost::proto::StopReason::User);
    CHECK(vthost::stopReasonFrom(1) == vthost::proto::StopReason::UpgradeRestart);
    CHECK(vthost::stopReasonFrom(2) == vthost::proto::StopReason::UserKeepingWorkspaces);
    // An unknown reason is refused, never read as a plain stop: a daemon that discarded the workspace on
    // a newer client's "keep it" would lose what the user asked to keep.
    CHECK_FALSE(vthost::stopReasonFrom(3).has_value());
    CHECK_FALSE(vthost::stopReasonFrom(255).has_value());
}

TEST_CASE("an UpgradeDaemon outcome travels as started, a refusal plus one, or an exec failure",
          "[vthost][wire][upgrade]")
{
    using vthost::handover::ExecFailure;
    using vthost::handover::UpgradeError;
    using vthost::handover::UpgradeRefusal;

    CHECK(vthost::upgradeStartedReply().result == 0);
    CHECK(vthost::upgradeRefusalFrom(0) == std::nullopt);

    auto const inProgress = vthost::upgradeFailureReply(UpgradeError { UpgradeRefusal::InProgress });
    CHECK(inProgress.result == 1);
    CHECK_FALSE(inProgress.detail.empty());
    CHECK(vthost::upgradeRefusalFrom(inProgress.result) == UpgradeRefusal::InProgress);

    auto const last = vthost::upgradeFailureReply(UpgradeError { vthost::handover::lastRefusal() });
    CHECK(vthost::upgradeRefusalFrom(last.result) == vthost::handover::lastRefusal());
    CHECK(last.detail == vthost::handover::describe(vthost::handover::lastRefusal()));

    auto const exec = vthost::upgradeFailureReply(
        UpgradeError { ExecFailure { .cause = std::make_error_code(std::errc::permission_denied) } });
    CHECK(exec.result == vthost::proto::UpgradeDaemonReply::ExecFailedResult);
    CHECK_FALSE(exec.detail.empty());
    CHECK(vthost::upgradeRefusalFrom(exec.result) == std::nullopt);

    // A refusal a newer daemon knows and this build does not reads as none.
    CHECK(vthost::upgradeRefusalFrom(100) == std::nullopt);
}
