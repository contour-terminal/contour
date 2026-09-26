// SPDX-License-Identifier: Apache-2.0
#include <core/async/Cancellation.hpp>
#include <core/async/Task.hpp>
#include <core/net/ConnectFlow.hpp>
#include <core/net/EventLoop.hpp>
#include <core/net/IAsyncAddressResolver.hpp>
#include <core/net/IConnector.hpp>
#include <core/net/SocketAddress.hpp>
#include <core/net/SocketBuffers.hpp>
#include <core/net/detail/StreamSocketOptions.hpp>
#include <core/net/testing/InMemorySocket.hpp>
#include <core/net/testing/TestLoop.hpp>
#include <core/platform/Clock.hpp>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <format>
#include <memory>
#include <optional>
#include <ranges>
#include <string>
#include <utility>
#include <vector>

using core::async::Task;
using core::net::DialOptions;
using core::net::KeepAlive;
using core::net::NetErrorCode;
using core::net::ResolvedEndpoint;
using core::net::ResolveResult;
using core::net::SocketResult;
using core::net::detail::runConnectFlow;
using core::platform::SteadyTimePoint;

namespace
{

/// A resolver with a script and a cost, and no DNS behind it.
///
/// The cost is what makes the total-budget rule a unit test rather than a sleep: resolution
/// advances the injected @c ManualClock by exactly as much as the case says it does.
class ScriptedAsyncResolver final: public core::net::IAsyncAddressResolver
{
  public:
    ScriptedAsyncResolver(core::platform::ManualClock* clock, std::size_t candidates) noexcept:
        _clock(clock), _candidates(candidates)
    {
    }

    [[nodiscard]] Task<ResolveResult> resolve(std::string host,
                                              std::uint16_t port,
                                              core::net::EventLoop*) override
    {
        ++calls;
        _clock->advance(cost);
        if (fail)
            co_return std::unexpected(core::net::resolveFailure(host, port, "scripted failure"));

        auto endpoints = std::vector<ResolvedEndpoint> {};
        endpoints.reserve(_candidates);
        // Each candidate is distinguishable by its `protocol` field, so a case can say WHICH
        // one a dial was handed rather than merely how many there were.
        for (auto const index: std::views::iota(std::size_t { 0 }, _candidates))
            endpoints.push_back(
                ResolvedEndpoint { .length = 16, .family = 2, .protocol = static_cast<int>(index) });
        co_return endpoints;
    }

    std::size_t calls = 0;
    std::chrono::milliseconds cost { 0 };
    bool fail = false;

  private:
    core::platform::ManualClock* _clock;
    std::size_t _candidates;
};

/// A resolver whose lookup never comes back — the nameserver that drops packets.
///
/// **It honours the flow's stop token, because the interface says it must**, and it does so the
/// cheapest way there is: it parks on the loop's own deadline, a day away on a clock nobody
/// advances that far, and that awaitable already unwinds on a stop. What it models is therefore
/// not a broken resolver but a correct one with a dead nameserver behind it, which is the case the
/// budget has to cover and the one a resolver that RETURNS cannot test: a scripted lookup that
/// advances the clock and then answers proves only that the budget is checked afterwards.
class SilentResolver final: public core::net::IAsyncAddressResolver
{
  public:
    explicit SilentResolver(core::platform::ManualClock* clock) noexcept: _clock(clock) {}

    [[nodiscard]] Task<ResolveResult> resolve(std::string /*host*/,
                                              std::uint16_t /*port*/,
                                              core::net::EventLoop* loop) override
    {
        ++calls;
        co_await loop->sleepUntil(_clock->now() + std::chrono::hours { 24 });
        co_return std::unexpected(core::net::makeNetError(NetErrorCode::SystemError, 0, "unreachable"));
    }

    std::size_t calls = 0;

  private:
    core::platform::ManualClock* _clock;
};

/// One dial attempt as a case describes it: what it costs, and what it answers.
struct DialScript
{
    core::platform::ManualClock* clock = nullptr;

    /// How much clock each attempt consumes.
    std::chrono::milliseconds cost { 0 };

    /// The candidate index (its `protocol` field) that succeeds; -1 for none.
    int succeedAt = -1;

    /// What each attempt saw, in order: the candidate index and the deadline it was given.
    std::vector<std::pair<int, SteadyTimePoint>> attempts {};

    KeepAlive sawKeepAlive = KeepAlive::No;
    core::net::SocketBufferSizes sawBuffers {};
};

/// The @c detail::DialStep every case here uses.
Task<SocketResult> scriptedDial(void* state,
                                ResolvedEndpoint endpoint,
                                SteadyTimePoint deadline,
                                core::net::detail::StreamSocketOptions options)
{
    auto& script = *static_cast<DialScript*>(state);
    script.attempts.emplace_back(endpoint.protocol, deadline);
    script.sawKeepAlive = options.keepAlive;
    script.sawBuffers = options.buffers;
    script.clock->advance(script.cost);

    if (endpoint.protocol == script.succeedAt)
    {
        // The dial's answer is all this flow asks of the socket, and a TestLoop has no kernel
        // behind it: the fake is the socket that needs neither.
        auto pair = core::net::testing::InMemorySocketPair::create();
        co_return std::unique_ptr<core::net::ISocket> { std::move(pair.client) };
    }
    co_return std::unexpected(core::net::makeNetError(
        NetErrorCode::ConnRefused, 0, std::format("candidate {} refused", endpoint.protocol)));
}

/// Drives the flow to completion on @p loop and hands the answer back.
Task<void> runFlow(core::net::IAsyncAddressResolver* resolver,
                   core::net::EventLoop* loop,
                   core::platform::IClock* clock,
                   std::string host,
                   DialOptions options,
                   DialScript* script,
                   SocketResult* out)
{
    *out =
        co_await runConnectFlow(resolver, loop, clock, std::move(host), 8080, options, &scriptedDial, script);
}

} // namespace

TEST_CASE("an empty host is refused before the resolver is touched", "[net]")
{
    // The shared resolver is bind-shaped: it turns an empty host into the wildcard address,
    // which is exactly right for a bind and is not something you can dial. Connecting to
    // `0.0.0.0` reaches localhost on Linux, so the mistake would not even be loud.
    auto clock = core::platform::ManualClock {};
    auto loop = core::net::testing::TestLoop { clock };
    auto resolver = ScriptedAsyncResolver { &clock, 1 };
    auto script = DialScript { .clock = &clock };

    auto answer = SocketResult {};
    loop.blockOn(runFlow(&resolver, &loop, &clock, "", DialOptions {}, &script, &answer));

    REQUIRE_FALSE(answer.has_value());
    CHECK(answer.error().code == NetErrorCode::AddressError);
    CHECK(resolver.calls == 0);
    CHECK(script.attempts.empty());
}

TEST_CASE("every candidate is tried and the LAST failure is reported", "[net]")
{
    // A peer whose name has both an AAAA and an A record, on a machine with no IPv6 route, is
    // reachable through the second — and a dial that gave up after the first would report a
    // healthy peer as down for a reason that is about this machine.
    auto clock = core::platform::ManualClock {};
    auto loop = core::net::testing::TestLoop { clock };
    auto resolver = ScriptedAsyncResolver { &clock, 3 };
    auto script = DialScript { .clock = &clock };

    auto answer = SocketResult {};
    loop.blockOn(runFlow(&resolver, &loop, &clock, "three.test", DialOptions {}, &script, &answer));

    REQUIRE_FALSE(answer.has_value());
    REQUIRE(script.attempts.size() == 3);
    CHECK(script.attempts[0].first == 0);
    CHECK(script.attempts[2].first == 2);
    CHECK(answer.error().context.contains("candidate 2 refused"));
}

TEST_CASE("the first candidate that connects wins and the rest are not tried", "[net]")
{
    auto clock = core::platform::ManualClock {};
    auto loop = core::net::testing::TestLoop { clock };
    auto resolver = ScriptedAsyncResolver { &clock, 3 };
    auto script = DialScript { .clock = &clock, .succeedAt = 1 };

    auto answer = SocketResult {};
    loop.blockOn(runFlow(&resolver, &loop, &clock, "three.test", DialOptions {}, &script, &answer));

    REQUIRE(answer.has_value());
    CHECK(*answer != nullptr);
    CHECK(script.attempts.size() == 2);
}

TEST_CASE("the budget covers the whole call, resolution included", "[net]")
{
    // A host with both an AAAA and an A record used to be able to take twice what the caller
    // asked for, which is a bound that is not one. Here resolution alone eats the allowance, so
    // no candidate is dialled at all.
    auto clock = core::platform::ManualClock {};
    auto loop = core::net::testing::TestLoop { clock };
    auto resolver = ScriptedAsyncResolver { &clock, 2 };
    resolver.cost = std::chrono::milliseconds { 1500 };
    auto script = DialScript { .clock = &clock };

    auto answer = SocketResult {};
    loop.blockOn(runFlow(&resolver,
                         &loop,
                         &clock,
                         "slow.test",
                         DialOptions { .connectTimeout = std::chrono::milliseconds { 1000 } },
                         &script,
                         &answer));

    REQUIRE_FALSE(answer.has_value());
    CHECK(answer.error().code == NetErrorCode::Timeout);
    CHECK(script.attempts.empty());
}

TEST_CASE("the budget ends a lookup that never answers, at the deadline and not after it", "[net]")
{
    // **The case the budget exists for, and the one the case above cannot be.** Above, the
    // resolver RETURNS after eating the allowance, so all it shows is that the budget is looked
    // at once the lookup is over. A nameserver that drops packets does not return: glibc retries
    // it for about 30 s, and a caller that asked for one second must get its answer in one
    // second, whatever the lookup is doing.
    //
    // Driven a turn at a time on a `ManualClock`, never through `blockOn`: a flow nothing
    // advances would spin a `TestLoop`'s `blockOn` for ever, and a defect here is exactly that
    // flow. This way a regression is a red CHECK, not a hang.
    auto clock = core::platform::ManualClock {};
    auto loop = core::net::testing::TestLoop { clock };
    auto resolver = SilentResolver { &clock };
    auto script = DialScript { .clock = &clock };

    auto answer = SocketResult {};
    auto finished = false;
    auto flow = [](core::net::IAsyncAddressResolver* r,
                   core::net::EventLoop* lp,
                   core::platform::IClock* c,
                   DialScript* s,
                   SocketResult* out,
                   bool* done) -> Task<void> {
        co_await runFlow(
            r, lp, c, "silent.test", DialOptions { .connectTimeout = std::chrono::seconds { 1 } }, s, out);
        *done = true;
    };
    loop.spawn(flow(&resolver, &loop, &clock, &script, &answer, &finished));
    loop.drain();
    REQUIRE(resolver.calls == 1);

    clock.advance(std::chrono::milliseconds { 999 });
    loop.drain();
    CHECK_FALSE(finished); // not early: the budget is what ends it, not something else

    clock.advance(std::chrono::milliseconds { 1 });
    loop.drain();
    CHECK(finished); // and not late: at the deadline, while the lookup is still outstanding

    if (finished)
    {
        REQUIRE_FALSE(answer.has_value());
        CHECK(answer.error().code == NetErrorCode::Timeout);
        CHECK(answer.error().context.contains("silent.test"));
    }
    CHECK(script.attempts.empty());

    // A flow the budget failed to end is still parked; unwind it, so a red here stays a red.
    loop.requestStop();
    loop.drain();
}

TEST_CASE("a stop ends a flow parked on a lookup, and it unwinds rather than answering", "[net]")
{
    // The other half of the same property, and the one `whenAny(connect, delay)` and a loop's
    // shutdown both depend on: with no budget at all, the flow's own stop token still ends it.
    auto clock = core::platform::ManualClock {};
    auto loop = core::net::testing::TestLoop { clock };
    auto resolver = SilentResolver { &clock };
    auto script = DialScript { .clock = &clock };

    auto answer = SocketResult {};
    auto threw = false;
    auto flow = [](core::net::IAsyncAddressResolver* r,
                   core::net::EventLoop* lp,
                   core::platform::IClock* c,
                   DialScript* s,
                   SocketResult* out,
                   bool* cancelled) -> Task<void> {
        try
        {
            co_await runFlow(r, lp, c, "silent.test", DialOptions {}, s, out);
        }
        catch (core::async::OperationCancelled const&)
        {
            *cancelled = true;
        }
    };
    loop.spawn(flow(&resolver, &loop, &clock, &script, &answer, &threw));
    loop.drain();
    REQUIRE(resolver.calls == 1);

    loop.rootStopSource().request_stop();
    loop.drain();

    CHECK(threw);
    CHECK(script.attempts.empty());
    loop.requestStop();
    loop.drain();
}

TEST_CASE("each candidate gets a share of what is left, not the whole of it", "[net]")
{
    // Both halves matter and they pull against each other. Handing every candidate the full
    // budget means a caller asking for one second can wait two. Handing the FIRST the whole
    // remaining budget defeats the fallback whenever that candidate black-holes rather than
    // refuses — the ordinary case for an AAAA on a machine with no IPv6 route.
    auto clock = core::platform::ManualClock {};
    auto loop = core::net::testing::TestLoop { clock };
    auto resolver = ScriptedAsyncResolver { &clock, 2 };
    auto script = DialScript { .clock = &clock };

    auto const start = clock.now();
    auto answer = SocketResult {};
    loop.blockOn(runFlow(&resolver,
                         &loop,
                         &clock,
                         "two.test",
                         DialOptions { .connectTimeout = std::chrono::milliseconds { 1000 } },
                         &script,
                         &answer));

    REQUIRE(script.attempts.size() == 2);
    // Two candidates, a full 1000ms left: the first is given half.
    CHECK(script.attempts[0].second == start + std::chrono::milliseconds { 500 });
    // The second is the last, so it gets everything that remains — which, since neither attempt
    // consumed any clock here, is the whole original deadline.
    CHECK(script.attempts[1].second == start + std::chrono::milliseconds { 1000 });
}

TEST_CASE("a dial with no budget hands every candidate an unbounded deadline", "[net]")
{
    // A non-positive budget means the caller did not ask for one, and that is spelled
    // `SteadyTimePoint::max()`. A default-constructed time point would mean "already expired",
    // which is the shape that turns an opt-out into an instant failure.
    auto clock = core::platform::ManualClock {};
    auto loop = core::net::testing::TestLoop { clock };
    auto resolver = ScriptedAsyncResolver { &clock, 1 };
    auto script = DialScript { .clock = &clock };

    auto answer = SocketResult {};
    loop.blockOn(runFlow(&resolver, &loop, &clock, "one.test", DialOptions {}, &script, &answer));

    REQUIRE(script.attempts.size() == 1);
    CHECK(script.attempts[0].second == SteadyTimePoint::max());
}

TEST_CASE("a resolver failure is reported as itself, not as a dial failure", "[net]")
{
    auto clock = core::platform::ManualClock {};
    auto loop = core::net::testing::TestLoop { clock };
    auto resolver = ScriptedAsyncResolver { &clock, 1 };
    resolver.fail = true;
    auto script = DialScript { .clock = &clock };

    auto answer = SocketResult {};
    loop.blockOn(runFlow(&resolver, &loop, &clock, "bad.test", DialOptions {}, &script, &answer));

    REQUIRE_FALSE(answer.has_value());
    // `AddressError`, "address resolution or parsing failed", as `NetError.hpp` defines it. Not
    // `AddressNotAvail`, which that header reserves for a bind whose address is not local.
    CHECK(answer.error().code == NetErrorCode::AddressError);
    CHECK(answer.error().context.contains("bad.test"));
    CHECK(script.attempts.empty());
}

TEST_CASE("keepalive and buffer sizes travel per call rather than per connector", "[net]")
{
    // Folding them into the connector's own state is how a per-call option quietly becomes a
    // per-connector one, which is the design `DialOptions` exists to rule out.
    auto clock = core::platform::ManualClock {};
    auto loop = core::net::testing::TestLoop { clock };
    auto resolver = ScriptedAsyncResolver { &clock, 1 };
    auto script = DialScript { .clock = &clock };
    auto const options =
        DialOptions { .keepAlive = KeepAlive::Yes,
                      .buffers = core::net::SocketBufferSizes { .send = 4096, .receive = 8192 } };

    auto answer = SocketResult {};
    loop.blockOn(runFlow(&resolver, &loop, &clock, "one.test", options, &script, &answer));

    CHECK(script.sawKeepAlive == KeepAlive::Yes);
    CHECK(script.sawBuffers.send == std::optional<std::size_t> { 4096 });
    CHECK(script.sawBuffers.receive == std::optional<std::size_t> { 8192 });
}
