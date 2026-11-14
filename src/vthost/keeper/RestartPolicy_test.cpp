// SPDX-License-Identifier: Apache-2.0
#include <catch2/catch_test_macros.hpp>

#include <array>
#include <chrono>
#include <ranges>

#include <vthost/keeper/RestartPolicy.hpp>

using namespace std::chrono_literals;
using namespace vthost::keeper;

namespace
{

constexpr auto Epoch = core::platform::SteadyTimePoint {};

constexpr core::platform::SteadyTimePoint at(std::chrono::milliseconds offset) noexcept
{
    return Epoch + offset;
}

} // namespace

TEST_CASE("every daemon end state classifies as one kind of exit", "[keeper][restart]")
{
    struct Row
    {
        DaemonEndState state;
        DaemonExitKind kind;
    };
    constexpr auto Table = std::array {
        Row { DaemonEndState::StartingFirst, DaemonExitKind::FailedToStart },
        Row { DaemonEndState::StartingRestart, DaemonExitKind::Crashed },
        Row { DaemonEndState::Serving, DaemonExitKind::Crashed },
        Row { DaemonEndState::ShutdownAnnounced, DaemonExitKind::ShutDown },
        Row { DaemonEndState::StopForwarded, DaemonExitKind::ShutDown },
        Row { DaemonEndState::HungKilled, DaemonExitKind::Hung },
        Row { DaemonEndState::FirstStartKilled, DaemonExitKind::FailedToStart },
    };
    for (auto const& row: Table)
        CHECK(classifyExit(row.state) == row.kind);
}

TEST_CASE("a forwarded stop is a shutdown", "[keeper][restart]")
{
    auto policy = CrashRestartPolicy { {} };
    auto const decision = policy.onExit(classifyExit(DaemonEndState::StopForwarded), at(0ms));
    CHECK(decision.action == RestartAction::ExitClean);
}

TEST_CASE("a first start that never served exits with its status", "[keeper][restart]")
{
    auto policy = CrashRestartPolicy { {} };
    auto const decision = policy.onExit(classifyExit(DaemonEndState::StartingFirst), at(0ms));
    CHECK(decision.action == RestartAction::ExitWithStatus);
}

TEST_CASE("a restart that dies before serving is a crash, not a failed start", "[keeper][restart]")
{
    auto policy = CrashRestartPolicy { {} };
    auto const decision = policy.onExit(classifyExit(DaemonEndState::StartingRestart), at(0ms));
    CHECK(decision.action == RestartAction::RestartNow);
}

TEST_CASE("a hung daemon counts as a crash", "[keeper][restart]")
{
    // The keeper kills a silent daemon and records HungKilled: that is a Hung exit, and it counts as a crash.
    CHECK(classifyExit(DaemonEndState::HungKilled) == DaemonExitKind::Hung);
    auto policyInUse = CrashRestartPolicy { {} };
    CHECK(policyInUse.onExit(DaemonExitKind::Hung, at(0ms)).action == RestartAction::RestartNow);
    auto const second = policyInUse.onExit(DaemonExitKind::Hung, at(1000ms));
    CHECK(second.action == RestartAction::RestartAfter);
}

TEST_CASE("the first crash restarts at once", "[keeper][restart]")
{
    auto policy = CrashRestartPolicy { {} };
    auto const decision = policy.onExit(DaemonExitKind::Crashed, at(0ms));
    CHECK(decision.action == RestartAction::RestartNow);
    CHECK(decision.delay == 0ms);
}

TEST_CASE("a crash after a quiet spell restarts at once again", "[keeper][restart]")
{
    auto policy = CrashRestartPolicy { {} };
    CHECK(policy.onExit(DaemonExitKind::Crashed, at(0ms)).action == RestartAction::RestartNow);
    CHECK(policy.onExit(DaemonExitKind::Crashed, at(31'000ms)).action == RestartAction::RestartNow);
}

TEST_CASE("a second crash within 30 s backs off 1 s, then 2 s, capped at 30 s", "[keeper][restart]")
{
    auto policy = CrashRestartPolicy { RestartPolicyConfig { .giveUpCrashes = 100 } };
    CHECK(policy.onExit(DaemonExitKind::Crashed, at(0ms)).action == RestartAction::RestartNow);

    auto now = 1000ms;
    for (auto const expected: { 1000ms, 2000ms, 4000ms, 8000ms, 16'000ms, 30'000ms, 30'000ms })
    {
        auto const decision = policy.onExit(DaemonExitKind::Crashed, at(now));
        CHECK(decision.action == RestartAction::RestartAfter);
        CHECK(decision.delay == expected);
        now += 1000ms; // each new crash lands soon after the last, so the run stays "quick"
    }
}

TEST_CASE("the fifth crash in five minutes gives up", "[keeper][restart]")
{
    auto policy = CrashRestartPolicy { {} };
    for (auto const second: { 0, 10, 20, 30 })
        CHECK(policy.onExit(DaemonExitKind::Crashed, at(std::chrono::seconds { second })).action
              != RestartAction::GiveUp);
    auto const decision = policy.onExit(DaemonExitKind::Crashed, at(40s));
    CHECK(decision.action == RestartAction::GiveUp);
    CHECK(decision.delay == 120s); // the grace period the keeper waits before the one retry
}

TEST_CASE("crashes spread over more than five minutes do not give up", "[keeper][restart]")
{
    auto policy = CrashRestartPolicy { {} };
    for (auto const index: std::views::iota(0, 20))
        CHECK(policy.onExit(DaemonExitKind::Crashed, at(std::chrono::seconds { index * 100 })).action
              != RestartAction::GiveUp);
}

TEST_CASE("after 2 minutes of giving up the newest image is tried once", "[keeper][restart]")
{
    auto policy = CrashRestartPolicy { RestartPolicyConfig { .giveUpCrashes = 2 } };
    (void) policy.onExit(DaemonExitKind::Crashed, at(0ms));
    CHECK(policy.onExit(DaemonExitKind::Crashed, at(1000ms)).action == RestartAction::GiveUp);
    CHECK(policy.onGraceElapsed(at(121'000ms)).action == RestartAction::RetryNewestImage);
}

TEST_CASE("a retry that crashes again ends everything", "[keeper][restart]")
{
    auto policy = CrashRestartPolicy { RestartPolicyConfig { .giveUpCrashes = 2 } };
    (void) policy.onExit(DaemonExitKind::Crashed, at(0ms));
    (void) policy.onExit(DaemonExitKind::Crashed, at(1000ms));
    CHECK(policy.onGraceElapsed(at(121'000ms)).action == RestartAction::RetryNewestImage);
    CHECK(policy.onExit(DaemonExitKind::Crashed, at(125'000ms)).action == RestartAction::EndAll);
}

TEST_CASE("a second grace period ends everything instead of retrying twice", "[keeper][restart]")
{
    auto policy = CrashRestartPolicy { RestartPolicyConfig { .giveUpCrashes = 2 } };
    (void) policy.onExit(DaemonExitKind::Crashed, at(0ms));
    (void) policy.onExit(DaemonExitKind::Crashed, at(1000ms));
    CHECK(policy.onGraceElapsed(at(121'000ms)).action == RestartAction::RetryNewestImage);
    CHECK(policy.onGraceElapsed(at(241'000ms)).action == RestartAction::EndAll);
}

TEST_CASE("5 stable minutes reset the count", "[keeper][restart]")
{
    auto policy = CrashRestartPolicy { {} };
    for (auto const second: { 0, 1, 2, 3 })
        (void) policy.onExit(DaemonExitKind::Crashed, at(std::chrono::seconds { second }));
    policy.onStable(at(400s));
    // Four more in quick succession: the earlier four no longer count towards giving up.
    CHECK(policy.onExit(DaemonExitKind::Crashed, at(401s)).action == RestartAction::RestartNow);
    for (auto const second: { 402, 403, 404 })
        CHECK(policy.onExit(DaemonExitKind::Crashed, at(std::chrono::seconds { second })).action
              == RestartAction::RestartAfter);
}

TEST_CASE("a retry that stays up clears the given-up state", "[keeper][restart]")
{
    auto policy = CrashRestartPolicy { RestartPolicyConfig { .giveUpCrashes = 2 } };
    (void) policy.onExit(DaemonExitKind::Crashed, at(0ms));
    (void) policy.onExit(DaemonExitKind::Crashed, at(1000ms));
    CHECK(policy.onGraceElapsed(at(121'000ms)).action == RestartAction::RetryNewestImage);
    policy.onStable(at(421'000ms));
    CHECK(policy.onExit(DaemonExitKind::Crashed, at(500'000ms)).action == RestartAction::RestartNow);
}

TEST_CASE("a clean end is never restarted", "[keeper][restart]")
{
    auto policy = CrashRestartPolicy { {} };
    CHECK(policy.onExit(DaemonExitKind::ShutDown, at(0ms)).action == RestartAction::ExitClean);
}

TEST_CASE("a stale grace timer after the policy recovered is ignored", "[keeper][restart]")
{
    auto policy = CrashRestartPolicy { RestartPolicyConfig { .giveUpCrashes = 2 } };
    CHECK(policy.onGraceElapsed(at(0ms)).action == RestartAction::Ignore);
    (void) policy.onExit(DaemonExitKind::Crashed, at(0ms));
    (void) policy.onExit(DaemonExitKind::Crashed, at(1000ms));
    policy.onStable(at(400s));
    CHECK(policy.onGraceElapsed(at(401s)).action == RestartAction::Ignore);
}
