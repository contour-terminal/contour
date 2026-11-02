// SPDX-License-Identifier: Apache-2.0
//
// Unit tests for the kitty graphics `o=z` inflater and its output cap.

#include <vtbackend/graphics/KittyInflate.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <ranges>
#include <span>
#include <vector>

#include <zlib.h>

using namespace vtbackend::kitty_graphics;

namespace
{
/// Deflates @p input into a zlib stream, as an application sending `o=z` would.
[[nodiscard]] std::vector<uint8_t> deflateZlib(std::span<uint8_t const> input)
{
    auto out = std::vector<uint8_t>(compressBound(static_cast<uLong>(input.size())));
    auto outSize = static_cast<uLongf>(out.size());
    auto const status =
        compress2(out.data(), &outSize, input.data(), static_cast<uLong>(input.size()), Z_BEST_SPEED);
    REQUIRE(status == Z_OK);
    out.resize(outSize);
    return out;
}

[[nodiscard]] std::vector<uint8_t> patterned(size_t size)
{
    auto data = std::vector<uint8_t>(size);
    for (auto const index: std::views::iota(size_t { 0 }, size))
        data[index] = static_cast<uint8_t>((index * 31 + index / 251) & 0xFF);
    return data;
}
} // namespace

TEST_CASE("inflate.roundtrip", "[kitty]")
{
    auto const original = patterned(size_t { 1 } << 20);
    auto const compressed = deflateZlib(original);
    auto const result = inflateZlib(compressed, original.size());
    REQUIRE(result.has_value());
    CHECK(*result == original);
}

TEST_CASE("inflate.corrupt_stream_is_Corrupt", "[kitty]")
{
    auto const original = patterned(4096);
    auto compressed = deflateZlib(original);
    // The header's check bits make a flipped first byte a certain failure.
    compressed.front() ^= 0xFF;
    auto const result = inflateZlib(compressed, original.size());
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error() == InflateError::Corrupt);
}

TEST_CASE("inflate.truncated_stream_is_Corrupt", "[kitty]")
{
    auto const original = patterned(size_t { 1 } << 16);
    auto compressed = deflateZlib(original);
    compressed.resize(compressed.size() / 2);
    auto const result = inflateZlib(compressed, original.size());
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error() == InflateError::Corrupt);
}

TEST_CASE("inflate.output_over_cap_is_TooLarge_without_allocating_it", "[kitty]")
{
    // 4 MiB of zeros deflates to a few KiB: the classic bomb shape, kept small enough to stay cheap.
    auto const zeros = std::vector<uint8_t>(size_t { 4 } << 20);
    auto const compressed = deflateZlib(zeros);
    REQUIRE(compressed.size() < zeros.size() / 100);

    auto const result = inflateZlib(compressed, 4096);
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error() == InflateError::TooLarge);
}

TEST_CASE("inflate.output_exactly_at_cap_is_accepted", "[kitty]")
{
    auto const original = patterned(5000);
    auto const compressed = deflateZlib(original);
    auto const result = inflateZlib(compressed, original.size());
    REQUIRE(result.has_value());
    CHECK(result->size() == original.size());

    auto const tooSmall = inflateZlib(compressed, original.size() - 1);
    REQUIRE_FALSE(tooSmall.has_value());
    CHECK(tooSmall.error() == InflateError::TooLarge);
}
