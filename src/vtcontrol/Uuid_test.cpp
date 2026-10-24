// SPDX-License-Identifier: Apache-2.0
#include <core/platform/Clock.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <chrono>
#include <ranges>
#include <set>

#include <vtcontrol/Uuid.hpp>
#include <vtcontrol/testing/SequentialUuidSource.hpp>

TEST_CASE("Uuid round-trips through its canonical text", "[vtcontrol][uuid]")
{
    auto const uuid = vtcontrol::parseUuid("018f3c4e-9a1b-7c2d-8e3f-0123456789ab");
    REQUIRE(uuid.has_value());
    CHECK(vtcontrol::toString(*uuid) == "018f3c4e-9a1b-7c2d-8e3f-0123456789ab");
    CHECK(vtcontrol::parseUuid("018F3C4E-9A1B-7C2D-8E3F-0123456789AB") == uuid);
    CHECK_FALSE(vtcontrol::parseUuid("018f3c4e9a1b7c2d8e3f0123456789ab"));
    CHECK_FALSE(vtcontrol::parseUuid("018f3c4e-9a1b-7c2d-8e3f-0123456789a"));
    CHECK_FALSE(vtcontrol::parseUuid("018f3c4e-9a1b-7c2d-8e3f-0123456789ag"));
    CHECK_FALSE(vtcontrol::parseUuid("018f3c4e_9a1b-7c2d-8e3f-0123456789ab"));
}

TEST_CASE("makeUuidV7 lays out time, version and variant", "[vtcontrol][uuid]")
{
    auto const random =
        std::array<uint8_t, 10> { 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff };
    auto const uuid = vtcontrol::makeUuidV7(
        std::chrono::sys_time<std::chrono::milliseconds> { std::chrono::milliseconds { 0x018f3c4e9a1b } },
        random);
    CHECK(vtcontrol::toString(uuid).starts_with("018f3c4e-9a1b-7"));
    CHECK((uuid.bytes[8] >> 6) == 0b10);
}

TEST_CASE("UuidV7Source never repeats within one millisecond", "[vtcontrol][uuid]")
{
    auto clock = core::platform::ManualWallClock {}; // never advanced
    auto source = vtcontrol::UuidV7Source { clock, [] { return uint64_t { 0 }; } };
    auto const a = source.next();
    auto const b = source.next();
    CHECK(a < b);
}

TEST_CASE("UuidV7Source borrows the next millisecond when its counter runs out", "[vtcontrol][uuid]")
{
    auto clock = core::platform::ManualWallClock {};
    auto source = vtcontrol::UuidV7Source { clock, [] { return uint64_t { 0 }; } };
    auto seen = std::set<vtcontrol::Uuid> {};
    auto previous = vtcontrol::Uuid {};
    for (auto const index: std::views::iota(0, 10000))
    {
        (void) index;
        auto const next = source.next();
        CHECK(previous < next);
        seen.insert(next);
        previous = next;
    }
    CHECK(seen.size() == 10000);
}

TEST_CASE("UuidV7Source follows the clock forward", "[vtcontrol][uuid]")
{
    auto clock = core::platform::ManualWallClock {};
    auto source = vtcontrol::UuidV7Source { clock, [] { return uint64_t { 0 }; } };
    auto const a = source.next();
    clock.advance(std::chrono::seconds { 1 });
    auto const b = source.next();
    CHECK(a < b);
    CHECK(b.bytes[5] != 0);
}

TEST_CASE("systemUuidSource mints distinct identifiers", "[vtcontrol][uuid]")
{
    auto const source = vtcontrol::systemUuidSource();
    CHECK(source->next() != source->next());
}

TEST_CASE("SequentialUuidSource counts from one", "[vtcontrol][uuid]")
{
    auto source = vtcontrol::testing::SequentialUuidSource {};
    CHECK(vtcontrol::toString(source.next()) == "00000000-0000-7000-8000-000000000001");
    CHECK(vtcontrol::toString(source.next()) == "00000000-0000-7000-8000-000000000002");
}

TEST_CASE("Uuid hashes by value", "[vtcontrol][uuid]")
{
    auto const a = *vtcontrol::parseUuid("018f3c4e-9a1b-7c2d-8e3f-0123456789ab");
    auto const b = *vtcontrol::parseUuid("018f3c4e-9a1b-7c2d-8e3f-0123456789ac");
    CHECK(std::hash<vtcontrol::Uuid> {}(a) == std::hash<vtcontrol::Uuid> {}(a));
    CHECK(std::hash<vtcontrol::Uuid> {}(a) != std::hash<vtcontrol::Uuid> {}(b));
}
