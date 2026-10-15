// SPDX-License-Identifier: Apache-2.0
//
// What an accept loop does about each failed accept: `AcceptErrorPolicy`, driven by a manual clock.
// Every case that walks several codes or several failures collects the wrong answers and asserts
// them ONCE, so a regression reads as one list rather than as a count of failed assertions.
#include <core/net/AcceptPolicy.hpp>
#include <core/net/NetError.hpp>
#include <core/platform/Clock.hpp>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstdint>
#include <format>
#include <ranges>
#include <string>
#include <string_view>
#include <tuple>
#include <vector>

using core::net::AcceptAction;
using core::net::AcceptConditionChange;
using core::net::AcceptDisposition;
using core::net::acceptDispositionOf;
using core::net::AcceptErrorPolicy;
using core::net::AcceptVerdict;
using core::net::NetErrorCode;

namespace
{

/// @return Every code in the enumeration, in declaration order.
[[nodiscard]] auto allCodes()
{
    return std::views::iota(0, static_cast<int>(NetErrorCode::Last))
           | std::views::transform([](int index) { return static_cast<NetErrorCode>(index); });
}

/// @param action An action.
/// @return Its name, for a message.
[[nodiscard]] std::string_view nameOf(AcceptAction action)
{
    switch (action)
    {
        case AcceptAction::AcceptAgain: return "accept again";
        case AcceptAction::Stop: return "stop";
        case AcceptAction::GiveUp: return "give up";
    }
    return "?";
}

/// @param verdict A verdict.
/// @return It in words, for a message.
[[nodiscard]] std::string describe(AcceptVerdict const& verdict)
{
    return std::format("{} after {} ms{}",
                       nameOf(verdict.action),
                       verdict.delay.count(),
                       verdict.warning.has_value() ? ", warning" : "");
}

} // namespace

TEST_CASE("Only a closed or dead listener ends an accept loop at once", "[net][accept-policy]")
{
    // The whole of the defect this policy exists for: a loop that ended on ANY failed accept. So
    // the set that ends a loop is asserted whole, rather than one member at a time -- a code added
    // to it is a server that can be stopped from outside.
    auto ending = std::vector<NetErrorCode> {};
    for (auto const code: allCodes())
    {
        auto const disposition = acceptDispositionOf(code);
        if (disposition == AcceptDisposition::Closed || disposition == AcceptDisposition::Dead)
            ending.push_back(code);
    }
    CHECK(ending == std::vector { NetErrorCode::Cancelled, NetErrorCode::BadHandle });

    // Exhaustion backs off without end; what nothing classifies backs off within a bound. Keying
    // the backoff on `SystemError` instead would back off on a permanently failing listener forever.
    auto wrong = std::string {};
    auto const expect = [&wrong](NetErrorCode code, AcceptDisposition want) {
        if (acceptDispositionOf(code) != want)
            wrong += std::format("{}; ", core::net::toString(code));
    };
    expect(NetErrorCode::ResourceExhausted, AcceptDisposition::Exhausted);
    expect(NetErrorCode::SystemError, AcceptDisposition::Unclassified);
    expect(NetErrorCode::ConnReset, AcceptDisposition::PeerFailed);
    // A poll timeout is one event under two names, and neither is a failure.
    expect(NetErrorCode::Timeout, AcceptDisposition::PollTick);
    expect(NetErrorCode::WouldBlock, AcceptDisposition::PollTick);
    // A value that is no code is unclassified: backed off on within a bound, never spun on.
    expect(NetErrorCode::Last, AcceptDisposition::Unclassified);
    INFO("classified wrongly: " << wrong);
    CHECK(wrong.empty());
}

TEST_CASE("An error pending on one new connection is stepped past without a backoff", "[net][accept-policy]")
{
    // Each of these is one connection's failure, so none may cost the loop a backoff, and none may
    // end it.
    auto const clock = core::platform::ManualClock {};
    auto wrong = std::string {};
    for (auto const code: { NetErrorCode::Eof,
                            NetErrorCode::ConnReset,
                            NetErrorCode::ConnRefused,
                            NetErrorCode::HostUnreach,
                            NetErrorCode::PermissionDenied,
                            NetErrorCode::Unsupported,
                            NetErrorCode::MessageTooLarge })
    {
        auto policy = AcceptErrorPolicy {};
        auto const verdict = policy.onError(code, clock.now());
        if (verdict.action != AcceptAction::AcceptAgain || verdict.delay != std::chrono::milliseconds {})
            wrong += std::format("{}: {}; ", core::net::toString(code), describe(verdict));
    }
    INFO("answered wrongly: " << wrong);
    CHECK(wrong.empty());
}

TEST_CASE("A failed connection is accepted past with a rate-limited warning", "[net][accept-policy]")
{
    auto clock = core::platform::ManualClock {};
    auto policy = AcceptErrorPolicy {};

    auto const first = policy.onError(NetErrorCode::ConnReset, clock.now());
    CHECK(first.action == AcceptAction::AcceptAgain);
    REQUIRE(first.warning.has_value());
    CHECK(first.warning->unreported == 0);

    // Inside the interval: accepted past, and not said.
    clock.advance(std::chrono::seconds { 1 });
    auto said = 0;
    for ([[maybe_unused]] auto const i: std::views::iota(0, 3))
        if (policy.onError(NetErrorCode::ConnReset, clock.now()).warning.has_value())
            ++said;
    CHECK(said == 0);

    // Past it: said again, carrying what it stands for.
    clock.advance(AcceptErrorPolicy::WarnInterval);
    auto const later = policy.onError(NetErrorCode::ConnReset, clock.now());
    REQUIRE(later.warning.has_value());
    CHECK(later.warning->unreported == 3);
    CHECK(core::net::describeAcceptFailure(
              "http", core::net::makeNetError(NetErrorCode::ConnReset, 0, "AcceptEx"), later)
          == "http: an accept failed (connection reset (AcceptEx)); accepting again (3 more since the last "
             "warning)");
}

TEST_CASE("An exhausted accept backs off within a bound, without end, and an accept resets it",
          "[net][accept-policy]")
{
    auto const clock = core::platform::ManualClock {};
    auto policy = AcceptErrorPolicy {};

    auto delays = std::vector<std::int64_t> {};
    for ([[maybe_unused]] auto const i: std::views::iota(0, 9))
        delays.push_back(policy.onError(NetErrorCode::ResourceExhausted, clock.now()).delay.count());
    CHECK(delays == std::vector<std::int64_t> { 10, 20, 40, 80, 160, 320, 640, 1000, 1000 });

    // Exhaustion is transient by definition: it never ends the loop, and is never reported as a
    // degraded one, however long it lasts.
    auto otherwise = std::string {};
    for (auto const i:
         std::views::iota(0, static_cast<int>(AcceptErrorPolicy::UnclassifiedBeforeDegraded) * 4))
    {
        auto const verdict = policy.onError(NetErrorCode::ResourceExhausted, clock.now());
        if (otherwise.empty()
            && (verdict.action != AcceptAction::AcceptAgain || verdict.change != AcceptConditionChange::None))
            otherwise = std::format("failure {}: {}", i, describe(verdict));
    }
    INFO(otherwise);
    CHECK(otherwise.empty());

    std::ignore = policy.onAccepted(clock.now());
    CHECK(policy.onError(NetErrorCode::ResourceExhausted, clock.now()).delay
          == AcceptErrorPolicy::FirstBackoff);

    // A per-connection failure never waits, whatever an exhaustion before it did.
    std::ignore = policy.onAccepted(clock.now());
    CHECK(policy.onError(NetErrorCode::ConnReset, clock.now()).delay == std::chrono::milliseconds {});
}

TEST_CASE("An unclassified failure is backed off on without end, and a long run is reported degraded once",
          "[net][accept-policy]")
{
    // What lands in `SystemError` is what nobody anticipated -- `WSAENETDOWN` while an adapter
    // resets, a completion nobody mapped -- and ending the loop on it would make a transient
    // condition a permanent outage. So it is backed off on for as long as it lasts, and a long run
    // is REPORTED, once, with how long it has lasted.
    auto clock = core::platform::ManualClock {};
    auto policy = AcceptErrorPolicy {};
    auto const bound = AcceptErrorPolicy::UnclassifiedBeforeDegraded;

    auto wrong = std::string {};
    auto degradedAt = std::uint32_t { 0 };
    auto reported = AcceptVerdict {};
    for (auto const failure: std::views::iota(std::uint32_t { 1 }, bound * 3))
    {
        auto const verdict = policy.onError(NetErrorCode::SystemError, clock.now());
        if (verdict.action != AcceptAction::AcceptAgain || verdict.delay == std::chrono::milliseconds {})
            wrong += std::format("failure {}: {}; ", failure, describe(verdict));
        if (verdict.change == AcceptConditionChange::Degraded)
        {
            if (degradedAt != 0)
                wrong += std::format("reported degraded again at {}; ", failure);
            degradedAt = failure;
            reported = verdict;
        }
        else if (verdict.change != AcceptConditionChange::None)
            wrong += std::format("failure {}: a change other than degraded; ", failure);
        clock.advance(verdict.delay);
    }
    if (degradedAt != bound)
        wrong += std::format("reported degraded at failure {}, expected {}; ", degradedAt, bound);
    if (reported.streak.failures != bound || reported.streak.code != NetErrorCode::SystemError
        || reported.streak.lasted <= std::chrono::seconds { 20 })
        wrong += std::format(
            "the run reported was {} failures over {} ms; ",
            reported.streak.failures,
            std::chrono::duration_cast<std::chrono::milliseconds>(reported.streak.lasted).count());
    INFO(wrong);
    CHECK(wrong.empty());

    // An accept ends the run: the recovery is reported, with the whole run, and the next long run
    // is reported again.
    auto const recovered = policy.onAccepted(clock.now());
    CHECK(recovered.change == AcceptConditionChange::Recovered);
    CHECK(recovered.streak.failures == (bound * 3) - 1);
    auto again = std::uint32_t { 0 };
    for (auto const failure: std::views::iota(std::uint32_t { 1 }, bound + 1))
        if (policy.onError(NetErrorCode::SystemError, clock.now()).change == AcceptConditionChange::Degraded)
            again = failure;
    CHECK(again == bound);
}

TEST_CASE("A run of unclassified failures is ended by any sign the listener is dequeuing",
          "[net][accept-policy]")
{
    // An accept, or a connection the listener dequeued and that then failed, says the listener is
    // alive: the run starts again, so the degraded report needs a run of `UnclassifiedBeforeDegraded`
    // IN A ROW. A run that was never reported recovers silently; one that was says so.
    auto const clock = core::platform::ManualClock {};
    auto const bound = AcceptErrorPolicy::UnclassifiedBeforeDegraded;
    auto wrong = std::string {};
    for (auto const evidence: { 0, 1 })
    {
        auto const name = evidence == 0 ? "an accept" : "a failed connection";
        auto const end = [&](AcceptErrorPolicy& policy) {
            return evidence == 0 ? policy.onAccepted(clock.now())
                                 : policy.onError(NetErrorCode::ConnReset, clock.now());
        };
        auto policy = AcceptErrorPolicy {};
        for ([[maybe_unused]] auto const i: std::views::iota(std::uint32_t { 1 }, bound))
            std::ignore = policy.onError(NetErrorCode::SystemError, clock.now());
        if (end(policy).change != AcceptConditionChange::None)
            wrong += std::format("{} after an unreported run reported a change; ", name);
        if (policy.onError(NetErrorCode::SystemError, clock.now()).change != AcceptConditionChange::None)
            wrong += std::format("{} did not start the run again; ", name);

        auto degraded = AcceptErrorPolicy {};
        for ([[maybe_unused]] auto const i: std::views::iota(std::uint32_t { 0 }, bound))
            std::ignore = degraded.onError(NetErrorCode::SystemError, clock.now());
        if (end(degraded).change != AcceptConditionChange::Recovered)
            wrong += std::format("{} after a reported run did not report the recovery; ", name);
    }
    INFO(wrong);
    CHECK(wrong.empty());
}

TEST_CASE("Answers that wait for nothing make the loop yield, and never back off", "[net][accept-policy]")
{
    // A listener answering a per-connection code, or its own poll tick, without suspending would
    // never yield its loop. But each failed connection consumed a queued one, so the answer is a
    // constant YIELD: a backoff that doubled held a port refusing for as long as a flood's backlog
    // of dead connections took to drain at one a second.
    auto const clock = core::platform::ManualClock {};
    auto wrong = std::string {};
    for (auto const code: { NetErrorCode::ConnReset, NetErrorCode::WouldBlock, NetErrorCode::Timeout })
    {
        auto policy = AcceptErrorPolicy {};
        auto const expect = [&](int round, std::uint32_t failure, std::chrono::milliseconds want) {
            auto const verdict = policy.onError(code, clock.now());
            if (verdict.delay != want || verdict.action != AcceptAction::AcceptAgain)
                wrong += std::format("{} round {} answer {}: {}, expected {} ms; ",
                                     core::net::toString(code),
                                     round,
                                     failure,
                                     describe(verdict),
                                     want.count());
        };
        for (auto const round: { 1, 2, 3 })
        {
            for (auto const failure:
                 std::views::iota(std::uint32_t { 1 }, AcceptErrorPolicy::FailuresBeforeYield))
                expect(round, failure, std::chrono::milliseconds {});
            // The same short yield every time: it does not grow.
            expect(round, AcceptErrorPolicy::FailuresBeforeYield, AcceptErrorPolicy::FirstBackoff);
        }
        // One accept and the count starts again.
        std::ignore = policy.onAccepted(clock.now());
        expect(4, 1, std::chrono::milliseconds {});
    }
    INFO(wrong);
    CHECK(wrong.empty());
}

TEST_CASE("A closed listener stops quietly, a dead one gives up, and a poll tick says nothing",
          "[net][accept-policy]")
{
    auto const clock = core::platform::ManualClock {};
    auto policy = AcceptErrorPolicy {};

    auto wrong = std::string {};
    auto const expect = [&](NetErrorCode code, AcceptAction want) {
        auto const verdict = policy.onError(code, clock.now());
        if (verdict.action != want || verdict.delay != std::chrono::milliseconds {}
            || verdict.warning.has_value())
            wrong += std::format("{}: {}, expected {} at once and quietly; ",
                                 core::net::toString(code),
                                 describe(verdict),
                                 nameOf(want));
    };
    expect(NetErrorCode::Cancelled, AcceptAction::Stop);
    expect(NetErrorCode::BadHandle, AcceptAction::GiveUp);
    expect(NetErrorCode::WouldBlock, AcceptAction::AcceptAgain);
    expect(NetErrorCode::Timeout, AcceptAction::AcceptAgain);
    INFO(wrong);
    CHECK(wrong.empty());

    CHECK(core::net::describeAcceptLoopEnded("http",
                                             core::net::makeNetError(NetErrorCode::BadHandle, 0, "accept"))
          == "http: accept loop ended (bad handle (accept)) on a listener that was not closed; it accepts no "
             "more "
             "connections");
}
