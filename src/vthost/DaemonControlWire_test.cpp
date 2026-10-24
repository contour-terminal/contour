// SPDX-License-Identifier: Apache-2.0
#include <catch2/catch_test_macros.hpp>

#include <vthost/DaemonControlWire.hpp>

TEST_CASE("a StopDaemon reason reads as an enumerator or as nothing", "[vthost][wire]")
{
    CHECK(vthost::stopReasonFrom(0) == vthost::proto::StopReason::User);
    CHECK(vthost::stopReasonFrom(1) == vthost::proto::StopReason::UpgradeRestart);
    CHECK(vthost::stopReasonFrom(2) == vthost::proto::StopReason::UserKeepingWorkspace);
    // An unknown reason is refused, never read as a plain stop: a daemon that discarded the workspace on
    // a newer client's "keep it" would lose what the user asked to keep.
    CHECK_FALSE(vthost::stopReasonFrom(3).has_value());
    CHECK_FALSE(vthost::stopReasonFrom(255).has_value());
}
