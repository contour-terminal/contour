// SPDX-License-Identifier: Apache-2.0
#include <contour/remote/ReactorThread.hpp>
#include <contour/remote/RemoteController.hpp>

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>

#include <coro/Cancellation.hpp>
#include <coro/Task.hpp>
#include <net/EventLoop.hpp>

using namespace std::chrono_literals;
using contour::remote::awaitMuxConnect;
using contour::remote::makeUnboundFallbackPty;
using contour::remote::MuxConnectPhase;

TEST_CASE("awaitMuxConnect reports Ready without waiting once the phase is set", "[mux][controller]")
{
    auto mutex = std::mutex {};
    auto cv = std::condition_variable {};
    auto phase = MuxConnectPhase::Ready; // already transitioned
    auto const failure = std::string {};

    auto const outcome = awaitMuxConnect(mutex, cv, phase, failure, 5s);

    CHECK(outcome.ready);
    CHECK_FALSE(outcome.timedOut);
}

TEST_CASE("awaitMuxConnect surfaces the recorded failure reason", "[mux][controller]")
{
    auto mutex = std::mutex {};
    auto cv = std::condition_variable {};
    auto phase = MuxConnectPhase::Failed;
    auto const failure = std::string { "daemon refused" };

    auto const outcome = awaitMuxConnect(mutex, cv, phase, failure, 5s);

    CHECK_FALSE(outcome.ready);
    CHECK_FALSE(outcome.timedOut);
    CHECK(outcome.failure == "daemon refused");
}

TEST_CASE("awaitMuxConnect times out while still Connecting", "[mux][controller]")
{
    auto mutex = std::mutex {};
    auto cv = std::condition_variable {};
    auto phase = MuxConnectPhase::Connecting; // never transitions
    auto const failure = std::string {};

    auto const outcome = awaitMuxConnect(mutex, cv, phase, failure, 20ms);

    CHECK(outcome.timedOut);
    CHECK_FALSE(outcome.ready);
}

TEST_CASE("awaitMuxConnect wakes on a cross-thread transition", "[mux][controller]")
{
    auto mutex = std::mutex {};
    auto cv = std::condition_variable {};
    auto phase = MuxConnectPhase::Connecting;
    auto const failure = std::string {};

    // The reactor thread: transition to Ready and notify, as a controller does.
    auto reactor = std::thread { [&] {
        std::this_thread::sleep_for(5ms);
        {
            auto const lock = std::lock_guard { mutex };
            phase = MuxConnectPhase::Ready;
        }
        cv.notify_all();
    } };

    auto const outcome = awaitMuxConnect(mutex, cv, phase, failure, 5s);
    reactor.join();

    CHECK(outcome.ready);
    CHECK_FALSE(outcome.timedOut);
}

TEST_CASE("makeUnboundFallbackPty honors the requested size, else defaults", "[mux][controller]")
{
    auto const sized =
        makeUnboundFallbackPty(vtbackend::PageSize { vtbackend::LineCount(10), vtbackend::ColumnCount(40) });
    REQUIRE(sized != nullptr);
    CHECK(sized->pageSize() == vtpty::PageSize { vtpty::LineCount(10), vtpty::ColumnCount(40) });

    auto const defaulted = makeUnboundFallbackPty(std::nullopt);
    REQUIRE(defaulted != nullptr);
    CHECK(defaulted->pageSize() == vtpty::PageSize { vtpty::LineCount(25), vtpty::ColumnCount(80) });
}

TEST_CASE("SelfUnbindingChannelPty runs its on-destroy callback exactly once", "[mux][controller]")
{
    auto unbinds = 0;
    {
        auto pty = contour::remote::SelfUnbindingChannelPty { vtpty::PageSize { vtpty::LineCount(12),
                                                                                vtpty::ColumnCount(40) },
                                                              {}, // write sink — unused by this test
                                                              {}, // resize sink — unused by this test
                                                              [&unbinds] { ++unbinds; } };
        CHECK(pty.pageSize() == vtpty::PageSize { vtpty::LineCount(12), vtpty::ColumnCount(40) });
        CHECK(unbinds == 0); // not until the terminal destroys it
    }
    CHECK(unbinds == 1); // exactly once, from the destructor
}

TEST_CASE("stopMuxReactor tears the reactor down once and ignores a second stop", "[mux][controller]")
{
    auto mutex = std::mutex {};
    auto stopped = false;
    auto reactor = contour::remote::ReactorThread {};

    // A root task that parks forever, so only requestStop can end it.
    reactor.start([](net::EventLoop* loop) -> coro::Task<void> {
        while (true)
            co_await loop->delay(1h);
    });

    auto detaches = std::atomic<int> { 0 };
    auto const detach = [&detaches] {
        detaches.fetch_add(1, std::memory_order_relaxed);
    };

    // First stop: performs the teardown (posts the detach, cancels, joins).
    CHECK(contour::remote::stopMuxReactor(mutex, stopped, reactor, detach));
    CHECK(stopped);
    CHECK(detaches.load(std::memory_order_relaxed) == 1);
    CHECK(reactor.wasCancelled());

    // Second stop: already stopped, so it neither re-posts nor re-joins.
    CHECK_FALSE(contour::remote::stopMuxReactor(mutex, stopped, reactor, detach));
    CHECK(detaches.load(std::memory_order_relaxed) == 1);
}

namespace
{

/// A root task that runs @p thrower before its first suspension.
///
/// The `if` is what keeps `co_return` reachable, and so keeps this a coroutine at all — an
/// unconditional throw would make it a plain function and never exercise `blockOn`'s rethrow.
/// @param thrower Invoked on the reactor thread; must throw.
[[nodiscard]] coro::Task<void> throwingRootTask(std::function<void()> thrower)
{
    if (thrower)
        thrower();
    co_return;
}

/// Wraps @ref throwingRootTask into the factory ReactorThread::start takes.
///
/// The factory is a plain lambda that CALLS the coroutine rather than being one itself: a
/// capturing lambda that is a coroutine outlives its own closure object
/// (cppcoreguidelines-avoid-capturing-lambda-coroutines), whereas a by-value coroutine PARAMETER
/// is copied into the frame and safe.
/// @param thrower Invoked on the reactor thread; must throw.
/// @return The factory.
[[nodiscard]] auto rootTaskThrowing(std::function<void()> thrower)
{
    return [thrower = std::move(thrower)](net::EventLoop*) {
        return throwingRootTask(thrower);
    };
}

} // namespace

// The reactor runs the whole remote client flow: PDU decode of frames up to proto::MaxFrameSize,
// vectors and strings grown to peer-named lengths, image data allocated from a daemon announcement.
// bad_alloc and length_error are therefore reachable from the wire — and an exception leaving a
// std::thread's function is unconditional std::terminate: no unwind, no handler, the whole Contour
// process gone with every local, non-daemon tab in every window. The daemon side already isolates
// each flow for exactly this reason (ConnectionAcceptor::superviseConnection); this is the client's
// half of that guarantee.
TEST_CASE("the reactor thread survives an exception escaping the root task", "[mux][controller]")
{
    auto reactor = contour::remote::ReactorThread {};
    auto reported = std::string {};

    reactor.start(rootTaskThrowing([] { throw std::runtime_error { "malformed peer" }; }),
                  [&reported](std::string const& reason) { reported = reason; });
    reactor.join(); // reaching this at all is the assertion: a terminate() never gets here

    CHECK_FALSE(reactor.wasCancelled());
    CHECK(reactor.failure() == "malformed peer");
    // The owner is told, on the reactor thread, so it can settle its own connection state instead of
    // leaving the GUI to discover a dead reactor by timing out.
    CHECK(reported == "malformed peer");
}

TEST_CASE("the reactor thread survives a non-std exception too", "[mux][controller]")
{
    auto reactor = contour::remote::ReactorThread {};
    // Deliberately not derived from std::exception: a third-party library's throw need not be, and
    // the thread body must still not let it escape.
    struct ForeignThrow
    {
    };

    reactor.start(rootTaskThrowing([] { throw ForeignThrow {}; }));
    reactor.join();

    CHECK(reactor.failure() == "unknown exception");
}

TEST_CASE("a cancelled reactor root task is still the silent shutdown", "[mux][controller]")
{
    // requestStop() unwinds the root task by throwing OperationCancelled; that is the INTENDED
    // shutdown and must stay distinguishable from a failure — the catch-all above must not swallow
    // it into a reported error.
    auto reactor = contour::remote::ReactorThread {};
    auto reported = std::string {};

    reactor.start(rootTaskThrowing([] { throw coro::OperationCancelled {}; }),
                  [&reported](std::string const& reason) { reported = reason; });
    reactor.join();

    CHECK(reactor.wasCancelled());
    CHECK(reactor.failure().empty());
    CHECK(reported.empty());
}
