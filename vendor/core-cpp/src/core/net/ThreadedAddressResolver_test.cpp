// SPDX-License-Identifier: Apache-2.0
#include <core/async/Cancellation.hpp>
#include <core/net/EventLoop.hpp>
#include <core/net/IoBackend.hpp>
#include <core/net/NetError.hpp>
#include <core/net/SocketAddress.hpp>
#include <core/net/ThreadedAddressResolver.hpp>
#include <core/net/detail/ScopeGuard.hpp>
#include <core/net/testing/ScriptedAddressResolver.hpp>
#include <core/net/testing/TestLoop.hpp>
#include <core/platform/Clock.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <ranges>
#include <string>
#include <thread>
#include <utility>
#include <vector>

using core::async::Task;
using core::net::EventLoop;
using core::net::ResolvedEndpoint;
using core::net::ResolveResult;
using core::net::ThreadedAddressResolver;
using core::net::testing::ScriptedAddressResolver;

namespace
{

/// How long a case waits for a held lookup to reach the resolver before it gives up.
constexpr auto EnterBudget = std::chrono::seconds { 10 };

/// Scripts @p inner to answer `example.test`; any other name is not found.
///
/// The whole point of the seam is that a test can say what resolution costs and observe which
/// thread paid for it, so every property below is provable without a name server anywhere.
/// @param inner The resolver to script.
void scriptExample(ScriptedAddressResolver& inner)
{
    inner.answer("example.test", { ResolvedEndpoint { .length = 16, .family = 2, .protocol = 6 } });
}

/// Resolves through @p resolver on @p loop and records what came back.
Task<void> resolveOn(EventLoop* loop,
                     core::net::IAsyncAddressResolver* resolver,
                     std::string host,
                     ResolveResult* out,
                     std::thread::id* resumedOn)
{
    *out = co_await resolver->resolve(std::move(host), 80, loop);
    *resumedOn = std::this_thread::get_id();
}

/// As @c resolveOn, but a cancellation is recorded rather than escaping.
Task<void> resolveOrUnwind(EventLoop* loop,
                           core::net::IAsyncAddressResolver* resolver,
                           std::string host,
                           ResolveResult* out,
                           bool* cancelled)
{
    try
    {
        *out = co_await resolver->resolve(std::move(host), 80, loop);
    }
    catch (core::async::OperationCancelled const&)
    {
        *cancelled = true;
    }
}

} // namespace

TEST_CASE("a literal never reaches the resolver pool", "[net]")
{
    // Load-bearing rather than an optimisation: every internal dial in the consuming projects is
    // to a literal, and paying a thread hand-off plus two context switches for `inet_pton` would
    // be a real regression on the hot path. It is also what lets the whole connect path be
    // exercised with no thread existing at all.
    auto inner = ScriptedAddressResolver {};
    scriptExample(inner);
    auto resolver = ThreadedAddressResolver { inner };

    auto clock = core::platform::ManualClock {};
    auto loop = core::net::testing::TestLoop { clock };

    auto answer = ResolveResult {};
    auto resumedOn = std::thread::id {};
    loop.blockOn(resolveOn(&loop, &resolver, "127.0.0.1", &answer, &resumedOn));

    CHECK(answer.has_value());
    CHECK(resolver.offloaded() == 0);
    CHECK(inner.calls() == 1); // it still resolved — inline, on this thread
    REQUIRE(inner.callers().size() == 1);
    CHECK(inner.callers().front() == std::this_thread::get_id());
}

TEST_CASE("a name is resolved on another thread and resumes on the loop's", "[net]")
{
    // The property the whole task exists for, asserted at the seam rather than through a dial:
    // the lookup runs somewhere that is NOT the thread driving the loop, and the coroutine that
    // asked for it continues where it was — on the loop.
    auto inner = ScriptedAddressResolver {};
    scriptExample(inner);
    auto resolver = ThreadedAddressResolver { inner };

    auto backend = core::net::makeBackend(core::net::preferredBackendKind());
    REQUIRE(backend != nullptr);
    auto loop = EventLoop { *backend };

    auto answer = ResolveResult {};
    auto resumedOn = std::thread::id {};
    loop.blockOn(resolveOn(&loop, &resolver, "example.test", &answer, &resumedOn));

    REQUIRE(answer.has_value());
    CHECK(resolver.offloaded() == 1);
    REQUIRE(inner.callers().size() == 1);
    CHECK(inner.callers().front() != std::this_thread::get_id());
    CHECK(resumedOn == std::this_thread::get_id());
}

TEST_CASE("a null loop resolves inline, because there is nowhere to hand a result back to", "[net]")
{
    // A correctness requirement rather than a second optimisation: with nothing to submit the
    // result to, offloading would park a coroutine that nothing could ever resume.
    auto inner = ScriptedAddressResolver {};
    scriptExample(inner);
    auto resolver = ThreadedAddressResolver { inner };

    auto clock = core::platform::ManualClock {};
    auto loop = core::net::testing::TestLoop { clock };

    auto answer = ResolveResult {};
    auto resumedOn = std::thread::id {};
    loop.blockOn(resolveOn(nullptr, &resolver, "example.test", &answer, &resumedOn));

    CHECK(answer.has_value());
    CHECK(resolver.offloaded() == 0);
    REQUIRE(inner.callers().size() == 1);
    CHECK(inner.callers().front() == std::this_thread::get_id());
}

TEST_CASE("a full resolver queue is refused rather than waited on", "[net]")
{
    // An unbounded queue is a memory-exhaustion hole reachable by whatever provokes dials, and
    // blocking the caller to wait for room would reintroduce, on the loop thread, exactly the
    // stall this class exists to remove. `ResourceExhausted`, which a caller may retry once a lookup
    // has returned, and never `WouldBlock`, which an accept loop reads as a deadline's tick.
    auto inner = ScriptedAddressResolver {};
    scriptExample(inner);
    inner.hold();
    auto resolver = ThreadedAddressResolver { inner, { .threads = 1, .maxQueueDepth = 1 } };

    auto backend = core::net::makeBackend(core::net::preferredBackendKind());
    REQUIRE(backend != nullptr);
    auto loop = EventLoop { *backend };

    // One lookup occupies the single worker; a second fills the one queue slot. Both are
    // arranged, not raced: the worker is parked INSIDE the scripted resolver.
    auto first = ResolveResult {};
    auto second = ResolveResult {};
    auto third = ResolveResult {};
    auto where = std::thread::id {};

    loop.spawn(resolveOn(&loop, &resolver, "one.test", &first, &where));
    loop.runUntilIdle();
    REQUIRE(inner.awaitEntered(1, EnterBudget) >= 1);
    loop.spawn(resolveOn(&loop, &resolver, "two.test", &second, &where));
    loop.runUntilIdle();

    // The third has nowhere to go: the worker is busy and the queue slot is taken.
    loop.blockOn(resolveOn(&loop, &resolver, "three.test", &third, &where));
    REQUIRE_FALSE(third.has_value());
    CHECK(third.error().code == core::net::NetErrorCode::ResourceExhausted);
    CHECK_FALSE(core::net::isDeadlineExpiry(third.error().code));
    CHECK(resolver.refused() == 1);

    inner.release();
    resolver.stop();
}

TEST_CASE("stopping the resolver resumes a queued lookup rather than stranding it", "[net]")
{
    // A queued lookup whose coroutine is never resumed is a leaked frame, so `stop` fails
    // everything queued instead of dropping it. A lookup already inside the blocking call cannot
    // be interrupted — there is no portable way — so the join waits for that one.
    auto inner = ScriptedAddressResolver {};
    scriptExample(inner);
    inner.hold();
    auto resolver = ThreadedAddressResolver { inner, { .threads = 1, .maxQueueDepth = 8 } };

    auto backend = core::net::makeBackend(core::net::preferredBackendKind());
    REQUIRE(backend != nullptr);
    auto loop = EventLoop { *backend };

    auto inFlight = ResolveResult {};
    auto queued = ResolveResult {};
    auto where = std::thread::id {};

    loop.spawn(resolveOn(&loop, &resolver, "in-flight.test", &inFlight, &where));
    loop.runUntilIdle();
    REQUIRE(inner.awaitEntered(1, EnterBudget) >= 1);
    loop.spawn(resolveOn(&loop, &resolver, "queued.test", &queued, &where));
    loop.runUntilIdle();

    // **`stop` runs on its own thread, and the in-flight lookup is released only once the queued
    // one has been failed.** Releasing first and then stopping — which this case used to do —
    // races the single worker for the queued job: if the worker takes it before `stop` swaps the
    // queue, the lookup succeeds and the assertion below reads a scheduling accident. `stop`
    // fails the queue and THEN joins, so its join waits on the held worker, and the queued
    // lookup's hand-back is visible on this loop before anything is released.
    auto stopper = std::thread { [&resolver] { resolver.stop(); } };
    auto const joinStopper = core::net::detail::ScopeGuard { [&]() noexcept {
        inner.release();
        if (stopper.joinable())
            stopper.join();
    } };
    for ([[maybe_unused]] auto const turn: std::views::iota(0, 5000))
    {
        if (loop.runUntilIdle() > 0)
            break;
        std::this_thread::sleep_for(std::chrono::milliseconds { 1 });
    }

    REQUIRE_FALSE(queued.has_value());
    CHECK(queued.error().code == core::net::NetErrorCode::Cancelled);
}

TEST_CASE("a stop ends a lookup that never returns and the answer that arrives later is dropped", "[net]")
{
    // **The flow's stop token reaches a flow parked on a lookup.** A nameserver that drops
    // packets holds `getaddrinfo` for the platform's full retry schedule, and nothing can
    // interrupt it; what CAN end is the wait for it. Before this, the park ignored the token, so
    // `whenAny(connect, delay(2s))`, a dial's own budget and a loop's shutdown all waited for the
    // lookup regardless.
    //
    // The inner is held for the whole of the flow's life, so from the flow's side it never
    // returns. It is released only after the flow is gone, which is the second half: the worker
    // then publishes an answer for a frame that no longer exists, and that answer must go
    // nowhere. A hand-back queued to the loop is what delivering it would look like.
    auto inner = ScriptedAddressResolver {};
    scriptExample(inner);
    inner.hold();
    auto resolver = ThreadedAddressResolver { inner, { .threads = 1, .maxQueueDepth = 8 } };

    auto clock = core::platform::ManualClock {};
    auto loop = core::net::testing::TestLoop { clock };

    auto answer = ResolveResult {};
    auto cancelled = false;
    loop.spawn(resolveOrUnwind(&loop, &resolver, "silent.test", &answer, &cancelled));
    loop.drain();
    REQUIRE(inner.awaitEntered(1, EnterBudget) >= 1);

    loop.rootStopSource().request_stop();
    loop.drain();
    CHECK(cancelled);

    inner.release();
    resolver.stop(); // joins: the worker has published by the time this returns

    // Nothing is left for the loop to run. Asked by DRAINING rather than by `pendingSubmissions`: a
    // hand-back from a worker arrives through the loop's cross-thread inbound queue, which that
    // counter now includes, but a turn that resumes something is what delivering the late answer
    // looks like, and it resumes rather than leaks it on the way.
    CHECK(loop.drain() == 0);
}

TEST_CASE("a stop from another thread ends a lookup that never returns", "[net]")
{
    // The same property with the stop requested where a watchdog or a signal handler would
    // request it: off the loop. The stop callback then runs on THAT thread, beside a worker
    // thread that may be publishing at the same moment, which is the arrangement
    // ThreadSanitizer needs to see.
    auto inner = ScriptedAddressResolver {};
    scriptExample(inner);
    inner.hold();
    auto resolver = ThreadedAddressResolver { inner, { .threads = 1, .maxQueueDepth = 8 } };

    auto backend = core::net::makeBackend(core::net::preferredBackendKind());
    REQUIRE(backend != nullptr);
    auto loop = EventLoop { *backend };

    auto answer = ResolveResult {};
    auto cancelled = false;
    auto flowDone = std::atomic<bool> { false };

    // **Bounded, and the bound says what it bounds.** The stopper waits up to five seconds for
    // the flow to finish after the stop, and then releases the lookup whatever happened. With the
    // stop honoured the flow is done within a turn and the wait ends at once; with it ignored, the
    // release is what finally resumes the flow — with an ANSWER, which the CHECK below reports as
    // a red instead of the case hanging until ctest's TIMEOUT says nothing about why.
    auto stopper = std::thread { [&loop, &inner, &flowDone] {
        // Not asserted: Catch2 is not thread-safe, and a lookup that never entered leaves the flow
        // parked, which the CHECK on the test's thread reports.
        static_cast<void>(inner.awaitEntered(1, EnterBudget));
        loop.rootStopSource().request_stop();
        for ([[maybe_unused]] auto const tick: std::views::iota(0, 5000))
        {
            if (flowDone.load(std::memory_order_acquire))
                break;
            std::this_thread::sleep_for(std::chrono::milliseconds { 1 });
        }
        inner.release();
    } };
    auto const joinStopper = core::net::detail::ScopeGuard { [&]() noexcept {
        flowDone.store(true, std::memory_order_release);
        if (stopper.joinable())
            stopper.join();
    } };

    loop.blockOn(resolveOrUnwind(&loop, &resolver, "silent.test", &answer, &cancelled));
    flowDone.store(true, std::memory_order_release);
    CHECK(cancelled);
    CHECK_FALSE((answer.has_value() && !answer->empty())); // no answer reached the flow after the stop

    if (stopper.joinable())
        stopper.join();
    resolver.stop();
}

TEST_CASE("a classified resolver failure survives the hop to and from the resolver thread", "[net]")
{
    // A name that does not exist and a resolver that could not answer are two different things to
    // the flow that asked, and the pool must not flatten them into one `AddressError` on the way
    // back. Both arms: the offloaded one, and the inline one a null loop takes.
    auto script = ScriptedAddressResolver {};
    script.failWithCode(core::net::makeNetError(core::net::NetErrorCode::NameNotFound, 0, "NXDOMAIN"));
    auto resolver = ThreadedAddressResolver { script, { .threads = 1, .maxQueueDepth = 8 } };

    auto backend = core::net::makeBackend(core::net::preferredBackendKind());
    REQUIRE(backend != nullptr);
    auto loop = EventLoop { *backend };

    auto offloaded = ResolveResult {};
    auto inlined = ResolveResult {};
    auto where = std::thread::id {};
    loop.blockOn(resolveOn(&loop, &resolver, "missing.test", &offloaded, &where));
    loop.blockOn(resolveOn(nullptr, &resolver, "missing.test", &inlined, &where));

    CHECK(resolver.offloaded() == 1);
    REQUIRE_FALSE(offloaded.has_value());
    CHECK(offloaded.error().code == core::net::NetErrorCode::NameNotFound);
    CHECK(offloaded.error().context == "cannot resolve missing.test:80: NXDOMAIN");
    REQUIRE_FALSE(inlined.has_value());
    CHECK(inlined.error().code == core::net::NetErrorCode::NameNotFound);
}

TEST_CASE("a held scripted lookup parks the flow until it is released", "[net]")
{
    // The scripted resolver's hold, driven across the thread it is for: the flow stays parked while
    // the worker is held inside the lookup, and resumes with the scripted answer once released.
    auto script = ScriptedAddressResolver {};
    script.answer("db.test", { ResolvedEndpoint { .length = 16, .family = 2, .protocol = 6 } });
    script.hold();
    auto resolver = ThreadedAddressResolver { script, { .threads = 1, .maxQueueDepth = 8 } };
    auto const releaseOnExit = core::net::detail::ScopeGuard { [&script]() noexcept { script.release(); } };

    auto backend = core::net::makeBackend(core::net::preferredBackendKind());
    REQUIRE(backend != nullptr);
    auto loop = EventLoop { *backend };

    auto answer = ResolveResult { std::unexpected(core::net::makeNetError(core::net::NetErrorCode::Ok)) };
    auto resumedOn = std::thread::id {};
    loop.spawn(resolveOn(&loop, &resolver, "db.test", &answer, &resumedOn));
    loop.runUntilIdle();
    REQUIRE(script.awaitEntered(1, EnterBudget) == 1);
    loop.runUntilIdle();
    CHECK(resumedOn == std::thread::id {}); // still parked: the worker is held

    script.release();
    for ([[maybe_unused]] auto const turn: std::views::iota(0, 5000))
    {
        if (resumedOn != std::thread::id {})
            break;
        loop.runUntilIdle();
        std::this_thread::sleep_for(std::chrono::milliseconds { 1 });
    }
    CHECK(resumedOn == std::this_thread::get_id());
    REQUIRE(answer.has_value());
    CHECK(answer->size() == 1);
    resolver.stop();
}

TEST_CASE("a host no name can be is refused before any resolver thread sees it", "[net]")
{
    // The same refusal as ExecutorAddressResolver's: an empty host (getaddrinfo's wildcard), a NUL,
    // or more than 253 bytes.
    auto const tooLong = std::string(254, 'a');
    auto const host = GENERATE_COPY(std::string {}, std::string { "example.test\0evil", 17 }, tooLong);
    auto const viaLoop = GENERATE(true, false);
    CAPTURE(host.size(), viaLoop);
    auto inner = ScriptedAddressResolver {};
    scriptExample(inner);
    auto resolver = ThreadedAddressResolver { inner };
    auto clock = core::platform::ManualClock {};
    auto loop = core::net::testing::TestLoop { clock };
    auto answer = ResolveResult {};
    auto resumedOn = std::thread::id {};
    loop.blockOn(resolveOn(viaLoop ? &loop : nullptr, &resolver, host, &answer, &resumedOn));
    REQUIRE_FALSE(answer.has_value());
    CHECK(answer.error().code == core::net::NetErrorCode::AddressError);
    CHECK(resolver.offloaded() == 0);
    CHECK(inner.calls() == 0);
}
