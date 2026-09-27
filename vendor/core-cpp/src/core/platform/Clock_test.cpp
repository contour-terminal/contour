// SPDX-License-Identifier: Apache-2.0
#include <core/platform/Clock.hpp>

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <type_traits>

// Single-threaded WebAssembly has no threads to start (Part I §1).
#if !defined(__EMSCRIPTEN__) || defined(__EMSCRIPTEN_PTHREADS__)
    #define CORE_CPP_TEST_THREADS 1
    #include <ranges>
    #include <thread>
    #include <vector>
#else
    #define CORE_CPP_TEST_THREADS 0
#endif

using namespace std::chrono_literals;
using core::platform::CachedClock;
using core::platform::IWallClock;
using core::platform::ManualClock;
using core::platform::ManualWallClock;
using core::platform::SteadyClock;
using core::platform::SteadyTimePoint;
using core::platform::SystemWallClock;
using core::platform::WallClockRef;

namespace
{

// The borrow guard, asserted as a property of the TYPES rather than demonstrated by a probe.
// Each refusal is stated beside the acceptance it must not have taken with it: on its own,
// `!is_constructible_v<WallClockRef, SystemWallClock>` is also true of a type constructible from
// nothing at all. Shown red by planting: deleting `WallClockRef(IWallClock const&&) = delete;`
// fires every negative row and leaves every positive row green.
static_assert(std::is_constructible_v<WallClockRef, SystemWallClock&>,
              "a named clock must still be borrowable, or the guard refuses every honest caller");
static_assert(!std::is_constructible_v<WallClockRef, SystemWallClock>,
              "a temporary must be refused: the borrow outlives the full expression");
static_assert(std::is_constructible_v<WallClockRef, ManualWallClock&>,
              "any IWallClock, not just the system one");
static_assert(!std::is_constructible_v<WallClockRef, ManualWallClock>, "including the one tests reach for");

// ...and the reason no rvalue is left for the guard to be too strict about.
static_assert(!std::is_copy_constructible_v<IWallClock> && !std::is_move_constructible_v<IWallClock>,
              "nothing may return an IWallClock by value (see IWallClock's deleted constructors)");

// The retainers, which hold the borrow. fastcached asserts these rows over its own types
// (FleetHistory, SchedulerService, CacheEngine); these test-local ones have the same shapes.

/// Keeps a borrowed wall clock the way a type should: a WallClockRef, taken and stored by value.
struct Retainer
{
    explicit Retainer(WallClockRef borrowed) noexcept: wall { borrowed } {}

    WallClockRef wall;
};

/// Stores no clock of its own and hands its borrow on to two retainers, among other arguments:
/// the shape fastcached#1028 had, where the temporary went to such a type. The borrow is a value,
/// so handing it on copies it, and the refusal already happened at this constructor's parameter.
struct Forwarder
{
    Forwarder(ManualClock& steadyClock, WallClockRef borrowed, int label) noexcept:
        first { borrowed }, second { borrowed }, steady { steadyClock }, tag { label }
    {
    }

    Retainer first;
    Retainer second;
    ManualClock& steady;
    int tag;
};

static_assert(std::is_constructible_v<Retainer, SystemWallClock&>, "lvalue into a retainer");
static_assert(!std::is_constructible_v<Retainer, SystemWallClock>, "rvalue into a retainer");
static_assert(std::is_constructible_v<Forwarder, ManualClock&, SystemWallClock&, int>,
              "lvalue through a forwarder, among other arguments");
static_assert(!std::is_constructible_v<Forwarder, ManualClock&, SystemWallClock, int>,
              "rvalue through a forwarder, among other arguments");
static_assert(!std::is_constructible_v<Forwarder, ManualClock&, ManualWallClock, int>,
              "including the clock tests reach for");

// For contrast, the guard WallClockRef replaced: a deleted rvalue overload on the storing type. It
// refuses a direct temporary...

/// Guards its borrow with a deleted `IWallClock const&&` overload.
struct OverloadRetainer
{
    explicit OverloadRetainer(IWallClock const& borrowed) noexcept: wall { &borrowed } {}
    explicit OverloadRetainer(IWallClock const&&) = delete;

    IWallClock const* wall;
};

/// Hands a reference on to an OverloadRetainer: inside it, the parameter is a named lvalue.
struct OverloadForwarder
{
    explicit OverloadForwarder(IWallClock const& borrowed) noexcept: retainer { borrowed } {}

    OverloadRetainer retainer;
};

static_assert(std::is_constructible_v<OverloadRetainer, SystemWallClock&>
                  && !std::is_constructible_v<OverloadRetainer, SystemWallClock>,
              "a deleted rvalue overload refuses a direct temporary...");
static_assert(std::is_constructible_v<OverloadForwarder, SystemWallClock>,
              "...and lets one through a forwarding constructor, which is why WallClockRef is a value");

} // namespace

TEST_CASE("SteadyClock::now is monotonic", "[clock]")
{
    auto const clock = SteadyClock {};
    auto const first = clock.now();
    auto const second = clock.now();
    REQUIRE(second >= first);
}

TEST_CASE("ManualClock starts at the constructed value and does not advance on its own", "[clock]")
{
    auto const start = SteadyTimePoint {} + 1s;
    auto const clock = ManualClock { start };
    REQUIRE(clock.now() == start);
    // Reading twice yields the same value: time is frozen until advance/setNow.
    REQUIRE(clock.now() == start);
}

TEST_CASE("ManualClock::advance moves time forward by the delta, and advances accumulate", "[clock]")
{
    auto clock = ManualClock {};
    auto const origin = clock.now();
    clock.advance(100ms);
    REQUIRE(clock.now() == origin + 100ms);
    clock.advance(200ms);
    clock.advance(50ms);
    REQUIRE(clock.now() == origin + 350ms);
}

TEST_CASE("ManualClock::setNow hard-sets the value", "[clock]")
{
    auto clock = ManualClock { SteadyTimePoint {} + 5s };
    auto const target = SteadyTimePoint {} + 2s;
    clock.setNow(target);
    REQUIRE(clock.now() == target);
}

TEST_CASE("defaultSteadyClock returns a usable singleton", "[clock]")
{
    auto& a = core::platform::defaultSteadyClock();
    auto& b = core::platform::defaultSteadyClock();
    REQUIRE(&a == &b);
    // Sample in a defined order (the macro's argument evaluation order is
    // unspecified, so two inline now() calls could otherwise read out of order).
    auto const first = a.now();
    auto const second = b.now();
    REQUIRE(second >= first);
}

TEST_CASE("refresh is a no-op for clocks that read their source directly", "[clock]")
{
    // SteadyClock and ManualClock inherit the default. An event loop calls refresh()
    // unconditionally on whatever clock it was given, so this must be safe and must not disturb
    // a test's manual timeline. Sampled into locals in a defined order: the two sides of a
    // REQUIRE are not sequenced.
    auto steady = SteadyClock {};
    auto const before = steady.now();
    steady.refresh();
    auto const after = steady.now();
    REQUIRE(after >= before);

    auto manual = ManualClock { SteadyTimePoint { 7s } };
    manual.refresh();
    REQUIRE(manual.now() == SteadyTimePoint { 7s });
}

TEST_CASE("CachedClock samples once at construction", "[clock]")
{
    // The value has to be usable before any event loop has run: startup logging and
    // configuration reload read the clock long before the first refresh.
    auto source = ManualClock { SteadyTimePoint { 5s } };
    auto const clock = CachedClock { source };
    REQUIRE(clock.now() == SteadyTimePoint { 5s });
}

TEST_CASE("CachedClock holds its value until refreshed", "[clock]")
{
    // This is the whole point: between refreshes every reader gets a stored value rather than
    // paying for an OS clock read.
    auto source = ManualClock { SteadyTimePoint { 1s } };
    auto clock = CachedClock { source };

    source.setNow(SteadyTimePoint { 60s });
    REQUIRE(clock.now() == SteadyTimePoint { 1s });

    clock.refresh();
    REQUIRE(clock.now() == SteadyTimePoint { 60s });
}

TEST_CASE("CachedClock never moves backwards", "[clock]")
{
    // IClock promises monotonicity, and several event loops share one instance, so a refresh
    // that observes an older sample than one already published must not win. A backwards jump
    // would make a live entry look expired.
    auto source = ManualClock { SteadyTimePoint { 100s } };
    auto clock = CachedClock { source };
    REQUIRE(clock.now() == SteadyTimePoint { 100s });

    source.setNow(SteadyTimePoint { 40s });
    clock.refresh();
    REQUIRE(clock.now() == SteadyTimePoint { 100s });
}

#if CORE_CPP_TEST_THREADS
TEST_CASE("CachedClock concurrent refreshes converge on the newest sample", "[clock]")
{
    // The production shape: N event loops refreshing one shared clock. Whatever the
    // interleaving, the published value must end up the newest sample and must never be
    // observed going backwards.
    auto source = SteadyClock {};
    auto clock = CachedClock { source };

    auto gate = std::atomic<bool> { false };
    auto gateTimedOut = std::atomic<bool> { false };
    auto regressed = std::atomic<bool> { false };

    // Bounded: a worker that never sees the gate open gives up and says so, rather than
    // spinning under the joins (LASTRADA-Software/fastcached#1446).
    constexpr auto GateBound = 10s;
    auto const waitForGate = [&] {
        auto const deadline = std::chrono::steady_clock::now() + GateBound;
        while (!gate.load(std::memory_order_acquire))
        {
            if (std::chrono::steady_clock::now() > deadline)
            {
                gateTimedOut.store(true, std::memory_order_relaxed);
                return false;
            }
            std::this_thread::yield();
        }
        return true;
    };

    constexpr auto WorkerCount = 8;
    auto workers = std::vector<std::thread> {};
    workers.reserve(WorkerCount);
    {
        // Joins every worker that started, however this scope is left...
        struct Joiner
        {
            std::vector<std::thread>& threads;
            ~Joiner()
            {
                for (auto& thread: threads)
                    thread.join();
            }
        } const joiner { .threads = workers };

        // ...after opening the gate, so a thread that failed to start cannot leave the ones that
        // did waiting under the joins.
        struct GateOpener
        {
            std::atomic<bool>& gate;
            ~GateOpener() { gate.store(true, std::memory_order_release); }
        } const opener { .gate = gate };

        for ([[maybe_unused]] auto const worker: std::views::iota(0, WorkerCount))
        {
            workers.emplace_back([&] {
                if (!waitForGate())
                    return;
                auto previous = clock.now();
                for ([[maybe_unused]] auto const i: std::views::iota(0, 20'000))
                {
                    clock.refresh();
                    auto const observed = clock.now();
                    if (observed < previous)
                        regressed.store(true, std::memory_order_relaxed);
                    previous = observed;
                }
            });
        }
    }

    CHECK_FALSE(gateTimedOut.load());
    CHECK_FALSE(regressed.load());
    auto const published = clock.now();
    REQUIRE(published <= source.now());
}
#endif

TEST_CASE("ManualWallClock only moves when told to", "[clock]")
{
    auto clock = ManualWallClock { std::chrono::system_clock::time_point { 1'700'000'000s } };
    REQUIRE(clock.now() == std::chrono::system_clock::time_point { 1'700'000'000s });

    clock.advance(42s);
    REQUIRE(clock.now() == std::chrono::system_clock::time_point { 1'700'000'042s });

    clock.setNow(std::chrono::system_clock::time_point { 5s });
    REQUIRE(clock.now() == std::chrono::system_clock::time_point { 5s });
}

TEST_CASE("defaultSystemWallClock returns a usable singleton", "[clock]")
{
    auto& a = core::platform::defaultSystemWallClock();
    auto& b = core::platform::defaultSystemWallClock();
    REQUIRE(&a == &b);
    // The epoch is 1970; any real system clock is well past it.
    REQUIRE(a.now() > std::chrono::system_clock::time_point {});
}

TEST_CASE("A borrowed wall clock answers as the clock it borrows", "[clock][borrow]")
{
    // The guard must not have cost the borrow its job. now() is asserted through the ref against
    // the same clock read directly, on a MANUAL clock so the two readings are the same instant
    // by construction rather than by being close together.
    auto clock =
        ManualWallClock { std::chrono::system_clock::time_point { std::chrono::seconds { 1'700'000'000 } } };
    auto const borrowed = WallClockRef { clock };

    CHECK(borrowed.now() == clock.now());
    CHECK(&borrowed.get() == &clock);

    clock.advance(std::chrono::seconds { 42 });
    CHECK(borrowed.now() == clock.now());
}

TEST_CASE("A borrow handed on through a forwarder still answers as the clock it borrows", "[clock][borrow]")
{
    // The rows above are about what does not compile; this is what does, through both hops.
    auto steady = ManualClock {};
    auto wall =
        ManualWallClock { std::chrono::system_clock::time_point { std::chrono::seconds { 1'700'000'000 } } };
    auto const forwarder = Forwarder { steady, wall, 7 };

    CHECK(&forwarder.first.wall.get() == &wall);
    CHECK(&forwarder.second.wall.get() == &wall);
    CHECK(&forwarder.steady == &steady);
    CHECK(forwarder.tag == 7);

    wall.advance(std::chrono::seconds { 1 });
    CHECK(forwarder.first.wall.now() == wall.now());
    CHECK(forwarder.second.wall.now() == wall.now());

    // The overload-guarded pair keeps a named clock just as well; it is only the temporary it
    // cannot refuse once a forwarder is in between.
    auto const overloaded = OverloadForwarder { wall };
    CHECK(overloaded.retainer.wall == &wall);
}
