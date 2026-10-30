// SPDX-License-Identifier: Apache-2.0
#include <contour/remote/ReactorThread.hpp>
#include <contour/remote/RemoteController.hpp>

#include <core/async/Cancellation.hpp>
#include <core/async/StopToken.hpp>
#include <core/async/Task.hpp>
#include <core/async/WhenAny.hpp>
#include <core/net/EventLoop.hpp>
#include <core/net/InterruptibleSleep.hpp>
#include <core/platform/Clock.hpp>

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <tuple>

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
    reactor.start([](core::net::EventLoop* loop) -> core::async::Task<void> {
        while (true)
            co_await loop->delay(1h);
    });

    auto detaches = std::atomic<int> { 0 };
    auto const detach = [&detaches] {
        detaches.fetch_add(1, std::memory_order_relaxed);
    };

    // First stop: performs the teardown (posts the detach, cancels, joins).
    // The root task ignores the detach, so the stop comes after the bound.
    CHECK(contour::remote::stopMuxReactor(mutex, stopped, reactor, detach, std::chrono::milliseconds { 50 }));
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
[[nodiscard]] core::async::Task<void> throwingRootTask(std::function<void()> thrower)
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
    return [thrower = std::move(thrower)](core::net::EventLoop*) {
        return throwingRootTask(thrower);
    };
}

/// Counts the frames of @ref sleepCountingItsEnd that have ended, however they ended.
class EndCounter
{
  public:
    explicit EndCounter(std::atomic<int>* ended): _ended { ended } {}
    EndCounter(EndCounter const&) = delete;
    EndCounter& operator=(EndCounter const&) = delete;
    EndCounter(EndCounter&&) = delete;
    EndCounter& operator=(EndCounter&&) = delete;
    ~EndCounter() { ++*_ended; }

  private:
    std::atomic<int>* _ended;
};

/// Sleeps for an hour unless @p token stops first, counting itself into @p parked before it parks and
/// into @p ended as its frame goes, so a frame torn down twice shows as one end too many.
/// @param loop The reactor's loop.
/// @param token What wakes the sleep early.
/// @param parked Raised just before the sleep parks.
/// @param ended Raised as the frame ends.
[[nodiscard]] core::async::Task<void> sleepCountingItsEnd(core::net::EventLoop* loop,
                                                          core::async::StopToken token,
                                                          std::atomic<int>* parked,
                                                          std::atomic<int>* ended)
{
    auto const counter = EndCounter { ended };
    ++*parked;
    std::ignore =
        co_await core::net::interruptibleSleepUntil(loop, std::move(token), loop->clock().now() + 1h);
}

/// A root task racing two sleeps, as ClientSupervisor races its connect against its stop.
[[nodiscard]] core::async::Task<void> racingRootTask(core::net::EventLoop* loop,
                                                     core::async::StopToken wake,
                                                     std::atomic<int>* parked,
                                                     std::atomic<int>* ended)
{
    std::ignore =
        co_await core::async::whenAny(sleepCountingItsEnd(loop, std::move(wake), parked, ended),
                                      sleepCountingItsEnd(loop, core::async::StopToken {}, parked, ended));
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

TEST_CASE("a throw from a posted callback stops the reactor in order", "[mux][controller][threading]")
{
    // A posted callback is the owner's code, run inside a loop turn. Its throw used to leave that turn
    // with the root task still suspended: blockOn then destroyed the root, and with it the whenAny
    // runner the same turn had queued to resume, so the loop's teardown resumed a freed frame -- the
    // sleep below ended twice, and a reconnect's thread state was handed to OpenSSL as well.
    auto stopSource = core::async::StopSource {};
    auto parked = std::atomic<int> { 0 };
    auto ended = std::atomic<int> { 0 };
    auto reported = std::string {};
    {
        auto reactor = contour::remote::ReactorThread {};
        reactor.start(
            [&](core::net::EventLoop* loop) {
                return racingRootTask(loop, stopSource.get_token(), &parked, &ended);
            },
            [&reported](std::string const& reason) { reported = reason; });
        while (parked.load() < 2)
            std::this_thread::yield();

        // Wakes one sleep, which queues its resumption in this very turn, and then throws.
        reactor.post([&stopSource] {
            stopSource.request_stop();
            throw std::runtime_error { "posted callback failed" };
        });
        reactor.join();

        CHECK_FALSE(reactor.wasCancelled());
        CHECK(reactor.failure() == "posted callback failed");
    } // the loop's teardown: where a frame left queued would be resumed after it was freed

    CHECK(reported == "posted callback failed");
    CHECK(ended.load() == 2);
}

TEST_CASE("a cancelled reactor root task is still the silent shutdown", "[mux][controller]")
{
    // requestStop() unwinds the root task by throwing OperationCancelled; that is the INTENDED
    // shutdown and must stay distinguishable from a failure — the catch-all above must not swallow
    // it into a reported error.
    auto reactor = contour::remote::ReactorThread {};
    auto reported = std::string {};

    reactor.start(rootTaskThrowing([] { throw core::async::OperationCancelled {}; }),
                  [&reported](std::string const& reason) { reported = reason; });
    reactor.join();

    CHECK(reactor.wasCancelled());
    CHECK(reactor.failure().empty());
    CHECK(reported.empty());
}

namespace
{

/// What a ConnectingController's connection does once released.
enum class OnRelease : uint8_t
{
    ComeUp, ///< The handshake completes, as a daemon's answer would complete it.
    Fail,   ///< The connect fails, as a refused or reset one would.
};

/// A controller still connecting: its root task parks until cancelled, and it has nothing to detach --
/// unless it is given a release, on which its connection comes up (or fails) as a daemon's answer would.
class ConnectingController final: public contour::remote::RemoteController
{
  public:
    /// @param clock What the connect budget is measured on.
    /// @param release Once it holds true, the connection settles; null to stay connecting forever.
    /// @param onRelease How it settles.
    explicit ConnectingController(core::platform::IClock const& clock,
                                  std::atomic<bool> const* release = nullptr,
                                  OnRelease onRelease = OnRelease::ComeUp):
        RemoteController(clock), _release(release), _onRelease(onRelease)
    {
    }
    ConnectingController(ConnectingController const&) = delete;
    ConnectingController& operator=(ConnectingController const&) = delete;
    ConnectingController(ConnectingController&&) = delete;
    ConnectingController& operator=(ConnectingController&&) = delete;
    ~ConnectingController() { stop(); }

    using RemoteController::isConnected;

    /// @return Whether stop() has torn the controller down.
    [[nodiscard]] bool stopped() const
    {
        auto const lock = std::lock_guard { _mutex };
        return _stopped;
    }

  protected:
    [[nodiscard]] core::async::Task<void> runClient(core::net::EventLoop* loop) override
    {
        if (_release != nullptr)
        {
            while (!_release->load(std::memory_order_acquire))
                co_await loop->delay(1ms);
            if (_onRelease == OnRelease::Fail)
                failConnect("refused");
            else
            {
                {
                    auto const lock = std::lock_guard { _mutex };
                    _state = State::Ready;
                }
                _connected.notify_all();
            }
        }
        while (true)
            co_await loop->delay(1h);
    }
    [[nodiscard]] bool detachOnReactor() override { return false; }
    void closeReactorBindings() override {}
    [[nodiscard]] std::string connectTimeoutMessage() const override { return "timed out"; }
    [[nodiscard]] std::string connectClosedMessage() const override { return "closed"; }

  private:
    std::atomic<bool> const* _release;
    OnRelease _onRelease;
};

/// Releases @p release from another thread after @p after, while the caller blocks in awaitConnected().
/// @return The releasing thread; joined on destruction.
[[nodiscard]] std::jthread releaseAfter(std::atomic<bool>& release, std::chrono::milliseconds after)
{
    return std::jthread { [&release, after] {
        std::this_thread::sleep_for(after);
        release.store(true, std::memory_order_release);
    } };
}

} // namespace

TEST_CASE("a stop during connect does not wait out the detach bound", "[mux][controller]")
{
    // The connect times out and stops the controller. With no client to detach, nothing will end
    // the root task by itself, so stop() must cancel at once rather than wait out
    // DefaultDetachBound on the GUI thread.
    auto controller = ConnectingController { core::platform::defaultSteadyClock() };
    auto const started = std::chrono::steady_clock::now();
    auto const connected = controller.connectAndWait(50ms);
    auto const took = std::chrono::steady_clock::now() - started;
    CHECK_FALSE(connected.has_value());
    CHECK(took < contour::remote::DefaultDetachBound / 2);
}

TEST_CASE("startConnecting returns before the daemon answers, awaitConnected then succeeds",
          "[remote][mux][controller]")
{
    auto release = std::atomic<bool> { false };
    auto controller = ConnectingController { core::platform::defaultSteadyClock(), &release };
    controller.startConnecting();
    controller.startConnecting(); // idempotent: one reactor, one connect
    CHECK_FALSE(controller.isConnected());

    release.store(true, std::memory_order_release);
    auto const connected = controller.awaitConnected(10s);
    CHECK(connected.has_value());
    CHECK(controller.isConnected());
}

TEST_CASE("awaitConnected past its budget fails with the timeout message and stops",
          "[remote][mux][controller]")
{
    auto split = ConnectingController { core::platform::defaultSteadyClock() };
    split.startConnecting();
    auto const awaited = split.awaitConnected(200ms);
    REQUIRE_FALSE(awaited.has_value());
    CHECK(split.stopped());

    auto joined = ConnectingController { core::platform::defaultSteadyClock() };
    auto const waited = joined.connectAndWait(200ms);
    REQUIRE_FALSE(waited.has_value());
    CHECK(awaited.error() == waited.error());
    CHECK(awaited.error() == "timed out");
}

TEST_CASE("awaitConnected counts its budget from startConnecting", "[remote][mux][controller]")
{
    auto clock = core::platform::ManualClock {};
    auto controller = ConnectingController { clock };
    controller.startConnecting();
    clock.advance(5s); // booting the GUI took the whole budget

    auto const started = std::chrono::steady_clock::now();
    auto const connected = controller.awaitConnected(5s);
    auto const took = std::chrono::steady_clock::now() - started;
    REQUIRE_FALSE(connected.has_value());
    CHECK(connected.error() == "timed out");
    CHECK(controller.stopped());
    CHECK(took >= contour::remote::MinimumConnectAwait); // the floor, not zero
    CHECK(took < 4s);                                    // and not a fresh 5 s
}

TEST_CASE("a connect that comes up within the floor succeeds although the budget is spent",
          "[remote][mux][controller]")
{
    auto clock = core::platform::ManualClock {};
    auto release = std::atomic<bool> { false };
    auto controller = ConnectingController { clock, &release };
    controller.startConnecting();
    clock.advance(6s); // a stalled GUI boot spent more than the whole budget

    auto const releaser = releaseAfter(release, 200ms);
    auto const connected = controller.awaitConnected(5s);
    CHECK(connected.has_value());
    CHECK(controller.isConnected());
}

TEST_CASE("a connect that fails within the floor still fails, and at once", "[remote][mux][controller]")
{
    auto clock = core::platform::ManualClock {};
    auto release = std::atomic<bool> { false };
    auto controller = ConnectingController { clock, &release, OnRelease::Fail };
    controller.startConnecting();
    clock.advance(6s);

    auto const releaser = releaseAfter(release, 200ms);
    auto const started = std::chrono::steady_clock::now();
    auto const connected = controller.awaitConnected(5s);
    auto const took = std::chrono::steady_clock::now() - started;
    REQUIRE_FALSE(connected.has_value());
    CHECK(connected.error() == "refused");
    CHECK(took < contour::remote::MinimumConnectAwait); // the failure ends the wait, not the floor
}
