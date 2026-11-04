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
constexpr auto WakeT0 = core::platform::SteadyTimePoint {} + 100s;

auto makeWakeTracker() -> LivenessTracker
{
    auto t =
        LivenessTracker { { .idleBeforeProbe = 5s, .deadAfter = 15s, .awakeGap = 5s, .wakeDeadline = 5s } };
    t.noteInbound(WakeT0);
    return t;
}
} // namespace

TEST_CASE("a tick late by a sleep probes instead of giving up", "[vthost][liveness]")
{
    auto t = makeWakeTracker();
    t.noteTick(WakeT0 + 1s, WakeT0 + 1000s);
    CHECK(t.decide(WakeT0 + 1000s) == LivenessAction::SendHeartbeat);
    t.noteProbeSent(WakeT0 + 1000s);
    CHECK(t.decide(WakeT0 + 1004s) == LivenessAction::None);
    CHECK(t.decide(WakeT0 + 1005s) == LivenessAction::DeclareLost);
    CHECK(t.lossLimit(WakeT0 + 1005s) == 5s);
}

TEST_CASE("an answer after a wake restores the normal limits", "[vthost][liveness]")
{
    auto t = makeWakeTracker();
    t.noteTick(WakeT0 + 1s, WakeT0 + 1000s);
    t.noteProbeSent(WakeT0 + 1000s);
    t.noteProbeAnswered(WakeT0 + 1001s);
    CHECK(t.lossLimit(WakeT0 + 1001s) == 15s);
    CHECK(t.decide(WakeT0 + 1005s) == LivenessAction::None);
    CHECK(t.decide(WakeT0 + 1006s) == LivenessAction::SendHeartbeat);
}

TEST_CASE("an on-time tick changes nothing", "[vthost][liveness]")
{
    auto t = makeWakeTracker();
    t.noteTick(WakeT0 + 1s, WakeT0 + 1s + 100ms);
    CHECK(t.decide(WakeT0 + 2s) == LivenessAction::None);
}

TEST_CASE("an announced sleep silences the verdict until the wake", "[vthost][liveness]")
{
    auto t = makeWakeTracker();
    t.noteSleep(WakeT0 + 1s);
    CHECK(t.decide(WakeT0 + 500s) == LivenessAction::None);
    t.noteWake(WakeT0 + 500s);
    CHECK(t.decide(WakeT0 + 500s) == LivenessAction::SendHeartbeat);
}

TEST_CASE("a sleep announced and never taken does not disable liveness", "[vthost][liveness]")
{
    auto t = makeWakeTracker();
    t.noteSleep(WakeT0 + 1s);
    for (auto const n: std::views::iota(2, 36))
        t.noteTick(WakeT0 + std::chrono::seconds { n }, WakeT0 + std::chrono::seconds { n });
    // The tick at +35 s is 34 s into the announced sleep, short of announcedSleepGrace: still asleep, as an
    // OS may take its 30 s to acknowledge a sleep before taking it.
    CHECK(t.decide(WakeT0 + 35s) == LivenessAction::None);
    for (auto const n: std::views::iota(36, 38))
        t.noteTick(WakeT0 + std::chrono::seconds { n }, WakeT0 + std::chrono::seconds { n });
    CHECK(t.decide(WakeT0 + 37s) == LivenessAction::SendHeartbeat);
}

TEST_CASE("a frame arriving during an announced sleep does not end it", "[vthost][liveness]")
{
    auto t = makeWakeTracker();
    t.noteSleep(WakeT0 + 1s);
    t.noteInbound(WakeT0 + 2s);
    // Awake, 498 s of silence would be a loss; asleep, there is no verdict at all.
    CHECK(t.decide(WakeT0 + 500s) == LivenessAction::None);
}

TEST_CASE("a late tick during an announced sleep enters the wake state", "[vthost][liveness]")
{
    auto t = makeWakeTracker();
    t.noteSleep(WakeT0 + 1s);
    t.noteTick(WakeT0 + 2s, WakeT0 + 500s);
    CHECK(t.decide(WakeT0 + 500s) == LivenessAction::SendHeartbeat);
    CHECK(t.lossLimit(WakeT0 + 500s) == 5s);
}

TEST_CASE("a wake forgets a probe sent before the sleep", "[vthost][liveness]")
{
    auto t = makeWakeTracker();
    REQUIRE(t.decide(WakeT0 + 5s) == LivenessAction::SendHeartbeat);
    t.noteProbeSent(WakeT0 + 5s);
    t.noteSleep(WakeT0 + 6s);
    t.noteWake(WakeT0 + 500s);
    // The probe out was sent on a connection the sleep may have killed: the wake probes afresh.
    CHECK(t.decide(WakeT0 + 500s) == LivenessAction::SendHeartbeat);
}

TEST_CASE("a second wake while waking restarts the wake deadline", "[vthost][liveness]")
{
    auto t = makeWakeTracker();
    t.noteWake(WakeT0 + 500s);
    t.noteProbeSent(WakeT0 + 500s);
    t.noteWake(WakeT0 + 503s);
    CHECK(t.decide(WakeT0 + 503s) == LivenessAction::SendHeartbeat);
    t.noteProbeSent(WakeT0 + 503s);
    CHECK(t.decide(WakeT0 + 507s) == LivenessAction::None); // past the first wake's deadline
    CHECK(t.decide(WakeT0 + 508s) == LivenessAction::DeclareLost);
}

TEST_CASE("a frame read while waking does not end the wake, but restarts its silence", "[vthost][liveness]")
{
    auto t = makeWakeTracker();
    t.noteWake(WakeT0 + 500s);
    REQUIRE(t.decide(WakeT0 + 500s) == LivenessAction::SendHeartbeat);
    t.noteProbeSent(WakeT0 + 500s);
    // Buffered before the sleep, read after it: it proves nothing about the connection now, so it neither
    // ends the wake nor clears the probe -- it only restarts the silence the wake deadline measures.
    t.noteInbound(WakeT0 + 503s);
    CHECK(t.lossLimit(WakeT0 + 503s) == 5s);
    CHECK(t.decide(WakeT0 + 507s) == LivenessAction::None);
    CHECK(t.decide(WakeT0 + 508s) == LivenessAction::DeclareLost); // 5 s after the frame
}

TEST_CASE("the wake probe's echo ends the wake", "[vthost][liveness]")
{
    auto t = makeWakeTracker();
    t.noteWake(WakeT0 + 500s);
    t.noteProbeSent(WakeT0 + 500s);
    t.noteProbeAnswered(WakeT0 + 502s);
    CHECK(t.lossLimit(WakeT0 + 502s) == 15s);
    CHECK(t.decide(WakeT0 + 506s) == LivenessAction::None);
    CHECK(t.decide(WakeT0 + 507s) == LivenessAction::SendHeartbeat); // idle again, from the echo
    // Awake again, any frame is life once more.
    t.noteProbeSent(WakeT0 + 507s);
    t.noteInbound(WakeT0 + 508s);
    CHECK(t.decide(WakeT0 + 512s) == LivenessAction::None);
}

TEST_CASE("a busy link keeps a waking connection alive until the cap", "[vthost][liveness]")
{
    auto t = makeWakeTracker();
    t.noteWake(WakeT0 + 500s);
    REQUIRE(t.decide(WakeT0 + 500s) == LivenessAction::SendHeartbeat);
    t.noteProbeSent(WakeT0 + 500s);
    for (auto const n: std::views::iota(1, 13)) // a frame every second, the echo stuck behind them
    {
        t.noteInbound(WakeT0 + 500s + std::chrono::seconds { n });
        CHECK(t.decide(WakeT0 + 500s + std::chrono::seconds { n }) == LivenessAction::None);
    }
    CHECK(t.decide(WakeT0 + 514s) == LivenessAction::None); // still within the 5 s after the frame at +12 s
    t.noteInbound(WakeT0 + 514s);
    CHECK(t.decide(WakeT0 + 514s) == LivenessAction::None);
    CHECK(t.decide(WakeT0 + 515s) == LivenessAction::DeclareLost); // deadAfter since the wake, traffic or not
    CHECK(t.lossLimit(WakeT0 + 515s) == 15s);                      // the cap fired, not the 5 s of silence
}

TEST_CASE("a stale burst at the wake then silence is lost at the wake deadline", "[vthost][liveness]")
{
    auto t = makeWakeTracker();
    t.noteWake(WakeT0 + 500s);
    t.noteProbeSent(WakeT0 + 500s);
    t.noteInbound(WakeT0 + 500s);
    t.noteInbound(WakeT0 + 500s + 20ms);
    CHECK(t.decide(WakeT0 + 504s) == LivenessAction::None);
    CHECK(t.decide(WakeT0 + 505s + 20ms) == LivenessAction::DeclareLost);
}

TEST_CASE("traffic then the echo ends the wake and the normal rules apply", "[vthost][liveness]")
{
    auto t = makeWakeTracker();
    t.noteWake(WakeT0 + 500s);
    t.noteProbeSent(WakeT0 + 500s);
    t.noteInbound(WakeT0 + 502s);
    t.noteProbeAnswered(WakeT0 + 504s);
    CHECK(t.lossLimit(WakeT0 + 504s) == 15s);
    CHECK(t.decide(WakeT0 + 508s) == LivenessAction::None);
    CHECK(t.decide(WakeT0 + 509s) == LivenessAction::SendHeartbeat);
    t.noteProbeSent(WakeT0 + 509s);
    CHECK(t.decide(WakeT0 + 518s) == LivenessAction::None); // past the cap of the old wake, no longer applied
}

TEST_CASE("inbound while waking leaves the probe outstanding", "[vthost][liveness]")
{
    auto t = makeWakeTracker();
    t.noteWake(WakeT0 + 500s);
    REQUIRE(t.decide(WakeT0 + 500s) == LivenessAction::SendHeartbeat);
    t.noteProbeSent(WakeT0 + 500s);
    t.noteInbound(WakeT0 + 501s);
    CHECK(t.decide(WakeT0 + 501s) == LivenessAction::None);
    CHECK(t.decide(WakeT0 + 505s) == LivenessAction::None); // no second heartbeat, nor loss yet
}
