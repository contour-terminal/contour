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
constexpr auto wakeT0 = core::platform::SteadyTimePoint {} + 100s;

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
    t.noteProbeAnswered(wakeT0 + 1001s);
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
    for (auto const n: std::views::iota(2, 36))
        t.noteTick(wakeT0 + std::chrono::seconds { n }, wakeT0 + std::chrono::seconds { n });
    // The tick at +35 s is 34 s into the announced sleep, short of announcedSleepGrace: still asleep, as an
    // OS may take its 30 s to acknowledge a sleep before taking it.
    CHECK(t.decide(wakeT0 + 35s) == LivenessAction::None);
    for (auto const n: std::views::iota(36, 38))
        t.noteTick(wakeT0 + std::chrono::seconds { n }, wakeT0 + std::chrono::seconds { n });
    CHECK(t.decide(wakeT0 + 37s) == LivenessAction::SendHeartbeat);
}

TEST_CASE("a frame arriving during an announced sleep does not end it", "[vthost][liveness]")
{
    auto t = makeWakeTracker();
    t.noteSleep(wakeT0 + 1s);
    t.noteInbound(wakeT0 + 2s);
    // Awake, 498 s of silence would be a loss; asleep, there is no verdict at all.
    CHECK(t.decide(wakeT0 + 500s) == LivenessAction::None);
}

TEST_CASE("a late tick during an announced sleep enters the wake state", "[vthost][liveness]")
{
    auto t = makeWakeTracker();
    t.noteSleep(wakeT0 + 1s);
    t.noteTick(wakeT0 + 2s, wakeT0 + 500s);
    CHECK(t.decide(wakeT0 + 500s) == LivenessAction::SendHeartbeat);
    CHECK(t.silenceLimit() == 5s);
}

TEST_CASE("a wake forgets a probe sent before the sleep", "[vthost][liveness]")
{
    auto t = makeWakeTracker();
    REQUIRE(t.decide(wakeT0 + 5s) == LivenessAction::SendHeartbeat);
    t.noteProbeSent(wakeT0 + 5s);
    t.noteSleep(wakeT0 + 6s);
    t.noteWake(wakeT0 + 500s);
    // The probe out was sent on a connection the sleep may have killed: the wake probes afresh.
    CHECK(t.decide(wakeT0 + 500s) == LivenessAction::SendHeartbeat);
}

TEST_CASE("a second wake while waking restarts the wake deadline", "[vthost][liveness]")
{
    auto t = makeWakeTracker();
    t.noteWake(wakeT0 + 500s);
    t.noteProbeSent(wakeT0 + 500s);
    t.noteWake(wakeT0 + 503s);
    CHECK(t.decide(wakeT0 + 503s) == LivenessAction::SendHeartbeat);
    t.noteProbeSent(wakeT0 + 503s);
    CHECK(t.decide(wakeT0 + 507s) == LivenessAction::None); // past the first wake's deadline
    CHECK(t.decide(wakeT0 + 508s) == LivenessAction::DeclareLost);
}

TEST_CASE("a frame read while waking neither ends the wake nor restarts its deadline", "[vthost][liveness]")
{
    auto t = makeWakeTracker();
    t.noteWake(wakeT0 + 500s);
    REQUIRE(t.decide(wakeT0 + 500s) == LivenessAction::SendHeartbeat);
    t.noteProbeSent(wakeT0 + 500s);
    // Buffered before the sleep, read after it: it proves nothing about the connection now.
    t.noteInbound(wakeT0 + 503s);
    CHECK(t.silenceLimit() == 5s);
    CHECK(t.decide(wakeT0 + 504s) == LivenessAction::None);
    CHECK(t.decide(wakeT0 + 505s) == LivenessAction::DeclareLost); // measured from the wake, not the frame
}

TEST_CASE("the wake probe's echo ends the wake", "[vthost][liveness]")
{
    auto t = makeWakeTracker();
    t.noteWake(wakeT0 + 500s);
    t.noteProbeSent(wakeT0 + 500s);
    t.noteProbeAnswered(wakeT0 + 502s);
    CHECK(t.silenceLimit() == 15s);
    CHECK(t.decide(wakeT0 + 506s) == LivenessAction::None);
    CHECK(t.decide(wakeT0 + 507s) == LivenessAction::SendHeartbeat); // idle again, from the echo
    // Awake again, any frame is life once more.
    t.noteProbeSent(wakeT0 + 507s);
    t.noteInbound(wakeT0 + 508s);
    CHECK(t.decide(wakeT0 + 512s) == LivenessAction::None);
}
