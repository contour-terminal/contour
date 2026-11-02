// SPDX-License-Identifier: Apache-2.0
//
// Unit tests for the kitty graphics `o=z` inflater and its output cap.

#include <vtbackend/graphics/KittyInflate.hpp>
#include <vtbackend/testing/Deflate.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <functional>
#include <ranges>
#include <vector>

using namespace vtbackend::kitty_graphics;
using vtbackend::test::deflateZlib;

namespace
{
[[nodiscard]] std::vector<uint8_t> patterned(size_t size)
{
    auto data = std::vector<uint8_t>(size);
    for (auto const index: std::views::iota(size_t { 0 }, size))
        data[index] = static_cast<uint8_t>(((index * 31) + (index / 251)) & 0xFF);
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

TEST_CASE("inflate.an_incomplete_stream_is_Corrupt", "[kitty]")
{
    struct Case
    {
        char const* name;
        size_t originalSize;
        std::function<void(std::vector<uint8_t>&)> damage;
    };
    auto const cases = std::vector<Case> {
        // The header's check bits make a flipped first byte a certain failure.
        { .name = "corrupt stream",
          .originalSize = 4096,
          .damage = [](std::vector<uint8_t>& stream) { stream.front() ^= 0xFF; } },
        { .name = "truncated stream",
          .originalSize = size_t { 1 } << 16,
          .damage = [](std::vector<uint8_t>& stream) { stream.resize(stream.size() / 2); } },
        { .name = "empty input",
          .originalSize = 4096,
          .damage = [](std::vector<uint8_t>& stream) { stream.clear(); } },
    };
    for (auto const& testCase: cases)
    {
        INFO(testCase.name);
        auto stream = deflateZlib(patterned(testCase.originalSize));
        testCase.damage(stream);
        auto const result = inflateZlib(stream, testCase.originalSize);
        REQUIRE_FALSE(result.has_value());
        CHECK(result.error() == InflateError::Corrupt);
    }
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

TEST_CASE("inflate.bytes_after_the_stream_end_are_ignored", "[kitty]")
{
    // Documented behaviour: the stream is complete once its end marker is read, and whatever follows
    // is not part of the image. Rejecting it would refuse encoders that pad the payload.
    auto const original = patterned(1000);
    auto compressed = deflateZlib(original);
    compressed.insert(compressed.end(), { 0xDE, 0xAD, 0xBE, 0xEF });
    auto const result = inflateZlib(compressed, original.size());
    REQUIRE(result.has_value());
    CHECK(*result == original);
}

TEST_CASE("inflate.a_bomb_is_refused_within_a_chunk_of_the_cap", "[kitty]")
{
    // The end-to-end bomb tests cannot observe how much was inflated before the refusal, so the proof
    // that the cap bites early lives here: a stream whose only valid prefix is tiny, followed by
    // garbage, answers TooLarge (the cap was crossed first) rather than Corrupt (the end was reached).
    auto const zeros = std::vector<uint8_t>(size_t { 4 } << 20);
    auto compressed = deflateZlib(zeros);
    compressed.resize(compressed.size() - 8); // Truncate the tail: a full inflate would be Corrupt.
    auto const result = inflateZlib(compressed, 1024);
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error() == InflateError::TooLarge);
}
