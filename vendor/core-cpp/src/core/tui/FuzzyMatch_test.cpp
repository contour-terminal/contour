// SPDX-License-Identifier: Apache-2.0
#include <core/tui/completer/FuzzyMatch.hpp>

#include <catch2/catch_test_macros.hpp>

#include <ranges>
#include <string_view>

using namespace core::tui;
using namespace core::tui::completer;

// ============================================================================
// Basic matching tests
// ============================================================================

TEST_CASE("FuzzyMatch.basic_match_ds_downloads")
{
    // "ds" occurs verbatim in "Downloa[ds]", so the contiguous substring is
    // preferred over the scattered D...s subsequence.
    auto result = FuzzyMatch::matchSmartCase("Downloads", "ds");
    CHECK(result.matches);
    CHECK(result.matchedChars == 2);
    REQUIRE(result.positions.size() == 2);
    CHECK(result.positions[0] == 7); // d
    CHECK(result.positions[1] == 8); // s
}

TEST_CASE("FuzzyMatch.basic_match_ds_documents")
{
    // Documents = D(0), o(1), c(2), u(3), m(4), e(5), n(6), t(7), s(8)
    auto result = FuzzyMatch::matchSmartCase("Documents", "ds");
    CHECK(result.matches);
    CHECK(result.matchedChars == 2);
    REQUIRE(result.positions.size() == 2);
    CHECK(result.positions[0] == 0); // D
    CHECK(result.positions[1] == 8); // s
}

TEST_CASE("FuzzyMatch.basic_match_dcm_documents")
{
    auto result = FuzzyMatch::matchSmartCase("Documents", "dcm");
    CHECK(result.matches);
    CHECK(result.matchedChars == 3);
    REQUIRE(result.positions.size() == 3);
    CHECK(result.positions[0] == 0); // D
    CHECK(result.positions[1] == 2); // c
    CHECK(result.positions[2] == 4); // m
}

TEST_CASE("FuzzyMatch.basic_no_match")
{
    auto result = FuzzyMatch::matchSmartCase("Downloads", "xyz");
    CHECK_FALSE(result.matches);
    CHECK(result.positions.empty());
}

TEST_CASE("FuzzyMatch.empty_pattern_matches")
{
    auto result = FuzzyMatch::matchSmartCase("Downloads", "");
    CHECK(result.matches);
    CHECK(result.matchedChars == 0);
    CHECK(result.positions.empty());
}

TEST_CASE("FuzzyMatch.pattern_longer_than_text")
{
    auto result = FuzzyMatch::matchSmartCase("hi", "hello");
    CHECK_FALSE(result.matches);
}

TEST_CASE("FuzzyMatch.empty_text_no_match")
{
    auto result = FuzzyMatch::matchSmartCase("", "ds");
    CHECK_FALSE(result.matches);
}

TEST_CASE("FuzzyMatch.exact_match")
{
    auto result = FuzzyMatch::matchSmartCase("hello", "hello");
    CHECK(result.matches);
    CHECK(result.matchedChars == 5);
    REQUIRE(result.positions.size() == 5);
    for (auto const i: std::views::iota(std::size_t { 0 }, result.positions.size()))
        CHECK(result.positions[i] == i);
}

// ============================================================================
// Smart case tests
// ============================================================================

TEST_CASE("FuzzyMatch.smartcase_lowercase_insensitive")
{
    // Lowercase pattern matches case-insensitively
    auto result = FuzzyMatch::matchSmartCase("Downloads", "ds");
    CHECK(result.matches);

    auto result2 = FuzzyMatch::matchSmartCase("DOWNLOADS", "ds");
    CHECK(result2.matches);

    auto result3 = FuzzyMatch::matchSmartCase("downloads", "ds");
    CHECK(result3.matches);
}

TEST_CASE("FuzzyMatch.smartcase_uppercase_sensitive")
{
    // Pattern with uppercase matches case-sensitively
    auto result = FuzzyMatch::matchSmartCase("Downloads", "Ds");
    CHECK(result.matches);

    auto result2 = FuzzyMatch::matchSmartCase("downloads", "Ds");
    CHECK_FALSE(result2.matches);

    auto result3 = FuzzyMatch::matchSmartCase("DOWNLOADS", "Ds");
    CHECK_FALSE(result3.matches);
}

TEST_CASE("FuzzyMatch.smartcase_all_uppercase_pattern")
{
    auto result = FuzzyMatch::matchSmartCase("Downloads", "DS");
    CHECK_FALSE(result.matches);

    auto result2 = FuzzyMatch::matchSmartCase("DOWNLOADS", "DS");
    CHECK(result2.matches);
}

TEST_CASE("FuzzyMatch.explicit_case_sensitive")
{
    // "Downloads" has 'd' at position 7, so "ds" matches case-sensitively
    auto result = FuzzyMatch::match("Downloads", "ds", true);
    CHECK(result.matches);
    REQUIRE(result.positions.size() == 2);
    CHECK(result.positions[0] == 7); // lowercase 'd' in "Downloads"
    CHECK(result.positions[1] == 8); // 's'

    // "DOWNLOADS" has no lowercase 'd', so "ds" doesn't match case-sensitively
    auto result2 = FuzzyMatch::match("DOWNLOADS", "ds", true);
    CHECK_FALSE(result2.matches);

    auto result3 = FuzzyMatch::match("downloads", "ds", true);
    CHECK(result3.matches);
}

TEST_CASE("FuzzyMatch.explicit_case_insensitive")
{
    auto result = FuzzyMatch::match("Downloads", "ds", false);
    CHECK(result.matches);

    auto result2 = FuzzyMatch::match("DOWNLOADS", "ds", false);
    CHECK(result2.matches);
}

// ============================================================================
// Consecutive run tracking tests
// ============================================================================

TEST_CASE("FuzzyMatch.consecutive_single_run")
{
    // "down" in "Downloads" - all consecutive
    auto result = FuzzyMatch::matchSmartCase("Downloads", "down");
    CHECK(result.matches);
    CHECK(result.consecutiveRuns == 1);
    CHECK(result.longestRun == 4);
}

TEST_CASE("FuzzyMatch.consecutive_multiple_runs")
{
    // "dws" in "Downloads" - d, w, s are not consecutive
    auto result = FuzzyMatch::matchSmartCase("Downloads", "dws");
    CHECK(result.matches);
    CHECK(result.consecutiveRuns == 3); // Each char is its own run
    CHECK(result.longestRun == 1);
}

TEST_CASE("FuzzyMatch.consecutive_mixed_runs")
{
    // "dols" in "Downloads" - "do" consecutive, then "l", then "s"
    auto result = FuzzyMatch::matchSmartCase("Downloads", "dols");
    CHECK(result.matches);
    // d(0), o(1) consecutive; l(5); s(8) - 3 runs
    CHECK(result.consecutiveRuns == 3);
    CHECK(result.longestRun == 2);
}

TEST_CASE("FuzzyMatch.consecutive_at_end")
{
    // "ads" in "Downloads" - "a" then "ds" consecutive
    auto result = FuzzyMatch::matchSmartCase("Downloads", "ads");
    CHECK(result.matches);
    // a(4), d(6), s(8) - wait, "Downloads" doesn't have consecutive "ds"
    // Let's use a better example
    auto result2 = FuzzyMatch::matchSmartCase("testcase", "tce");
    CHECK(result2.matches);
    // t(0), c(4), e(7) - 3 runs
    CHECK(result2.consecutiveRuns == 3);
}

// ============================================================================
// Word boundary tests
// ============================================================================

TEST_CASE("FuzzyMatch.isWordBoundary")
{
    CHECK(FuzzyMatch::isWordBoundary('/'));
    CHECK(FuzzyMatch::isWordBoundary('_'));
    CHECK(FuzzyMatch::isWordBoundary('-'));
    CHECK(FuzzyMatch::isWordBoundary(' '));
    CHECK(FuzzyMatch::isWordBoundary('\t'));
    CHECK(FuzzyMatch::isWordBoundary('.'));

    CHECK_FALSE(FuzzyMatch::isWordBoundary('a'));
    CHECK_FALSE(FuzzyMatch::isWordBoundary('Z'));
    CHECK_FALSE(FuzzyMatch::isWordBoundary('0'));
}

TEST_CASE("FuzzyMatch.word_start_at_beginning")
{
    auto result = FuzzyMatch::matchSmartCase("Downloads", "d");
    CHECK(result.matches);
    CHECK(result.wordStartMatches == 1); // 'd' at position 0
}

TEST_CASE("FuzzyMatch.word_start_after_separator")
{
    auto result = FuzzyMatch::matchSmartCase("my_downloads", "md");
    CHECK(result.matches);
    CHECK(result.wordStartMatches == 2); // 'm' at 0, 'd' after '_'
}

TEST_CASE("FuzzyMatch.word_start_after_slash")
{
    // "path/to/file" = p(0), a(1), t(2), h(3), /(4), t(5), o(6), /(7), f(8), ...
    // Greedy match: 'p' at 0, 't' at 2 (in "path"), 'f' at 8 (after '/')
    // So only 2 word starts: 'p' at start, 'f' after '/'
    auto result = FuzzyMatch::matchSmartCase("path/to/file", "ptf");
    CHECK(result.matches);
    CHECK(result.wordStartMatches == 2); // 'p' at 0, 'f' after '/'
    REQUIRE(result.positions.size() == 3);
    CHECK(result.positions[0] == 0); // p
    CHECK(result.positions[1] == 2); // t (in "path", not "to")
    CHECK(result.positions[2] == 8); // f
}

TEST_CASE("FuzzyMatch.word_start_no_boundary_match")
{
    auto result = FuzzyMatch::matchSmartCase("Downloads", "wn");
    CHECK(result.matches);
    CHECK(result.wordStartMatches == 0); // Neither 'w' nor 'n' at word start
}

// ============================================================================
// Score calculation tests
// ============================================================================

TEST_CASE("FuzzyMatch.score_100_percent_match")
{
    auto result = FuzzyMatch::matchSmartCase("abc", "abc");
    int const score = FuzzyMatch::calculateScore(50, "abc", "abc", result);
    // 50 base + 100 (100% match) + 10 (1 consecutive run) + 15 (word start)
    // Note: No prefix bonus - that's reserved for actual prefix matches (handled by caller)
    CHECK(score == 50 + 100 + 10 + 15);
}

TEST_CASE("FuzzyMatch.score_50_percent_match")
{
    auto result = FuzzyMatch::matchSmartCase("abcd", "ab");
    int const score = FuzzyMatch::calculateScore(50, "abcd", "ab", result);
    // 50 base + 50 (50% match) + 10 (1 run) + 15 (word start)
    CHECK(score == 50 + 50 + 10 + 15);
}

TEST_CASE("FuzzyMatch.score_no_word_start_bonus")
{
    auto result = FuzzyMatch::matchSmartCase("xabc", "abc");
    int const score = FuzzyMatch::calculateScore(50, "xabc", "abc", result);
    // 50 base + 75 (75% match) + 10 (1 run) + 0 (no word start for a,b,c)
    CHECK(score == 50 + 75 + 10 + 0);
}

TEST_CASE("FuzzyMatch.score_with_config")
{
    FuzzyConfig config;
    config.maxMatchPercentBonus = 200;
    config.consecutiveBonus = 20;
    config.wordStartBonus = 30;
    config.prefixMatchBonus = 100; // Not used by calculateScore, only by caller

    auto result = FuzzyMatch::matchSmartCase("abc", "abc");
    int const score = FuzzyMatch::calculateScore(0, "abc", "abc", result, config);
    // 0 base + 200 (100% match) + 20 (1 run) + 30 (word start)
    CHECK(score == 0 + 200 + 20 + 30);
}

TEST_CASE("FuzzyMatch.score_non_match_returns_base")
{
    auto result = FuzzyMatch::matchSmartCase("abc", "xyz");
    int const score = FuzzyMatch::calculateScore(50, "abc", "xyz", result);
    CHECK(score == 50); // Just base score, no bonuses
}

// ============================================================================
// Quality threshold tests
// ============================================================================

TEST_CASE("FuzzyMatchResult.quality_calculation")
{
    FuzzyMatchResult result;
    result.matches = true;
    result.matchedChars = 2;

    CHECK(result.quality(10) == 0.2); // 2/10 = 20%
    CHECK(result.quality(4) == 0.5);  // 2/4 = 50%
    CHECK(result.quality(2) == 1.0);  // 2/2 = 100%
}

TEST_CASE("FuzzyMatchResult.quality_non_match")
{
    FuzzyMatchResult result;
    result.matches = false;
    result.matchedChars = 2;

    CHECK(result.quality(10) == 0.0); // Non-match returns 0
}

TEST_CASE("FuzzyMatchResult.quality_empty_text")
{
    FuzzyMatchResult result;
    result.matches = true;
    result.matchedChars = 0;

    CHECK(result.quality(0) == 0.0);
}

// ============================================================================
// Contiguous substring detection tests
// ============================================================================

TEST_CASE("FuzzyMatchResult.isContiguousSubstring_true")
{
    // "yazi" is a contiguous substring of "io.github.sxyazi.yazi"
    auto result = FuzzyMatch::matchSmartCase("io.github.sxyazi.yazi", "yazi");
    CHECK(result.matches);
    CHECK(result.isContiguousSubstring());
    // Quality is below threshold (4/21 ≈ 0.19 < 0.2) but substring match should still be accepted
    CHECK(result.quality(FuzzyMatch::countGraphemes("io.github.sxyazi.yazi")) < 0.2);
}

TEST_CASE("FuzzyMatchResult.isContiguousSubstring_false_scattered")
{
    // "dws" in "Downloads" — d, w, s are scattered (not consecutive)
    auto result = FuzzyMatch::matchSmartCase("Downloads", "dws");
    CHECK(result.matches);
    CHECK_FALSE(result.isContiguousSubstring());
}

TEST_CASE("FuzzyMatchResult.isContiguousSubstring_true_despite_earlier_grapheme")
{
    // Regression: pressing Ctrl+R and typing "demo.exe" must match the history
    // entry "./build/clangcl-debug/src/shell/demo.exe". The leading 'd' of the
    // pattern also occurs earlier in "./build/clangcl-debug", so a greedy matcher
    // would bind to that 'd' and highlight a stray 'd' plus "emo.exe". The
    // substring-first match must instead report the contiguous "demo.exe" block.
    auto const text = std::string_view { "./build/clangcl-debug/src/shell/demo.exe" };
    auto result = FuzzyMatch::matchSmartCase(text, "demo.exe");
    CHECK(result.matches);
    CHECK(result.isContiguousSubstring());

    // Positions must cover the trailing "demo.exe" block, not the stray 'd' in
    // "build" — this is what gets highlighted in the Ctrl+R popup.
    auto const start = FuzzyMatch::countGraphemes("./build/clangcl-debug/src/shell/");
    REQUIRE(result.positions.size() == 8);
    for (auto const i: std::views::iota(size_t { 0 }, result.positions.size()))
        CHECK(result.positions[i] == start + i);

    // Quality alone does not reliably carry this match: for an entry this long it
    // sits at/below the default threshold, so isContiguousSubstring() is what
    // keeps it from being filtered out of history search.
    auto const threshold = FuzzyConfig {}.minMatchThreshold;
    CHECK(result.quality(FuzzyMatch::countGraphemes(text)) <= threshold);
}

TEST_CASE("FuzzyMatchResult.isContiguousSubstring_true_when_first_grapheme_repeats")
{
    // "cat" appears contiguously in "con[cat]enate"; the pattern's leading 'c'
    // also occurs earlier at index 0, so a greedy matcher would scatter the
    // positions. Substring-first matching must report the contiguous block.
    auto result = FuzzyMatch::matchSmartCase("concatenate", "cat");
    CHECK(result.matches);
    CHECK(result.isContiguousSubstring());
    REQUIRE(result.positions.size() == 3);
    CHECK(result.positions[0] == 3); // c in "concat"
    CHECK(result.positions[1] == 4); // a
    CHECK(result.positions[2] == 5); // t
}

TEST_CASE("FuzzyMatchResult.isContiguousSubstring_false_no_match")
{
    auto result = FuzzyMatch::matchSmartCase("hello", "xyz");
    CHECK_FALSE(result.isContiguousSubstring());
}

TEST_CASE("FuzzyMatchResult.isContiguousSubstring_false_empty_pattern")
{
    auto result = FuzzyMatch::matchSmartCase("hello", "");
    CHECK(result.matches);
    CHECK_FALSE(result.isContiguousSubstring()); // matchedChars == 0
}

// ============================================================================
// Grapheme/Unicode tests
// ============================================================================

TEST_CASE("FuzzyMatch.grapheme_basic_ascii")
{
    auto count = FuzzyMatch::countGraphemes("hello");
    CHECK(count == 5);
}

TEST_CASE("FuzzyMatch.grapheme_empty")
{
    auto count = FuzzyMatch::countGraphemes("");
    CHECK(count == 0);
}

TEST_CASE("FuzzyMatch.grapheme_multibyte_utf8")
{
    // "café" - 4 graphemes (c, a, f, é)
    auto count = FuzzyMatch::countGraphemes("café");
    CHECK(count == 4);

    auto result = FuzzyMatch::matchSmartCase("café", "cf");
    CHECK(result.matches);
    REQUIRE(result.positions.size() == 2);
    CHECK(result.positions[0] == 0); // c
    CHECK(result.positions[1] == 2); // f
}

TEST_CASE("FuzzyMatch.grapheme_emoji")
{
    // Single emoji is one grapheme
    auto count = FuzzyMatch::countGraphemes("📁");
    CHECK(count == 1);

    auto result = FuzzyMatch::matchSmartCase("📁Downloads", "📁d");
    CHECK(result.matches);
    REQUIRE(result.positions.size() == 2);
    CHECK(result.positions[0] == 0); // 📁
    CHECK(result.positions[1] == 1); // D
}

TEST_CASE("FuzzyMatch.grapheme_wide_cjk")
{
    // CJK characters - each is one grapheme
    auto count = FuzzyMatch::countGraphemes("文件夹");
    CHECK(count == 3);

    auto result = FuzzyMatch::matchSmartCase("文件夹", "文夹");
    CHECK(result.matches);
    REQUIRE(result.positions.size() == 2);
    CHECK(result.positions[0] == 0); // 文
    CHECK(result.positions[1] == 2); // 夹
}

// ============================================================================
// Real-world scenario tests
// ============================================================================

TEST_CASE("FuzzyMatch.scenario_home_directory")
{
    // Simulating "cd ~/ds" matching files
    // "Desktop" = D(0), e(1), s(2), k(3), t(4), o(5), p(6) - has 's' at position 2
    auto result1 = FuzzyMatch::matchSmartCase("Downloads", "ds");
    auto result2 = FuzzyMatch::matchSmartCase("Documents", "ds");
    auto result3 = FuzzyMatch::matchSmartCase("Desktop", "ds");

    CHECK(result1.matches);
    CHECK(result2.matches);
    CHECK(result3.matches); // "Desktop" has 'D' at 0 and 's' at 2

    // "Pictures" doesn't match "ds" - no 'd' before 's'
    auto result4 = FuzzyMatch::matchSmartCase("Pictures", "ds");
    CHECK_FALSE(result4.matches);
}

TEST_CASE("FuzzyMatch.scenario_config_files")
{
    auto result1 = FuzzyMatch::matchSmartCase(".config", "cfg");
    auto result2 = FuzzyMatch::matchSmartCase("CMakeCache.txt", "cfg");

    CHECK(result1.matches);
    CHECK_FALSE(result2.matches); // No 'g' after 'cf' in order
}

TEST_CASE("FuzzyMatch.scenario_cpp_files")
{
    // "FuzzyMatch.cpp" = F(0), u(1), z(2), z(3), y(4), M(5), a(6), t(7), c(8), h(9), .(10), c(11), p(12),
    // p(13)
    auto result = FuzzyMatch::matchSmartCase("FuzzyMatch.cpp", "fmc");
    CHECK(result.matches);
    REQUIRE(result.positions.size() == 3);
    CHECK(result.positions[0] == 0); // F
    CHECK(result.positions[1] == 5); // M
    CHECK(result.positions[2] == 8); // c (in "Match")
}

TEST_CASE("FuzzyMatch.scenario_command_completion")
{
    auto result = FuzzyMatch::matchSmartCase("git-commit", "gc");
    CHECK(result.matches);
    CHECK(result.wordStartMatches == 2); // 'g' at start, 'c' after '-'
}

// ============================================================================
// Edge cases
// ============================================================================

TEST_CASE("FuzzyMatch.repeated_chars_in_pattern")
{
    auto result = FuzzyMatch::matchSmartCase("aardvark", "aa");
    CHECK(result.matches);
    REQUIRE(result.positions.size() == 2);
    CHECK(result.positions[0] == 0);
    CHECK(result.positions[1] == 1);
}

TEST_CASE("FuzzyMatch.repeated_chars_scattered")
{
    auto result = FuzzyMatch::matchSmartCase("banana", "aaa");
    CHECK(result.matches);
    REQUIRE(result.positions.size() == 3);
    CHECK(result.positions[0] == 1); // first 'a'
    CHECK(result.positions[1] == 3); // second 'a'
    CHECK(result.positions[2] == 5); // third 'a'
}

TEST_CASE("FuzzyMatch.single_char_pattern")
{
    auto result = FuzzyMatch::matchSmartCase("Downloads", "d");
    CHECK(result.matches);
    REQUIRE(result.positions.size() == 1);
    CHECK(result.positions[0] == 0);
}

TEST_CASE("FuzzyMatch.single_char_text")
{
    auto result = FuzzyMatch::matchSmartCase("d", "d");
    CHECK(result.matches);
    REQUIRE(result.positions.size() == 1);
    CHECK(result.positions[0] == 0);
}

TEST_CASE("FuzzyMatch.pattern_at_very_end")
{
    // "testfile.txt" = t(0), e(1), s(2), t(3), f(4), i(5), l(6), e(7), .(8), t(9), x(10), t(11)
    // "txt" occurs verbatim as the trailing extension ".[txt]", so the contiguous
    // substring {9,10,11} is preferred over the scattered greedy match {0,10,11}.
    auto result = FuzzyMatch::matchSmartCase("testfile.txt", "txt");
    CHECK(result.matches);
    REQUIRE(result.positions.size() == 3);
    CHECK(result.positions[0] == 9);  // 't' in ".txt"
    CHECK(result.positions[1] == 10); // 'x'
    CHECK(result.positions[2] == 11); // last 't'
}
