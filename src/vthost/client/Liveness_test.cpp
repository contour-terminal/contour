// SPDX-License-Identifier: Apache-2.0
#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <ranges>

#include <vthost/client/Liveness.hpp>

using namespace std::chrono_literals;
using vthost::client::LivenessAction;
using vthost::client::LivenessTracker;

TEST_CASE("liveness probes an idle connection and gives up on a silent one", "[vthost][liveness]")
{
    auto const t0 = core::platform::SteadyTimePoint {} + 100s;
    auto t = LivenessTracker { { .idleBeforeProbe = 5s, .deadAfter = 15s } };
    CHECK(t.decide(t0) == LivenessAction::None); // nothing arrived yet
    t.noteInbound(t0);
    CHECK(t.decide(t0 + 4s) == LivenessAction::None);
    CHECK(t.decide(t0 + 5s) == LivenessAction::SendHeartbeat);
    t.noteProbeSent(t0 + 5s);
    CHECK(t.decide(t0 + 6s) == LivenessAction::None);
    CHECK(t.decide(t0 + 15s) == LivenessAction::DeclareLost);
    t.noteInbound(t0 + 14s);
    CHECK(t.decide(t0 + 15s) == LivenessAction::None);
    CHECK(t.decide(t0 + 19s) == LivenessAction::SendHeartbeat); // the echo answered the probe out
}

namespace
{
auto const wakeT0 = core::platform::SteadyTimePoint {} + 100s;

auto makeWakeTracker() -> LivenessTracker
{
    auto t =
        LivenessTracker { { .idleBeforeProbe = 5s, .deadAfter = 15s, .awakeGap = 5s, .wakeDeadline = 5s } };
    t.noteInbound(wakeT0);
    return t;
}
} // namespace

TEST_CASE("a tick late by a sleep probes instead of giving up", "[vthost][liveness]")
{
    auto t = makeWakeTracker();
    t.noteTick(wakeT0 + 1s, wakeT0 + 1000s);
    CHECK(t.decide(wakeT0 + 1000s) == LivenessAction::SendHeartbeat);
    t.noteProbeSent(wakeT0 + 1000s);
    CHECK(t.decide(wakeT0 + 1004s) == LivenessAction::None);
    CHECK(t.decide(wakeT0 + 1005s) == LivenessAction::DeclareLost);
    CHECK(t.silenceLimit() == 5s);
}

TEST_CASE("an answer after a wake restores the normal limits", "[vthost][liveness]")
{
    auto t = makeWakeTracker();
    t.noteTick(wakeT0 + 1s, wakeT0 + 1000s);
    t.noteProbeSent(wakeT0 + 1000s);
    t.noteInbound(wakeT0 + 1001s);
    CHECK(t.silenceLimit() == 15s);
    CHECK(t.decide(wakeT0 + 1005s) == LivenessAction::None);
    CHECK(t.decide(wakeT0 + 1006s) == LivenessAction::SendHeartbeat);
}

TEST_CASE("an on-time tick changes nothing", "[vthost][liveness]")
{
    auto t = makeWakeTracker();
    t.noteTick(wakeT0 + 1s, wakeT0 + 1s + 100ms);
    CHECK(t.decide(wakeT0 + 2s) == LivenessAction::None);
}

TEST_CASE("an announced sleep silences the verdict until the wake", "[vthost][liveness]")
{
    auto t = makeWakeTracker();
    t.noteSleep(wakeT0 + 1s);
    CHECK(t.decide(wakeT0 + 500s) == LivenessAction::None);
    t.noteWake(wakeT0 + 500s);
    CHECK(t.decide(wakeT0 + 500s) == LivenessAction::SendHeartbeat);
}

TEST_CASE("a sleep announced and never taken does not disable liveness", "[vthost][liveness]")
{
    auto t = makeWakeTracker();
    t.noteSleep(wakeT0 + 1s);
    for (auto const n: std::views::iota(2, 8))
        t.noteTick(wakeT0 + std::chrono::seconds { n }, wakeT0 + std::chrono::seconds { n });
    CHECK(t.decide(wakeT0 + 7s) == LivenessAction::SendHeartbeat);
}
