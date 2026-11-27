// SPDX-License-Identifier: Apache-2.0
//
// What a unit test can see of the constant-time helpers: the answers. That the comparison takes the
// same time for every pair of equal-length inputs is a property of the code generated, held by
// construction (see ConstantTime.hpp) and checked by reading the disassembly, not by a clock.
#include <core/ConstantTime.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <span>
#include <vector>

using core::constantTimeEquals;
using core::secureZero;

namespace
{

[[nodiscard]] std::vector<std::byte> filled(std::size_t size, std::byte value)
{
    return std::vector<std::byte>(size, value);
}

} // namespace

TEST_CASE("constantTimeEquals compares every byte and the length", "[base][constant-time]")
{
    auto const reference = filled(32, std::byte { 0x5A });
    SECTION("equal")
    {
        CHECK(constantTimeEquals(reference, filled(32, std::byte { 0x5A })));
    }
    SECTION("the last byte differs")
    {
        auto other = reference;
        other.back() = std::byte { 0x5B };
        CHECK_FALSE(constantTimeEquals(reference, other));
    }
    SECTION("the first byte differs")
    {
        auto other = reference;
        other.front() = std::byte { 0x00 };
        CHECK_FALSE(constantTimeEquals(reference, other));
    }
    SECTION("a single bit differs in the middle")
    {
        auto other = reference;
        other[16] ^= std::byte { 0x80 };
        CHECK_FALSE(constantTimeEquals(reference, other));
    }
    SECTION("different lengths, one a prefix of the other")
    {
        auto const longer = filled(33, std::byte { 0x5A });
        CHECK_FALSE(constantTimeEquals(reference, longer));
        CHECK_FALSE(constantTimeEquals(longer, reference));
    }
    SECTION("both empty")
    {
        CHECK(constantTimeEquals(std::span<std::byte const> {}, std::span<std::byte const> {}));
    }
    SECTION("one empty")
    {
        CHECK_FALSE(constantTimeEquals(std::span<std::byte const> {}, reference));
    }
}

TEST_CASE("secureZero clears every byte", "[base][constant-time]")
{
    SECTION("a 64-byte buffer")
    {
        auto buffer = filled(64, std::byte { 0xA5 });
        secureZero(buffer);
        CHECK(std::ranges::all_of(buffer, [](std::byte value) { return value == std::byte { 0 }; }));
    }
    SECTION("an array on the stack, and only the span given")
    {
        auto buffer = std::array<std::byte, 8> {};
        std::ranges::fill(buffer, std::byte { 0xA5 });
        secureZero(std::span { buffer }.first(4));
        CHECK(std::ranges::all_of(std::span { buffer }.first(4),
                                  [](std::byte value) { return value == std::byte { 0 }; }));
        CHECK(std::ranges::all_of(std::span { buffer }.last(4),
                                  [](std::byte value) { return value == std::byte { 0xA5 }; }));
    }
    SECTION("an empty span touches nothing")
    {
        auto buffer = std::array<std::byte, 4> {};
        std::ranges::fill(buffer, std::byte { 0xA5 });
        secureZero(std::span { buffer }.first(0));
        secureZero(std::span<std::byte> {});
        CHECK(std::ranges::all_of(buffer, [](std::byte value) { return value == std::byte { 0xA5 }; }));
    }
}
