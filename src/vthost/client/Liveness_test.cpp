// SPDX-License-Identifier: Apache-2.0
#include <catch2/catch_test_macros.hpp>

#include <chrono>

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
