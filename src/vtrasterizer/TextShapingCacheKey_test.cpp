// SPDX-License-Identifier: Apache-2.0
#include <vtrasterizer/TextShapingCacheKey.h>

#include <catch2/catch_test_macros.hpp>

#include <set>
#include <vector>

using namespace vtrasterizer;
using unicode::Bidi_Direction;

// The shaping cache is a 4000-entry LRU keyed by this hash. If two runs that shape differently
// collide, one is served the other's glyphs -- intermittently, and only under content that mixes the
// two, which is why this is tested here rather than left to an end-to-end case.
TEST_CASE("TextShapingCacheKey.direction is part of the key", "[textrenderer]")
{
    auto constexpr Text = U"שלום"; // Hebrew: shalom

    auto const ltr = hashTextAndStyle(Text, TextStyle::Regular, Bidi_Direction::Left_To_Right);
    auto const rtl = hashTextAndStyle(Text, TextStyle::Regular, Bidi_Direction::Right_To_Left);

    // Same codepoints, same style, different direction -- and therefore different glyphs, because
    // Arabic joining and mirrored brackets both depend on it.
    CHECK(ltr != rtl);
}

TEST_CASE("TextShapingCacheKey.left_to_right is not a no-op", "[textrenderer]")
{
    // Bidi_Direction::Left_To_Right is the zero enumerator. operator*(strong_hash, uint32_t) mixes
    // rather than multiplies, so folding it in must still perturb the hash; if it were arithmetic,
    // the LTR key would silently equal the un-directed one and half the guard would be missing.
    auto constexpr Text = U"hello";
    auto const withDirection = hashTextAndStyle(Text, TextStyle::Regular, Bidi_Direction::Left_To_Right);
    auto const textOnly = crispy::strong_hash::compute(std::u32string_view(Text));

    CHECK(withDirection != textOnly);
}

TEST_CASE("TextShapingCacheKey.every input is distinguished", "[textrenderer]")
{
    auto constexpr Texts = std::array { U"abc", U"abd", U"של" };
    auto constexpr Styles = std::array {
        TextStyle::Invalid, TextStyle::Regular, TextStyle::Bold, TextStyle::Italic, TextStyle::BoldItalic,
    };
    auto constexpr Directions = std::array { Bidi_Direction::Left_To_Right, Bidi_Direction::Right_To_Left };

    auto seen = std::set<std::vector<uint32_t>> {};
    auto count = size_t { 0 };

    for (auto const* text: Texts)
        for (auto const style: Styles)
            for (auto const direction: Directions)
            {
                auto const hash = hashTextAndStyle(std::u32string_view(text), style, direction);
                seen.insert(
                    std::vector<uint32_t> { hash.value[0], hash.value[1], hash.value[2], hash.value[3] });
                ++count;
            }

    // TextStyle::Invalid is also a zero enumerator, so this covers the same trap from the other side.
    INFO("distinct keys " << seen.size() << " of " << count);
    CHECK(seen.size() == count);
}
