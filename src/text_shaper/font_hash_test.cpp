// SPDX-License-Identifier: Apache-2.0
#include <text_shaper/font.h>

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <unordered_set>

using namespace text;

namespace
{
[[nodiscard]] glyph_key makeKey(unsigned fontValue, unsigned indexValue, double pt)
{
    auto key = glyph_key {};
    key.font = font_key { fontValue };
    key.index = glyph_index { indexValue };
    key.size = font_size { pt };
    return key;
}

[[nodiscard]] std::size_t hashOf(glyph_key const& key)
{
    return std::hash<glyph_key> {}(key);
}
} // namespace

TEST_CASE("glyph_key hash keeps its three components distinct", "[font]")
{
    // The hash packs font, glyph index and size (in tenths of a point) into disjoint bit fields. It
    // used to build that packing in size_t and shift the font component left by 32 -- which is exactly
    // the width of size_t on a 32-bit target, and therefore undefined. A compiler is free to fold such
    // a term to zero, which drops the font out of the hash entirely and collapses every font's glyphs
    // into the same buckets: the very collision the packing exists to prevent. Doing the arithmetic in
    // a fixed-width uint64_t makes the shift well defined everywhere, and where size_t cannot hold all
    // 48 bits the halves are folded so that all three components still reach the result.

    SECTION("the font component reaches the hash")
    {
        CHECK(hashOf(makeKey(1, 7, 12.0)) != hashOf(makeKey(2, 7, 12.0)));
    }

    SECTION("the glyph index reaches the hash")
    {
        CHECK(hashOf(makeKey(1, 7, 12.0)) != hashOf(makeKey(1, 8, 12.0)));
    }

    SECTION("the size reaches the hash")
    {
        CHECK(hashOf(makeKey(1, 7, 12.0)) != hashOf(makeKey(1, 7, 13.0)));
    }

    SECTION("equal keys hash equally")
    {
        CHECK(hashOf(makeKey(3, 9, 11.5)) == hashOf(makeKey(3, 9, 11.5)));
    }

    SECTION("many fonts sharing one glyph index do not collapse into one bucket")
    {
        // With the font term folded away, all of these would hash identically.
        auto hashes = std::unordered_set<std::size_t> {};
        for (auto fontValue = 0u; fontValue < 64u; ++fontValue)
            hashes.insert(hashOf(makeKey(fontValue, 42, 12.0)));
        CHECK(hashes.size() == 64);
    }
}
