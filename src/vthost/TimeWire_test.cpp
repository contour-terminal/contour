// SPDX-License-Identifier: Apache-2.0
#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstdint>
#include <limits>
#include <ratio>

#include <vthost/TimeWire.hpp>

using namespace std::chrono_literals;

TEST_CASE("a wall-clock instant survives the wire's nanoseconds", "[vthost][wire][blocks]")
{
    // Divisible by a microsecond, the coarsest system_clock period in use (libc++), so the value is
    // exact on every platform's clock rather than merely close.
    auto const instant = std::chrono::system_clock::time_point {
        std::chrono::duration_cast<std::chrono::system_clock::duration>(1'700'000'000'123'456'000ns)
    };
    CHECK(vthost::toWireTime(instant) == 1'700'000'000'123'456'000);
    CHECK(vthost::fromWireTime(vthost::toWireTime(instant)) == instant);
    CHECK(vthost::toWireTime(std::chrono::system_clock::time_point {}) == 0);
}

TEST_CASE("a peer's instant converts as sent, whatever its value", "[vthost][wire][blocks]")
{
    // The wire only changes the unit, and a system_clock tick is never finer than a nanosecond, so even the
    // extremes convert, truncated to this platform's tick.
    auto const at = [](int64_t nanoseconds) {
        return std::chrono::system_clock::time_point {
            std::chrono::duration_cast<std::chrono::system_clock::duration>(
                std::chrono::nanoseconds { nanoseconds })
        };
    };

    CHECK(vthost::fromWireTime(-1'000) == at(-1'000));
    CHECK(vthost::fromWireTime(std::numeric_limits<int64_t>::min())
          == at(std::numeric_limits<int64_t>::min()));
    CHECK(vthost::fromWireTime(std::numeric_limits<int64_t>::max())
          == at(std::numeric_limits<int64_t>::max()));
}

TEST_CASE("a local instant nanoseconds cannot count lands on the edge they can", "[vthost][wire][blocks]")
{
    // An instant ticked coarser than a nanosecond can lie beyond what nanoseconds count, and its plain
    // conversion then overflows. Ticked as MSVC's system_clock ticks (100 ns) and as libc++'s does (1 us),
    // on every platform whatever its own clock ticks, the extremes land on the last tick inside the range.
    using HundredNanoseconds = std::chrono::duration<int64_t, std::ratio<1, 10'000'000>>;
    CHECK(vthost::toWireTime(std::chrono::sys_time<HundredNanoseconds>::min()) == -9'223'372'036'854'775'800);
    CHECK(vthost::toWireTime(std::chrono::sys_time<HundredNanoseconds>::max()) == 9'223'372'036'854'775'800);
    CHECK(vthost::toWireTime(std::chrono::sys_time<std::chrono::microseconds>::min())
          == -9'223'372'036'854'775'000);
    CHECK(vthost::toWireTime(std::chrono::sys_time<std::chrono::microseconds>::max())
          == 9'223'372'036'854'775'000);

    using std::chrono::system_clock;
    // This platform's own clock: the edges of what an int64_t count of nanoseconds names, as it ticks them --
    // the extremes themselves where it ticks in nanoseconds, a tick inside them where it ticks coarser.
    auto const earliest = std::chrono::nanoseconds { std::chrono::ceil<system_clock::duration>(
                                                         std::chrono::nanoseconds::min()) }
                              .count();
    auto const latest = std::chrono::nanoseconds { std::chrono::floor<system_clock::duration>(
                                                       std::chrono::nanoseconds::max()) }
                            .count();

    CHECK(vthost::toWireTime(system_clock::time_point::min()) == earliest);
    CHECK(vthost::toWireTime(system_clock::time_point::max()) == latest);
    // An instant nanoseconds can count, however far from today, converts unchanged.
    auto const farFuture = system_clock::time_point { std::chrono::floor<system_clock::duration>(
        std::chrono::nanoseconds { int64_t { 1 } << 62 }) };
    CHECK(vthost::toWireTime(farFuture) == std::chrono::nanoseconds { farFuture.time_since_epoch() }.count());
}

TEST_CASE("a steady-clock span survives the wire's nanoseconds", "[vthost][wire][blocks]")
{
    auto const span = std::chrono::duration_cast<std::chrono::steady_clock::duration>(12'345'000ns);
    CHECK(vthost::toWireDuration(span) == 12'345'000);
    CHECK(vthost::fromWireDuration(vthost::toWireDuration(span)) == span);
}

TEST_CASE("a peer's span converts as sent, whatever its value", "[vthost][wire][blocks]")
{
    // The wire only changes the unit: a negative or huge length arrives as the length it names.
    auto const span = [](int64_t nanoseconds) {
        return std::chrono::duration_cast<std::chrono::steady_clock::duration>(
            std::chrono::nanoseconds { nanoseconds });
    };

    CHECK(vthost::fromWireDuration(-1) == span(-1));
    CHECK(vthost::fromWireDuration(std::numeric_limits<int64_t>::max())
          == span(std::numeric_limits<int64_t>::max()));
}
