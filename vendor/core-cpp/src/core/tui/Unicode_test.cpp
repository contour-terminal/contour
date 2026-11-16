// SPDX-License-Identifier: Apache-2.0
#include <core/tui/Unicode.hpp>

#include <catch2/catch_test_macros.hpp>

using namespace core::tui;

// ============================================================================
// graphemeClusterWidth tests
// ============================================================================

TEST_CASE("Unicode.graphemeClusterWidth.empty")
{
    CHECK(graphemeClusterWidth(U"") == 0);
}

TEST_CASE("Unicode.graphemeClusterWidth.ascii")
{
    CHECK(graphemeClusterWidth(U"A") == 1);
    CHECK(graphemeClusterWidth(U"z") == 1);
    CHECK(graphemeClusterWidth(U"!") == 1);
}

TEST_CASE("Unicode.graphemeClusterWidth.wide_cjk")
{
    // Chinese character (CJK) - width 2
    CHECK(graphemeClusterWidth(U"\u4E2D") == 2); // "中"
    CHECK(graphemeClusterWidth(U"\u6587") == 2); // "文"
}

TEST_CASE("Unicode.graphemeClusterWidth.simple_emoji")
{
    // Simple emoji with emoji presentation - width 2
    CHECK(graphemeClusterWidth(U"\U0001F600") == 2); // Grinning face
    CHECK(graphemeClusterWidth(U"\U0001F44B") == 2); // Waving hand
}

TEST_CASE("Unicode.graphemeClusterWidth.emoji_zwj_sequence")
{
    // Family: Woman, Woman, Girl, Girl (ZWJ sequence)
    // 👩 (U+1F469) + ZWJ + 👩 + ZWJ + 👧 (U+1F467) + ZWJ + 👧
    std::u32string const familyEmoji = U"\U0001F469\u200D\U0001F469\u200D\U0001F467\u200D\U0001F467";
    CHECK(graphemeClusterWidth(familyEmoji) == 2);
}

TEST_CASE("Unicode.graphemeClusterWidth.emoji_skin_tone")
{
    // Waving hand with skin tone modifier
    // 👋 (U+1F44B) + skin tone modifier (U+1F3FD)
    std::u32string const wavingHandMedium = U"\U0001F44B\U0001F3FD";
    CHECK(graphemeClusterWidth(wavingHandMedium) == 2);
}

TEST_CASE("Unicode.graphemeClusterWidth.flag_emoji")
{
    // Flag emoji (regional indicators) - 🇺🇸
    // U+1F1FA (Regional Indicator Symbol Letter U) + U+1F1F8 (Regional Indicator Symbol Letter S)
    std::u32string const usFlag = U"\U0001F1FA\U0001F1F8";
    CHECK(graphemeClusterWidth(usFlag) == 2);
}

TEST_CASE("Unicode.graphemeClusterWidth.combining_characters")
{
    // e + combining acute accent = é (single grapheme cluster)
    std::u32string const eWithAcute = U"e\u0301";
    CHECK(graphemeClusterWidth(eWithAcute) == 1);
}

TEST_CASE("Unicode.graphemeClusterWidth.zero_width_joiner_alone")
{
    // ZWJ alone should still have width 1 (minimum)
    CHECK(graphemeClusterWidth(U"\u200D") == 1);
}
