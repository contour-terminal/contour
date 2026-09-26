// SPDX-License-Identifier: Apache-2.0
///
/// The host-driven backend, on every platform.
///
/// It exists for a loop that does not own its thread — a browser's, a Qt
/// application's — and the temptation is to test it only where such a loop exists.
/// That would leave its behaviour observable exclusively in a node run, which is the
/// one place a regression is hardest to read. @c testing::ManualHostScheduler is the
/// host instead: the case asserts the exact delay the backend asked for and fires the
/// callback itself, so this runs on Linux, macOS, Windows and WebAssembly alike.
#include <core/net/HostDrivenBackend.hpp>
#include <core/net/IoBackend.hpp>
#include <core/net/testing/BackendMatrix.hpp>
#include <core/net/testing/ManualHostScheduler.hpp>
#include <core/net/testing/NullBackend.hpp>
#include <core/net/testing/ScriptedBackend.hpp>
#include <core/platform/Clock.hpp>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <memory>
#include <optional>

using core::net::HostDrivenBackend;
using core::net::Interest;
using core::net::ReadinessHandler;
using core::net::testing::ManualHostScheduler;
using core::platform::ManualClock;

namespace
{

/// Counts the pumps a backend delivered to its driver, which is what an `EventLoop`
/// will be once Task B4 wires one up.
struct PumpCounter
{
    int pumps = 0;

    static void onPump(void* state) noexcept { ++static_cast<PumpCounter*>(state)->pumps; }
};

} // namespace

TEST_CASE("a host-driven backend has no readiness, and says so", "[net][backend][hostdriven]")
{
    // A refusal rather than a silent acceptance: accepting would park a flow on a
    // registration nothing can ever report, which is a hang with no message. Readiness
    // in a browser goes through its own APIs and reaches the loop as a `post`.
    auto host = ManualHostScheduler {};
    auto clock = ManualClock {};
    auto backend = HostDrivenBackend { host, clock };

    auto handler = ReadinessHandler {};
    auto const attached = backend.attach(handler);
    REQUIRE_FALSE(attached.has_value());
    CHECK(attached.error().code == core::net::NetErrorCode::Unsupported);

    auto const armed = backend.setInterest(handler, Interest::Read);
    REQUIRE_FALSE(armed.has_value());
    CHECK(armed.error().code == core::net::NetErrorCode::Unsupported);

    // And detaching what was refused is harmless, or every teardown path would have to
    // remember which backend it is on.
    backend.detach(handler);
}

TEST_CASE("a host-driven wait returns at once, whatever the timeout", "[net][backend][hostdriven]")
{
    // There is nothing to block on and, under single-threaded WebAssembly, nothing to
    // block with. The timeout is not ignored but DELEGATED: it reaches the host
    // through armWakeAt, and the host is what waits.
    auto host = ManualHostScheduler {};
    auto clock = ManualClock {};
    auto backend = HostDrivenBackend { host, clock };

    CHECK(backend.wait(std::nullopt) == core::net::WaitResult {});
    CHECK(backend.wait(std::chrono::hours { 24 }) == core::net::WaitResult {});
    CHECK(backend.wait(core::platform::SteadyDuration::zero()) == core::net::WaitResult {});

    // A wait asks the host for nothing: only wake() and armWakeAt() do.
    CHECK(host.requestCount() == 0);
}

TEST_CASE("two wakes in one turn schedule exactly one pump", "[net][backend][hostdriven]")
{
    // Without the coalescing, a burst of posts queues one browser timer each and the
    // page spends its frame budget in the scheduler.
    auto host = ManualHostScheduler {};
    auto clock = ManualClock {};
    auto backend = HostDrivenBackend { host, clock };
    auto counter = PumpCounter {};
    backend.setPump(&PumpCounter::onPump, &counter);

    backend.wake();
    backend.wake();
    backend.wake();

    CHECK(host.pendingCount() == 1);
    CHECK(backend.pumpScheduled());
    CHECK(host.pending().front().delay == std::chrono::milliseconds { 0 });

    host.pump();
    CHECK(counter.pumps == 1);
    CHECK(backend.pumpCount() == 1);
    CHECK_FALSE(backend.pumpScheduled());

    // And the next turn may ask again: the coalescing is per pump, not for ever.
    backend.wake();
    CHECK(host.pendingCount() == 1);
}

TEST_CASE("a pump clears its own schedule before it runs, so the turn can re-arm",
          "[net][backend][hostdriven]")
{
    // The ordering that keeps a host-driven loop alive. A pump drives one turn; that
    // turn arms the next deadline and may wake for work it queued. If the flag were
    // cleared AFTER the pump, both of those would be dropped as "one is already
    // scheduled" — and the one scheduled is the pump that is running. The loop would
    // then stop, silently, on the first turn that produced more work.
    auto host = ManualHostScheduler {};
    auto clock = ManualClock {};
    auto backend = HostDrivenBackend { host, clock };

    auto rearm = [](void* state) noexcept {
        static_cast<HostDrivenBackend*>(state)->wake();
    };
    backend.setPump(rearm, &backend);

    backend.wake();
    REQUIRE(host.pendingCount() == 1);

    host.pump();
    CHECK(backend.pumpCount() == 1);
    CHECK(host.pendingCount() == 1); // the turn's own wake, asked for from inside the pump
    CHECK(backend.pumpScheduled());
}

TEST_CASE("armWakeAt asks the host for the deadline's delay, clamped at zero", "[net][backend][hostdriven]")
{
    auto host = ManualHostScheduler {};
    auto clock = ManualClock {};
    auto backend = HostDrivenBackend { host, clock };

    SECTION("a deadline in the future is the delay the host is given")
    {
        backend.armWakeAt(clock.now() + std::chrono::milliseconds { 250 });
        REQUIRE(host.pendingCount() == 1);
        CHECK(host.pending().front().delay == std::chrono::milliseconds { 250 });
    }

    SECTION("a deadline already past asks for the next turn, not a negative delay")
    {
        // `setTimeout` reads a negative delay as zero on one host and refuses it on
        // another, so the clamp is here rather than in each host.
        clock.advance(std::chrono::seconds { 1 });
        backend.armWakeAt(clock.now() - std::chrono::milliseconds { 500 });
        REQUIRE(host.pendingCount() == 1);
        CHECK(host.pending().front().delay == std::chrono::milliseconds { 0 });
    }

    SECTION("no deadline schedules nothing")
    {
        // A loop with no timer must not be pumped at the host's timer resolution for
        // having nothing to do: only a wake should bring it back.
        backend.armWakeAt(std::nullopt);
        CHECK(host.pendingCount() == 0);
        CHECK_FALSE(backend.pumpScheduled());
    }

    SECTION("a later deadline does not displace a pump already scheduled")
    {
        backend.wake(); // due now
        backend.armWakeAt(clock.now() + std::chrono::seconds { 5 });
        CHECK(host.pendingCount() == 1);
        CHECK(host.pending().front().delay == std::chrono::milliseconds { 0 });
    }

    SECTION("an earlier deadline is scheduled beside the later one, not instead of it")
    {
        // A host's timer cannot be retracted, so the later pump still arrives. That
        // costs one empty turn; dropping the earlier request would cost a deadline
        // that never fires.
        backend.armWakeAt(clock.now() + std::chrono::seconds { 5 });
        backend.armWakeAt(clock.now() + std::chrono::milliseconds { 10 });
        REQUIRE(host.pendingCount() == 2);
        CHECK(host.pending().front().delay == std::chrono::seconds { 5 });
        CHECK(host.pending().back().delay == std::chrono::milliseconds { 10 });
    }
}

TEST_CASE("a pump with no driver still runs, and still clears its schedule", "[net][backend][hostdriven]")
{
    // A backend is usable before a loop has claimed it — which is what lets these
    // cases exist at all, and what Task B4's EventLoop relies on: it needs the backend
    // before it can hand over a pointer to itself.
    auto host = ManualHostScheduler {};
    auto clock = ManualClock {};
    auto backend = HostDrivenBackend { host, clock };

    backend.wake();
    host.pump();

    CHECK(backend.pumpCount() == 1);
    CHECK_FALSE(backend.pumpScheduled());
}

TEST_CASE("only the host-driven backend reports itself host-driven", "[net][backend][hostdriven]")
{
    auto host = ManualHostScheduler {};
    auto clock = ManualClock {};
    auto backend = HostDrivenBackend { host, clock };
    CHECK(backend.kind() == core::net::BackendKind::HostDriven);
    CHECK(backend.isHostDriven());

    // The predicate decides whether a loop may `run()` or `blockOn()` at all, so a
    // backend that answered it wrongly would be a loop that blocks a browser's thread
    // or one that never advances. Every other backend this platform has says no.
    for (auto const& entry: core::net::testing::BackendMatrix)
        if (auto const other = core::net::makeBackend(entry.kind))
            CHECK_FALSE(other->isHostDriven());

    auto scripted = core::net::testing::ScriptedBackend {};
    auto null = core::net::testing::NullBackend {};
    CHECK_FALSE(scripted.isHostDriven());
    CHECK_FALSE(null.isHostDriven());
}

TEST_CASE("the default backend on a host-driven platform is the host-driven one",
          "[net][backend][hostdriven]")
{
    // On Emscripten this is the whole of what makeDefaultBackend() can answer; on
    // every other platform it must NOT be, or a native loop would never block and
    // would spin instead.
    auto const backend = core::net::makeDefaultBackend();
    REQUIRE(backend != nullptr);

#ifdef __EMSCRIPTEN__
    CHECK(backend->kind() == core::net::BackendKind::HostDriven);
    CHECK(backend->isHostDriven());
    CHECK(core::net::preferredBackendKind() == core::net::BackendKind::HostDriven);
#else
    CHECK_FALSE(backend->isHostDriven());
    CHECK(core::net::preferredBackendKind() != core::net::BackendKind::HostDriven);
    // And a host-driven backend never comes from the factory here: it needs the
    // IHostScheduler its host provides, which a no-argument factory has nowhere to
    // take from.
    CHECK(core::net::makeBackend(core::net::BackendKind::HostDriven) == nullptr);
#endif
}

TEST_CASE("a pump that arrives after its backend is gone runs nothing", "[net][backend][hostdriven]")
{
    // A host's timer cannot be retracted, so a backend destroyed with a pump out leaves that pump
    // to arrive later. It used to arrive with the backend's address and write into freed storage.
    auto host = ManualHostScheduler {};
    auto clock = ManualClock {};
    auto counter = PumpCounter {};
    auto backend = std::make_unique<HostDrivenBackend>(host, clock);
    backend->setPump(&PumpCounter::onPump, &counter);
    backend->wake();
    backend->armWakeAt(clock.now());                              // coalesced into the wake's pump
    backend->armWakeAt(clock.now() + std::chrono::seconds { 5 }); // coalesced as well
    REQUIRE(host.pendingCount() == 1);

    backend.reset();
    host.pump();
    CHECK(counter.pumps == 0);
    CHECK(host.pendingCount() == 0);
}

TEST_CASE("a host destroyed with pumps pending delivers them rather than leaking what they own",
          "[net][backend][hostdriven]")
{
    // What a pump carries is freed by the pump. A test double that dropped its pending requests on
    // destruction would leak one per case that ends with a pump out, which is most of them.
    auto counter = PumpCounter {};
    {
        auto host = ManualHostScheduler {};
        auto clock = ManualClock {};
        auto backend = HostDrivenBackend { host, clock };
        backend.setPump(&PumpCounter::onPump, &counter);
        backend.wake();
        REQUIRE(host.pendingCount() == 1);
    }
    CHECK(counter.pumps == 0);
}
