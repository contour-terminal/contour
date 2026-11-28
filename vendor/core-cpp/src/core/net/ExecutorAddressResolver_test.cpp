// SPDX-License-Identifier: Apache-2.0
//
// `ExecutorAddressResolver`: a lookup runs on the executor it was given and on no thread of the
// resolver's own, a stop ends the wait and not the lookup, and its `maxInFlight` slot is held until
// the lookup it stands for has actually returned.
#include <core/async/Cancellation.hpp>
#include <core/async/StopToken.hpp>
#include <core/async/Task.hpp>
#include <core/async/ThreadPoolExecutor.hpp>
#include <core/async/testing/ManualExecutor.hpp>
#include <core/net/EventLoop.hpp>
#include <core/net/ExecutorAddressResolver.hpp>
#include <core/net/NetError.hpp>
#include <core/net/SocketAddress.hpp>
#include <core/net/detail/ScopeGuard.hpp>
#include <core/net/testing/ScriptedAddressResolver.hpp>
#include <core/net/testing/TestLoop.hpp>
#include <core/platform/Clock.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <memory>
#include <optional>
#include <ranges>
#include <string>
#include <thread>
#include <utility>
#include <vector>

using core::async::Task;
using core::async::testing::ManualExecutor;
using core::net::EventLoop;
using core::net::ExecutorAddressResolver;
using core::net::NetErrorCode;
using core::net::ResolvedEndpoint;
using core::net::ResolveResult;
using core::net::testing::ScriptedAddressResolver;
using core::net::testing::TestLoop;

namespace
{

/// What one awaited lookup came to: nothing yet, an answer (possibly a failure), or a stop.
struct Outcome
{
    std::optional<ResolveResult> answer;
    bool isCancelled = false;
    std::atomic<bool> isFinished { false };
};

/// Resolves @p host through @p resolver on @p loop and records how it ended, a stop included.
Task<void> resolveInto(EventLoop* loop, ExecutorAddressResolver* resolver, std::string host, Outcome* out)
{
    try
    {
        out->answer = co_await resolver->resolve(std::move(host), 80, loop);
    }
    catch (core::async::OperationCancelled const&)
    {
        out->isCancelled = true;
    }
    out->isFinished.store(true, std::memory_order_release);
}

/// A scripted inner that answers `db.test` with one endpoint.
[[nodiscard]] std::shared_ptr<ScriptedAddressResolver> scriptedInner()
{
    auto inner = std::make_shared<ScriptedAddressResolver>();
    inner->answer("db.test", { ResolvedEndpoint { .length = 16, .family = 2, .protocol = 6 } });
    return inner;
}

} // namespace

TEST_CASE("a literal resolves inline and never reaches the blocking executor", "[net][resolver]")
{
    auto clock = core::platform::ManualClock {};
    auto loop = TestLoop { clock };
    auto blocking = ManualExecutor {};
    auto inner = scriptedInner();
    auto resolver = ExecutorAddressResolver { blocking, inner };

    auto outcome = Outcome {};
    // Spawned and drained rather than blocked on: a lookup that parked by mistake fails here
    // instead of hanging the case.
    loop.spawn(resolveInto(&loop, &resolver, "127.0.0.1", &outcome));
    loop.drain();
    REQUIRE(outcome.isFinished.load());

    REQUIRE(outcome.answer.has_value());
    REQUIRE(outcome.answer->has_value());
    CHECK((*outcome.answer)->size() == 1);
    CHECK(blocking.pending() == 0);
    CHECK(resolver.offloaded() == 0);
    CHECK(inner->calls() == 0);
}

TEST_CASE("a null loop resolves inline, because there is nowhere to hand a result back to", "[net][resolver]")
{
    auto clock = core::platform::ManualClock {};
    auto loop = TestLoop { clock };
    auto blocking = ManualExecutor {};
    auto inner = scriptedInner();
    auto resolver = ExecutorAddressResolver { blocking, inner };

    auto outcome = Outcome {};
    // Spawned and drained rather than blocked on: a lookup that parked by mistake fails here
    // instead of hanging the case.
    loop.spawn(resolveInto(nullptr, &resolver, "db.test", &outcome));
    loop.drain();
    REQUIRE(outcome.isFinished.load());

    REQUIRE(outcome.answer.has_value());
    CHECK(outcome.answer->has_value());
    CHECK(blocking.pending() == 0);
    CHECK(resolver.offloaded() == 0);
    CHECK(inner->calls() == 1);
}

TEST_CASE("a name is looked up on the injected executor and answered on the loop", "[net][resolver]")
{
    // The executor is drained by this thread, so the lookup running here proves it ran where the
    // executor runs things: the resolver has no thread to run it anywhere else.
    auto clock = core::platform::ManualClock {};
    auto loop = TestLoop { clock };
    auto blocking = ManualExecutor {};
    auto inner = scriptedInner();
    auto resolver = ExecutorAddressResolver { blocking, inner };

    auto outcome = Outcome {};
    loop.spawn(resolveInto(&loop, &resolver, "db.test", &outcome));
    loop.drain();
    REQUIRE(blocking.pending() == 1);
    CHECK(resolver.offloaded() == 1);
    CHECK(resolver.inFlight() == 1);
    CHECK(inner->calls() == 0);
    CHECK_FALSE(outcome.isFinished.load());

    CHECK(blocking.drain() == 1);
    CHECK_FALSE(outcome.isFinished.load()); // handed back to the loop, never resumed by the executor
    CHECK(loop.drain() > 0);

    REQUIRE(outcome.answer.has_value());
    REQUIRE(outcome.answer->has_value());
    CHECK((*outcome.answer)->size() == 1);
    CHECK(resolver.inFlight() == 0);
    REQUIRE(inner->callers().size() == 1);
    CHECK(inner->callers().front() == std::this_thread::get_id());
}

TEST_CASE("a classified failure survives the hop to and from the blocking executor", "[net][resolver]")
{
    auto clock = core::platform::ManualClock {};
    auto loop = TestLoop { clock };
    auto blocking = ManualExecutor {};
    auto inner = std::make_shared<ScriptedAddressResolver>();
    inner->failWithCode(core::net::makeNetError(NetErrorCode::ResolverTemporaryFailure, 0, "SERVFAIL"));
    auto resolver = ExecutorAddressResolver { blocking, inner };

    auto outcome = Outcome {};
    loop.spawn(resolveInto(&loop, &resolver, "flaky.test", &outcome));
    loop.drain();
    blocking.drain();
    loop.drain();

    REQUIRE(outcome.answer.has_value());
    REQUIRE_FALSE(outcome.answer->has_value());
    CHECK(outcome.answer->error().code == NetErrorCode::ResolverTemporaryFailure);
    CHECK(outcome.answer->error().context == "cannot resolve flaky.test:80: SERVFAIL");
}

TEST_CASE("a stop during an offloaded lookup throws on the loop and discards the late answer",
          "[net][resolver]")
{
    // `getaddrinfo` cannot be interrupted, so a stop ends the WAIT: the flow is handed back to its
    // loop at once and throws, while the lookup keeps its slot until it has really returned. Its
    // answer then lands in a call nobody waits on, and a resumption queued to the loop is what
    // delivering it into the gone frame would look like.
    auto clock = core::platform::ManualClock {};
    auto loop = TestLoop { clock };
    auto blocking = ManualExecutor {};
    auto inner = scriptedInner();
    auto resolver = ExecutorAddressResolver { blocking, inner };

    auto outcome = Outcome {};
    loop.spawn(resolveInto(&loop, &resolver, "db.test", &outcome));
    loop.drain();
    REQUIRE(blocking.pending() == 1);

    loop.rootStopSource().request_stop();
    loop.drain();
    CHECK(outcome.isCancelled);
    CHECK_FALSE(outcome.answer.has_value());
    CHECK(resolver.inFlight() == 1); // the lookup has not run yet, so it still holds its slot

    blocking.drain();
    CHECK(loop.drain() == 0);
    CHECK(resolver.inFlight() == 0);
    CHECK(inner->calls() == 1);
    CHECK_FALSE(outcome.answer.has_value());
}

TEST_CASE("a blocking executor dropping a queued lookup settles it Cancelled and frees its slot",
          "[net][resolver]")
{
    // A job its executor destroys unrun is freed, not resumed; the lookup's guard runs in the
    // freed frame and is what keeps the waiter from being stranded and the slot from leaking.
    auto clock = core::platform::ManualClock {};
    auto loop = TestLoop { clock };
    auto blocking = std::make_unique<ManualExecutor>();
    auto inner = scriptedInner();
    auto resolver = ExecutorAddressResolver { *blocking, inner };

    auto outcome = Outcome {};
    loop.spawn(resolveInto(&loop, &resolver, "db.test", &outcome));
    loop.drain();
    REQUIRE(blocking->pending() == 1);

    SECTION("with the flow still waiting")
    {
        blocking.reset();
        CHECK(resolver.inFlight() == 0);
        CHECK(loop.drain() > 0);
        REQUIRE(outcome.answer.has_value());
        REQUIRE_FALSE(outcome.answer->has_value());
        CHECK(outcome.answer->error().code == NetErrorCode::Cancelled);
        CHECK_FALSE(outcome.isCancelled);
    }

    SECTION("after the flow was stopped")
    {
        loop.rootStopSource().request_stop();
        loop.drain();
        REQUIRE(outcome.isCancelled);
        CHECK(resolver.inFlight() == 1);

        blocking.reset();
        CHECK(resolver.inFlight() == 0);
        CHECK(loop.drain() == 0);
    }

    CHECK(inner->calls() == 0);
}

TEST_CASE("a stopped lookup keeps its slot until the blocking call returns", "[net][resolver][threading]")
{
    // The slot stands for the executor's thread, and a stop does not give that thread back: the
    // lookup is still inside `getaddrinfo`. So the slot is held while the inner resolver is, and
    // freed only once it returns.
    constexpr auto Budget = std::chrono::seconds { 10 };

    auto clock = core::platform::ManualClock {};
    auto loop = TestLoop { clock };
    auto inner = scriptedInner();
    inner->hold();
    auto pool = core::async::ThreadPoolExecutor { 1 };
    // Released on every way out, before the pool joins its held thread.
    auto const releaseOnExit = core::net::detail::ScopeGuard { [&inner]() noexcept { inner->release(); } };
    auto resolver = ExecutorAddressResolver { pool, inner };

    auto outcome = Outcome {};
    loop.spawn(resolveInto(&loop, &resolver, "db.test", &outcome));
    loop.drain();
    REQUIRE(inner->awaitEntered(1, Budget) == 1);

    loop.rootStopSource().request_stop();
    loop.drain();
    CHECK(outcome.isCancelled);
    CHECK(resolver.inFlight() == 1);

    inner->release();
    auto const deadline = std::chrono::steady_clock::now() + Budget;
    while (resolver.inFlight() != 0 && std::chrono::steady_clock::now() < deadline)
        std::this_thread::yield();
    CHECK(resolver.inFlight() == 0);
    pool.stop();
    CHECK(loop.drain() == 0);
    CHECK_FALSE(outcome.answer.has_value());
}

TEST_CASE("a scoped IPv6 literal is offloaded, so its zone reaches the resolver", "[net][resolver]")
{
    // Parsed in place, the zone would be dropped where the platform's inet_pton accepts it (macOS);
    // the blocking resolver keeps it, on every platform.
    auto clock = core::platform::ManualClock {};
    auto loop = TestLoop { clock };
    auto blocking = ManualExecutor {};
    auto inner = scriptedInner();
    inner->answer("fe80::1%lo", { ResolvedEndpoint { .length = 16, .family = 2, .protocol = 6 } });
    auto resolver = ExecutorAddressResolver { blocking, inner };

    auto outcome = Outcome {};
    loop.spawn(resolveInto(&loop, &resolver, "fe80::1%lo", &outcome));
    loop.drain();
    CHECK(blocking.pending() == 1);
    CHECK(resolver.offloaded() == 1);

    blocking.drain();
    loop.drain();
    REQUIRE(outcome.answer.has_value());
    CHECK(outcome.answer->has_value());
    CHECK(inner->calls() == 1);
}

TEST_CASE("a flow already stopped throws before it takes a slot", "[net][resolver]")
{
    auto clock = core::platform::ManualClock {};
    auto loop = TestLoop { clock };
    auto blocking = ManualExecutor {};
    auto inner = scriptedInner();
    auto resolver = ExecutorAddressResolver { blocking, inner };

    loop.rootStopSource().request_stop();
    auto outcome = Outcome {};
    loop.spawn(resolveInto(&loop, &resolver, "db.test", &outcome));
    loop.drain();

    REQUIRE(outcome.isFinished.load());
    CHECK(outcome.isCancelled);
    CHECK(blocking.pending() == 0);
    CHECK(resolver.offloaded() == 0);
    CHECK(resolver.inFlight() == 0);
}

TEST_CASE("a full lookup budget is refused rather than waited on", "[net][resolver]")
{
    auto clock = core::platform::ManualClock {};
    auto loop = TestLoop { clock };
    auto blocking = ManualExecutor {};
    auto inner = scriptedInner();
    auto resolver = ExecutorAddressResolver { blocking, inner, { .maxInFlight = 1 } };

    auto first = Outcome {};
    loop.spawn(resolveInto(&loop, &resolver, "db.test", &first));
    loop.drain();
    REQUIRE(resolver.inFlight() == 1);

    auto second = Outcome {};
    // Spawned and drained rather than blocked on: a lookup that parked by mistake fails here
    // instead of hanging the case.
    loop.spawn(resolveInto(&loop, &resolver, "db.test", &second));
    loop.drain();
    REQUIRE(second.isFinished.load());
    REQUIRE(second.answer.has_value());
    REQUIRE_FALSE(second.answer->has_value());
    CHECK(second.answer->error().code == NetErrorCode::ResourceExhausted);
    CHECK_FALSE(core::net::isDeadlineExpiry(second.answer->error().code));
    CHECK(resolver.refused() == 1);
    CHECK(resolver.offloaded() == 1);
    CHECK(blocking.pending() == 1);

    blocking.drain();
    loop.drain();
    REQUIRE(first.answer.has_value());
    CHECK(first.answer->has_value());
    CHECK(resolver.inFlight() == 0);

    // The slot is back: a third lookup is offloaded rather than refused.
    auto third = Outcome {};
    loop.spawn(resolveInto(&loop, &resolver, "db.test", &third));
    loop.drain();
    CHECK(blocking.pending() == 1);
    CHECK(resolver.refused() == 1);
    blocking.drain();
    loop.drain();
    REQUIRE(third.answer.has_value());
    CHECK(third.answer->has_value());
}

TEST_CASE("a lookup outlives the resolver that started it", "[net][resolver]")
{
    // The job holds what it needs -- the inner resolver and the slot budget -- by share, so
    // destroying the resolver first leaves it nothing dangling to touch (ASan watches this one).
    auto clock = core::platform::ManualClock {};
    auto loop = TestLoop { clock };
    auto blocking = ManualExecutor {};
    auto inner = scriptedInner();
    auto resolver = std::make_unique<ExecutorAddressResolver>(blocking, inner);

    auto outcome = Outcome {};
    loop.spawn(resolveInto(&loop, resolver.get(), "db.test", &outcome));
    loop.drain();
    REQUIRE(blocking.pending() == 1);

    resolver.reset();
    CHECK(blocking.drain() == 1);
    CHECK(loop.drain() > 0);
    REQUIRE(outcome.answer.has_value());
    CHECK(outcome.answer->has_value());
    CHECK(inner->calls() == 1);
}

TEST_CASE("a stop racing a completion on a pool ends the flow exactly one way", "[net][resolver][threading]")
{
    // The stop is requested on this thread while a pool thread finishes the lookup and settles
    // it, so the two take the waiter in either order. Whoever takes it decides: an answer or an
    // `OperationCancelled`, never both and never neither. The stop comes after a delay that varies
    // from round to round, so that both orders are actually met rather than assumed.
    //
    // One loop for every round, outliving the pool: an answer handed back to a loop is the loop's
    // to receive until the pool has stopped, and a flow's own stop source is what each round needs.
    constexpr auto Rounds = 1000;
    constexpr auto Budget = std::chrono::seconds { 10 };

    auto clock = core::platform::ManualClock {};
    auto loop = TestLoop { clock };
    auto inner = scriptedInner();
    auto pool = core::async::ThreadPoolExecutor { 2 };
    auto resolver = ExecutorAddressResolver { pool, inner };

    auto answered = 0;
    auto cancelled = 0;
    for (auto const round: std::views::iota(0, Rounds))
    {
        auto source = core::async::StopSource {};
        auto outcome = Outcome {};
        auto flow = resolveInto(&loop, &resolver, "db.test", &outcome);
        flow.handle().promise().setStopToken(source.get_token());
        flow.handle().resume();

        for ([[maybe_unused]] auto const spin: std::views::iota(0, round % 64))
            std::this_thread::yield();
        source.request_stop();

        auto const deadline = std::chrono::steady_clock::now() + Budget;
        while (!outcome.isFinished.load(std::memory_order_acquire)
               && std::chrono::steady_clock::now() < deadline)
        {
            if (loop.drain() == 0)
                std::this_thread::yield();
        }
        REQUIRE(outcome.isFinished.load(std::memory_order_acquire));
        REQUIRE(flow.done());
        REQUIRE(outcome.answer.has_value() != outcome.isCancelled);
        if (outcome.isCancelled)
            ++cancelled;
        else
            ++answered;

        // A cancelled round's lookup may still be running; let it return before the next round.
        while (resolver.inFlight() != 0 && std::chrono::steady_clock::now() < deadline)
            std::this_thread::yield();
        REQUIRE(resolver.inFlight() == 0);
    }
    pool.stop();
    CHECK(loop.drain() == 0);
    CHECK(answered + cancelled == Rounds);
    CHECK(answered > 0);
    CHECK(cancelled > 0);
    CHECK(inner->calls() == static_cast<std::size_t>(Rounds));
}

TEST_CASE("a host no name can be is refused before the inner resolver or the executor sees it",
          "[net][resolver]")
{
    // An empty host would reach getaddrinfo as the wildcard address, a NUL would cut the name it
    // looks up short, and more than 253 bytes is no DNS name: none of them is a lookup to run.
    auto const tooLong = std::string(254, 'a');
    auto const host = GENERATE_COPY(std::string {}, std::string { "db.test\0evil", 12 }, tooLong);
    auto const viaLoop = GENERATE(true, false);
    CAPTURE(host.size(), viaLoop);
    auto clock = core::platform::ManualClock {};
    auto loop = TestLoop { clock };
    auto blocking = ManualExecutor {};
    auto inner = scriptedInner();
    auto resolver = ExecutorAddressResolver { blocking, inner };

    auto outcome = Outcome {};
    loop.spawn(resolveInto(viaLoop ? &loop : nullptr, &resolver, host, &outcome));
    loop.drain();
    REQUIRE(outcome.isFinished.load());
    REQUIRE(outcome.answer.has_value());
    REQUIRE_FALSE(outcome.answer->has_value());
    CHECK(outcome.answer->error().code == NetErrorCode::AddressError);
    CHECK(blocking.pending() == 0);
    CHECK(inner->calls() == 0);
}

TEST_CASE("a 253-byte name is still looked up", "[net][resolver]")
{
    auto clock = core::platform::ManualClock {};
    auto loop = TestLoop { clock };
    auto blocking = ManualExecutor {};
    auto inner = scriptedInner();
    auto resolver = ExecutorAddressResolver { blocking, inner };

    auto outcome = Outcome {};
    loop.spawn(resolveInto(nullptr, &resolver, std::string(253, 'a'), &outcome));
    loop.drain();
    REQUIRE(outcome.isFinished.load());
    CHECK(inner->calls() == 1);
}
